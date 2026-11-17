#include "tocagent.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

namespace toc {
namespace {

QString extractJson(const QString &content)
{
    QString text = content.trimmed();
    if (text.startsWith(QStringLiteral("```"))) {
        const int firstLine = text.indexOf(QLatin1Char('\n'));
        const int lastFence = text.lastIndexOf(QStringLiteral("```"));
        if (firstLine >= 0 && lastFence > firstLine)
            text = text.mid(firstLine + 1, lastFence - firstLine - 1).trimmed();
    }
    const int begin = text.indexOf(QLatin1Char('{'));
    const int end = text.lastIndexOf(QLatin1Char('}'));
    if (begin >= 0 && end > begin)
        return text.mid(begin, end - begin + 1);
    return text;
}

QString cleanTitle(QString title)
{
    title.replace(QChar(0x00a0), QLatin1Char(' '));
    title.replace(QChar(0x3000), QLatin1Char(' '));
    title = title.simplified();
    while (!title.isEmpty()) {
        const QChar first = title.front();
        if (first == QLatin1Char('-') || first == QChar(0x2014) || first == QChar(0x2013)
            || first == QChar(0x00b7) || first == QChar(0x2022)
            || first == QLatin1Char('*') || first.isSpace()) {
            title.removeFirst();
        } else {
            break;
        }
    }
    static const QRegularExpression trailingLeaders(
        QStringLiteral(R"(\s*[.．·•…]{2,}\s*\d{1,4}\s*$)"));
    static const QRegularExpression trailingParenPage(
        QStringLiteral(R"(\s*[（(]\s*\d{1,4}\s*[）)]\s*$)"));
    title.remove(trailingLeaders);
    title.remove(trailingParenPage);
    while (!title.isEmpty()) {
        const QChar last = title.back();
        if (last == QChar(0x2026) || last == QLatin1Char('.') || last == QChar(0x00b7)
            || last == QChar(0x2022) || last == QChar(0x2014) || last == QChar(0x2013)) {
            title.chop(1);
        } else {
            break;
        }
    }
    return title.trimmed();
}

bool isNoise(const QString &title)
{
    const QString key = title.toLower().simplified();
    return key == QStringLiteral("目录") || key == QStringLiteral("目 录")
           || key == QStringLiteral("contents") || key == QStringLiteral("table of contents")
           || key == QStringLiteral("章节") || key == QStringLiteral("页码");
}

int pageFrom(const QJsonObject &object)
{
    QJsonValue value = object.value(QStringLiteral("printedPage"));
    if (value.isUndefined())
        value = object.value(QStringLiteral("printed_page"));
    if (value.isUndefined())
        value = object.value(QStringLiteral("page"));

    if (value.isDouble())
        return qRound(value.toDouble());
    if (!value.isString())
        return -1;

    QString text = value.toString().trimmed();
    static const QRegularExpression digits(QStringLiteral(R"((\d{1,4}))"));
    const auto match = digits.match(text);
    if (!match.hasMatch())
        return -1;
    bool ok = false;
    const int page = match.captured(1).toInt(&ok);
    return ok ? page : -1;
}

int levelFrom(const QJsonObject &object)
{
    QJsonValue value = object.value(QStringLiteral("level"));
    if (value.isUndefined())
        value = object.value(QStringLiteral("depth"));
    int level = value.isDouble() ? qRound(value.toDouble()) : 1;
    return qBound(1, level, 6);
}

QString systemPrompt()
{
    return QStringLiteral(
        "你是书籍目录整理助手。输入是 OCR 得到的目录页文本，可能包含多栏、点线、"
        "括号页码、跨页续表和缩进层级。请只依据输入整理，不要补造、改写或翻译条目。\n"
        "输出一个 JSON 对象，格式严格为：\n"
        "{\"entries\":[{\"title\":\"...\",\"printedPage\":134,\"level\":2}]}\n"
        "要求：\n"
        "1. 按目录的阅读顺序输出全部条目；多栏目录不要串列。\n"
        "2. title 只保留标题本身，去掉点线、页码、括号页码和多余空白。\n"
        "3. printedPage 是目录上印的页码，必须是整数；没有页码时填 -1，不要猜。\n"
        "4. level 表示层级：部/编/卷为 1，章/风/大类为 2，篇目/诗题/节为 3，"
        "更细的层级依次增加；没有明显层级时填 1。\n"
        "5. 组标题（如「魏风」「唐风」）本身有页码时也要保留。\n"
        "6. 忽略「目录」「目 录」「Contents」、页眉页脚和单独的数字页码。\n"
        "只输出 JSON，不要 Markdown 代码块，不要解释。");
}

QString userPrompt(const AgentRequest &request)
{
    QString text = request.text.trimmed();
    const int maxChars = 120000;
    if (text.size() > maxChars)
        text = text.left(maxChars) + QStringLiteral("\n…（目录文本过长，已截断）");
    return QStringLiteral("书名：%1\nPDF 总页数：%2\n\n目录页 OCR 文本：\n%3")
        .arg(request.title.isEmpty() ? QStringLiteral("（未知）") : request.title)
        .arg(request.pageCount)
        .arg(text);
}

} // namespace

Agent::Agent(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

Agent::~Agent() = default;

QString Agent::endpointFor(const QString &baseUrl)
{
    QUrl url(baseUrl.trimmed());
    if (!url.isValid() || url.scheme().isEmpty())
        url = QUrl(QStringLiteral("https://api.deepseek.com"));
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (path.endsWith(QStringLiteral("/tools")))
        path.chop(6);
    path += QStringLiteral("/chat/completions");
    if (path.isEmpty())
        path = QStringLiteral("/chat/completions");
    url.setPath(path);
    return url.toString();
}

void Agent::cancel()
{
    m_canceled = true;
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
}

void Agent::generate(const AgentRequest &request)
{
    cancel();
    m_canceled = false;

    if (request.apiKey.trimmed().isEmpty()) {
        emit failed(QStringLiteral("缺少 API Key，无法调用目录模型"));
        return;
    }
    if (request.text.trimmed().isEmpty()) {
        emit failed(QStringLiteral("目录页没有可用的 OCR 文本"));
        return;
    }

    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("system"));
    message.insert(QStringLiteral("content"), systemPrompt());
    QJsonObject user;
    user.insert(QStringLiteral("role"), QStringLiteral("user"));
    user.insert(QStringLiteral("content"), userPrompt(request));

    QJsonObject body;
    body.insert(QStringLiteral("model"),
                request.model.trimmed().isEmpty() ? QStringLiteral("deepseek-flash")
                                                  : request.model.trimmed());
    body.insert(QStringLiteral("messages"), QJsonArray{message, user});
    body.insert(QStringLiteral("temperature"), 0);
    body.insert(QStringLiteral("max_tokens"), 16384);
    body.insert(QStringLiteral("stream"), false);
    body.insert(QStringLiteral("thinking"), QJsonObject{{QStringLiteral("type"),
                                                          QStringLiteral("disabled")}});
    body.insert(QStringLiteral("response_format"),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}});

    QNetworkRequest network(QUrl(endpointFor(request.baseUrl)));
    network.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    network.setRawHeader("Authorization", "Bearer " + request.apiKey.toUtf8());
    network.setTransferTimeout(180000);
    network.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    m_reply = m_nam->post(network, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_reply == reply)
            handleReply(reply);
        reply->deleteLater();
    });
}

