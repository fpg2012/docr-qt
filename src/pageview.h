// 页面视图：显示渲染出的页面，并把识别框叠加在图片上。
#pragma once

#include <QGraphicsView>
#include <QVector>

#include "ocr.h"

class PageView : public QGraphicsView {
    Q_OBJECT
public:
    explicit PageView(QWidget *parent = nullptr);

    void setPage(const QImage &image, const QVector<ocr::Block> &blocks, bool showBoxes);
    void clearPage();
    void updateBoxes(const QVector<ocr::Block> &blocks);
    void setBoxesVisible(bool visible);
    void updateBackground();

    void fitWidth();
    void zoomBy(double factor);
    void resetZoom();

    int selectedBlock() const { return m_selected; }
    void selectBlock(int index);

signals:
    void blockClicked(int index);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QImage m_image;
    QVector<ocr::Block> m_blocks;
    QGraphicsScene *m_scene = nullptr;
    QGraphicsPixmapItem *m_pixmap = nullptr;
    QVector<QGraphicsRectItem *> m_items;
    int m_selected = -1;
    bool m_boxesVisible = true;
    bool m_fitPending = false;
    double m_zoom = 1.0;

    void rebuildItems();
    void paintItem(int index);
};
