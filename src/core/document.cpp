#include "document.h"

#include <QCollator>
#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <functional>

QStringList Document::imageNameFilters()
{
    return {QStringLiteral("*.png"),  QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
            QStringLiteral("*.webp"), QStringLiteral("*.gif"), QStringLiteral("*.bmp"),
            QStringLiteral("*.tif"),  QStringLiteral("*.tiff")};
}

bool Document::addPdf(const QString &path, int dpi, QString *error)
{
    std::shared_ptr<Poppler::Document> pdf(Poppler::Document::load(path));
    if (!pdf || pdf->isLocked()) {
        if (error)
            *error = QStringLiteral("无法打开 PDF（可能已加密或损坏）：%1").arg(path);
        return false;
    }
    const int pages = pdf->numPages();
    if (pages <= 0) {
        if (error)
            *error = QStringLiteral("PDF 没有页面：%1").arg(path);
        return false;
    }

    pdf->setRenderHint(Poppler::Document::TextAntialiasing, true);
    pdf->setRenderHint(Poppler::Document::Antialiasing, true);

    const QString base = QFileInfo(path).fileName();
    m_pages.reserve(m_pages.size() + pages);
    const int basePage = m_pages.size();
    for (int i = 0; i < pages; ++i) {
        PageEntry entry;
        entry.label = QStringLiteral("%1 · %2").arg(base).arg(i + 1);
        entry.sourcePath = path;
        entry.pdf = pdf;
        entry.pdfPage = i;
        if (std::unique_ptr<Poppler::Page> page = std::unique_ptr<Poppler::Page>(pdf->page(i))) {
            entry.pdfSize = page->pageSizeF();
            const QString label = page->label().trimmed();
            // poppler 对没有标签的页面也会回退成物理页号；只保留真正有区分度的标签。
            if (!label.isEmpty() && label != QString::number(i + 1))
                entry.pageLabelText = label;
        }
        m_pages.append(entry);
    }
    m_dpi = dpi;
    if (m_path.isEmpty())
        m_path = QFileInfo(path).absoluteFilePath();

    std::function<QVector<OutlineEntry>(const QVector<Poppler::OutlineItem> &)> convert =
        [&](const QVector<Poppler::OutlineItem> &items) {
            QVector<OutlineEntry> result;
            for (const Poppler::OutlineItem &item : items) {
                OutlineEntry entry;
                entry.title = item.name().trimmed();
                if (entry.title.isEmpty())
                    entry.title = QStringLiteral("（未命名）");
                const auto destination = item.destination();
                if (destination && item.externalFileName().isEmpty()
                    && item.uri().isEmpty()) {
                    const int target = destination->pageNumber() - 1;
                    if (target >= 0 && target < pages)
                        entry.page = basePage + target;
                }
                entry.children = convert(item.children());
                result.append(entry);
            }
            return result;
        };
    m_outline += convert(pdf->outline());
    return true;
}

bool Document::addImage(const QString &path, QString *error)
{
    QImageReader reader(path);
    if (!reader.canRead()) {
        if (error)
            *error = QStringLiteral("无法读取图片：%1").arg(path);
        return false;
    }
    // 只记录尺寸，等页面进入可视区再解码整图；图片序列不会在打开时占满内存。
    PageEntry entry;
    entry.label = QFileInfo(path).fileName();
    entry.sourcePath = path;
    entry.imageSize = reader.size();
    m_pages.append(entry);
    if (m_path.isEmpty())
        m_path = QFileInfo(path).absolutePath();
    return true;
}

QStringList Document::expandPaths(const QStringList &paths)
{
    QStringList files;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QDir dir(path);
            QStringList names = dir.entryList(imageNameFilters(), QDir::Files, QDir::NoSort);
            QCollator collator;
            collator.setNumericMode(true);
            std::sort(names.begin(), names.end(),
                      [&collator](const QString &a, const QString &b) {
                          return collator.compare(a, b) < 0;
                      });
            for (const QString &name : names)
                files << dir.filePath(name);
        } else if (info.isFile()) {
            files << info.absoluteFilePath();
        }
    }
    return files;
}

bool Document::addPaths(const QStringList &paths, int dpi, DocumentLoadReport *report)
{
    const QStringList files = expandPaths(paths);
    if (report) {
        report->files = files;
        report->errors.clear();
    }
    if (files.isEmpty())
        return false;

    m_dpi = dpi;
    const int before = m_pages.size();
    for (const QString &file : files) {
        QString error;
        const bool isPdf =
            QFileInfo(file).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0;
        const bool ok = isPdf ? addPdf(file, dpi, &error) : addImage(file, &error);
        if (!ok && report)
            report->errors.append(error);
    }
    return m_pages.size() > before;
}

int Document::addImageDir(const QString &dir)
{
    QDir d(dir);
    const QStringList names = d.entryList(imageNameFilters(), QDir::Files, QDir::Name);

    QCollator collator;
    collator.setNumericMode(true);
    QStringList sorted = names;
    std::sort(sorted.begin(), sorted.end(),
              [&collator](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });

    int added = 0;
    for (const QString &name : sorted) {
        if (addImage(d.filePath(name), nullptr))
            ++added;
    }
    return added;
}

