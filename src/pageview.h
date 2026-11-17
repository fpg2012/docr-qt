// 连续页面视图：把文档所有页竖排在同一画布里，滚动跨页不停顿。
// 页面图片按可视区惰性渲染；版面框、块选择、跨页字符级文本选择都在这一个视图里完成。
#pragma once

#include <QElapsedTimer>
#include <QFont>
#include <QGraphicsView>
#include <QImage>
#include <QSet>
#include <QVector>

#include "core/ocr.h"

class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QPainter;
class Document;

class PageView : public QGraphicsView {
    Q_OBJECT
public:
    enum class Tool { Pointer, Text };

    explicit PageView(QWidget *parent = nullptr);

    // 绑定文档并重建连续页面布局；view 不拥有 Document。
    void setDocument(Document *document, bool showBoxes);
    // 清空文档视图。
    void clearPage();
    // 某一页的 OCR 结果更新后刷新版面框与文本流。
    void refreshPage(int page);

    void setBoxesVisible(bool visible);
    void updateBackground();

    void fitWidth();
    void zoomBy(double factor);
    void resetZoom();

    // 当前页（视口里可见面积最大的页）。滚动跨页时自动更新。
    int currentPage() const { return m_currentPage; }
    // 跳转到指定页并把页首滚到视口顶部。
    void setCurrentPage(int page);

    // 当前页选中的版面块下标（升序）。文本选择会返回被选中的块的并集。
    QVector<int> selection() const;
    // notify=false 时不发 selectionChanged，用于和右侧卡片列表双向同步。
    void setSelection(const QVector<int> &blocks, bool notify = true);
    void ensureBlockVisible(int index);

    void setTool(Tool tool);
    Tool tool() const { return m_tool; }

    // 拖入文件时在视口上画提示框。
    void setDropActive(bool active);

    // 按阅读顺序拼接选中文本；字符级文本选择优先。
    QString selectedText() const;
    // 复制选中内容，返回复制的块数。
    int copySelection();

    bool hasTextSelection() const { return m_textHasSelection; }
    void clearTextSelection();
    // 调试/截图用：按全局字符偏移选中一段。
    void selectTextRange(int begin, int end);

signals:
    void blockClicked(int index);
    void selectionChanged(const QVector<int> &blocks);
    void textSelectionChanged(int blocks, int chars);
    void textCopied(int blocks, int chars);
    void imageCopied();
    void imageExportRequested(int page, int block);
    void currentPageChanged(int page);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void drawForeground(QPainter *painter, const QRectF &rect) override;
    void resizeEvent(QResizeEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct PageLayout {
        QRectF rect;                 // 页面在场景里的矩形（未缩放文档像素）
        QSizeF size;
        QGraphicsRectItem *background = nullptr;
        QGraphicsPixmapItem *pixmap = nullptr;
        QVector<ocr::Block> blocks;
        QVector<int> order;          // 页面内阅读顺序
        QVector<int> flatIndex;      // 每个块在 m_flat 里的下标
        bool itemsBuilt = false;
        bool imageLoaded = false;
    };

    struct FlatBlock {
        int page = -1;
        int block = -1;
        QRectF box;                  // 场景坐标
        QString text;
        int textStart = -1;          // 在 m_flowText 里的起点
        mutable QFont font;          // 按框大小估算出的字号缓存
    };

    struct BoxRef {
        QGraphicsRectItem *item = nullptr;
        int flat = -1;
        int page = -1;
    };

    void rebuildBlocks();
    void rebuildTextFlow();
    void clearBlockItems();
    void ensurePageLoaded(int page);
    void ensurePageItems(int page);
    void unloadPage(int page);
    void ensureVisiblePages();
    void updateCurrentPageFromScroll();
    int pageAtY(qreal sceneY) const;
    int blockAtScene(const QPointF &scenePos) const;
    int blockAt(const QPoint &viewportPos) const;
    int flatForBlock(int page, int block) const;
    int localBlock(int flat) const;
    QVector<int> blocksInRect(const QRectF &sceneRect) const;
    void repaintBlock(int flat);
    void repaintAll();
    void updateHover(int flat);
    void beginBand(const QPoint &origin);
    void updateBand(const QPoint &pos);
    void finishBand(bool commit);
    void updateCursor();
    void applySelection(const QSet<int> &next, bool notify = true);
    void setCurrentPageInternal(int page, bool scroll);

    bool flatIsImage(int flat) const;
    QImage imageForFlat(int flat);
    bool copyImage(int flat);
    QRectF blockBox(int flat) const;
    int charIndexAt(int flat, const QPointF &scenePos) const;
    int textOffsetAt(const QPointF &scenePos) const;
    QFont textFontFor(int flat, const QRectF &box) const;
    void setTextRange(int begin, int end, int caret);
    void paintTextSelection(QPainter *painter, const QRectF &exposed) const;

    Document *m_doc = nullptr;
    QVector<PageLayout> m_pages;
    mutable QVector<FlatBlock> m_flat;
    QVector<BoxRef> m_items;
    QGraphicsScene *m_scene = nullptr;
    QSet<int> m_selection;  // flat 下标
    QSet<int> m_bandBase;
    int m_anchor = -1;
    int m_pendingToggle = -1;
    int m_hover = -1;
    int m_currentPage = -1;
    Tool m_tool = Tool::Pointer;
    bool m_boxesVisible = true;
    bool m_dropActive = false;
    bool m_fitMode = true;
    bool m_navigating = false;
    double m_zoom = 1.0;

    QGraphicsRectItem *m_band = nullptr;
    bool m_banding = false;
    QPoint m_bandOrigin;
    QPoint m_bandLast;

    // 字符级文本选择（所有页按阅读顺序拼接后的全局偏移）
    QString m_flowText;
    int m_textAnchor = -1;
    int m_textBegin = -1;
    int m_textEnd = -1;
    int m_textCaret = -1;
    bool m_textHasSelection = false;
    bool m_textDragging = false;

    QElapsedTimer m_wheelTimer;
    int m_wheelRun = 0;
    int m_wheelSign = 0;
};
