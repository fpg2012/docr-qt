#include "pageview.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>
#include <QTextOption>

#include <limits>
#include <QWheelEvent>

#include <algorithm>

#include "theme.h"

namespace {

constexpr int kBandThreshold = 3;

QPen bandPen(const QColor &color)
{
    QPen pen(color, 1.2, Qt::DashLine);
    pen.setCosmetic(true);
    pen.setDashPattern({5, 4});
    return pen;
}

// QTextLayout 不会自动为每行设置 y 坐标；不显式 setPosition 时所有 line.y()
// 都是 0，按行命中会永远落在第一行。统一在这里排版并给行定位。
void layoutTextLines(QTextLayout &layout, qreal width)
{
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    qreal y = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        line.setPosition(QPointF(0, y));
        y += line.height();
    }
    layout.endLayout();
}

int textLineCount(const QFont &font, qreal width, const QString &text)
{
    if (text.isEmpty() || width <= 0)
        return 0;
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    int lines = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        ++lines;
    }
    layout.endLayout();
    return lines;
}

} // namespace

PageView::PageView(QWidget *parent)
    : QGraphicsView(parent)
{
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setAlignment(Qt::AlignCenter);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    updateBackground();
    updateCursor();
}

void PageView::setPage(const QImage &image, const QVector<ocr::Block> &blocks, bool showBoxes)
{
    clearTextSelection();
    const bool hadSelection = !m_selection.isEmpty();
    m_image = image;
    m_blocks = blocks;
    m_selection.clear();
    m_bandBase.clear();
    m_banding = false;
    m_band = nullptr;
    m_anchor = -1;
    m_pendingToggle = -1;
    m_hover = -1;
    m_boxesVisible = showBoxes;

    m_scene->clear();
    m_pixmap = nullptr;
    m_items.clear();
    m_order.clear();

    if (image.isNull()) {
        m_texts.clear();
        m_textStart.clear();
        m_textFonts.clear();
        m_flowText.clear();
        if (hadSelection)
            emit selectionChanged({});
        return;
    }

    m_pixmap = m_scene->addPixmap(QPixmap::fromImage(image));
    m_pixmap->setTransformationMode(Qt::SmoothTransformation);
    m_pixmap->setZValue(0);
    m_scene->setSceneRect(QRectF(QPointF(0, 0), image.size()));

    rebuildItems();
    m_fitPending = true;
    fitWidth();
    horizontalScrollBar()->setValue(horizontalScrollBar()->minimum());
    verticalScrollBar()->setValue(verticalScrollBar()->minimum());
    if (hadSelection)
        emit selectionChanged({});
}

void PageView::clearPage()
{
    clearTextSelection();
    m_image = QImage();
    m_blocks.clear();
    m_order.clear();
    m_order.squeeze();
    m_items.clear();
    m_selection.clear();
    m_bandBase.clear();
    m_anchor = -1;
    m_pendingToggle = -1;
    m_hover = -1;
    m_banding = false;
    m_band = nullptr;
    m_texts.clear();
    m_textStart.clear();
    m_textFonts.clear();
    m_flowText.clear();
    m_scene->clear();
    m_pixmap = nullptr;
}

void PageView::setBoxesVisible(bool visible)
{
    m_boxesVisible = visible;
    for (const BoxRef &ref : m_items)
        ref.item->setVisible(visible);
}

