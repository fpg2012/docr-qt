#include "textlayout.h"

#include <QChar>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace textlayout {
namespace {

struct Band {
    int y0 = 0;
    int y1 = 0;
};

struct Range {
    int start = 0;
    int length = 0;
};

QRect pixelRect(const QRectF &region, const QSize &size)
{
    const int left = qBound(0, int(std::floor(region.left() / 1000.0 * size.width())),
                            qMax(0, size.width() - 1));
    const int top = qBound(0, int(std::floor(region.top() / 1000.0 * size.height())),
                           qMax(0, size.height() - 1));
    const int right = qBound(left + 1,
                             int(std::ceil(region.right() / 1000.0 * size.width())),
                             size.width());
    const int bottom = qBound(top + 1,
                              int(std::ceil(region.bottom() / 1000.0 * size.height())),
                              size.height());
    return QRect(left, top, right - left, bottom - top);
}

int otsuThreshold(const QImage &gray)
{
    qint64 histogram[256] = {};
    const int width = gray.width();
    const int height = gray.height();
    for (int y = 0; y < height; ++y) {
        const uchar *line = gray.constScanLine(y);
        for (int x = 0; x < width; ++x)
            ++histogram[line[x]];
    }

    const qint64 total = qint64(width) * height;
    double sum = 0;
    for (int i = 0; i < 256; ++i)
        sum += i * double(histogram[i]);

    double sumBackground = 0;
    qint64 background = 0;
    double bestVariance = -1;
    int best = 160;
    for (int t = 0; t < 256; ++t) {
        background += histogram[t];
        if (background == 0)
            continue;
        const qint64 foreground = total - background;
        if (foreground == 0)
            break;
        sumBackground += t * double(histogram[t]);
        const double meanBackground = sumBackground / double(background);
        const double meanForeground = (sum - sumBackground) / double(foreground);
        const double variance = double(background) * double(foreground)
                                * (meanBackground - meanForeground)
                                * (meanBackground - meanForeground);
        if (variance > bestVariance) {
            bestVariance = variance;
            best = t;
        }
    }
    return qBound(32, best, 224);
}

qreal charEm(QChar ch)
{
    const ushort u = ch.unicode();
    if (ch == QLatin1Char('\t'))
        return 2.0;
    if (ch == QLatin1Char(' '))
        return 0.32;
    if (ch.isSpace())
        return 0.3;
    if (u < 0x0080) {
        if ((u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z'))
            return 0.56;
        if (u == '.' || u == ',' || u == ':' || u == ';' || u == '\'' || u == '!'
            || u == '|' || u == '(' || u == ')' || u == '[' || u == ']')
            return 0.34;
        return 0.5;
    }
    // CJK、假名、全角标点以及常见符号按一个全角字宽处理。
    if ((u >= 0x1100 && u <= 0x11FF) || (u >= 0x2E80 && u <= 0xA4CF)
        || (u >= 0xAC00 && u <= 0xD7AF) || (u >= 0xF900 && u <= 0xFAFF)
        || (u >= 0xFE30 && u <= 0xFE4F) || (u >= 0xFF00 && u <= 0xFFEF)) {
        return 1.0;
    }
    return 0.62;
}

QVector<Range> wrapText(const QString &text, const QVector<qreal> &widths, qreal fontPx)
{
    QVector<Range> ranges;
    const int length = text.size();
    int pos = 0;

    for (int line = 0; pos < length; ++line) {
        if (text.at(pos) == QLatin1Char('\n')) {
            ++pos;
            --line;
            continue;
        }
        const qreal limit =
            qMax<qreal>(fontPx, widths.at(qMin(line, widths.size() - 1)));
        const int start = pos;
        qreal used = 0;
        int end = start;
        int lastSpace = -1;
        while (end < length) {
            const QChar ch = text.at(end);
            if (ch == QLatin1Char('\n'))
                break;
            const qreal advance = charEm(ch) * fontPx;
            if (end > start && used + advance > limit)
                break;
            if (ch.isSpace())
                lastSpace = end;
            used += advance;
            ++end;
        }
        if (end == start)
            end = start + 1;
        // 英文尽量在空格处断行，但不要把一行拆得太短。
        if (lastSpace > start && end < length
            && lastSpace + 1 - start >= (end - start) * 3 / 5) {
            end = lastSpace + 1;
        }
        ranges.append({start, end - start});
        pos = end;
        if (pos < length && text.at(pos) == QLatin1Char('\n'))
            ++pos;
    }

    return ranges;
}

QVector<QRectF> fallbackBoxes(const QString &text, const QRectF &region)
{
    int lines = text.count(QLatin1Char('\n')) + 1;
    if (lines <= 1 && text.size() > 24) {
        const int estimate = int(std::ceil(double(text.size()) / 48.0));
        lines = qBound(1, estimate, 80);
    }
    if (lines <= 1)
        return {region};
    const qreal height = region.height() / lines;
    QVector<QRectF> boxes;
    boxes.reserve(lines);
    for (int i = 0; i < lines; ++i) {
        boxes.append(QRectF(region.left(), region.top() + height * i + height * 0.08,
                            region.width(), height * 0.84));
    }
    return boxes;
}

bool isTableSeparator(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || !trimmed.contains(QLatin1Char('|')))
        return false;
    const QStringList cells = trimmed.split(QLatin1Char('|'), Qt::KeepEmptyParts);
    bool sawCell = false;
    for (QString cell : cells) {
        cell = cell.trimmed();
        if (cell.isEmpty())
            continue;
        sawCell = true;
        cell.remove(QLatin1Char(':'));
        if (cell.size() < 3)
            return false;
        for (QChar ch : cell) {
            if (ch != QLatin1Char('-'))
                return false;
        }
    }
    return sawCell;
}

QVector<qreal> tableColumnBounds(const QRectF &region, const QImage &pageImage,
                                int columns)
{
    if (columns < 2)
        return {region.left(), region.right()};

    QVector<qreal> bounds;
    if (!pageImage.isNull()) {
        const QRect cropRect = pixelRect(region, pageImage.size());
        if (cropRect.width() > 10 && cropRect.height() > 10) {
            const QImage crop =
                pageImage.copy(cropRect).convertToFormat(QImage::Format_Grayscale8);
            const int threshold = otsuThreshold(crop);
            QVector<int> columnInk(crop.width(), 0);
            for (int y = 0; y < crop.height(); ++y) {
                const uchar *line = crop.constScanLine(y);
                for (int x = 0; x < crop.width(); ++x) {
                    if (line[x] < threshold)
                        ++columnInk[x];
                }
            }
            const int minimum = qMax(4, int(crop.height() * 0.42));
            QVector<int> lines;
            for (int x = 1; x + 1 < crop.width(); ++x) {
                if (columnInk.at(x) >= minimum)
                    lines.append(x);
            }
            // 合并相邻的粗线/抗锯齿列。
            QVector<int> merged;
            for (int x : lines) {
                if (merged.isEmpty() || x - merged.last() > 2)
                    merged.append(x);
            }
            if (merged.size() >= columns + 1) {
                QVector<int> selected;
                selected.append(merged.first());
                for (int i = 1; i < columns; ++i) {
                    const qreal target = merged.first()
                                         + (merged.last() - merged.first())
                                               * (qreal(i) / columns);
                    int best = merged.at(1);
                    qreal bestDistance = 1e9;
                    for (int x : merged) {
                        if (x <= selected.last())
                            continue;
                        const qreal distance = qAbs(x - target);
                        if (distance < bestDistance) {
                            bestDistance = distance;
                            best = x;
                        }
                    }
                    selected.append(best);
                }
                selected.append(merged.last());
                for (int x : selected) {
                    const qreal value = (cropRect.left() + x) / qreal(pageImage.width()) * 1000.0;
                    if (bounds.isEmpty() || value - bounds.last() > 1.0)
                        bounds.append(value);
                }
            }
        }
    }

    if (bounds.size() != columns + 1) {
        bounds.clear();
        const qreal step = region.width() / columns;
        for (int i = 0; i <= columns; ++i)
            bounds.append(region.left() + step * i);
    }
    return bounds;
}

QVector<ocr::TextLine> placeTable(const QString &text, const QRectF &region,
                                  const QImage &pageImage)
{
    struct Cell {
        QString text;
        int start = 0;
        int length = 0;
    };
    struct Row {
        QVector<Cell> cells;
    };
    QVector<Row> rows;
    int offset = 0;
    int columns = 1;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const int lineStart = offset;
        offset += raw.size() + 1;
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty() || isTableSeparator(trimmed))
            continue;

        Row row;
        int search = 0;
        for (const QString &part : raw.split(QLatin1Char('|'), Qt::KeepEmptyParts)) {
            const QString cellText = part.trimmed();
            if (cellText.isEmpty())
                continue;
            int position = raw.indexOf(cellText, search);
            if (position < 0)
                position = search;
            row.cells.append({cellText, lineStart + position, int(cellText.size())});
            search = position + cellText.size();
        }
        if (row.cells.isEmpty())
            continue;
        columns = qMax(columns, row.cells.size());
        rows.append(row);
    }
    if (rows.isEmpty())
        return {};

