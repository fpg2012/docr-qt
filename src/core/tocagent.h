// 用 deepseek-flash 把目录页的 OCR 文本整理成多级目录。
// 与规则解析相比，这里能处理多栏、跨页、分组标题和「鸡鸣 … (134)」这类
// 没有统一行格式的目录。
#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "tocparser.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace toc {

struct AgentRequest {
    QString baseUrl;
    QString apiKey;
    QString model = QStringLiteral("deepseek-flash");
    QString title;
    QString text;
    int pageCount = 0;
};

class Agent : public QObject {
    Q_OBJECT
public:
    explicit Agent(QObject *parent = nullptr);
    ~Agent() override;

    void generate(const AgentRequest &request);
    void cancel();

    // 从 Chat Completions 的响应体/消息正文中解析 entries。公开出来便于离线测试。
    static QVector<Entry> parseResponse(const QByteArray &raw, QString *error);
    static QVector<Entry> parseEntriesJson(const QByteArray &json, QString *error);
    static QString endpointFor(const QString &baseUrl);

signals:
    void finished(const QVector<toc::Entry> &entries);
    void failed(const QString &error);

private:
    void handleReply(QNetworkReply *reply);

    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    bool m_canceled = false;
};

} // namespace toc
