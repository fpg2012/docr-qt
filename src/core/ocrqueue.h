// OCR 队列：并发调度、取消、空结果自动重试。
#pragma once

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QVector>

#include "ocr.h"

class Document;
class QImage;

class OcrQueue : public QObject {
    Q_OBJECT
public:
    explicit OcrQueue(Document *doc, QObject *parent = nullptr);

    void configure(const QString &baseUrl, const QString &apiKey,
                   const QString &purpose, int concurrency);
    void configureGlm(const QString &endpoint, const QString &model,
                      const QString &apiKey,
                      const QString &layoutModel = QString());
    void setBackend(ocr::Backend backend);
    void setPurpose(const QString &purpose);
    void setConcurrency(int concurrency);

    bool isBusy() const;
    // 识别所有没有结果的页面；返回新入队的页数。
    int startAll();
    // 识别指定页集合；返回新入队的页数。
    int startPages(const QVector<int> &pages);
    // 识别指定页；若正在识别，alreadyRunning 置 true 并返回 false。
    bool startPage(int page, bool *alreadyRunning = nullptr);
    void stop();

    static bool pageHasInk(const QImage &image);

signals:
    void pageQueued(int page);
    void pageStarted(int page);
    void pageRetrying(int page);
    void pageFinished(int page, const ocr::PageResult &result);
    void busyChanged(bool busy);
    void allFinished();

private:
    void pump();
    void submit(int page);
    void handleResult(int page, int generation, const ocr::PageResult &result);
    void schedulePump();

    Document *m_doc = nullptr;
    ocr::Client m_client;
    QString m_purpose = QStringLiteral("rich_layout_recognition");
    int m_concurrency = 3;
    QQueue<int> m_pending;
    QSet<int> m_running;
    QSet<int> m_emptyRetries;
    int m_generation = 0;
    bool m_allDoneEmitted = true;
};