    QVector<QRectF> boxes;
    if (!pageImage.isNull()) {
        const QVector<QRectF> detected = detectLines(pageImage, region);
        if (detected.size() == rows.size())
            boxes = detected;
    }
    if (boxes.isEmpty()) {
        const qreal rowHeight = region.height() / rows.size();
        for (int i = 0; i < rows.size(); ++i) {
            boxes.append(QRectF(region.left(), region.top() + rowHeight * i + rowHeight * 0.12,
                                region.width(), rowHeight * 0.76));
        }
    }

    const QVector<qreal> columnBounds = tableColumnBounds(region, pageImage, columns);
    QVector<ocr::TextLine> lines;
    const int rowCount = qMin(rows.size(), boxes.size());
    for (int r = 0; r < rowCount; ++r) {
        const Row &row = rows.at(r);
        const QRectF rowBox = boxes.at(r);
        if (row.cells.size() == columns && columnBounds.size() == columns + 1) {
            for (int c = 0; c < row.cells.size(); ++c) {
                const Cell &cell = row.cells.at(c);
                const qreal left = columnBounds.at(c);
                const qreal right = columnBounds.at(c + 1);
                if (right - left <= 0)
                    continue;
                lines.append({QRectF(left + (right - left) * 0.01, rowBox.top(),
                                     (right - left) * 0.98, rowBox.height()),
                              cell.text, cell.start, cell.length});
            }
        } else {
            const qreal cellWidth = rowBox.width() / qMax(1, row.cells.size());
            for (int c = 0; c < row.cells.size(); ++c) {
                const Cell &cell = row.cells.at(c);
                lines.append({QRectF(rowBox.left() + cellWidth * c, rowBox.top(),
                                     cellWidth, rowBox.height()),
                              cell.text, cell.start, cell.length});
            }
        }
    }
    return lines;
}

