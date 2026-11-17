#include "blockcards.h"

#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QResizeEvent>
#include <QSet>
#include <QStyle>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>

#include "theme.h"

namespace {

constexpr int kCardHeight = 58;
constexpr int kImageCardHeight = 156;

// 用 QTextLayout 手动折两行，最后一行截断加省略号。
void drawPreview(QPainter *painter, const QRect &rect, const QString &text, int maxLines)
{
    if (text.isEmpty())
        return;
    const QFontMetrics metrics(painter->font());
    const int lineHeight = metrics.lineSpacing();

    QTextLayout layout(text, painter->font());
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QVector<QTextLine> lines;
    while (lines.size() < maxLines) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(rect.width());
        lines.append(line);
    }
    layout.endLayout();

    for (int i = 0; i < lines.size(); ++i) {
        const QTextLine &line = lines.at(i);
        QString content = text.mid(line.textStart(), line.textLength());
        const bool last = i == lines.size() - 1;
        const int consumed = line.textStart() + line.textLength();
        if (last && consumed < text.size())
            content = metrics.elidedText(text.mid(line.textStart()), Qt::ElideRight,
                                         rect.width());
        painter->drawText(QRect(rect.left(), rect.top() + i * lineHeight, rect.width(),
                                lineHeight),
                          Qt::AlignLeft | Qt::AlignVCenter, content.trimmed());
    }
}

} // namespace