void PageView::rebuildItems()
{
    for (const BoxRef &ref : m_items)
        delete ref.item;
    m_items.clear();
    m_order.clear();
    if (m_band) {
        m_scene->removeItem(m_band);
        delete m_band;
        m_band = nullptr;
        m_banding = false;
    }
    if (m_image.isNull())
        return;

    const double w = m_image.width();
    const double h = m_image.height();
    for (int i = 0; i < m_blocks.size(); ++i) {
        for (const QRectF &box : m_blocks.at(i).boxes) {
            const QRectF pixel(box.x() * w / 1000.0, box.y() * h / 1000.0,
                               box.width() * w / 1000.0, box.height() * h / 1000.0);
            auto *item = new QGraphicsRectItem(pixel);
            item->setData(0, i);
            item->setZValue(10);
            item->setToolTip(ocr::displayText(m_blocks.at(i).text).left(400));
            m_scene->addItem(item);
            m_items.append({item, i});
        }
    }

    // 阅读顺序：按块左上角，避免同一行内左右错位。
    m_order.resize(m_blocks.size());
    for (int i = 0; i < m_blocks.size(); ++i)
        m_order[i] = i;
    const auto topOf = [this](int index) {
        if (m_blocks.at(index).boxes.isEmpty())
            return 1e9;
        return m_blocks.at(index).boxes.first().top();
    };
    const auto leftOf = [this](int index) {
        if (m_blocks.at(index).boxes.isEmpty())
            return 1e9;
        return m_blocks.at(index).boxes.first().left();
    };
    std::stable_sort(m_order.begin(), m_order.end(), [&](int a, int b) {
        if (qAbs(topOf(a) - topOf(b)) > 6.0)
            return topOf(a) < topOf(b);
        return leftOf(a) < leftOf(b);
    });

    rebuildTextFlow();
    repaintAll();
    setBoxesVisible(m_boxesVisible);
}

void PageView::rebuildTextFlow()
{
    const int n = m_blocks.size();
    m_texts.resize(n);
    m_textStart = QVector<int>(n, -1);
    m_textFonts = QVector<QFont>(n);
    m_flowText.clear();

    for (int i = 0; i < n; ++i)
        m_texts[i] = ocr::displayText(m_blocks.at(i).text);

    bool first = true;
    for (int block : m_order) {
        if (m_texts.at(block).isEmpty())
            continue;
        if (!first)
            m_flowText += QLatin1Char('\n');
        m_textStart[block] = m_flowText.size();
        m_flowText += m_texts.at(block);
        first = false;
    }

    m_textAnchor = -1;
    m_textBegin = -1;
    m_textEnd = -1;
    m_textCaret = -1;
    m_textHasSelection = false;
    m_textDragging = false;
}

void PageView::repaintBlock(int index)
{
    if (index < 0 || index >= m_blocks.size())
        return;
    const QColor highlight = palette().color(QPalette::Highlight);
    const bool selected = m_selection.contains(index);
    const bool hovered = m_hover == index;
    QColor color = selected ? highlight : ocr::colorForKind(m_blocks.at(index).kind);
    QPen pen(color, selected ? 2.4 : hovered ? 2.0 : 1.4);
    pen.setCosmetic(true);
    QColor fill = color;
    fill.setAlpha(selected ? 64 : hovered ? 34 : 18);

    for (const BoxRef &ref : m_items) {
        if (ref.block != index)
            continue;
        ref.item->setPen(pen);
        ref.item->setBrush(fill);
    }
}

void PageView::repaintAll()
{
    for (int i = 0; i < m_blocks.size(); ++i)
        repaintBlock(i);
}

void PageView::applySelection(const QSet<int> &next, bool notify)
{
    if (next == m_selection)
        return;
    QSet<int> changed = m_selection;
    changed.unite(next);
    m_selection = next;
    for (int index : changed)
        repaintBlock(index);
    if (notify)
        emit selectionChanged(selection());
}

void PageView::setSelection(const QVector<int> &blocks, bool notify)
{
    clearTextSelection();
    QSet<int> next;
    for (int index : blocks) {
        if (index >= 0 && index < m_blocks.size())
            next.insert(index);
    }
    applySelection(next, notify);
}

void PageView::updateHover(int index)
{
    if (index == m_hover)
        return;
    const int previous = m_hover;
    m_hover = index;
    repaintBlock(previous);
    repaintBlock(index);
}

QRectF PageView::blockBox(int block) const
{
    QRectF rect;
    for (const BoxRef &ref : m_items) {
        if (ref.block != block)
            continue;
        rect = rect.isNull() ? ref.item->sceneBoundingRect()
                             : rect.united(ref.item->sceneBoundingRect());
    }
    return rect;
}