QVector<ocr::TextLine> placePlain(const QString &text, const QRectF &region,
                                  const QImage &pageImage, const QString &kind)
{
    QVector<QRectF> boxes;
    if (!pageImage.isNull())
        boxes = detectLines(pageImage, region);
    if (boxes.isEmpty())
        boxes = fallbackBoxes(text, region);
    if (boxes.isEmpty())
        return {};

    QVector<qreal> widths;
    widths.reserve(boxes.size());
    qreal medianHeight = 0;
    QVector<qreal> heights;
    heights.reserve(boxes.size());
    const qreal referenceWidth = pageImage.isNull() ? 1000.0 : pageImage.width();
    const qreal referenceHeight = pageImage.isNull() ? 1414.0 : pageImage.height();
    for (const QRectF &box : boxes) {
        widths.append(qMax<qreal>(1.0, box.width() / 1000.0 * referenceWidth));
        heights.append(qMax<qreal>(1.0, box.height() / 1000.0 * referenceHeight));
    }
    std::sort(heights.begin(), heights.end());
    medianHeight = heights.isEmpty() ? 12.0 : heights.at(heights.size() / 2);

    // 用字宽模型找字号：字号越大行数越多，取不超过检测行数的最大字号；
    // 这样正文不会被塞进一行，也不会因为字号过大溢出块底。
    qreal low = 2.0;
    qreal high = qMax<qreal>(4.0, medianHeight * 1.35);
    QVector<Range> best;
    for (int i = 0; i < 24; ++i) {
        const qreal mid = (low + high) / 2.0;
        const QVector<Range> ranges = wrapText(text, widths, mid);
        if (ranges.size() <= boxes.size()) {
            best = ranges;
            low = mid;
        } else {
            high = mid;
        }
    }
    if (best.isEmpty())
        best = wrapText(text, widths, qMax<qreal>(2.0, medianHeight * 0.55));

    // 极端情况下文本比检测行多，把余下字符压进最后一行，保证文本不丢失。
    if (best.size() > boxes.size() && !boxes.isEmpty()) {
        Range merged{best.at(boxes.size() - 1).start,
                     best.last().start + best.last().length
                         - best.at(boxes.size() - 1).start};
        best.resize(boxes.size() - 1);
        best.append(merged);
    }

    QVector<ocr::TextLine> lines;
    const int count = qMin(best.size(), boxes.size());
    lines.reserve(count);
    for (int i = 0; i < count; ++i) {
        const Range &range = best.at(i);
        QString lineText = text.mid(range.start, range.length);
        lineText.remove(QLatin1Char('\n'));
        lineText.remove(QLatin1Char('\r'));
        if (lineText.trimmed().isEmpty())
            continue;
        lines.append({boxes.at(i), lineText, range.start, range.length});
    }
    Q_UNUSED(kind);
    return lines;
}

} // namespace

