#include "pdfexport.h"

#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QMarginsF>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include "ocr.h"
#include "unicodenormalize.h"

namespace pdfexport {
namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString hexToUnicode(const QString &hex)
{
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    if (bytes.isEmpty() || bytes.size() % 2 != 0)
        return {};
    std::vector<char16_t> units;
    units.reserve(size_t(bytes.size() / 2));
    for (int i = 0; i < bytes.size(); i += 2) {
        const auto hi = static_cast<unsigned char>(bytes.at(i));
        const auto lo = static_cast<unsigned char>(bytes.at(i + 1));
        units.push_back(static_cast<char16_t>((hi << 8) | lo));
    }
    return QString::fromUtf16(units.data(), int(units.size()));
}

QString unicodeToHex(const QString &text)
{
    QByteArray bytes;
    bytes.reserve(text.size() * 2);
    const ushort *units = text.utf16();
    for (int i = 0; i < text.size(); ++i) {
        bytes.append(char((units[i] >> 8) & 0xff));
        bytes.append(char(units[i] & 0xff));
    }
    return QString::fromLatin1(bytes.toHex().toUpper());
}

QString normalizeHexToken(const QString &token)
{
    if (token.size() < 4 || !token.startsWith(QLatin1Char('<'))
        || !token.endsWith(QLatin1Char('>')))
        return token;
    const QString text = hexToUnicode(token.mid(1, token.size() - 2));
    if (text.isEmpty())
        return token;
    const QString normalized = unicodenormalize::normalizeForPdf(text);
    if (normalized == text)
        return token;
    return QLatin1Char('<') + unicodeToHex(normalized) + QLatin1Char('>');
}

// Qt 的 PDF 引擎会把部分 CJK 字形反向映射成康熙部首（例如 U+2F0A），
// 导致提取/搜索得到“⼊”而不是“入”。在复制文本层前对所有 ToUnicode
// 目标做 NFKC 和 CJK 部首归一化，把这些兼容字符还原成可搜索的常规字符。
QString normalizeToUnicodeCmap(const QString &cmap)
{
    QString result = cmap;
    static const QRegularExpression bfcharRe(
        QStringLiteral("beginbfchar(.*?)endbfchar"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression pairRe(
        QStringLiteral("(<[0-9A-Fa-f]+>)\\s*(<[0-9A-Fa-f]+>)"));
    static const QRegularExpression bfrangeArrayRe(
        QStringLiteral("(<[0-9A-Fa-f]+>)\\s*(<[0-9A-Fa-f]+>)\\s*\\[(.*?)\\]"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression hexRe(QStringLiteral("<[0-9A-Fa-f]+>"));

    QString rebuilt;
    int last = 0;
    auto blockIt = bfcharRe.globalMatch(cmap);
    while (blockIt.hasNext()) {
        const auto block = blockIt.next();
        rebuilt += cmap.mid(last, block.capturedStart() - last);
        QString body = block.captured(1);
        int bodyLast = 0;
        QString newBody;
        auto pairIt = pairRe.globalMatch(body);
        while (pairIt.hasNext()) {
            const auto pair = pairIt.next();
            newBody += body.mid(bodyLast, pair.capturedStart() - bodyLast);
            newBody += pair.captured(1) + QLatin1Char(' ')
                       + normalizeHexToken(pair.captured(2));
            bodyLast = pair.capturedEnd();
        }
        newBody += body.mid(bodyLast);
        rebuilt += QStringLiteral("beginbfchar") + newBody + QStringLiteral("endbfchar");
        last = block.capturedEnd();
    }
    rebuilt += cmap.mid(last);
    result = rebuilt;

    // bfrange 的数组形式逐项映射，可以安全地原地归一化；单个目标值的
    // 连续区间保持不动（Qt 主要用于 ASCII，NFKC 不会改变它）。
    rebuilt.clear();
    last = 0;
    auto rangeIt = bfrangeArrayRe.globalMatch(result);
    while (rangeIt.hasNext()) {
        const auto range = rangeIt.next();
        rebuilt += result.mid(last, range.capturedStart() - last);
        QString array = range.captured(3);
        QString newArray;
        int arrayLast = 0;
        auto hexIt = hexRe.globalMatch(array);
        while (hexIt.hasNext()) {
            const auto hex = hexIt.next();
            newArray += array.mid(arrayLast, hex.capturedStart() - arrayLast);
            newArray += normalizeHexToken(hex.captured());
            arrayLast = hex.capturedEnd();
        }
        newArray += array.mid(arrayLast);
        rebuilt += range.captured(1) + QLatin1Char(' ') + range.captured(2)
                   + QStringLiteral(" [") + newArray + QLatin1Char(']');
        last = range.capturedEnd();
    }
    rebuilt += result.mid(last);
    return rebuilt;
}

void normalizeFontCMaps(QPDF &overlay, QPDFPageObjectHelper &page)
{
    QPDFObjectHandle resources = page.getAttribute("/Resources", true);
    if (!resources.isDictionary())
        return;
    QPDFObjectHandle fonts = resources.getKey("/Font");
    if (!fonts.isDictionary())
        return;

    std::set<QPDFObjGen> seen;
    for (const std::string &key : fonts.getKeys()) {
        QPDFObjectHandle font = fonts.getKey(key);
        if (!font.isDictionary())
            continue;
        const QPDFObjGen og = font.getObjGen();
        if (og.getObj() != 0 && !seen.insert(og).second)
            continue;
        QPDFObjectHandle toUnicode = font.getKey("/ToUnicode");
        if (!toUnicode.isStream())
            continue;
        std::shared_ptr<Buffer> buffer = toUnicode.getStreamData();
        if (!buffer)
            continue;
        const QString cmap = QString::fromLatin1(
            reinterpret_cast<const char *>(buffer->getBuffer()), int(buffer->getSize()));
        const QString normalized = normalizeToUnicodeCmap(cmap);
        if (normalized != cmap) {
            toUnicode.replaceStreamData(normalized.toLatin1().toStdString(),
                                        QPDFObjectHandle::newNull(),
                                        QPDFObjectHandle::newNull());
        }
    }
}

QStringList wrapLine(const QString &line, const QFontMetricsF &metrics, qreal width)
{
    QStringList result;
    if (line.isEmpty()) {
        result << QString();
        return result;
    }
    int start = 0;
    while (start < line.size()) {
        int end = line.size();
        while (end > start
               && metrics.horizontalAdvance(line.mid(start, end - start)) > width)
            --end;
        if (end == start)
            end = start + 1;
        result << line.mid(start, end - start);
        start = end;
    }
    return result;
}

void drawTextLayer(const PageEntry &entry, const QSizeF &pageSize, QPainter &painter)
{
    const qreal width = qMax<qreal>(1.0, pageSize.width());
    const qreal height = qMax<qreal>(1.0, pageSize.height());

    QFont font;
    font.setFamilies({QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Source Han Sans CN"),
                      QStringLiteral("Microsoft YaHei"), QStringLiteral("PingFang SC"),
                      QStringLiteral("SimHei"), QStringLiteral("DejaVu Sans")});
    font.setStyleStrategy(QFont::PreferOutline);

    const QVector<ocr::Block> &blocks = entry.result.blocks;
    if (!blocks.isEmpty()) {
        for (const ocr::Block &block : blocks) {
            const QString text = ocr::displayText(block.text).trimmed();
            if (text.isEmpty() || block.boxes.isEmpty())
                continue;

            QRectF box;
            for (const QRectF &b : block.boxes)
                box = box.isNull() ? b : box.united(b);
            if (box.width() <= 0 || box.height() <= 0)
                continue;

            const QRectF target(box.x() / 1000.0 * width, box.y() / 1000.0 * height,
                                box.width() / 1000.0 * width,
                                box.height() / 1000.0 * height);
            QStringList rawLines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            if (rawLines.isEmpty())
                rawLines << text;

            const qreal baseSize =
                qBound<qreal>(4.0, target.height() * 0.78 / qMax(1, rawLines.size()), 48.0);
            font.setPixelSize(qMax(1, qRound(baseSize)));
            QFontMetricsF metrics(font);
            painter.setFont(font);

            QStringList lines;
            for (const QString &raw : rawLines) {
                const QString line = raw.simplified();
                if (line.isEmpty())
                    continue;
                lines += wrapLine(line, metrics, qMax<qreal>(1.0, target.width()));
            }
            if (lines.isEmpty())
                continue;

            const qreal lineHeight = qMax<qreal>(1.0, metrics.height());
            const qreal ascent = metrics.ascent();
            for (int i = 0; i < lines.size(); ++i) {
                const qreal baseline = target.top() + ascent + i * lineHeight;
                painter.drawText(QPointF(target.left(), baseline), lines.at(i));
            }
        }
        return;
    }

    // text_recognition 没有版面坐标：仍写入完整文本，位置放在页首。
    const QString text = ocr::plainText(entry.result.payload, entry.result.purpose).trimmed();
    if (text.isEmpty())
        return;
    font.setPixelSize(10);
    QFontMetricsF metrics(font);
    painter.setFont(font);
    const qreal left = 36;
    const qreal usableWidth = qMax<qreal>(1.0, width - 72);
    const qreal lineHeight = qMax<qreal>(1.0, metrics.height());
    qreal baseline = 36 + metrics.ascent();
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.simplified();
        for (const QString &wrapped : wrapLine(line, metrics, usableWidth)) {
            if (baseline > height - 12)
                return;
            painter.drawText(QPointF(left, baseline), wrapped);
            baseline += lineHeight;
        }
    }
}

bool writeOverlay(const PageEntry &entry, const QSizeF &pageSize, const QString &path,
                  QString *error)
{
    QPdfWriter writer(path);
    writer.setResolution(72);
    writer.setPageSize(QPageSize(pageSize, QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setTitle(QStringLiteral("docr-qt text layer"));

    QPainter painter(&writer);
    if (!painter.isActive()) {
        setError(error, QStringLiteral("无法创建 PDF 文本层"));
        return false;
    }
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(0, 0, 0, 0));
    drawTextLayer(entry, pageSize, painter);
    painter.end();

    if (!QFileInfo::exists(path) || QFileInfo(path).size() == 0) {
        setError(error, QStringLiteral("PDF 文本层写入失败"));
        return false;
    }
    return true;
}

bool addTextOverlay(QPDF &out, QPDFPageObjectHelper &page, const QString &overlayPath,
                    QString *error)
{
    QPDF overlay;
    try {
        overlay.processFile(QFile::encodeName(overlayPath).constData());
    } catch (const std::exception &e) {
        setError(error, QStringLiteral("读取 PDF 文本层失败：%1")
                            .arg(QString::fromUtf8(e.what())));
        return false;
    }

    QPDFPageDocumentHelper overlayHelper(overlay);
    const auto overlayPages = overlayHelper.getAllPages();
    if (overlayPages.empty()) {
        setError(error, QStringLiteral("PDF 文本层没有页面"));
        return false;
    }
    QPDFPageObjectHelper overlayPage = overlayPages.front();
    normalizeFontCMaps(overlay, overlayPage);

    std::string content;
    for (QPDFObjectHandle stream : overlayPage.getPageContents()) {
        std::shared_ptr<Buffer> buffer = stream.getStreamData();
        if (!buffer)
            continue;
        content.append(reinterpret_cast<const char *>(buffer->getBuffer()), buffer->getSize());
        content.push_back('\n');
    }
    if (content.empty())
        return true;

    QPDFObjectHandle form = overlay.newStream(content);
    form.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    form.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
    const auto mediaBox = overlayPage.getMediaBox(false).getArrayAsRectangle();
    form.getDict().replaceKey("/BBox",
                              QPDFObjectHandle::newArray(QPDFObjectHandle::Rectangle(
                                  mediaBox.llx, mediaBox.lly, mediaBox.urx, mediaBox.ury)));
    QPDFObjectHandle resources = overlayPage.getAttribute("/Resources", true);
    if (!resources.isDictionary())
        resources = QPDFObjectHandle::newDictionary();
    form.getDict().replaceKey("/Resources", resources);
    form = overlay.makeIndirectObject(form);

    QPDFObjectHandle copied;
    try {
        copied = out.copyForeignObject(form);
    } catch (const std::exception &e) {
        setError(error, QStringLiteral("复制 PDF 文本层失败：%1")
                            .arg(QString::fromUtf8(e.what())));
        return false;
    }

    QPDFObjectHandle pageObject = page.getObjectHandle();
    QPDFObjectHandle pageResources = page.getAttribute("/Resources", true);
    if (!pageResources.isDictionary()) {
        pageResources = QPDFObjectHandle::newDictionary();
        pageObject.replaceKey("/Resources", pageResources);
    }
    QPDFObjectHandle xobjects = pageResources.getKey("/XObject");
    if (!xobjects.isDictionary()) {
        xobjects = QPDFObjectHandle::newDictionary();
        pageResources.replaceKey("/XObject", xobjects);
    }
    int suffix = 1;
    const std::string name = pageResources.getUniqueResourceName("/DocrText", suffix);
    xobjects.replaceKey(name, copied);

    const QPDFObjectHandle::Rectangle trim = page.getTrimBox(false, true).getArrayAsRectangle();
    const std::string placement =
        page.placeFormXObject(copied, name, trim, true, true, true);
    page.addPageContents(out.newStream(placement), false);
    return true;
}

bool addImagePage(QPDF &out, const QImage &source, const QSizeF &pageSize, QString *error)
{
    QImage image = source;
    if (image.isNull()) {
        setError(error, QStringLiteral("页面图片为空"));
        return false;
    }
    if (image.hasAlphaChannel()) {
        QImage flattened(image.size(), QImage::Format_RGB888);
        flattened.fill(Qt::white);
        QPainter p(&flattened);
        p.drawImage(0, 0, image);
        p.end();
        image = flattened;
    } else {
        image = image.convertToFormat(QImage::Format_RGB888);
    }

    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", 92)) {
        setError(error, QStringLiteral("图片编码失败"));
        return false;
    }

    const qreal width = pageSize.width();
    const qreal height = pageSize.height();
    QPDFObjectHandle imageStream = out.newStream(std::string(jpeg.constData(), jpeg.size()));
    imageStream.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    imageStream.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    imageStream.getDict().replaceKey("/Width", QPDFObjectHandle::newInteger(image.width()));
    imageStream.getDict().replaceKey("/Height", QPDFObjectHandle::newInteger(image.height()));
    imageStream.getDict().replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    imageStream.getDict().replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    imageStream.getDict().replaceKey("/Filter", QPDFObjectHandle::newName("/DCTDecode"));
    imageStream = out.makeIndirectObject(imageStream);

    QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
    xobjects.replaceKey("/Im0", imageStream);
    QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/XObject", xobjects);

    const std::string content =
        QStringLiteral("q\n%1 0 0 %2 0 0 cm\n/Im0 Do\nQ\n")
            .arg(width, 0, 'f', 4)
            .arg(height, 0, 'f', 4)
            .toStdString();
    QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
    page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
    page.replaceKey("/MediaBox",
                    QPDFObjectHandle::newArray(
                        QPDFObjectHandle::Rectangle(0, 0, width, height)));
    page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", out.makeIndirectObject(out.newStream(content)));
    out.addPage(page, false);
    return true;
}

struct OutlineLevel {
    QPDFObjectHandle first;
    QPDFObjectHandle last;
    int count = 0;
};

OutlineLevel addOutlineLevel(QPDF &out, const QVector<OutlineEntry> &entries,
                             const std::vector<QPDFObjectHandle> &pages,
                             QPDFObjectHandle parent)
{
    OutlineLevel level;
    std::vector<QPDFObjectHandle> nodes;
    nodes.reserve(size_t(entries.size()));

    for (const OutlineEntry &entry : entries) {
        QPDFObjectHandle node = out.makeIndirectObject(QPDFObjectHandle::newDictionary());
        node.replaceKey("/Title",
                        QPDFObjectHandle::newUnicodeString(
                            entry.title.isEmpty() ? QStringLiteral("（未命名）").toUtf8().toStdString()
                                                  : entry.title.toUtf8().toStdString()));
        node.replaceKey("/Parent", parent);
        if (entry.page >= 0 && entry.page < int(pages.size()))
            node.replaceKey("/Dest", QPDFObjectHandle::newArray(
                                         {pages.at(size_t(entry.page)),
                                          QPDFObjectHandle::newName("/Fit")}));
        nodes.push_back(node);
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (i > 0)
            nodes[i].replaceKey("/Prev", nodes[i - 1]);
        if (i + 1 < nodes.size())
            nodes[i].replaceKey("/Next", nodes[i + 1]);
    }

    int total = int(nodes.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[size_t(i)].children.isEmpty())
            continue;
        OutlineLevel child = addOutlineLevel(out, entries[size_t(i)].children, pages, nodes[i]);
        if (!child.first.isNull()) {
            nodes[i].replaceKey("/First", child.first);
            nodes[i].replaceKey("/Last", child.last);
            nodes[i].replaceKey("/Count", QPDFObjectHandle::newInteger(child.count));
            total += child.count;
        }
    }
    if (!nodes.empty()) {
        level.first = nodes.front();
        level.last = nodes.back();
        level.count = total;
        parent.replaceKey("/First", level.first);
        parent.replaceKey("/Last", level.last);
        parent.replaceKey("/Count", QPDFObjectHandle::newInteger(total));
    }
    return level;
}

void addOutline(QPDF &out, const QVector<OutlineEntry> &outline)
{
    if (outline.isEmpty())
        return;
    const auto pages = out.getAllPages();
    if (pages.empty())
        return;

    QPDFObjectHandle root = out.makeIndirectObject(QPDFObjectHandle::newDictionary());
    root.replaceKey("/Type", QPDFObjectHandle::newName("/Outlines"));
    out.getRoot().replaceKey("/Outlines", root);
    out.getRoot().replaceKey("/PageMode", QPDFObjectHandle::newName("/UseOutlines"));
    addOutlineLevel(out, outline, pages, root);
}

QSizeF pdfPageSize(const PageEntry &entry, QPDFPageObjectHelper &page)
{
    if (!entry.pdfSize.isEmpty())
        return entry.pdfSize;
    const auto box = page.getTrimBox(false, true).getArrayAsRectangle();
    return QSizeF(box.urx - box.llx, box.ury - box.lly);
}

QSizeF imagePageSize(const Document &doc, const PageEntry &entry)
{
    if (entry.imageSize.isEmpty())
        return QSizeF(595, 842);
    const qreal dpi = doc.dpi() > 0 ? doc.dpi() : 150.0;
    return QSizeF(entry.imageSize) * (72.0 / dpi);
}

bool pageHasText(const PageEntry &entry)
{
    if (!entry.result.ok())
        return false;
    if (!entry.result.blocks.isEmpty())
        return true;
    return !ocr::plainText(entry.result.payload, entry.result.purpose).trimmed().isEmpty();
}

} // namespace

bool writeSearchablePdf(Document &doc, const QVector<OutlineEntry> &outline,
                        const QString &path, QString *error, const Progress &progress)
{
    QString localError;
    if (!error)
        error = &localError;
    error->clear();

    if (doc.count() == 0) {
        setError(error, QStringLiteral("没有可导出的页面"));
        return false;
    }
    if (path.isEmpty()) {
        setError(error, QStringLiteral("导出路径为空"));
        return false;
    }

    const QFileInfo target(path);
    if (!target.absoluteDir().exists()) {
        setError(error, QStringLiteral("导出目录不存在：%1").arg(target.absolutePath()));
        return false;
    }

    QTemporaryDir overlays;
    if (!overlays.isValid()) {
        setError(error, QStringLiteral("无法创建临时目录"));
        return false;
    }
    QTemporaryFile output(QDir(target.absolutePath())
                              .filePath(QStringLiteral(".docr-pdf-XXXXXX.pdf")));
    output.setAutoRemove(false);
    if (!output.open()) {
        setError(error, QStringLiteral("无法创建临时导出文件"));
        return false;
    }
    const QString outputPath = output.fileName();
    output.close();

    bool ok = false;
    try {
        QPDF out;
        out.emptyPDF();
        QPDFPageDocumentHelper outHelper(out);
        std::map<QString, std::unique_ptr<QPDF>> sources;

        auto sourceFor = [&](const QString &sourcePath, QString *sourceError) -> QPDF * {
            auto it = sources.find(sourcePath);
            if (it != sources.end())
                return it->second.get();
            auto source = std::make_unique<QPDF>();
            try {
                source->processFile(QFile::encodeName(sourcePath).constData());
            } catch (const std::exception &e) {
                if (sourceError)
                    *sourceError = QString::fromUtf8(e.what());
                return nullptr;
            }
            QPDF *raw = source.get();
            sources.emplace(sourcePath, std::move(source));
            return raw;
        };

        for (int i = 0; i < doc.count(); ++i) {
            const PageEntry &entry = doc.page(i);
            if (entry.pdf) {
                QString sourceError;
                QPDF *source = sourceFor(entry.sourcePath, &sourceError);
                if (!source) {
                    setError(error, QStringLiteral("无法读取 PDF：%1").arg(sourceError));
                    break;
                }
                QPDFPageDocumentHelper sourceHelper(*source);
                const auto sourcePages = sourceHelper.getAllPages();
                if (entry.pdfPage < 0 || entry.pdfPage >= int(sourcePages.size())) {
                    setError(error, QStringLiteral("PDF 页码超出范围：%1").arg(entry.sourcePath));
                    break;
                }
                outHelper.addPage(sourcePages.at(size_t(entry.pdfPage)), false);
                QPDFPageObjectHelper page(out.getAllPages().back());
                const QSizeF size = pdfPageSize(entry, page);
                if (pageHasText(entry)) {
                    const QString overlayPath = overlays.filePath(
                        QStringLiteral("overlay-%1.pdf").arg(i));
                    if (!writeOverlay(entry, size, overlayPath, error))
                        break;
                    if (!addTextOverlay(out, page, overlayPath, error))
                        break;
                }
            } else {
                QImage image = doc.ensureImage(i);
                if (image.isNull()) {
                    setError(error, QStringLiteral("无法读取页面图片：%1").arg(entry.label));
                    break;
                }
                QSizeF size = imagePageSize(doc, entry);
                if (size.isEmpty())
                    size = QSizeF(595, 842);
                if (!addImagePage(out, image, size, error))
                    break;
                QPDFPageObjectHelper page(out.getAllPages().back());
                if (pageHasText(entry)) {
                    const QString overlayPath = overlays.filePath(
                        QStringLiteral("overlay-%1.pdf").arg(i));
                    if (!writeOverlay(entry, size, overlayPath, error))
                        break;
                    if (!addTextOverlay(out, page, overlayPath, error))
                        break;
                }
            }

            if (progress && !progress(i + 1, doc.count())) {
                setError(error, QStringLiteral("已取消"));
                break;
            }
        }

        if (error->isEmpty()) {
            addOutline(out, outline);
            const QByteArray outputName = QFile::encodeName(outputPath);
            QPDFWriter writer(out, outputName.constData());
            writer.setMinimumPDFVersion("1.7");
            writer.setObjectStreamMode(qpdf_o_preserve);
            writer.write();
            ok = true;
        }
    } catch (const std::exception &e) {
        setError(error, QStringLiteral("导出 PDF 失败：%1").arg(QString::fromUtf8(e.what())));
        ok = false;
    } catch (...) {
        setError(error, QStringLiteral("导出 PDF 失败"));
        ok = false;
    }

    if (ok) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            setError(error, QStringLiteral("无法覆盖目标文件：%1").arg(path));
            ok = false;
        } else if (!QFile::rename(outputPath, path)) {
            setError(error, QStringLiteral("无法写入目标文件：%1").arg(path));
            ok = false;
        }
    }
    if (!ok)
        QFile::remove(outputPath);
    return ok;
}

} // namespace pdfexport
