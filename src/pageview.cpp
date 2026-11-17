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
#include <QWheelEvent>

#include <algorithm>
#include <limits>

#include "core/document.h"
#include "theme.h"

namespace {

constexpr int kBandThreshold = 3;
constexpr qreal kPageMargin = 24.0;
constexpr qreal kPageGap = 24.0;
constexpr qreal kHorizontalPadding = 24.0;

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

QRectF pageBox(const QRectF &page, const QRectF &normalized)
{
    return QRectF(page.left() + normalized.x() * page.width() / 1000.0,
                  page.top() + normalized.y() * page.height() / 1000.0,
                  normalized.width() * page.width() / 1000.0,
                  normalized.height() * page.height() / 1000.0);
}

QRectF blockRect(const QRectF &page, const ocr::Block &block)
{
    QRectF rect;
    for (const QRectF &box : block.boxes) {
        const QRectF pixel = pageBox(page, box);
        rect = rect.isNull() ? pixel : rect.united(pixel);
    }
    return rect;
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
    setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    updateBackground();
    updateCursor();

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (m_pages.isEmpty())
            return;
        ensureVisiblePages();
        if (!m_navigating)
            updateCurrentPageFromScroll();
    });
}

void PageView::setDocument(Document *document, bool showBoxes)
{
    clearPage();
    m_doc = document;
    m_boxesVisible = showBoxes;
    if (!m_doc || m_doc->count() == 0)
        return;

    const int count = m_doc->count();
    m_pages.resize(count);
    qreal maxWidth = 1.0;
    for (int i = 0; i < count; ++i) {
        PageLayout &page = m_pages[i];
        page.size = m_doc->pageSize(i);
        if (page.size.width() < 1 || page.size.height() < 1)
            page.size = QSizeF(1000, 1414);
        maxWidth = qMax(maxWidth, page.size.width());

        const PageEntry &entry = m_doc->page(i);
        if (entry.result.ok()
            && entry.result.purpose == QStringLiteral("rich_layout_recognition")) {
            page.blocks = entry.result.blocks;
        }
    }

    qreal y = kPageMargin;
    for (PageLayout &page : m_pages) {
        const qreal x = (maxWidth - page.size.width()) / 2.0;
        page.rect = QRectF(x, y, page.size.width(), page.size.height());
        y += page.size.height() + kPageGap;
    }
    const qreal totalHeight =
        m_pages.isEmpty() ? kPageMargin * 2 : y - kPageGap + kPageMargin;
    m_scene->setSceneRect(QRectF(0, 0, maxWidth, totalHeight));

    for (PageLayout &page : m_pages) {
        auto *background = new QGraphicsRectItem(page.rect);
        background->setZValue(-1);
        background->setBrush(QColor(255, 255, 255));
        QPen border(palette().color(QPalette::Mid), 1.0);
        border.setCosmetic(true);
        background->setPen(border);
        m_scene->addItem(background);
        page.background = background;
    }

    rebuildBlocks();
    fitWidth();
    m_navigating = true;
    verticalScrollBar()->setValue(verticalScrollBar()->minimum());
    horizontalScrollBar()->setValue(horizontalScrollBar()->minimum());
    m_navigating = false;
    ensureVisiblePages();
    m_currentPage = 0;
    if (viewport()->height() > 0) {
        QRectF visible = mapToScene(viewport()->rect()).boundingRect();
        qreal bestArea = -1;
        for (int i = 0; i < m_pages.size(); ++i) {
            const QRectF inter = m_pages.at(i).rect.intersected(visible);
            const qreal area = inter.width() * inter.height();
            if (area > bestArea) {
                bestArea = area;
                m_currentPage = i;
            }
        }
    }
    emit currentPageChanged(m_currentPage);
}