QVector<QRectF> detectLines(const QImage &pageImage, const QRectF &region)
{
    if (pageImage.isNull() || region.width() <= 0 || region.height() <= 0)
        return {};

    const QRect cropRect = pixelRect(region, pageImage.size());
    if (cropRect.width() < 6 || cropRect.height() < 4)
        return {};

    const QImage crop = pageImage.copy(cropRect).convertToFormat(QImage::Format_Grayscale8);
    if (crop.isNull())
        return {};

    const int threshold = otsuThreshold(crop);
    QVector<int> rowInk(crop.height(), 0);
    for (int y = 0; y < crop.height(); ++y) {
        const uchar *line = crop.constScanLine(y);
        int count = 0;
        for (int x = 0; x < crop.width(); ++x) {
            if (line[x] < threshold)
                ++count;
        }
        rowInk[y] = count;
    }

    const int minInk = qMax(1, crop.width() / 260);
    QVector<int> smooth(crop.height(), 0);
    for (int y = 0; y < crop.height(); ++y) {
        const int a = rowInk.at(qMax(0, y - 1));
        const int b = rowInk.at(y);
        const int c = rowInk.at(qMin(crop.height() - 1, y + 1));
        smooth[y] = (a + 2 * b + c) / 4;
    }

    QVector<Band> raw;
    bool active = false;
    int start = 0;
    for (int y = 0; y < crop.height(); ++y) {
        const bool now = smooth.at(y) >= minInk;
        if (now && !active) {
            start = y;
            active = true;
        } else if (!now && active) {
            raw.append({start, y});
            active = false;
        }
    }
    if (active)
        raw.append({start, crop.height()});

    QVector<Band> merged;
    for (const Band &band : raw) {
        if (!merged.isEmpty() && band.y0 - merged.last().y1 <= 2)
            merged.last().y1 = band.y1;
        else
            merged.append(band);
    }

    QVector<int> heights;
    for (const Band &band : merged) {
        if (band.y1 - band.y0 >= 2)
            heights.append(band.y1 - band.y0);
    }
    if (heights.isEmpty())
        return {};
    std::sort(heights.begin(), heights.end());
    const int medianHeight = heights.at(heights.size() / 2);

    QVector<Band> bands;
    for (const Band &band : merged) {
        const int h = band.y1 - band.y0;
        if (h < 2)
            continue;
        if (medianHeight > 0 && h > medianHeight * 2.1) {
            const int parts = qBound(2, int(std::lround(double(h) / medianHeight)), 6);
            for (int i = 0; i < parts; ++i) {
                const int y0 = band.y0 + h * i / parts;
                const int y1 = band.y0 + h * (i + 1) / parts;
                if (y1 - y0 >= 2)
                    bands.append({y0, y1});
            }
        } else {
            bands.append(band);
        }
    }

    QVector<QRectF> result;
    result.reserve(bands.size());
    const qreal pageWidth = qMax(1, pageImage.width());
    const qreal pageHeight = qMax(1, pageImage.height());
    for (const Band &band : bands) {
        const int bandHeight = band.y1 - band.y0;
        const int minColumnInk = qMax(1, bandHeight / 8);
        int left = -1;
        int right = -1;
        for (int x = 0; x < crop.width(); ++x) {
            int count = 0;
            for (int y = band.y0; y < band.y1; ++y) {
                if (crop.constScanLine(y)[x] < threshold)
                    ++count;
            }
            if (count >= minColumnInk) {
                if (left < 0)
                    left = x;
                right = x + 1;
            }
        }
        if (left < 0 || right - left < 4)
            continue;
        left = qMax(0, left - 1);
        right = qMin(crop.width(), right + 1);
        const qreal nx0 = (cropRect.left() + left) / pageWidth * 1000.0;
        const qreal ny0 = (cropRect.top() + band.y0) / pageHeight * 1000.0;
        const qreal nx1 = (cropRect.left() + right) / pageWidth * 1000.0;
        const qreal ny1 = (cropRect.top() + band.y1) / pageHeight * 1000.0;
        result.append(QRectF(nx0, ny0, qMax<qreal>(0.1, nx1 - nx0),
                             qMax<qreal>(0.1, ny1 - ny0)));
    }
    return result;
}

QVector<ocr::TextLine> place(const QString &text, const QRectF &region,
                             const QImage &pageImage, const QString &kind)
{
    if (text.trimmed().isEmpty() || region.width() <= 0 || region.height() <= 0)
        return {};
    if (kind == QLatin1String("table"))
        return placeTable(text, region, pageImage);
    if (kind == QLatin1String("equation"))
        return {ocr::TextLine{region, text, 0, int(text.size())}};
    return placePlain(text, region, pageImage, kind);
}

} // namespace textlayout
