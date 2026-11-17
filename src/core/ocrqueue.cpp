#include "ocrqueue.h"

#include <QBuffer>
#include <QImage>
#include <QTimer>

#include "document.h"

OcrQueue::OcrQueue(Document *doc, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_client(this)
{
    connect(&m_client, &ocr::Client::finished, this,
            [this](int page, int generation, const ocr::PageResult &result) {
                handleResult(page, generation, result);
            });
}

void OcrQueue::configure(const QString &baseUrl, const QString &apiKey,
                         const QString &purpose, int concurrency)
{
    m_purpose = purpose;
    m_concurrency = qBound(1, concurrency, 8);
    m_client.configure(baseUrl, apiKey);
}

void OcrQueue::configureGlm(const QString &endpoint, const QString &model,
                            const QString &apiKey, const QString &layoutModel)
{
    m_client.configureGlm(endpoint, model, apiKey, layoutModel);
}

void OcrQueue::setBackend(ocr::Backend backend)
{
    m_client.setBackend(backend);
}

void OcrQueue::setPurpose(const QString &purpose)
{
    m_purpose = purpose;
}

void OcrQueue::setConcurrency(int concurrency)
{
    m_concurrency = qBound(1, concurrency, 8);
}

bool OcrQueue::isBusy() const
{
    return !m_pending.isEmpty() || !m_running.isEmpty();
}

int OcrQueue::startAll()
{
    if (!m_doc)
        return 0;

    QVector<int> pages;
    for (int i = 0; i < m_doc->count(); ++i) {
        const PageEntry &entry = m_doc->page(i);
        if (!entry.hasResult() || ocr::needsRetry(entry.result))
            pages.append(i);
    }
    return startPages(pages);
}

int OcrQueue::startPages(const QVector<int> &pages)
{
    if (!m_doc)
        return 0;

    QSet<int> seen;
    int added = 0;
    for (int page : pages) {
        if (page < 0 || page >= m_doc->count() || seen.contains(page))
            continue;
        seen.insert(page);
        if (m_pending.contains(page) || m_running.contains(page))
            continue;
        m_pending.enqueue(page);
        ++added;
    }
    if (added == 0)
        return 0;

    m_emptyRetries.clear();
    m_allDoneEmitted = false;
    emit busyChanged(true);
    pump();
    return added;
}

bool OcrQueue::startPage(int page, bool *alreadyRunning)
{
    if (alreadyRunning)
        *alreadyRunning = false;
    if (!m_doc || page < 0 || page >= m_doc->count())
        return false;

    if (m_running.contains(page)) {
        if (alreadyRunning)
            *alreadyRunning = true;
        return false;
    }

    m_emptyRetries.remove(page);
    m_doc->page(page).result = ocr::PageResult();
    if (m_pending.contains(page)) {
        QQueue<int> reordered;
        reordered.enqueue(page);
        for (int pending : m_pending) {
            if (pending != page)
                reordered.enqueue(pending);
        }
        m_pending = reordered;
    } else {
        m_pending.enqueue(page);
    }

    m_allDoneEmitted = false;
    emit pageQueued(page);
    emit busyChanged(true);
    pump();
    return true;
}

void OcrQueue::stop()
{
    m_generation++;
    m_client.cancelPending();
    m_pending.clear();
    m_running.clear();
    m_emptyRetries.clear();
    m_allDoneEmitted = true;
    emit busyChanged(false);
}

void OcrQueue::pump()
{
    while (m_running.size() < m_concurrency && !m_pending.isEmpty()) {
        const int page = m_pending.dequeue();
        if (m_running.contains(page))
            continue;
        m_running.insert(page);
        emit pageStarted(page);
        submit(page);
    }

    emit busyChanged(isBusy());
    if (m_pending.isEmpty() && m_running.isEmpty() && !m_allDoneEmitted) {
        m_allDoneEmitted = true;
        emit busyChanged(false);
        emit allFinished();
    }
}

void OcrQueue::submit(int page)
{
    if (!m_doc || page < 0 || page >= m_doc->count())
        return;

    const QImage image = m_doc->ensureImage(page);
    if (image.isNull()) {
        ocr::PageResult result;
        result.page = page;
        result.purpose = m_purpose;
        result.error = QStringLiteral("渲染失败");
        handleResult(page, m_generation, result);
        return;
    }

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");

    const QString name = QStringLiteral("page-%1.png").arg(page + 1);
    m_client.submit(page, m_generation, bytes, name, m_purpose);
}

void OcrQueue::handleResult(int page, int generation, const ocr::PageResult &result)
{
    if (generation != m_generation)
        return;
    m_running.remove(page);
    if (!m_doc || page < 0 || page >= m_doc->count()) {
        schedulePump();
        return;
    }

    const bool layout = ocr::isLayoutPurpose(result.purpose);
    const bool empty = result.ok() && layout && result.blocks.isEmpty()
                       && !ocr::hasText(result.payload);
    // 只有空结果才需要判断页面是否有墨迹；正常页不必再做一次缩图扫描。
    const bool hasInk = empty && pageHasInk(m_doc->ensureImage(page));

    // 页面有内容但服务端返回空结果：自动重试一次。
    if (empty && hasInk && !m_emptyRetries.contains(page)) {
        m_emptyRetries.insert(page);
        m_doc->page(page).result = ocr::PageResult();
        m_doc->releaseImage(page);
        emit pageRetrying(page);
        m_pending.enqueue(page);
        schedulePump();
        return;
    }

    ocr::PageResult final = result;
    final.blank = empty && !hasInk;
    m_doc->page(page).result = final;
    // 识别完的整页图不再常驻；连续视图需要时会从 PDF 重新渲染。
    // 否则批量识别 500 页会把所有整页位图都留在内存里。
    m_doc->releaseImage(page);
    emit pageFinished(page, final);
    schedulePump();
}

void OcrQueue::schedulePump()
{
    QTimer::singleShot(0, this, [this] { pump(); });
}

bool OcrQueue::pageHasInk(const QImage &image)
{
    if (image.isNull())
        return false;
    const QImage small = image.scaled(200, 200, Qt::KeepAspectRatio, Qt::FastTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    int dark = 0;
    for (int y = 0; y < small.height(); ++y) {
        const uchar *line = small.constScanLine(y);
        for (int x = 0; x < small.width(); ++x) {
            if (line[x] < 100)
                ++dark;
        }
    }
    return dark * 100 > small.width() * small.height(); // 超过 1% 像素是深色
}
