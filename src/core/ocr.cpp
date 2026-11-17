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
#include <QTextDocument>
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

bool isImageKind(const QString &kind)
{
    return kind == QLatin1String("image") || kind == QLatin1String("figure");
}

bool isLayoutPurpose(const QString &purpose)
{
    return purpose == QLatin1String("rich_layout_recognition")
           || purpose == QLatin1String("glmocr");
}

bool needsRetry(const PageResult &result)
{
    if (!result.error.isEmpty())
        return true;
    if (!result.ok())
        return false;
    return isLayoutPurpose(result.purpose) && result.blocks.isEmpty()
           && !hasText(result.payload) && !result.blank;
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
    if (isImageKind(kind))
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

QString defaultGlmEndpoint()
{
    return QStringLiteral("http://127.0.0.1:8080/v1/chat/completions");
}

Backend backendFromString(const QString &name)
{
    return name.compare(QStringLiteral("glmocr"), Qt::CaseInsensitive) == 0
                   || name.compare(QStringLiteral("glm-ocr"), Qt::CaseInsensitive) == 0
                   || name.compare(QStringLiteral("glm"), Qt::CaseInsensitive) == 0
               ? Backend::GlmOcr
               : Backend::DeepSeek;
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

void Client::configureGlm(const QString &endpoint, const QString &model,
                          const QString &apiKey)
{
    QString url = endpoint.trimmed();
    if (url.isEmpty())
        url = defaultGlmEndpoint();
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    if (url.endsWith(QStringLiteral("/v1")))
        url += QStringLiteral("/chat/completions");
    else if (!url.contains(QStringLiteral("/v1/chat/completions"))
             && QUrl(url).path().isEmpty())
        url += QStringLiteral("/v1/chat/completions");
    m_glmEndpoint = url;
    m_glmModel = model.trimmed().isEmpty() ? QStringLiteral("glm-ocr") : model.trimmed();
    m_glmApiKey = apiKey;
}

void Client::setBackend(Backend backend)
{
    m_backend = backend;
}

void Client::submit(int page, int generation, const QByteArray &data,
                    const QString &filename, const QString &purpose)
{
    if (m_backend == Backend::GlmOcr) {
        submitGlm(page, generation, data);
        return;
    }
    submitDeepSeek(page, generation, data, filename, purpose);
}

void Client::submitDeepSeek(int page, int generation, const QByteArray &data,
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

void Client::submitGlm(int page, int generation, const QByteArray &data)
{
    ocr::PageResult early;
    early.page = page;
    early.purpose = QStringLiteral("glmocr");
    if (data.isEmpty()) {
        early.error = QStringLiteral("页面内容为空");
        emit finished(page, generation, early);
        return;
    }

    const QString dataUri =
        QStringLiteral("data:image/png;base64,") + QString::fromLatin1(data.toBase64());
    QJsonArray content;
    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                               {QStringLiteral("image_url"),
                                QJsonObject{{QStringLiteral("url"), dataUri}}}});
    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("text"), QStringLiteral("Text Recognition:")}});
    QJsonObject message{{QStringLiteral("role"), QStringLiteral("user")},
                        {QStringLiteral("content"), content}};

    QJsonObject body;
    body.insert(QStringLiteral("model"),
                m_glmModel.isEmpty() ? QStringLiteral("glm-ocr") : m_glmModel);
    body.insert(QStringLiteral("messages"), QJsonArray{message});
    body.insert(QStringLiteral("max_tokens"), 8192);
    body.insert(QStringLiteral("temperature"), 0);
    body.insert(QStringLiteral("top_p"), 0.00001);
    body.insert(QStringLiteral("top_k"), 1);
    body.insert(QStringLiteral("repetition_penalty"), 1.1);
    body.insert(QStringLiteral("stream"), false);

    QNetworkRequest request(QUrl(m_glmEndpoint.isEmpty() ? defaultGlmEndpoint()
                                                         : m_glmEndpoint));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_glmApiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_glmApiKey.toUtf8());
    request.setTransferTimeout(300000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply *reply = m_nam.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, page, generation, started] {
                handleGlmReply(reply, page, generation, started);
                reply->deleteLater();
            });
}