QFont PageView::textFontFor(int block, const QRectF &box) const
{
    if (block < 0 || block >= m_textFonts.size())
        return font();
    if (m_textFonts.at(block).pixelSize() > 0)
        return m_textFonts.at(block);

    QFont base = font();
    const QString &text = m_texts.at(block);
    const int maxPx = qBound(6, int(box.height() * 0.9), 64);
    for (int px = maxPx; px >= 6; --px) {
        base.setPixelSize(px);
        const int lines = textLineCount(base, box.width(), text);
        const qreal total = lines * QFontMetrics(base).lineSpacing();
        if (total <= box.height() * 1.08) {
            m_textFonts[block] = base;
            return base;
        }
    }
    base.setPixelSize(6);
    m_textFonts[block] = base;
    return base;
}

int PageView::charAt(int block, const QPointF &scenePos) const
{
    if (block < 0 || block >= m_texts.size() || m_texts.at(block).isEmpty())
        return -1;
    const QRectF box = blockBox(block);
    if (box.isEmpty() || box.width() <= 0)
        return -1;

    QTextLayout layout(m_texts.at(block), textFontFor(block, box));
    layoutTextLines(layout, box.width());
    if (layout.lineCount() == 0)
        return -1;

    const QPointF p = scenePos - box.topLeft();
    int bestLine = 0;
    qreal bestDistance = std::numeric_limits<qreal>::max();
    for (int i = 0; i < layout.lineCount(); ++i) {
        const QTextLine line = layout.lineAt(i);
        const qreal distance = qAbs(p.y() - (line.y() + line.height() / 2.0));
        if (distance < bestDistance) {
            bestDistance = distance;
            bestLine = i;
        }
    }
    const QTextLine line = layout.lineAt(bestLine);
    const int cursor = line.xToCursor(p.x() - line.x());
    return qBound(0, cursor, m_texts.at(block).size());
}

int PageView::textOffsetAt(const QPointF &scenePos) const
{
    const int block = blockAtScene(scenePos);
    if (block < 0 || block >= m_textStart.size() || m_textStart.at(block) < 0)
        return -1;
    const int ch = charAt(block, scenePos);
    if (ch < 0)
        return -1;
    return m_textStart.at(block) + ch;
}

void PageView::setTextRange(int begin, int end, int caret)
{
    if (m_flowText.isEmpty()) {
        clearTextSelection();
        return;
    }
    m_textBegin = qBound(0, begin, m_flowText.size());
    m_textEnd = qBound(0, end, m_flowText.size());
    if (m_textEnd < m_textBegin)
        std::swap(m_textBegin, m_textEnd);
    m_textCaret = qBound(0, caret, m_flowText.size());
    m_textHasSelection = m_textEnd > m_textBegin;

    int blocks = 0;
    for (int block : m_order) {
        if (m_textStart.at(block) < 0)
            continue;
        const int start = m_textStart.at(block);
        const int stop = start + m_texts.at(block).size();
        if (start < m_textEnd && stop > m_textBegin)
            ++blocks;
    }
    viewport()->update();
    emit textSelectionChanged(blocks, m_textHasSelection ? m_textEnd - m_textBegin : 0);
}

void PageView::selectTextRange(int begin, int end)
{
    if (m_flowText.isEmpty())
        return;
    begin = qBound(0, begin, m_flowText.size());
    end = qBound(0, end, m_flowText.size());
    m_textAnchor = begin;
    setTextRange(qMin(begin, end), qMax(begin, end), end);
}

void PageView::clearTextSelection()
{
    if (m_textBegin < 0 && m_textEnd < 0 && m_textCaret < 0 && m_textAnchor < 0)
        return;
    m_textAnchor = -1;
    m_textBegin = -1;
    m_textEnd = -1;
    m_textCaret = -1;
    m_textHasSelection = false;
    m_textDragging = false;
    viewport()->update();
    emit textSelectionChanged(0, 0);
}

QVector<int> PageView::selection() const
{
    if (m_textHasSelection) {
        QVector<int> result;
        for (int block : m_order) {
            if (m_textStart.at(block) < 0)
                continue;
            const int start = m_textStart.at(block);
            const int stop = start + m_texts.at(block).size();
            if (start < m_textEnd && stop > m_textBegin)
                result.append(block);
        }
        return result;
    }

    QVector<int> result;
    result.reserve(m_selection.size());
    for (int index : m_order) {
        if (m_selection.contains(index))
            result.append(index);
    }
    return result;
}