void PageView::clearPage()
{
    clearTextSelection();
    m_selection.clear();
    m_bandBase.clear();
    m_anchor = -1;
    m_pendingToggle = -1;
    m_hover = -1;
    m_banding = false;
    m_band = nullptr;
    m_items.clear();
    m_loadedPages.clear();
    m_flat.clear();
    m_pages.clear();
    m_flowText.clear();
    m_textBegin = m_textEnd = m_textCaret = m_textAnchor = -1;
    m_textHasSelection = false;
    m_textDragging = false;
    m_currentPage = -1;
    m_doc = nullptr;
    m_wheelRun = 0;
    m_wheelSign = 0;
    m_wheelTimer.invalidate();
    m_scene->clear();
    m_scene->setSceneRect(QRectF());
    resetTransform();
    m_fitMode = true;
    emit currentPageChanged(-1);
    emit selectionChanged({});
}

void PageView::refreshPage(int page)
{
    if (!m_doc || page < 0 || page >= m_pages.size())
        return;
    const PageEntry &entry = m_doc->page(page);
    PageLayout &layout = m_pages[page];
    layout.blocks.clear();
    if (entry.result.ok()
        && entry.result.purpose == QStringLiteral("rich_layout_recognition")) {
        layout.blocks = entry.result.blocks;
    }
    clearTextSelection();
    applySelection({}, false);
    // 只替换本页的平面块，避免每识别完一页都重排整本 500 页。
    replacePageBlocks(page);
    ensureVisiblePages();
    emit selectionChanged({});
    update();
}

void PageView::setBoxesVisible(bool visible)
{
    m_boxesVisible = visible;
    for (const BoxRef &ref : m_items)
        ref.item->setVisible(visible);
}

void PageView::updateBackground()
{
    QColor background = palette().color(QPalette::Window);
    background = background.lightness() < 128 ? background.darker(115)
                                              : background.darker(108);
    setBackgroundBrush(background);
    const QColor border = palette().color(QPalette::Mid);
    for (PageLayout &page : m_pages) {
        if (!page.background)
            continue;
        QPen pen(border, 1.0);
        pen.setCosmetic(true);
        page.background->setPen(pen);
    }
}

void PageView::rebuildPageOrder(int page)
{
    PageLayout &layout = m_pages[page];
    layout.order.resize(layout.blocks.size());
    for (int i = 0; i < layout.blocks.size(); ++i)
        layout.order[i] = i;

    const auto topOf = [&layout](int index) {
        if (layout.blocks.at(index).boxes.isEmpty())
            return 1e9;
        return layout.blocks.at(index).boxes.first().top();
    };
    const auto leftOf = [&layout](int index) {
        if (layout.blocks.at(index).boxes.isEmpty())
            return 1e9;
        return layout.blocks.at(index).boxes.first().left();
    };
    std::stable_sort(layout.order.begin(), layout.order.end(), [&](int a, int b) {
        if (qAbs(topOf(a) - topOf(b)) > 6.0)
            return topOf(a) < topOf(b);
        return leftOf(a) < leftOf(b);
    });

    layout.flatIndex = QVector<int>(layout.blocks.size(), -1);
    for (int position = 0; position < layout.order.size(); ++position)
        layout.flatIndex[layout.order.at(position)] = position;
}

void PageView::rebuildBlocks()
{
    clearBlockItems();
    m_flat.clear();

    for (int p = 0; p < m_pages.size(); ++p) {
        PageLayout &page = m_pages[p];
        rebuildPageOrder(p);
        page.flatStart = m_flat.size();
        page.flatCount = page.order.size();
        for (int b : page.order) {
            const ocr::Block &block = page.blocks.at(b);
            FlatBlock flat;
            flat.page = p;
            flat.block = b;
            flat.box = blockRect(page.rect, block);
            flat.text = ocr::displayText(block.text);
            m_flat.append(flat);
        }
    }
    rebuildTextFlow();
}

