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

    repaintAll();
    setBoxesVisible(m_boxesVisible);
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

QVector<int> PageView::selection() const
{
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
    QRectF rect;
    bool found = false;
    for (const BoxRef &ref : m_items) {
        if (ref.block != index)
            continue;
        rect = found ? rect.united(ref.item->sceneBoundingRect()) : ref.item->sceneBoundingRect();
        found = true;
    }
    if (found)
        ensureVisible(rect.adjusted(-20, -20, 20, 20), 24, 24);
}

void PageView::setTool(Tool tool)
{
    if (m_tool == tool)
        return;
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

    const int dy = event->angleDelta().y();
    auto *bar = verticalScrollBar();
    const bool scrollable = bar->maximum() > bar->minimum();
    const bool atTop = !scrollable || bar->value() <= bar->minimum();
    const bool atBottom = !scrollable || bar->value() >= bar->maximum();
    if (dy != 0 && ((dy < 0 && atBottom) || (dy > 0 && atTop))) {
        // 翻页节流，避免触控板惯性一次跳很多页。
        if (!m_wheelClock.isValid() || m_wheelClock.elapsed() > 320) {
            m_wheelClock.restart();
            emit pageStepRequested(dy < 0 ? 1 : -1);
        }
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy)) {
        if (copySelection() > 0)
            event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && !m_selection.isEmpty()) {
        m_anchor = -1;
        applySelection({});
        event->accept();
        return;
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

    if (m_tool == Tool::Text) {
        m_anchor = hit;
        m_bandBase.clear();
        if (hit >= 0)
            m_bandBase.insert(hit);
        applySelection(m_bandBase);
        if (hit >= 0)
            emit blockClicked(hit);
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