QString PageView::selectedText() const
{
    if (m_textHasSelection)
        return m_flowText.mid(m_textBegin, m_textEnd - m_textBegin);

    QStringList parts;
    for (int index : m_order) {
        if (!m_selection.contains(index))
            continue;
        const QString text = ocr::displayText(m_blocks.at(index).text);
        if (!text.isEmpty())
            parts << text;
    }
    return parts.join(QLatin1Char('\n'));
}

int PageView::copySelection()
{
    if (m_textHasSelection) {
        const QString text = selectedText();
        if (text.isEmpty())
            return 0;
        QApplication::clipboard()->setText(text);
        emit textCopied(selection().size(), text.size());
        return selection().size();
    }

    const int count = m_selection.size();
    if (count == 0)
        return 0;
    const QString text = selectedText();
    if (text.isEmpty())
        return 0;
    QApplication::clipboard()->setText(text);
    emit textCopied(count, text.size());
    return count;
}

int PageView::blockAtScene(const QPointF &scenePos) const
{
    for (int i = m_items.size() - 1; i >= 0; --i) {
        const BoxRef &ref = m_items.at(i);
        if (ref.item->sceneBoundingRect().contains(scenePos))
            return ref.block;
    }
    return -1;
}

int PageView::blockAt(const QPoint &viewportPos) const
{
    return blockAtScene(mapToScene(viewportPos));
}

QVector<int> PageView::blocksInRect(const QRectF &sceneRect) const
{
    QVector<int> result;
    if (sceneRect.width() < kBandThreshold && sceneRect.height() < kBandThreshold)
        return result;
    QSet<int> seen;
    for (const BoxRef &ref : m_items) {
        if (!seen.contains(ref.block) && ref.item->sceneBoundingRect().intersects(sceneRect)) {
            seen.insert(ref.block);
            result.append(ref.block);
        }
    }
    return result;
}

void PageView::beginBand(const QPoint &origin)
{
    if (!m_band) {
        m_band = new QGraphicsRectItem();
        m_band->setZValue(1000);
        m_band->setPen(bandPen(palette().color(QPalette::Highlight)));
        QColor fill = palette().color(QPalette::Highlight);
        fill.setAlpha(38);
        m_band->setBrush(fill);
        m_scene->addItem(m_band);
    }
    m_banding = true;
    m_bandOrigin = origin;
    m_bandLast = origin;
    m_band->setRect(QRectF());
}

void PageView::updateBand(const QPoint &pos)
{
    if (!m_banding || !m_band)
        return;
    m_bandLast = pos;
    if ((pos - m_bandOrigin).manhattanLength() > kBandThreshold)
        m_pendingToggle = -1;
    const QRectF rect = QRectF(mapToScene(m_bandOrigin), mapToScene(pos)).normalized();
    m_band->setRect(rect);

    QSet<int> next = m_bandBase;
    for (int index : blocksInRect(rect))
        next.insert(index);
    applySelection(next);
}

void PageView::finishBand(bool commit)
{
    if (m_banding && commit)
        updateBand(m_bandLast);
    if (m_pendingToggle >= 0) {
        QSet<int> next = m_selection;
        if (next.contains(m_pendingToggle))
            next.remove(m_pendingToggle);
        else
            next.insert(m_pendingToggle);
        applySelection(next);
        m_pendingToggle = -1;
    }
    if (m_band) {
        m_scene->removeItem(m_band);
        delete m_band;
        m_band = nullptr;
    }
    m_banding = false;
    m_bandBase.clear();
}

void PageView::ensureBlockVisible(int index)
{
    const QRectF rect = blockBox(index);
    if (!rect.isEmpty())
        ensureVisible(rect.adjusted(-20, -20, 20, 20), 24, 24);
}

void PageView::setTool(Tool tool)
{
    if (m_tool == tool)
        return;
    if (m_tool == Tool::Text)
        clearTextSelection();
    if (tool == Tool::Text)
        applySelection({});
    clearTextSelection();
    m_tool = tool;
    if (m_banding)
        finishBand(false);
    updateCursor();
}

void PageView::updateCursor()
{
    viewport()->setCursor(m_tool == Tool::Text ? Qt::IBeamCursor : Qt::OpenHandCursor);
}

