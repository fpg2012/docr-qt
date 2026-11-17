// 页面视图：显示渲染出的页面，并把识别框叠加在图片上。
// 支持单选/多选、Ctrl 框选、右键复制、文本连续选择和翻页手势。
#pragma once

#include <QElapsedTimer>
#include <QGraphicsView>
#include <QSet>
#include <QVector>

#include "ocr.h"

class QGraphicsPixmapItem;
class QGraphicsRectItem;

class PageView : public QGraphicsView {
    Q_OBJECT
public:
    enum class Tool { Pointer, Text };

    explicit PageView(QWidget *parent = nullptr);

    void setPage(const QImage &image, const QVector<ocr::Block> &blocks, bool showBoxes);
    void clearPage();
    void setBoxesVisible(bool visible);
    void updateBackground();

    void fitWidth();
    void zoomBy(double factor);
    void resetZoom();

    // 当前选中的版面块下标（升序）。
    QVector<int> selection() const;
    // notify=false 时不发 selectionChanged，用于和右侧卡片列表双向同步。
    void setSelection(const QVector<int> &blocks, bool notify = true);
    void ensureBlockVisible(int index);

    void setTool(Tool tool);
    Tool tool() const { return m_tool; }

    // 拖入文件时在视口上画提示框。
    void setDropActive(bool active);

    // 按阅读顺序拼接选中块的文本。
    QString selectedText() const;
    // 复制选中块，返回复制的块数。
    int copySelection();

signals:
    void blockClicked(int index);
    void selectionChanged(const QVector<int> &blocks);
    void textCopied(int blocks, int chars);
    void pageStepRequested(int delta);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct BoxRef {
        QGraphicsRectItem *item = nullptr;
        int block = -1;
    };

    int blockAtScene(const QPointF &scenePos) const;
    int blockAt(const QPoint &viewportPos) const;
    QVector<int> blocksInRect(const QRectF &sceneRect) const;
    void repaintBlock(int index);
    void repaintAll();
    void rebuildItems();
    void updateHover(int index);
    void beginBand(const QPoint &origin);
    void updateBand(const QPoint &pos);
    void finishBand(bool commit);
    void updateCursor();
    void applySelection(const QSet<int> &next, bool notify = true);

    QImage m_image;
    QVector<ocr::Block> m_blocks;
    QVector<int> m_order; // 阅读顺序（按块的左上角）
    QGraphicsScene *m_scene = nullptr;
    QGraphicsPixmapItem *m_pixmap = nullptr;
    QVector<BoxRef> m_items;
    QSet<int> m_selection;
    QSet<int> m_bandBase;
    int m_anchor = -1;
    int m_pendingToggle = -1;
    int m_hover = -1;
    Tool m_tool = Tool::Pointer;
    bool m_boxesVisible = true;
    bool m_fitPending = false;
    bool m_dropActive = false;
    double m_zoom = 1.0;

    QGraphicsRectItem *m_band = nullptr;
    bool m_banding = false;
    QPoint m_bandOrigin;
    QPoint m_bandLast;

    QElapsedTimer m_wheelClock;
};
