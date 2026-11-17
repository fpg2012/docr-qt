// 右侧「版面块」卡片列表：类型色条 + 完整文本，支持多选与复制。
#pragma once

#include <QFont>
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QVector>

#include "core/ocr.h"

class QLabel;

enum BlockCardRole {
    BlockIndexRole = Qt::UserRole + 1,
    BlockKindRole,
    BlockTextRole,
    BlockPixmapRole,
};

class BlockCardDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit BlockCardDelegate(QObject *parent = nullptr);

    // 按给定宽度和字体计算卡片高度；文本完整排版，不截断。
    int heightFor(const QModelIndex &index, const QFont &font, int width) const;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};

class BlockCardList : public QListWidget {
    Q_OBJECT
public:
    explicit BlockCardList(QWidget *parent = nullptr);

    void setBlocks(const QVector<ocr::Block> &blocks, const QImage &pageImage = QImage());
    void showPlaceholder(const QString &text);

    QVector<int> selectedBlocks() const;
    void setSelectedBlocks(const QVector<int> &indices);
    void scrollToBlock(int index);

signals:
    void blockActivated(int index);
    void cardsSelectionChanged(const QVector<int> &blocks);
    void copyRequested();
    void imageCopyRequested(int blockIndex);
    void imageExportRequested(int blockIndex);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void relayout();
    void scheduleRelayout();

    QLabel *m_empty = nullptr;
    bool m_updating = false;
    bool m_relayouting = false;
    int m_layoutWidth = -1;
};