void PageView::setDropActive(bool active)
{
    if (m_dropActive == active)
        return;
    m_dropActive = active;
    viewport()->update();
}

void PageView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        zoomBy(event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15);
        event->accept();
        return;
    }

    // 触控板给 pixelDelta（已是像素），普通滚轮给 angleDelta（120 = 一格）。
    // 不再交给 QGraphicsView：它一格会滚掉好几行，短页面上几乎等于整页。
    const bool fromAngle = event->pixelDelta().isNull();
    QPoint delta = fromAngle ? event->angleDelta() : event->pixelDelta();
    if (event->modifiers().testFlag(Qt::ShiftModifier) && delta.x() == 0)
        delta = QPoint(delta.y(), 0);
    if (delta.isNull()) {
        QGraphicsView::wheelEvent(event);
        return;
    }

    const auto toPixels = [fromAngle](int value) {
        return fromAngle ? qRound(value * 0.75) : value;
    };
    const int dx = toPixels(delta.x());
    const int dy = toPixels(delta.y());

    if (event->phase() == Qt::ScrollBegin)
        m_wheelOverscroll = 0;

    auto *hbar = horizontalScrollBar();
    if (dx != 0 && hbar->maximum() > hbar->minimum())
        hbar->setValue(hbar->value() - dx);

    auto *bar = verticalScrollBar();
    const bool scrollable = bar->maximum() > bar->minimum();
    const bool atTop = !scrollable || bar->value() <= bar->minimum();
    const bool atBottom = !scrollable || bar->value() >= bar->maximum();
    const bool towardEdge = dy > 0 ? atTop : atBottom;

    if (dy == 0) {
        event->accept();
        return;
    }

    if (!towardEdge) {
        bar->setValue(bar->value() - dy);
        m_wheelOverscroll = 0;
        event->accept();
        return;
    }

    // 已经贴边：累计滚动量，够一次翻页才翻，避免轻轻一碰就跳页。
    if ((m_wheelOverscroll > 0) != (dy > 0))
        m_wheelOverscroll = 0;
    m_wheelOverscroll += dy;

    constexpr int kPageFlipThreshold = 150;
    if (qAbs(m_wheelOverscroll) >= kPageFlipThreshold
        && (!m_wheelClock.isValid() || m_wheelClock.elapsed() >= 500)) {
        m_wheelClock.restart();
        m_wheelOverscroll = 0;
        emit pageStepRequested(dy < 0 ? 1 : -1);
    }
    event->accept();
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy)) {
        if (copySelection() > 0)
            event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (m_textHasSelection || m_textCaret >= 0) {
            clearTextSelection();
            event->accept();
            return;
        }
        if (!m_selection.isEmpty()) {
            m_anchor = -1;
            applySelection({});
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_PageDown || event->key() == Qt::Key_PageUp) {
        auto *bar = verticalScrollBar();
        const bool down = event->key() == Qt::Key_PageDown;
        const bool scrollable = bar->maximum() > bar->minimum();
        const bool atEdge = down ? bar->value() >= bar->maximum()
                                 : bar->value() <= bar->minimum();
        if (!scrollable || atEdge) {
            emit pageStepRequested(down ? 1 : -1);
            event->accept();
            return;
        }
    }
    QGraphicsView::keyPressEvent(event);
}