void Agent::handleReply(QNetworkReply *reply)
{
    m_reply = nullptr;
    if (m_canceled)
        return;

    const QByteArray raw = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && status == 0) {
        emit failed(QStringLiteral("目录模型网络错误：%1").arg(reply->errorString()));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (status != 200) {
        QString detail;
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonValue error = doc.object().value(QStringLiteral("error"));
            detail = error.isObject()
                         ? error.toObject().value(QStringLiteral("message")).toString()
                         : error.toString();
        }
        if (detail.isEmpty())
            detail = QString::fromUtf8(raw.left(300));
        emit failed(QStringLiteral("目录模型 HTTP %1：%2").arg(status).arg(detail));
        return;
    }

    QString error;
    const QVector<Entry> entries = parseResponse(raw, &error);
    if (entries.isEmpty()) {
        emit failed(error.isEmpty() ? QStringLiteral("目录模型没有返回条目") : error);
        return;
    }
    emit finished(entries);
}

QVector<Entry> Agent::parseResponse(const QByteArray &raw, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("目录模型响应不是合法 JSON");
        return {};
    }

    const QJsonObject root = doc.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty()) {
        const QJsonObject choice = choices.first().toObject();
        if (choice.value(QStringLiteral("finish_reason")).toString()
            == QStringLiteral("length")) {
            if (error)
                *error = QStringLiteral("目录太长，模型输出被截断；请分几段目录页重试");
            return {};
        }
        const QJsonObject message = choice.value(QStringLiteral("message")).toObject();
        const QString content = message.value(QStringLiteral("content")).toString();
        if (content.trimmed().isEmpty()) {
            if (error)
                *error = QStringLiteral("目录模型返回了空内容");
            return {};
        }
        return parseEntriesJson(extractJson(content).toUtf8(), error);
    }

    // 也接受直接传入 {"entries":[...]} 的离线测试数据。
    return parseEntriesJson(raw, error);
}

QVector<Entry> Agent::parseEntriesJson(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("目录 JSON 解析失败：%1").arg(parseError.errorString());
        return {};
    }

    QJsonArray array;
    if (doc.isObject())
        array = doc.object().value(QStringLiteral("entries")).toArray();
    else if (doc.isArray())
        array = doc.array();
    if (array.isEmpty()) {
        if (error)
            *error = QStringLiteral("目录 JSON 中没有 entries");
        return {};
    }

    QVector<Entry> result;
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        Entry entry;
        entry.title = cleanTitle(object.value(QStringLiteral("title")).toString());
        entry.printedPage = pageFrom(object);
        entry.level = levelFrom(object);
        if (entry.title.isEmpty() || isNoise(entry.title))
            continue;
        if (entry.printedPage == 0)
            entry.printedPage = -1;

        bool duplicate = false;
        for (const Entry &existing : result) {
            if (existing.title == entry.title && existing.printedPage == entry.printedPage) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            result.append(entry);
    }
    if (result.isEmpty() && error)
        *error = QStringLiteral("目录 JSON 中没有有效条目");
    return result;
}

} // namespace toc
