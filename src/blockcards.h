// 右侧「版面块」卡片列表：类型色条 + 两行文本预览，支持多选与复制。
#pragma once

#include <QListWidget>
#include <QStyledItemDelegate>
#include <QVector>

#include "ocr.h"

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

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QLabel *m_empty = nullptr;
    bool m_updating = false;
};