void PageView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QGraphicsView::mousePressEvent(event);
        return;
    }

    const QPoint pos = event->pos();

    if (m_tool == Tool::Text) {
        const int offset = textOffsetAt(mapToScene(pos));
        if (offset < 0) {
            clearTextSelection();
            return;
        }
        if (!m_selection.isEmpty())
            applySelection({});
        setFocus();
        m_textAnchor = offset;
        m_textDragging = true;
        setTextRange(offset, offset, offset);
        return;
    }

    const int hit = blockAt(pos);
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);

    if (ctrl) {
        // Ctrl：点一下切换单个块，拖动则从当前位置拉框批量加选。
        m_anchor = hit;
        m_pendingToggle = hit;
        m_bandBase = m_selection;
        beginBand(pos);
        return;
    }

    if (hit >= 0) {
        m_anchor = hit;
        applySelection(QSet<int>{hit});
        emit blockClicked(hit);
        return;
    }

    m_anchor = -1;
    applySelection({});
    QGraphicsView::mousePressEvent(event); // 手型拖动平移
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_textDragging) {
        const int offset = textOffsetAt(mapToScene(event->pos()));
        if (offset >= 0) {
            setTextRange(qMin(m_textAnchor, offset), qMax(m_textAnchor, offset), offset);
        }
        event->accept();
        return;
    }
    if (m_banding) {
        updateBand(event->pos());
        event->accept();
        return;
    }
    if (event->buttons().testFlag(Qt::LeftButton)) {
        QGraphicsView::mouseMoveEvent(event);
        return;
    }
    updateHover(blockAt(event->pos()));
    QGraphicsView::mouseMoveEvent(event);
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_textDragging && event->button() == Qt::LeftButton) {
        m_textDragging = false;
        event->accept();
        return;
    }
    if (m_banding && event->button() == Qt::LeftButton) {
        updateBand(event->pos());
        finishBand(true);
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void PageView::contextMenuEvent(QContextMenuEvent *event)
{
    if (m_tool == Tool::Text) {
        QMenu menu(this);
        QAction *copy = menu.addAction(theme::icon(QStringLiteral("copy")),
                                       m_textHasSelection
                                           ? QStringLiteral("复制选中文本")
                                           : QStringLiteral("复制文本"));
        copy->setEnabled(m_textHasSelection);
        menu.addSeparator();
        QAction *clear = menu.addAction(QStringLiteral("取消选择"));
        clear->setEnabled(m_textHasSelection || m_textCaret >= 0);

        QAction *chosen = menu.exec(event->globalPos());
        if (chosen == copy)
            copySelection();
        else if (chosen == clear)
            clearTextSelection();
        return;
    }

    const int hit = blockAt(event->pos());
    if (hit >= 0 && !m_selection.contains(hit)) {
        m_anchor = hit;
        applySelection(QSet<int>{hit});
        emit blockClicked(hit);
    }

    QMenu menu(this);
    const int count = m_selection.size();
    QAction *copy = menu.addAction(theme::icon(QStringLiteral("copy")),
                                   count > 1
                                       ? QStringLiteral("复制 %1 个块的文本").arg(count)
                                       : QStringLiteral("复制文本"));
    copy->setEnabled(count > 0);
    menu.addSeparator();
    QAction *clear = menu.addAction(QStringLiteral("取消选择"));
    clear->setEnabled(count > 0);

    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == copy) {
        copySelection();
    } else if (chosen == clear) {
        m_anchor = -1;
        applySelection({});
    }
}

void PageView::drawForeground(QPainter *painter, const QRectF &rect)
{
    QGraphicsView::drawForeground(painter, rect);
    paintTextSelection(painter);
}

void PageView::paintTextSelection(QPainter *painter) const
{
    if (m_textBegin < 0 && m_textCaret < 0)
        return;

    painter->save();
    QColor highlight = palette().color(QPalette::Highlight);
    QColor fill = highlight;
    fill.setAlpha(85);
    QPen caretPen(highlight, 1.2);
    caretPen.setCosmetic(true);
    bool caretDrawn = false;

    for (int block : m_order) {
        if (block < 0 || block >= m_textStart.size() || m_textStart.at(block) < 0)
            continue;
        const int blockStart = m_textStart.at(block);
        const int blockEnd = blockStart + m_texts.at(block).size();

        if (m_textHasSelection) {
            if (blockStart >= m_textEnd || blockEnd <= m_textBegin)
                continue;
        } else if (m_textCaret < blockStart || m_textCaret > blockEnd) {
            continue;
        }

        const QRectF box = blockBox(block);
        if (box.isEmpty() || box.width() <= 0)
            continue;

        QTextLayout layout(m_texts.at(block), textFontFor(block, box));
        layoutTextLines(layout, box.width());

        for (int i = 0; i < layout.lineCount(); ++i) {
            const QTextLine line = layout.lineAt(i);
            const int lineStart = line.textStart();
            const int lineEnd = lineStart + line.textLength();

            if (m_textHasSelection) {
                const int localBegin = qMax(m_textBegin - blockStart, lineStart);
                const int localEnd = qMin(m_textEnd - blockStart, lineEnd);
                if (localBegin >= localEnd)
                    continue;
                const qreal x1 = line.cursorToX(localBegin);
                const qreal x2 = line.cursorToX(localEnd);
                const QRectF r(box.left() + line.x() + x1, box.top() + line.y(),
                               qMax<qreal>(1.0, x2 - x1), line.height());
                painter->fillRect(r, fill);
            } else if (!caretDrawn && m_textCaret >= blockStart + lineStart
                       && m_textCaret <= blockStart + lineEnd) {
                const qreal x = line.cursorToX(m_textCaret - blockStart);
                painter->setPen(caretPen);
                painter->drawLine(QPointF(box.left() + line.x() + x, box.top() + line.y()),
                                  QPointF(box.left() + line.x() + x,
                                          box.top() + line.y() + line.height()));
                caretDrawn = true;
            }
        }
    }
    painter->restore();
}