void PageView::replacePageBlocks(int page)
{
    if (page < 0 || page >= m_pages.size())
        return;

    clearPageItems(page);
    PageLayout &layout = m_pages[page];
    rebuildPageOrder(page);

    QVector<FlatBlock> fresh;
    fresh.reserve(layout.order.size());
    for (int b : layout.order) {
        const ocr::Block &block = layout.blocks.at(b);
        FlatBlock flat;
        flat.page = page;
        flat.block = b;
        flat.box = blockRect(layout.rect, block);
        flat.text = ocr::displayText(block.text);
        fresh.append(flat);
    }

    const int oldCount = layout.flatCount;
    const int delta = fresh.size() - oldCount;
    const int start = layout.flatStart;

    if (oldCount > 0)
        m_flat.remove(start, oldCount);
    for (int i = page + 1; i < m_pages.size(); ++i)
        m_pages[i].flatStart += delta;
    if (!fresh.isEmpty()) {
        m_flat.insert(start, fresh.size(), FlatBlock());
        for (int i = 0; i < fresh.size(); ++i)
            m_flat[start + i] = fresh.at(i);
    }

    layout.flatStart = start;
    layout.flatCount = fresh.size();
    if (m_hover >= start + oldCount)
        m_hover += delta;
    else if (m_hover >= start)
        m_hover = -1;

    // 后面的页已经建好的框仍指向旧 flat 下标，统一平移。
    for (BoxRef &ref : m_items) {
        if (ref.page > page)
            ref.flat += delta;
    }

    rebuildTextFlow();
}

void PageView::rebuildTextFlow()
{
    m_flowText.clear();
    qsizetype needed = 0;
    for (const FlatBlock &block : m_flat) {
        if (block.text.isEmpty())
            continue;
        needed += block.text.size() + 1;
    }
    if (m_flowText.capacity() < needed)
        m_flowText.reserve(needed);

    bool first = true;
    for (FlatBlock &block : m_flat) {
        if (block.text.isEmpty()) {
            block.textStart = -1;
            continue;
        }
        if (!first)
            m_flowText += QLatin1Char('\n');
        block.textStart = m_flowText.size();
        m_flowText += block.text;
        first = false;
    }
}

void PageView::clearBlockItems()
{
    for (const BoxRef &ref : m_items)
        delete ref.item;
    m_items.clear();
    for (PageLayout &page : m_pages)
        page.itemsBuilt = false;
}

void PageView::clearPageItems(int page)
{
    if (page < 0 || page >= m_pages.size())
        return;
    for (int i = m_items.size() - 1; i >= 0; --i) {
        if (m_items.at(i).page != page)
            continue;
        delete m_items.at(i).item;
        m_items.removeAt(i);
    }
    m_pages[page].itemsBuilt = false;
}

void PageView::ensurePageItems(int page)
{
    if (page < 0 || page >= m_pages.size())
        return;
    PageLayout &layout = m_pages[page];
    if (layout.itemsBuilt)
        return;
    layout.itemsBuilt = true;
    if (layout.blocks.isEmpty())
        return;

    for (int b : layout.order) {
        const int flat = flatForBlock(page, b);
        if (flat < 0 || flat >= m_flat.size())
            continue;
        const ocr::Block &block = layout.blocks.at(b);
        for (const QRectF &box : block.boxes) {
            auto *item = new QGraphicsRectItem(pageBox(layout.rect, box));
            item->setZValue(10);
            item->setToolTip(ocr::displayText(block.text).left(400));
            item->setVisible(m_boxesVisible);
            m_scene->addItem(item);
            m_items.append({item, flat, page});
        }
        // 新页面的框一出现就应该带类型颜色，而不是等鼠标悬停才上色。
        repaintBlock(flat);
    }
}

void PageView::ensurePageLoaded(int page)
{
    if (!m_doc || page < 0 || page >= m_pages.size())
        return;
    PageLayout &layout = m_pages[page];
    if (!layout.imageLoaded) {
        const QImage image = m_doc->ensureImage(page);
        if (!image.isNull()) {
            if (!layout.pixmap) {
                layout.pixmap = new QGraphicsPixmapItem;
                layout.pixmap->setZValue(0);
                layout.pixmap->setTransformationMode(Qt::SmoothTransformation);
                m_scene->addItem(layout.pixmap);
            }
            layout.pixmap->setPixmap(QPixmap::fromImage(image));
            layout.pixmap->setTransform(QTransform::fromScale(
                layout.rect.width() / image.width(), layout.rect.height() / image.height()));
            layout.pixmap->setPos(layout.rect.topLeft());
            layout.pixmap->setVisible(true);
            layout.imageLoaded = true;
            m_loadedPages.insert(page);
        }
    }
    ensurePageItems(page);
}

