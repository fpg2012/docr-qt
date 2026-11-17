#include "document.h"

#include <QCollator>
#include <algorithm>
#include <QDir>
#include <QFileInfo>

bool Document::openPdf(const QString &path, int dpi, QString *error)
{
    clear();
    std::unique_ptr<Poppler::Document> doc(Poppler::Document::load(path));
    if (!doc || doc->isLocked()) {
        if (error)
            *error = QStringLiteral("无法打开 PDF（可能已加密或损坏）：%1").arg(path);
        return false;
    }
    const int pages = doc->numPages();
    if (pages <= 0) {
        if (error)
            *error = QStringLiteral("PDF 没有页面");
        return false;
    }

    doc->setRenderHint(Poppler::Document::TextAntialiasing, true);
    doc->setRenderHint(Poppler::Document::Antialiasing, true);

    m_pdf = std::move(doc);
    m_path = path;
    m_dpi = dpi;

    const QString base = QFileInfo(path).fileName();
    m_pages.reserve(pages);
    for (int i = 0; i < pages; ++i) {
        PageEntry entry;
        entry.label = QStringLiteral("%1 · %2").arg(base).arg(i + 1);
        entry.sourcePath = path;
        entry.pdfPage = i;
        m_pages.append(entry);
    }
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
    const QStringList names = d.entryList(
        {QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
         QStringLiteral("*.webp"), QStringLiteral("*.gif"), QStringLiteral("*.bmp"),
         QStringLiteral("*.tif"), QStringLiteral("*.tiff")},
        QDir::Files, QDir::Name);

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
    m_pdf.reset();
    m_path.clear();
}

QImage Document::renderPdfPage(int pdfPage)
{
    if (!m_pdf)
        return {};
    std::unique_ptr<Poppler::Page> page(m_pdf->page(pdfPage));
    if (!page)
        return {};
    return page->renderToImage(m_dpi, m_dpi);
}

QImage Document::ensureImage(int index)
{
    if (index < 0 || index >= m_pages.size())
        return {};
    PageEntry &entry = m_pages[index];
    if (!entry.image.isNull())
        return entry.image;
    if (entry.pdfPage >= 0)
        entry.image = renderPdfPage(entry.pdfPage);
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
