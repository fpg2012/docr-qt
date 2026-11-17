// DeepSeek OCR 接口封装：请求、multipart 组包、rich-layout 标记解析。
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QVector>

namespace ocr {

// 一页中的一个版面块。boxes 为 0-1000 归一化坐标（页面左上角为原点）。
struct Block {
    QString kind;
    QVector<QRectF> boxes;
    QString text;
};

struct PageResult {
    int page = -1;
    QString purpose;
    QJsonObject payload;
    QByteArray raw; // 原始响应体，便于空结果/异常诊断
    QVector<Block> blocks;
    qint64 elapsedMs = 0;
    QString error;

    bool ok() const { return error.isEmpty() && !payload.isEmpty(); }
    QJsonObject usage() const { return payload.value(QStringLiteral("usage")).toObject(); }
};

// 解析 rich_layout_recognition 输出中的 <|ref|>/<|box|> 标记。
QVector<Block> parseBlocks(const QString &text);

// 拼接可读文本（表格转成 "a | b" 行）。
QString plainText(const QJsonObject &payload, const QString &purpose);

// 单个块的文本转成可读形式（表格转成 "a | b" 行，HTML 去标签）。
QString displayText(const QString &raw);

// 按给定顺序拼接若干块的文本，块之间用换行分隔。
QString textOf(const QVector<Block> &blocks, const QVector<int> &indices);

// 版面块类型的中文名。
QString kindLabel(const QString &kind);

// 页级输出是否因长度被截断。
bool isTruncated(const QJsonObject &payload);

// 响应里是否有任何非空文字（用于识别“服务端空结果”）。
bool hasText(const QJsonObject &payload);

// 颜色按块类型区分，供叠加层和列表使用。
QColor colorForKind(const QString &kind);

// 读取 API Key：环境变量优先，其次 DSH 凭据库（只读）。
QString dshApiKey();

class Client : public QObject {
    Q_OBJECT
public:
    explicit Client(QObject *parent = nullptr);

    void configure(const QString &baseUrl, const QString &apiKey);

    // 异步提交一页；generation 用于丢弃取消之后到达的结果。
    void submit(int page, int generation, const QByteArray &data,
                const QString &filename, const QString &purpose);

signals:
    void finished(int page, int generation, const ocr::PageResult &result);

private:
    void handleReply(class QNetworkReply *reply, int page, int generation,
                     const QString &purpose, qint64 started);

    QNetworkAccessManager m_nam;
    QString m_baseUrl;
    QString m_apiKey;
};

} // namespace ocr