void PageView::unloadPage(int page)
{
    if (page < 0 || page >= m_pages.size())
        return;
    PageLayout &layout = m_pages[page];
    if (layout.pixmap) {
        m_scene->removeItem(layout.pixmap);
        delete layout.pixmap;
        layout.pixmap = nullptr;
    }
    layout.imageLoaded = false;
    m_loadedPages.remove(page);
    clearPageItems(page);
    if (m_doc)
        m_doc->releaseImage(page);
}

void PageView::ensureVisiblePages()
{
    if (!m_doc || m_pages.isEmpty())
        return;
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    const qreal h = qMax<qreal>(visible.height(), 400.0);
    const QRectF load = visible.adjusted(0, -h * 0.75, 0, h * 0.75);
    const QRectF keep = visible.adjusted(0, -h * 1.75, 0, h * 1.75);

    int first = 0;
    int last = -1;
    visiblePageRange(load, &first, &last);
    for (int i = first; i <= last; ++i) {
        if (m_pages.at(i).rect.intersects(load))
            ensurePageLoaded(i);
    }

    // 已经载入的页不多，按集合回收 pixmap；下面的图片缓存检查只做轻量遍历。
    for (auto it = m_loadedPages.begin(); it != m_loadedPages.end();) {
        const int i = *it;
        if (!keep.intersects(m_pages.at(i).rect)) {
            it = m_loadedPages.erase(it);
            unloadPage(i);
        } else {
            ++it;
        }
    }

    // OCR 上传等路径可能留下没有 pixmap 的整页缓存；只保留 keep 区。
    for (int i = 0; i < m_pages.size(); ++i) {
        if (!m_loadedPages.contains(i) && !keep.intersects(m_pages.at(i).rect))
            m_doc->releaseImage(i);
    }
}

void PageView::visiblePageRange(const QRectF &sceneRect, int *first, int *last) const
{
    *first = 0;
    *last = -1;
    if (m_pages.isEmpty() || sceneRect.isEmpty())
        return;

    int lo = 0;
    int hi = m_pages.size() - 1;
    int begin = m_pages.size();
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (m_pages.at(mid).rect.bottom() < sceneRect.top()) {
            lo = mid + 1;
        } else {
            begin = mid;
            hi = mid - 1;
        }
    }

    lo = 0;
    hi = m_pages.size() - 1;
    int end = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (m_pages.at(mid).rect.top() <= sceneRect.bottom()) {
            end = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    *first = qBound(0, begin, m_pages.size() - 1);
    *last = end;
}

