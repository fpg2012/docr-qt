#include "document.h"

#include <QCollator>
#include <algorithm>
#include <QDir>
#include <QFileInfo>

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
    for (int i = 0; i < pages; ++i) {
        PageEntry entry;
        entry.label = QStringLiteral("%1 · %2").arg(base).arg(i + 1);
        entry.sourcePath = path;
        entry.pdf = pdf;
        entry.pdfPage = i;
        m_pages.append(entry);
    }
    m_dpi = dpi;
    if (m_path.isEmpty())
        m_path = QFileInfo(path).absoluteFilePath();
    return true;
}

bool Document::addImage(const QString &path, QString *error)
{
    QImage image(path);
    if (image.isNull()) {
        if (error)
            *error = QStringLiteral("无法读取图片：%1").arg(path);
        return false;
    }
    PageEntry entry;
    entry.label = QFileInfo(path).fileName();
    entry.sourcePath = path;
    entry.image = image;
    m_pages.append(entry);
    if (m_path.isEmpty())
        m_path = QFileInfo(path).absolutePath();
    return true;
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
    if (entry.pdf)
        entry.image = renderPdfPage(entry.pdf, entry.pdfPage);
    return entry.image;
}

QPixmap Document::ensureThumb(int index)
{
    if (index < 0 || index >= m_pages.size())
        return {};
    PageEntry &entry = m_pages[index];
    if (!entry.thumb.isNull())
        return entry.thumb;
    const QImage image = ensureImage(index);
    if (image.isNull())
        return {};
    entry.thumb = QPixmap::fromImage(
        image.scaledToHeight(180, Qt::SmoothTransformation));
    return entry.thumb;
}