void PageView::leaveEvent(QEvent *event)
{
    updateHover(-1);
    QGraphicsView::leaveEvent(event);
}

void PageView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (m_fitPending)
        fitWidth();
}

void PageView::paintEvent(QPaintEvent *event)
{
    QGraphicsView::paintEvent(event);
    if (!m_dropActive)
        return;

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor accent = palette().color(QPalette::Highlight);
    QRect area = viewport()->rect().adjusted(14, 14, -14, -14);
    QColor veil = palette().color(QPalette::Base);
    veil.setAlpha(220);
    painter.setPen(Qt::NoPen);
    painter.setBrush(veil);
    painter.drawRoundedRect(area, 14, 14);

    QColor fill = accent;
    fill.setAlpha(24);
    painter.setBrush(fill);
    QPen border(accent, 1.8, Qt::DashLine);
    border.setDashPattern({7, 5});
    painter.setPen(border);
    painter.drawRoundedRect(area.adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);

    QFont title = font();
    title.setPointSizeF(title.pointSizeF() + 1.5);
    title.setBold(true);
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.95);
    const QFontMetrics titleMetrics(title);
    const QFontMetrics smallMetrics(small);

    const QString line1 = QStringLiteral("松开以打开");
    const QString line2 = QStringLiteral("PDF · 图片 · 文件夹 · 可多选");
    const int textWidth = qMax(titleMetrics.horizontalAdvance(line1),
                               smallMetrics.horizontalAdvance(line2));
    const int boxWidth = qMin(area.width() - 24, textWidth + 56);
    const int boxHeight = titleMetrics.height() + smallMetrics.height() + 28;
    const QRect pill(area.center().x() - boxWidth / 2, area.center().y() - boxHeight / 2,
                     boxWidth, boxHeight);

    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::Base));
    painter.drawRoundedRect(pill, 10, 10);
    painter.setPen(QPen(accent, 1.2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(pill, 10, 10);

    painter.setFont(title);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRect(pill.left(), pill.top() + 8, pill.width(), titleMetrics.height()),
                     Qt::AlignHCenter | Qt::AlignVCenter, line1);

    painter.setFont(small);
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.drawText(QRect(pill.left(), pill.top() + 12 + titleMetrics.height(), pill.width(),
                           smallMetrics.height()),
                     Qt::AlignHCenter | Qt::AlignVCenter, line2);
}

void PageView::fitWidth()
{
    if (!m_pixmap || m_image.isNull())
        return;
    const int available = viewport()->width() - 24;
    if (available <= 0)
        return;
    const double factor = double(available) / m_image.width();
    resetTransform();
    scale(factor, factor);
    m_zoom = factor;
    m_fitPending = false;
}

void PageView::zoomBy(double factor)
{
    if (!m_pixmap)
        return;
    const double next = m_zoom * factor;
    if (next < 0.05 || next > 12.0)
        return;
    scale(factor, factor);
    m_zoom = next;
    m_fitPending = false;
}

void PageView::resetZoom()
{
    resetTransform();
    m_zoom = 1.0;
    m_fitPending = false;
}

void PageView::updateBackground()
{
    QColor background = palette().color(QPalette::Window);
    background = background.lightness() < 128 ? background.darker(115)
                                              : background.darker(108);
    setBackgroundBrush(background);
}
