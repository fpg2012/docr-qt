#include "pageview.h"

#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>

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
    updateBackground();
}

void PageView::setPage(const QImage &image, const QVector<ocr::Block> &blocks, bool showBoxes)
{
    m_image = image;
    m_blocks = blocks;
    m_selected = -1;
    m_boxesVisible = showBoxes;

    m_scene->clear();
    m_pixmap = nullptr;
    m_items.clear();

    if (image.isNull())
        return;

    m_pixmap = m_scene->addPixmap(QPixmap::fromImage(image));
    m_pixmap->setTransformationMode(Qt::SmoothTransformation);
    m_pixmap->setZValue(0);
    m_scene->setSceneRect(QRectF(QPointF(0, 0), image.size()));

    rebuildItems();
    m_fitPending = true;
    fitWidth();
}

void PageView::clearPage()
{
    m_image = QImage();
    m_blocks.clear();
    m_items.clear();
    m_selected = -1;
    m_scene->clear();
    m_pixmap = nullptr;
}

void PageView::updateBoxes(const QVector<ocr::Block> &blocks)
{
    m_blocks = blocks;
    m_selected = -1;
    rebuildItems();
}

void PageView::setBoxesVisible(bool visible)
{
    m_boxesVisible = visible;
    for (auto *item : m_items)
        item->setVisible(visible);
}

void PageView::rebuildItems()
{
    for (auto *item : m_items)
        delete item;
    m_items.clear();
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
            item->setAcceptHoverEvents(true);
            item->setFlag(QGraphicsItem::ItemIsSelectable, true);
            item->setToolTip(m_blocks.at(i).text.left(300));
            m_scene->addItem(item);
            m_items.append(item);
        }
    }
    for (int i = 0; i < m_items.size(); ++i)
        paintItem(m_items.at(i)->data(0).toInt());
    setBoxesVisible(m_boxesVisible);
}

void PageView::paintItem(int index)
{
    if (index < 0 || index >= m_blocks.size())
        return;
    const QColor color = ocr::colorForKind(m_blocks.at(index).kind);
    for (auto *item : m_items) {
        if (item->data(0).toInt() != index)
            continue;
        const bool selected = index == m_selected;
        QPen pen(color, selected ? 4.0 : 1.6);
        pen.setCosmetic(true);
        item->setPen(pen);
        QColor fill = color;
        fill.setAlpha(selected ? 60 : 22);
        item->setBrush(fill);
    }
}

void PageView::selectBlock(int index)
{
    const int previous = m_selected;
    m_selected = index;
    paintItem(previous);
    paintItem(index);
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

void PageView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        zoomBy(event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15);
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void PageView::mousePressEvent(QMouseEvent *event)
{
    QGraphicsView::mousePressEvent(event);
    const QPointF scenePos = mapToScene(event->pos());
    const auto items = m_scene->items(scenePos);
    for (QGraphicsItem *item : items) {
        const QVariant data = item->data(0);
        if (data.isValid()) {
            const int index = data.toInt();
            selectBlock(index);
            emit blockClicked(index);
            return;
        }
    }
}

void PageView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (m_fitPending)
        fitWidth();
}

void PageView::updateBackground()
{
    QColor background = palette().color(QPalette::Window);
    background = background.lightness() < 128 ? background.darker(115)
                                              : background.darker(108);
    setBackgroundBrush(background);
}