BlockCardDelegate::BlockCardDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize BlockCardDelegate::sizeHint(const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
{
    const bool hasImage = !index.data(BlockPixmapRole).value<QPixmap>().isNull();
    return QSize(qMax(180, option.rect.width()), hasImage ? kImageCardHeight : kCardHeight);
}

void BlockCardDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRect outer = option.rect.adjusted(6, 2, -6, -2);
    if (outer.width() <= 0)
        return;

    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const QColor highlight = option.palette.color(QPalette::Highlight);

    QColor card = option.palette.color(QPalette::AlternateBase);
    if (card == option.palette.color(QPalette::Base))
        card = option.palette.color(QPalette::Window);
    if (selected) {
        card = highlight;
        card.setAlpha(46);
    } else if (hovered) {
        card = highlight;
        card.setAlpha(22);
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(card);
    painter->drawRect(outer);

    // 底部分隔线，卡片之间保持清晰但不抢眼。
    QColor separator = option.palette.color(QPalette::Mid);
    separator.setAlpha(90);
    painter->setPen(QPen(separator, 1));
    painter->drawLine(outer.left(), outer.bottom(), outer.right(), outer.bottom());

    if (selected) {
        painter->setPen(QPen(highlight, 1.2));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(outer.adjusted(0, 0, 0, -1));
    }

    const QString kind = index.data(BlockKindRole).toString();
    const QColor accent = theme::tone(ocr::colorForKind(kind));
    const QPixmap pixmap = index.data(BlockPixmapRole).value<QPixmap>();
    const bool hasImage = !pixmap.isNull();

    QFont labelFont = option.font;
    labelFont.setPointSizeF(option.font.pointSizeF() * 0.82);
    labelFont.setBold(true);
    QFont bodyFont = option.font;
    bodyFont.setPointSizeF(option.font.pointSizeF() * 0.92);
    const QFontMetrics labelMetrics(labelFont);

    const int padding = 8;
    const int textLeft = outer.left() + padding;
    const int textRight = outer.right() - padding;
    const int labelTop = outer.top() + 5;

    painter->setFont(labelFont);
    painter->setPen(accent);
    painter->drawText(QRect(textLeft, labelTop, textRight - textLeft - 30,
                            labelMetrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter, ocr::kindLabel(kind));

    painter->setPen(option.palette.color(QPalette::PlaceholderText));
    painter->drawText(QRect(textRight - 30, labelTop, 30, labelMetrics.height()),
                      Qt::AlignRight | Qt::AlignVCenter,
                      QStringLiteral("#%1").arg(index.row() + 1));

    if (hasImage) {
        // 图片卡片：让裁剪出的页面区域占据卡片主体，等比缩放居中。
        const QRect imageArea(outer.left() + padding, labelTop + labelMetrics.height() + 4,
                              outer.width() - padding * 2,
                              outer.bottom() - 6 - (labelTop + labelMetrics.height() + 4));
        const QSize target = pixmap.size().scaled(imageArea.size(), Qt::KeepAspectRatio);
        const QRect targetRect(imageArea.center().x() - target.width() / 2,
                               imageArea.center().y() - target.height() / 2,
                               target.width(), target.height());
        painter->drawPixmap(targetRect, pixmap);
        painter->setPen(QPen(separator, 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(targetRect.adjusted(0, 0, -1, -1));
        painter->restore();
        return;
    }

    const QString text = index.data(BlockTextRole).toString();
    if (!text.isEmpty()) {
        painter->setFont(bodyFont);
        painter->setPen(option.palette.color(QPalette::Text));
        const QRect bodyRect(textLeft, labelTop + labelMetrics.height() + 1,
                             textRight - textLeft,
                             outer.bottom() - 3 - (labelTop + labelMetrics.height() + 1));
        drawPreview(painter, bodyRect, text, 2);
    }

    painter->restore();
}

BlockCardList::BlockCardList(QWidget *parent)
    : QListWidget(parent)
{
    setItemDelegate(new BlockCardDelegate(this));
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionBehavior(QAbstractItemView::SelectItems);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setSpacing(0);

    m_empty = new QLabel(viewport());
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setForegroundRole(QPalette::PlaceholderText);
    m_empty->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_empty->setText(QStringLiteral("尚未识别"));
    m_empty->hide();

    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!m_updating && item)
            emit blockActivated(row(item));
    });
    connect(this, &QListWidget::itemSelectionChanged, this, [this] {
        if (!m_updating)
            emit cardsSelectionChanged(selectedBlocks());
    });
}

void BlockCardList::setBlocks(const QVector<ocr::Block> &blocks, const QImage &pageImage)
{
    m_updating = true;
    clear();
    for (int i = 0; i < blocks.size(); ++i) {
        const ocr::Block &block = blocks.at(i);
        auto *item = new QListWidgetItem(this);
        item->setData(BlockIndexRole, i);
        item->setData(BlockKindRole, block.kind);

        const bool isFigure = block.kind == QLatin1String("image")
                              || block.kind == QLatin1String("figure");
        QPixmap pixmap;
        if (isFigure && !pageImage.isNull() && !block.boxes.isEmpty()) {
            QRectF unionRect;
            for (const QRectF &box : block.boxes)
                unionRect = unionRect.isNull() ? box : unionRect.united(box);
            QRect pixel(qRound(unionRect.x() * pageImage.width() / 1000.0),
                        qRound(unionRect.y() * pageImage.height() / 1000.0),
                        qRound(unionRect.width() * pageImage.width() / 1000.0),
                        qRound(unionRect.height() * pageImage.height() / 1000.0));
            pixel = pixel.intersected(pageImage.rect());
            if (pixel.width() >= 8 && pixel.height() >= 8) {
                const QImage crop = pageImage.copy(pixel);
                const QImage scaled = crop.scaled(QSize(320, 320), Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation);
                pixmap = QPixmap::fromImage(scaled);
            }
        }
        item->setData(BlockPixmapRole, pixmap);

        QString text = ocr::displayText(block.text);
        if (text.isEmpty() && pixmap.isNull())
            text = QStringLiteral("（无文本）");
        text.replace(QLatin1Char('\n'), QLatin1Char(' '));
        item->setData(BlockTextRole, text);
        item->setToolTip(text);
    }
    m_updating = false;
    m_empty->setVisible(blocks.isEmpty());
    m_empty->raise();
}

void BlockCardList::showPlaceholder(const QString &text)
{
    m_empty->setText(text);
    m_empty->setVisible(count() == 0);
    m_empty->raise();
}

QVector<int> BlockCardList::selectedBlocks() const
{
    QVector<int> indices;
    for (const QListWidgetItem *item : selectedItems())
        indices.append(item->data(BlockIndexRole).toInt());
    std::sort(indices.begin(), indices.end());
    return indices;
}

void BlockCardList::setSelectedBlocks(const QVector<int> &indices)
{
    QSet<int> wanted;
    for (int index : indices)
        wanted.insert(index);

    m_updating = true;
    for (int row = 0; row < count(); ++row) {
        QListWidgetItem *item = this->item(row);
        const bool on = wanted.contains(item->data(BlockIndexRole).toInt());
        if (item->isSelected() != on)
            item->setSelected(on);
    }
    m_updating = false;
}

void BlockCardList::scrollToBlock(int index)
{
    if (index < 0 || index >= count())
        return;
    scrollToItem(item(index), QAbstractItemView::PositionAtCenter);
}

void BlockCardList::resizeEvent(QResizeEvent *event)
{
    QListWidget::resizeEvent(event);
    m_empty->setGeometry(viewport()->rect().adjusted(24, 24, -24, -24));
}

void BlockCardList::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy)) {
        emit copyRequested();
        event->accept();
        return;
    }
    QListWidget::keyPressEvent(event);
}

void BlockCardList::contextMenuEvent(QContextMenuEvent *event)
{
    QListWidgetItem *item = itemAt(event->pos());
    if (item && !item->isSelected())
        setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
    if (!item)
        return;

    QMenu menu(this);
    QAction *copy = menu.addAction(theme::icon(QStringLiteral("copy")),
                                   selectedItems().size() > 1
                                       ? QStringLiteral("复制 %1 个块的文本")
                                             .arg(selectedItems().size())
                                       : QStringLiteral("复制文本"));
    QAction *chosen = menu.exec(viewport()->mapToGlobal(event->pos()));
    if (chosen == copy)
        emit copyRequested();
}
