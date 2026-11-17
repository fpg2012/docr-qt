#include "ocr.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextDocumentFragment>
#include <QUrl>

namespace ocr {
namespace {

const char *kDefaultBase = "https://api.deepseek.com/v1/tools";

QString stripHtml(const QString &html)
{
    return QTextDocumentFragment::fromHtml(html).toPlainText().trimmed();
}

QString tableToLines(const QString &html)
{
    QStringList lines;
    static const QRegularExpression rowRe(QStringLiteral("<tr[^>]*>(.*?)</tr>"),
                                         QRegularExpression::DotMatchesEverythingOption
                                             | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cellRe(QStringLiteral("<t[dh][^>]*>(.*?)</t[dh]>"),
                                           QRegularExpression::DotMatchesEverythingOption
                                               | QRegularExpression::CaseInsensitiveOption);
    auto it = rowRe.globalMatch(html);
    while (it.hasNext()) {
        const QString row = it.next().captured(1);
        QStringList cells;
        auto cellIt = cellRe.globalMatch(row);
        while (cellIt.hasNext())
            cells << stripHtml(cellIt.next().captured(1));
        if (!cells.isEmpty())
            lines << cells.join(QStringLiteral(" | "));
    }
    return lines.join(QLatin1Char('\n'));
}

QString mimeForName(const QString &name)
{
    const QString lower = name.toLower();
    if (lower.endsWith(QStringLiteral(".pdf"))) return QStringLiteral("application/pdf");
    if (lower.endsWith(QStringLiteral(".png"))) return QStringLiteral("image/png");
    if (lower.endsWith(QStringLiteral(".webp"))) return QStringLiteral("image/webp");
    if (lower.endsWith(QStringLiteral(".gif"))) return QStringLiteral("image/gif");
    if (lower.endsWith(QStringLiteral(".bmp"))) return QStringLiteral("image/bmp");
    if (lower.endsWith(QStringLiteral(".tif")) || lower.endsWith(QStringLiteral(".tiff")))
        return QStringLiteral("image/tiff");
    if (lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg")))
        return QStringLiteral("image/jpeg");
    if (lower.endsWith(QStringLiteral(".docx")))
        return QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    if (lower.endsWith(QStringLiteral(".pptx")))
        return QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.presentation");
    return QStringLiteral("application/octet-stream");
}

} // namespace

QVector<Block> parseBlocks(const QString &text)
{
    QVector<Block> blocks;
    // 服务端用全角竖线包标记：<｜ref｜>title<｜/ref｜><｜box｜>[[..]]<｜/box｜> 文本
    static const QRegularExpression markRe(
        QStringLiteral(R"(<[｜|]ref[｜|]>(.*?)<[｜|]/ref[｜|]>\s*)"
                       R"(<[｜|]box[｜|]>(.*?)<[｜|]/box[｜|]>\s*)"
                       R"((.*?)(?=<[｜|]ref[｜|]>|\z))"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression boxRe(QStringLiteral(R"(\[\[([^\]]+)\]\])"));

    auto it = markRe.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        Block block;
        block.kind = match.captured(1).trimmed().toLower();
        block.text = match.captured(3).trimmed();

        auto boxIt = boxRe.globalMatch(match.captured(2));
        while (boxIt.hasNext()) {
            const QStringList nums = boxIt.next().captured(1)
                                         .split(QRegularExpression(QStringLiteral("[,\\s]+")),
                                                Qt::SkipEmptyParts);
            if (nums.size() != 4)
                continue;
            bool ok = true;
            double v[4];
            for (int i = 0; i < 4 && ok; ++i) {
                v[i] = nums.at(i).toDouble(&ok);
                v[i] = qBound(0.0, v[i], 1000.0);
            }
            if (ok)
                block.boxes.append(QRectF(v[0], v[1], v[2] - v[0], v[3] - v[1]));
        }
        if (!block.text.isEmpty() || !block.boxes.isEmpty())
            blocks.append(block);
    }
    return blocks;
}

QString displayText(const QString &raw)
{
    if (raw.contains(QStringLiteral("<table"), Qt::CaseInsensitive))
        return tableToLines(raw).trimmed();
    return stripHtml(raw);
}

QString textOf(const QVector<Block> &blocks, const QVector<int> &indices)
{
    QStringList parts;
    for (int index : indices) {
        if (index < 0 || index >= blocks.size())
            continue;
        const QString text = displayText(blocks.at(index).text);
        if (!text.isEmpty())
            parts << text;
    }
    return parts.join(QLatin1Char('\n'));
}

QString kindLabel(const QString &kind)
{
    if (kind == QLatin1String("title")) return QStringLiteral("标题");
    if (kind == QLatin1String("text")) return QStringLiteral("正文");
    if (kind == QLatin1String("table")) return QStringLiteral("表格");
    if (kind.contains(QLatin1String("page_number"))
        || kind.contains(QLatin1String("page_footer")))
        return QStringLiteral("页码");
    if (kind == QLatin1String("header")) return QStringLiteral("页眉");
    if (kind == QLatin1String("footer")) return QStringLiteral("页脚");
    if (kind == QLatin1String("list")) return QStringLiteral("列表");
    if (kind == QLatin1String("section")) return QStringLiteral("章节");
    if (kind == QLatin1String("abstract")) return QStringLiteral("摘要");
    if (kind.contains(QLatin1String("caption"))) return QStringLiteral("题注");
    if (kind.contains(QLatin1String("figure")) || kind.contains(QLatin1String("image")))
        return QStringLiteral("图片");
    if (kind.contains(QLatin1String("footnote"))) return QStringLiteral("脚注");
    if (kind == QLatin1String("equation")) return QStringLiteral("公式");
    return kind;
}

QString plainText(const QJsonObject &payload, const QString &purpose)
{
    const QJsonArray pages = payload.value(QStringLiteral("pages")).toArray();
    QStringList out;
    for (const QJsonValue &value : pages) {
        QString text = value.toObject().value(QStringLiteral("text")).toString();
        if (purpose == QStringLiteral("rich_layout_recognition")) {
            QStringList lines;
            for (const Block &block : parseBlocks(text)) {
                const QString plain = displayText(block.text);
                if (!plain.isEmpty())
                    lines << plain;
            }
            text = lines.join(QLatin1Char('\n'));
        }
        text = text.trimmed();
        if (!text.isEmpty())
            out << text;
    }
    return out.join(QStringLiteral("\n\n"));
}

bool isTruncated(const QJsonObject &payload)
{
    const QJsonArray pages = payload.value(QStringLiteral("pages")).toArray();
    for (const QJsonValue &value : pages) {
        if (value.toObject().value(QStringLiteral("finish_reason")).toString()
            == QStringLiteral("length"))
            return true;
    }
    return false;
}

bool hasText(const QJsonObject &payload)
{
    const QJsonArray pages = payload.value(QStringLiteral("pages")).toArray();
    for (const QJsonValue &value : pages) {
        if (!value.toObject().value(QStringLiteral("text")).toString().trimmed().isEmpty())
            return true;
    }
    return false;
}

QColor colorForKind(const QString &kind)
{
    if (kind == QLatin1String("title")) return QColor(230, 25, 75);
    if (kind == QLatin1String("text")) return QColor(59, 130, 246);
    if (kind == QLatin1String("table")) return QColor(245, 158, 11);
    if (kind.contains(QLatin1String("caption"))) return QColor(139, 92, 246);
    if (kind.contains(QLatin1String("figure"))) return QColor(16, 185, 129);
    if (kind.contains(QLatin1String("footnote"))) return QColor(107, 114, 128);
    if (kind == QLatin1String("equation")) return QColor(236, 72, 153);
    return QColor(100, 116, 139);
}

QString dshApiKey()
{
    const QByteArray env = qgetenv("DEEPSEEK_API_KEY");
    if (!env.isEmpty())
        return QString::fromUtf8(env);

    const QString path = QDir::homePath() + QStringLiteral("/.dsh/.credentials.yaml");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    static const QRegularExpression re(
        QStringLiteral(R"(DEEPSEEK_API_KEY:\s*["']?([A-Za-z0-9_\-]+))"));
    const auto match = re.match(QString::fromUtf8(file.readAll()));
    return match.hasMatch() ? match.captured(1) : QString();
}

Client::Client(QObject *parent)
    : QObject(parent)
{
    m_baseUrl = QString::fromLatin1(kDefaultBase);
}

void Client::configure(const QString &baseUrl, const QString &apiKey)
{
    m_baseUrl = baseUrl.trimmed().isEmpty() ? QString::fromLatin1(kDefaultBase) : baseUrl.trimmed();
    m_apiKey = apiKey;
}

void Client::submit(int page, int generation, const QByteArray &data,
                    const QString &filename, const QString &purpose)
{
    PageResult early;
    early.page = page;
    early.purpose = purpose;

    if (m_apiKey.isEmpty()) {
        early.error = QStringLiteral("缺少 API Key（设置里填写，或导出 DEEPSEEK_API_KEY）");
        emit finished(page, generation, early);
        return;
    }
    if (data.isEmpty()) {
        early.error = QStringLiteral("页面内容为空");
        emit finished(page, generation, early);
        return;
    }

    const QByteArray boundary =
        "----docrqt" + QByteArray::number(QRandomGenerator::global()->generate64(), 16);

    QByteArray body;
    body.reserve(data.size() + 512);
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"purpose\"\r\n\r\n";
    body += purpose.toUtf8();
    body += "\r\n--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"";
    body += filename.toUtf8();
    body += "\"\r\nContent-Type: " + mimeForName(filename).toUtf8() + "\r\n\r\n";
    body += data;
    body += "\r\n--" + boundary + "--\r\n";

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/ocr")));
    request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      "multipart/form-data; boundary=" + boundary);
    request.setTransferTimeout(300000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_nam.post(request, body);
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, page, generation, purpose, started] {
                handleReply(reply, page, generation, purpose, started);
                reply->deleteLater();
            });
}

void Client::handleReply(QNetworkReply *reply, int page, int generation,
                         const QString &purpose, qint64 started)
{
    PageResult result;
    result.page = page;
    result.purpose = purpose;
    result.elapsedMs = QDateTime::currentMSecsSinceEpoch() - started;
    const QByteArray raw = reply->readAll();
    result.raw = raw;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError && status == 0) {
        result.error = QStringLiteral("网络错误：%1").arg(reply->errorString());
        emit finished(page, generation, result);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);

    if (status != 200) {
        QString detail;
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonValue err = doc.object().value(QStringLiteral("error"));
            detail = err.isObject() ? err.toObject().value(QStringLiteral("message")).toString()
                                    : err.toString();
        }
        if (detail.isEmpty())
            detail = QString::fromUtf8(raw.left(200));
        result.error = QStringLiteral("HTTP %1：%2").arg(status).arg(detail);
        emit finished(page, generation, result);
        return;
    }

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("响应不是合法 JSON");
        emit finished(page, generation, result);
        return;
    }

    const QJsonObject payload = doc.object();
    const QJsonValue err = payload.value(QStringLiteral("error"));
    if (err.isObject()) {
        result.error = err.toObject().value(QStringLiteral("message")).toString(
            QStringLiteral("OCR 识别失败"));
        emit finished(page, generation, result);
        return;
    }
    if (!payload.contains(QStringLiteral("pages"))) {
        result.error = QStringLiteral("响应缺少 pages 字段");
        emit finished(page, generation, result);
        return;
    }

    result.payload = payload;
    if (purpose == QStringLiteral("rich_layout_recognition")) {
        for (const QJsonValue &value : payload.value(QStringLiteral("pages")).toArray())
            result.blocks += parseBlocks(value.toObject().value(QStringLiteral("text")).toString());
    }
    emit finished(page, generation, result);
}

} // namespace ocr