void Document::clear()
{
    m_pages.clear();
    m_outline.clear();
    m_path.clear();
}

QImage Document::renderPdfPage(const std::shared_ptr<Poppler::Document> &pdf, int page)
{
    if (!pdf)
        return {};
    std::unique_ptr<Poppler::Page> popplerPage(pdf->page(page));
    if (!popplerPage)
        return {};
    return popplerPage->renderToImage(m_dpi, m_dpi);
}

QImage Document::ensureImage(int index)
{
    if (index < 0 || index >= m_pages.size())
        return {};
    PageEntry &entry = m_pages[index];
    if (!entry.image.isNull())
        return entry.image;
    if (entry.pdf) {
        entry.image = renderPdfPage(entry.pdf, entry.pdfPage);
    } else if (!entry.sourcePath.isEmpty()) {
        // 连续视图释放后重新解码；尺寸和首次打开时保持一致。
        QImageReader reader(entry.sourcePath);
        entry.image = reader.read();
    }
    return entry.image;
}

QImage Document::ensureThumb(int index)
{
    if (index < 0 || index >= m_pages.size())
        return {};
    PageEntry &entry = m_pages[index];
    if (!entry.thumb.isNull())
        return entry.thumb;

    // 缩略图单独走低分辨率路径，不复用整页缓存：打开大文档时不会因为侧栏缩略图
    // 把每一页都按 150 dpi 渲染、常驻内存。PDF 按最终高度的 2 倍渲染，既能
    // 保证 180px 缩略图清晰，又不会为一张小图解码整页 72/150 dpi。
    if (entry.pdf) {
        std::unique_ptr<Poppler::Page> page(entry.pdf->page(entry.pdfPage));
        if (page) {
            QSizeF points = entry.pdfSize;
            if (points.isEmpty())
                points = page->pageSizeF();
            double dpi = 72.0 * 360.0 / qMax<qreal>(1.0, points.height());
            dpi = qBound(18.0, dpi, 72.0);
            const QImage image = page->renderToImage(dpi, dpi);
            if (!image.isNull())
                entry.thumb = image.scaledToHeight(180, Qt::SmoothTransformation);
            return entry.thumb;
        }
        return {};
    }

    if (!entry.sourcePath.isEmpty()) {
        QImageReader reader(entry.sourcePath);
        const QSize size = reader.size();
        if (size.isValid() && size.height() > 0) {
            const int width = qMax(1, qRound(size.width() * 360.0 / size.height()));
            reader.setScaledSize(QSize(width, 360));
        }
        const QImage image = reader.read();
        if (!image.isNull()) {
            entry.thumb = image.height() == 180
                              ? image
                              : image.scaledToHeight(180, Qt::SmoothTransformation);
            return entry.thumb;
        }
    }

    const QImage image = ensureImage(index);
    if (!image.isNull())
        entry.thumb = image.scaledToHeight(180, Qt::SmoothTransformation);
    return entry.thumb;
}

QSizeF Document::pageSize(int index) const
{
    if (index < 0 || index >= m_pages.size())
        return {};
    const PageEntry &entry = m_pages.at(index);
    if (entry.pdf && !entry.pdfSize.isEmpty())
        return entry.pdfSize * (m_dpi / 72.0);
    if (!entry.imageSize.isEmpty())
        return entry.imageSize;
    if (!entry.image.isNull())
        return entry.image.size();
    return QSizeF(1000, 1414);
}

void Document::releaseImage(int index)
{
    if (index < 0 || index >= m_pages.size())
        return;
    m_pages[index].image = QImage();
}

QImage Document::blockImage(int pageIndex, int blockIndex)
{
    if (pageIndex < 0 || pageIndex >= m_pages.size())
        return {};
    const PageEntry &entry = m_pages.at(pageIndex);
    if (blockIndex < 0 || blockIndex >= entry.result.blocks.size())
        return {};
    const ocr::Block &block = entry.result.blocks.at(blockIndex);
    if (block.boxes.isEmpty())
        return {};

    QRectF unionRect;
    for (const QRectF &box : block.boxes)
        unionRect = unionRect.isNull() ? box : unionRect.united(box);
    if (unionRect.width() <= 0 || unionRect.height() <= 0)
        return {};

    const QImage image = ensureImage(pageIndex);
    if (image.isNull())
        return {};
    QRect pixel(qRound(unionRect.x() * image.width() / 1000.0),
                qRound(unionRect.y() * image.height() / 1000.0),
                qRound(unionRect.width() * image.width() / 1000.0),
                qRound(unionRect.height() * image.height() / 1000.0));
    pixel = pixel.intersected(image.rect());
    if (pixel.width() < 1 || pixel.height() < 1)
        return {};
    return image.copy(pixel);
}

QString Document::pageLabel(int index) const
{
    if (index < 0 || index >= m_pages.size())
        return {};
    return m_pages.at(index).pageLabelText;
}