void Client::handleGlmReply(QNetworkReply *reply, int page, int generation,
                            qint64 started)
{
    ocr::PageResult result;
    result.page = page;
    result.purpose = QStringLiteral("glmocr");
    result.elapsedMs = QDateTime::currentMSecsSinceEpoch() - started;
    const QByteArray raw = reply->readAll();
    result.raw = raw;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError && status == 0) {
        result.error = QStringLiteral("GLM-OCR 网络错误：%1").arg(reply->errorString());
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
            detail = QString::fromUtf8(raw.left(300));
        result.error = QStringLiteral("GLM-OCR HTTP %1：%2").arg(status).arg(detail);
        emit finished(page, generation, result);
        return;
    }
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("GLM-OCR 响应不是合法 JSON");
        emit finished(page, generation, result);
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonValue err = root.value(QStringLiteral("error"));
    if (!err.isUndefined() && !err.isNull()) {
        result.error = err.isObject()
                           ? err.toObject().value(QStringLiteral("message")).toString(
                                 QStringLiteral("GLM-OCR 识别失败"))
                           : err.toString(QStringLiteral("GLM-OCR 识别失败"));
        emit finished(page, generation, result);
        return;
    }

    finishGlm(page, generation, root, raw, started);
}

void Client::finishGlm(int page, int generation, const QJsonObject &response,
                       const QByteArray &raw, qint64 started)
{
    ocr::PageResult result;
    result.page = page;
    result.purpose = QStringLiteral("glmocr");
    result.elapsedMs = QDateTime::currentMSecsSinceEpoch() - started;
    result.raw = raw;

    const QJsonArray choices = response.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        result.error = QStringLiteral("GLM-OCR 响应缺少 choices");
        emit finished(page, generation, result);
        return;
    }
    const QJsonObject choice = choices.first().toObject();
    const QJsonObject message = choice.value(QStringLiteral("message")).toObject();
    QString content = message.value(QStringLiteral("content")).toString();
    if (content.trimmed().isEmpty()) {
        // 交给 OcrQueue 判断是空白页还是服务端空结果，后者会自动重试一次。
        QJsonObject pageObject;
        pageObject.insert(QStringLiteral("index"), 0);
        pageObject.insert(QStringLiteral("text"), QString());
        pageObject.insert(QStringLiteral("finish_reason"), QStringLiteral("stop"));
        QJsonObject payload;
        payload.insert(QStringLiteral("pages"), QJsonArray{pageObject});
        payload.insert(QStringLiteral("usage"), response.value(QStringLiteral("usage")));
        payload.insert(QStringLiteral("provider"), QStringLiteral("glmocr"));
        result.payload = payload;
        emit finished(page, generation, result);
        return;
    }

    if (content.trimmed().startsWith(QStringLiteral("```"))) {
        const QString trimmed = content.trimmed();
        const int firstLine = trimmed.indexOf(QLatin1Char('\n'));
        const int lastFence = trimmed.lastIndexOf(QStringLiteral("```"));
        if (firstLine >= 0 && lastFence > firstLine)
            content = trimmed.mid(firstLine + 1, lastFence - firstLine - 1);
    }

    QString plain;
    {
        QTextDocument markdown;
        markdown.setMarkdown(content);
        plain = markdown.toPlainText().trimmed();
    }
    if (plain.isEmpty() && content.contains(QStringLiteral("<table"), Qt::CaseInsensitive)) {
        // Markdown 解析器不认某些 HTML 表格时，退回带全部可读文本的 HTML 处理。
        plain = stripHtml(content).trimmed();
        plain.replace(QLatin1Char('\t'), QStringLiteral(" | "));
    }
    if (plain.isEmpty()) {
        result.error = QStringLiteral("GLM-OCR 没有可显示的文本");
        emit finished(page, generation, result);
        return;
    }

    ocr::Block block;
    block.kind = QStringLiteral("text");
    block.text = plain;
    block.boxes.append(QRectF(20, 20, 960, 960));
    result.blocks.append(block);

    QJsonObject pageObject;
    pageObject.insert(QStringLiteral("index"), 0);
    pageObject.insert(QStringLiteral("text"), plain);
    pageObject.insert(QStringLiteral("finish_reason"),
                      choice.value(QStringLiteral("finish_reason")).toString(
                          QStringLiteral("stop")));
    QJsonObject payload;
    payload.insert(QStringLiteral("pages"), QJsonArray{pageObject});
    payload.insert(QStringLiteral("usage"), response.value(QStringLiteral("usage")));
    payload.insert(QStringLiteral("provider"), QStringLiteral("glmocr"));
    result.payload = payload;
    emit finished(page, generation, result);
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