int PageView::pageAtY(qreal sceneY) const
{
    if (m_pages.isEmpty())
        return -1;
    int lo = 0;
    int hi = m_pages.size() - 1;
    int result = 0;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (m_pages.at(mid).rect.top() <= sceneY + 0.5) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

void PageView::updateCurrentPageFromScroll()
{
    if (m_pages.isEmpty())
        return;
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    int first = 0;
    int last = -1;
    visiblePageRange(visible, &first, &last);

    int best = -1;
    qreal bestArea = 0;
    for (int i = first; i <= last; ++i) {
        const QRectF inter = m_pages.at(i).rect.intersected(visible);
        const qreal area = inter.width() * inter.height();
        if (area > bestArea) {
            bestArea = area;
            best = i;
        }
    }
    if (best < 0)
        best = pageAtY(visible.top());
    if (best != m_currentPage) {
        m_currentPage = best;
        emit currentPageChanged(best);
        emit selectionChanged(selection());
    }
}

void PageView::setCurrentPage(int page)
{
    setCurrentPageInternal(page, true);
}

void PageView::setCurrentPageInternal(int page, bool scroll)
{
    if (m_pages.isEmpty())
        return;
    page = qBound(0, page, m_pages.size() - 1);
    ensurePageLoaded(page);
    if (scroll) {
        const qreal scale = transform().m11();
        const int target = qRound(m_pages.at(page).rect.top() * scale);
        m_navigating = true;
        verticalScrollBar()->setValue(target);
        m_navigating = false;
    }
    ensureVisiblePages();
    if (m_currentPage != page) {
        m_currentPage = page;
        emit currentPageChanged(page);
        emit selectionChanged(selection());
    }
}

int PageView::flatForBlock(int page, int block) const
{
    if (page < 0 || page >= m_pages.size())
        return -1;
    const PageLayout &layout = m_pages.at(page);
    const int local = layout.flatIndex.value(block, -1);
    return local < 0 ? -1 : layout.flatStart + local;
}

int PageView::localBlock(int flat) const
{
    if (flat < 0 || flat >= m_flat.size())
        return -1;
    return m_flat.at(flat).block;
}

QRectF PageView::blockBox(int flat) const
{
    if (flat < 0 || flat >= m_flat.size())
        return {};
    return m_flat.at(flat).box;
}

int PageView::charIndexAt(int flat, const QPointF &scenePos) const
{
    if (flat < 0 || flat >= m_flat.size())
        return -1;
    const FlatBlock &block = m_flat.at(flat);
    if (block.text.isEmpty())
        return -1;
    const QRectF box = block.box;
    if (box.isEmpty() || box.width() <= 0)
        return -1;

    QTextLayout layout(block.text, textFontFor(flat, box));
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
    return qBound(0, cursor, block.text.size());
}

int PageView::textOffsetAt(const QPointF &scenePos) const
{
    const int flat = blockAtScene(scenePos);
    if (flat < 0 || flat >= m_flat.size() || m_flat.at(flat).textStart < 0)
        return -1;
    const int ch = charIndexAt(flat, scenePos);
    if (ch < 0)
        return -1;
    return m_flat.at(flat).textStart + ch;
}

QFont PageView::textFontFor(int flat, const QRectF &box) const
{
    if (flat < 0 || flat >= m_flat.size())
        return font();
    FlatBlock &block = m_flat[flat];
    if (block.font.pixelSize() > 0)
        return block.font;
    if (block.text.isEmpty() || box.isEmpty() || box.width() <= 0)
        return font();

    QFont base = font();
    const int maxPx = qBound(6, int(box.height() * 0.9), 64);
    for (int px = maxPx; px >= 6; --px) {
        base.setPixelSize(px);
        const int lines = textLineCount(base, box.width(), block.text);
        const qreal total = lines * QFontMetrics(base).lineSpacing();
        if (total <= box.height() * 1.08) {
            block.font = base;
            return base;
        }
    }
    base.setPixelSize(6);
    block.font = base;
    return base;
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
    for (const FlatBlock &block : m_flat) {
        if (block.textStart < 0)
            continue;
        const int start = block.textStart;
        const int stop = start + block.text.size();
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
    if (m_currentPage < 0 || m_currentPage >= m_pages.size())
        return {};
    const PageLayout &page = m_pages.at(m_currentPage);
    QVector<int> result;
    for (int b : page.order) {
        const int flat = flatForBlock(m_currentPage, b);
        if (flat >= 0 && m_selection.contains(flat))
            result.append(b);
    }
    return result;
}

QString PageView::selectedText() const
{
    if (m_textHasSelection)
        return m_flowText.mid(m_textBegin, m_textEnd - m_textBegin);

    if (m_currentPage < 0 || m_currentPage >= m_pages.size())
        return {};
    const PageLayout &page = m_pages.at(m_currentPage);
    QStringList parts;
    for (int b : page.order) {
        const int flat = flatForBlock(m_currentPage, b);
        if (flat < 0 || !m_selection.contains(flat))
            continue;
        const QString text = m_flat.at(flat).text;
        if (!text.isEmpty())
            parts.append(text);
    }
    return parts.join(QLatin1Char('\n'));
}

int PageView::copySelection()
{
    if (!m_textHasSelection) {
        const QVector<int> blocks = selection();
        bool allImages = !blocks.isEmpty();
        for (int block : blocks) {
            const int flat = flatForBlock(m_currentPage, block);
            if (flat < 0 || !flatIsImage(flat)) {
                allImages = false;
                break;
            }
        }
        if (allImages) {
            const int flat = flatForBlock(m_currentPage, blocks.first());
            if (flat >= 0 && copyImage(flat))
                return 1;
        }
    }

    const QString text = selectedText();
    if (text.isEmpty())
        return 0;
    QApplication::clipboard()->setText(text);
    int blocks = 0;
    if (m_textHasSelection) {
        for (const FlatBlock &block : m_flat) {
            if (block.textStart < 0)
                continue;
            if (block.textStart < m_textEnd
                && block.textStart + block.text.size() > m_textBegin)
                ++blocks;
        }
    } else {
        blocks = selection().size();
    }
    emit textCopied(blocks, text.size());
    return blocks;
}

bool PageView::flatIsImage(int flat) const
{
    if (flat < 0 || flat >= m_flat.size())
        return false;
    const FlatBlock &block = m_flat.at(flat);
    if (block.page < 0 || block.page >= m_pages.size())
        return false;
    const PageLayout &page = m_pages.at(block.page);
    if (block.block < 0 || block.block >= page.blocks.size())
        return false;
    return ocr::isImageKind(page.blocks.at(block.block).kind);
}

QImage PageView::imageForFlat(int flat)
{
    if (!m_doc || flat < 0 || flat >= m_flat.size())
        return {};
    const FlatBlock &block = m_flat.at(flat);
    return m_doc->blockImage(block.page, block.block);
}

bool PageView::copyImage(int flat)
{
    const QImage image = imageForFlat(flat);
    if (image.isNull())
        return false;
    QApplication::clipboard()->setImage(image);
    emit imageCopied();
    return true;
}

void PageView::applySelection(const QSet<int> &next, bool notify)
{
    if (next == m_selection)
        return;
    QSet<int> changed = m_selection;
    changed.unite(next);
    m_selection = next;
    for (int flat : changed)
        repaintBlock(flat);
    if (notify)
        emit selectionChanged(selection());
}

void PageView::setSelection(const QVector<int> &blocks, bool notify)
{
    clearTextSelection();
    QSet<int> next;
    for (int block : blocks) {
        const int flat = flatForBlock(m_currentPage, block);
        if (flat >= 0)
            next.insert(flat);
    }
    applySelection(next, notify);
}

void PageView::repaintBlock(int flat)
{
    if (flat < 0 || flat >= m_flat.size())
        return;
    const FlatBlock &block = m_flat.at(flat);
    const ocr::Block &data = m_pages.at(block.page).blocks.at(block.block);
    const QColor highlight = palette().color(QPalette::Highlight);
    const bool selected = m_selection.contains(flat);
    const bool hovered = m_hover == flat;
    QColor color = selected ? highlight : ocr::colorForKind(data.kind);
    QPen pen(color, selected ? 2.4 : hovered ? 2.0 : 1.4);
    pen.setCosmetic(true);
    QColor fill = color;
    fill.setAlpha(selected ? 64 : hovered ? 34 : 18);

    for (const BoxRef &ref : m_items) {
        if (ref.flat != flat)
            continue;
        ref.item->setPen(pen);
        ref.item->setBrush(fill);
    }
}

void PageView::repaintAll()
{
    for (int i = 0; i < m_flat.size(); ++i)
        repaintBlock(i);
}

void PageView::updateHover(int flat)
{
    if (flat == m_hover)
        return;
    const int previous = m_hover;
    m_hover = flat;
    repaintBlock(previous);
    repaintBlock(flat);
}

int PageView::blockAtScene(const QPointF &scenePos) const
{
    for (int i = m_items.size() - 1; i >= 0; --i) {
        const BoxRef &ref = m_items.at(i);
        if (ref.item->sceneBoundingRect().contains(scenePos))
            return ref.flat;
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
        if (ref.page != m_currentPage || seen.contains(ref.flat))
            continue;
        if (!ref.item->sceneBoundingRect().intersects(sceneRect))
            continue;
        seen.insert(ref.flat);
        result.append(ref.flat);
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
    for (int flat : blocksInRect(rect))
        next.insert(flat);
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
    const int flat = flatForBlock(m_currentPage, index);
    if (flat < 0)
        return;
    const QRectF rect = blockBox(flat);
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
        const int direction = event->angleDelta().y() != 0 ? event->angleDelta().y()
                                                           : event->pixelDelta().y();
        zoomBy(direction > 0 ? 1.15 : 1.0 / 1.15);
        event->accept();
        return;
    }

    // 触控板给 pixelDelta（已是像素），普通滚轮给 angleDelta（120 = 一格）。
    // 连续画布直接滚动；滚动距离随连续滚动量加速，快速翻很多页时不至于太慢。
    const bool fromAngle = event->pixelDelta().isNull();
    QPoint delta = fromAngle ? event->angleDelta() : event->pixelDelta();
    if (event->modifiers().testFlag(Qt::ShiftModifier) && delta.x() == 0)
        delta = QPoint(delta.y(), 0);
    if (delta.isNull()) {
        QGraphicsView::wheelEvent(event);
        return;
    }

    int dx = delta.x();
    int dy = delta.y();

    const int sign = dy != 0 ? (dy > 0 ? 1 : -1) : (dx > 0 ? 1 : -1);
    if (event->phase() == Qt::ScrollBegin) {
        m_wheelRun = 0;
        m_wheelSign = 0;
        m_wheelTimer.invalidate();
    }
    const qint64 gap = m_wheelTimer.isValid() ? m_wheelTimer.elapsed() : 100000;
    m_wheelTimer.restart();
    if (gap > 180 || sign != m_wheelSign)
        m_wheelRun = 0;
    m_wheelSign = sign;

    const double cap = fromAngle ? 3.0 : 1.0;
    const double boost = 1.0 + qMin(cap, m_wheelRun / 360.0);
    m_wheelRun += qAbs(dy != 0 ? dy : dx);
    dx = qRound(dx * boost);
    dy = qRound(dy * boost);

    if (dx != 0)
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - dx);
    if (dy != 0)
        verticalScrollBar()->setValue(verticalScrollBar()->value() - dy);
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
    if (event->key() == Qt::Key_PageDown || event->key() == Qt::Key_PageUp
        || event->key() == Qt::Key_Space) {
        const bool up = event->key() == Qt::Key_PageUp
                        || (event->key() == Qt::Key_Space
                            && event->modifiers().testFlag(Qt::ShiftModifier));
        const int step = qRound(viewport()->height() * 0.88);
        verticalScrollBar()->setValue(verticalScrollBar()->value() + (up ? -step : step));
        event->accept();
        return;
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
    if (hit >= 0) {
        const int page = m_flat.at(hit).page;
        if (page != m_currentPage)
            setCurrentPageInternal(page, !ctrl); // Ctrl 框选不能移动画布
    }

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
        emit blockClicked(localBlock(hit));
        return;
    }

    m_anchor = -1;
    applySelection({});
    QGraphicsView::mousePressEvent(event); // 手型拖动平移
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_textDragging) {
        // 拖到视口上下边缘时自动滚动，方便跨页连续选择。
        const int edge = 26;
        if (event->pos().y() < edge)
            verticalScrollBar()->setValue(verticalScrollBar()->value() - 24);
        else if (event->pos().y() > viewport()->height() - edge)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + 24);

        const int offset = textOffsetAt(mapToScene(event->pos()));
        if (offset >= 0)
            setTextRange(qMin(m_textAnchor, offset), qMax(m_textAnchor, offset), offset);
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
    if (hit >= 0) {
        const int page = m_flat.at(hit).page;
        if (page != m_currentPage)
            setCurrentPageInternal(page, true);
    }
    if (hit >= 0 && !m_selection.contains(hit)) {
        m_anchor = hit;
        applySelection(QSet<int>{hit});
        emit blockClicked(localBlock(hit));
    }

    QMenu menu(this);
    const int count = m_selection.size();
    const bool imageHit = hit >= 0 && flatIsImage(hit);
    QAction *copyImageAction = nullptr;
    QAction *exportImageAction = nullptr;
    if (imageHit) {
        copyImageAction = menu.addAction(theme::icon(QStringLiteral("copy")),
                                         QStringLiteral("复制图片"));
        exportImageAction = menu.addAction(QStringLiteral("导出 PNG…"));
        menu.addSeparator();
    }
    QAction *copy = menu.addAction(theme::icon(QStringLiteral("copy")),
                                   count > 1
                                       ? QStringLiteral("复制 %1 个块的文本").arg(count)
                                       : QStringLiteral("复制文本"));
    copy->setEnabled(count > 0 && !selectedText().trimmed().isEmpty());
    menu.addSeparator();
    QAction *clear = menu.addAction(QStringLiteral("取消选择"));
    clear->setEnabled(count > 0);

    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == copyImageAction) {
        copyImage(hit);
    } else if (chosen == exportImageAction) {
        const FlatBlock &block = m_flat.at(hit);
        emit imageExportRequested(block.page, block.block);
    } else if (chosen == copy) {
        copySelection();
    } else if (chosen == clear) {
        m_anchor = -1;
        applySelection({});
    }
}

void PageView::drawForeground(QPainter *painter, const QRectF &rect)
{
    QGraphicsView::drawForeground(painter, rect);
    paintTextSelection(painter, rect);
}

void PageView::paintTextSelection(QPainter *painter, const QRectF &exposed) const
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

    for (int blockIndex = 0; blockIndex < m_flat.size(); ++blockIndex) {
        const FlatBlock &block = m_flat.at(blockIndex);
        if (block.textStart < 0)
            continue;
        const int blockStart = block.textStart;
        const int blockEnd = blockStart + block.text.size();

        if (m_textHasSelection) {
            if (blockStart >= m_textEnd || blockEnd <= m_textBegin)
                continue;
        } else if (m_textCaret < blockStart || m_textCaret > blockEnd) {
            continue;
        }

        const QRectF box = block.box;
        if (box.isEmpty() || box.width() <= 0 || !box.intersects(exposed))
            continue;

        QTextLayout layout(block.text, textFontFor(blockIndex, box));
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
    if (m_fitMode && !m_pages.isEmpty())
        fitWidth();
    else
        ensureVisiblePages();
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

    QPen dashed(accent, 1.6, Qt::DashLine);
    dashed.setDashPattern({6, 5});
    painter.setPen(dashed);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(area.adjusted(1, 1, -1, -1), 14, 14);

    QFont title = font();
    title.setPointSizeF(title.pointSizeF() * 1.25);
    title.setBold(true);
    QFont small = font();
    QFontMetrics titleMetrics(title);
    QFontMetrics smallMetrics(small);
    const QString line1 = QStringLiteral("松开以打开");
    const QString line2 = QStringLiteral("PDF / 图片 / 图片文件夹");
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
    m_fitMode = true;
    if (!m_pages.isEmpty()) {
        const QRectF rect = m_scene->sceneRect();
        const qreal available = viewport()->width() - kHorizontalPadding * 2;
        if (rect.width() <= 0 || available <= 0)
            return;
        const QPointF anchor = mapToScene(viewport()->rect().center());
        const qreal factor = available / rect.width();
        m_navigating = true;
        resetTransform();
        scale(factor, factor);
        m_zoom = factor;
        if (m_currentPage >= 0)
            centerOn(anchor);
        m_navigating = false;
        ensureVisiblePages();
        if (m_currentPage >= 0)
            updateCurrentPageFromScroll();
    }
}

void PageView::zoomBy(double factor)
{
    if (m_pages.isEmpty())
        return;
    const double next = m_zoom * factor;
    if (next < 0.05 || next > 12.0)
        return;
    m_fitMode = false;
    const QPointF anchor = mapToScene(viewport()->rect().center());
    scale(factor, factor);
    centerOn(anchor);
    m_zoom = next;
    ensureVisiblePages();
    updateCurrentPageFromScroll();
}

void PageView::resetZoom()
{
    if (m_pages.isEmpty())
        return;
    m_fitMode = false;
    const QPointF anchor = mapToScene(viewport()->rect().center());
    resetTransform();
    m_zoom = 1.0;
    centerOn(anchor);
    ensureVisiblePages();
    if (m_currentPage >= 0)
        updateCurrentPageFromScroll();
}
