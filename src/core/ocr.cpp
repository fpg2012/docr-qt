#include "ocr.h"

#include "layout.h"
#include "textlayout.h"

#include <QBuffer>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQueue>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QUrl>
#include <QtConcurrent>

#include <cmath>
#include <memory>

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

QJsonObject glmBody(const QString &model, const QString &prompt,
                    const QByteArray &png, int maxTokens)
{
    const QString dataUri =
        QStringLiteral("data:image/png;base64,")
        + QString::fromLatin1(png.toBase64());
    QJsonArray content;
    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                               {QStringLiteral("image_url"),
                                QJsonObject{{QStringLiteral("url"), dataUri}}}});
    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("text"), prompt}});
    QJsonObject message{{QStringLiteral("role"), QStringLiteral("user")},
                        {QStringLiteral("content"), content}};

    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("messages"), QJsonArray{message});
    body.insert(QStringLiteral("max_tokens"), maxTokens);
    body.insert(QStringLiteral("temperature"), 0);
    body.insert(QStringLiteral("top_p"), 0.00001);
    body.insert(QStringLiteral("top_k"), 1);
    body.insert(QStringLiteral("repetition_penalty"), 1.1);
    body.insert(QStringLiteral("stream"), false);
    return body;
}

QString stripFence(QString content)
{
    content = content.trimmed();
    if (!content.startsWith(QStringLiteral("```")))
        return content;
    const int firstLine = content.indexOf(QLatin1Char('\n'));
    const int lastFence = content.lastIndexOf(QStringLiteral("```"));
    if (firstLine >= 0 && lastFence > firstLine)
        return content.mid(firstLine + 1, lastFence - firstLine - 1).trimmed();
    return content;
}

QString plainFromMarkdown(const QString &markdown)
{
    QTextDocument doc;
    doc.setMarkdown(markdown);
    QString text = doc.toPlainText().trimmed();
    if (text.isEmpty() && markdown.contains(QStringLiteral("<table"), Qt::CaseInsensitive))
        text = stripHtml(markdown).trimmed();
    return text;
}

QString cleanRegionText(const QString &content, const QString &kind)
{
    const QString body = stripFence(content);
    if (kind == QLatin1String("table")) {
        if (body.contains(QStringLiteral("<table"), Qt::CaseInsensitive))
            return tableToLines(body).trimmed();
        return body.trimmed();
    }
    if (kind == QLatin1String("equation"))
        return body.trimmed();
    QString text = plainFromMarkdown(body);
    if (kind == QLatin1String("title"))
        text.remove(QRegularExpression(QStringLiteral("^#{1,6}\\s*")));
    return text.trimmed();
}

// 把归一化框转成像素裁剪；四周留一点边，避免切掉笔画。
QImage cropRegion(const QImage &page, const QRectF &box, int paddingPercent)
{
    if (page.isNull())
        return {};
    const double padX = page.width() * paddingPercent / 1000.0;
    const double padY = page.height() * paddingPercent / 1000.0;
    const int left = qMax(0, int(std::floor(box.left() / 1000.0 * page.width() - padX)));
    const int top = qMax(0, int(std::floor(box.top() / 1000.0 * page.height() - padY)));
    const int right = qMin(page.width(),
                           int(std::ceil(box.right() / 1000.0 * page.width() + padX)));
    const int bottom = qMin(page.height(),
                            int(std::ceil(box.bottom() / 1000.0 * page.height() + padY)));
    if (right <= left || bottom <= top)
        return {};
    return page.copy(QRect(left, top, right - left, bottom - top));
}

QByteArray pngBytes(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

bool parseGlmAnswer(const QByteArray &raw, int status, QString *content,
                    QString *error, QJsonObject *usage)
{
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
        *error = QStringLiteral("GLM-OCR HTTP %1：%2").arg(status).arg(detail);
        return false;
    }
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("GLM-OCR 响应不是合法 JSON");
        return false;
    }
    const QJsonObject root = doc.object();
    const QJsonValue err = root.value(QStringLiteral("error"));
    if (!err.isUndefined() && !err.isNull()) {
        *error = err.isObject()
                     ? err.toObject().value(QStringLiteral("message")).toString(
                           QStringLiteral("GLM-OCR 识别失败"))
                     : err.toString(QStringLiteral("GLM-OCR 识别失败"));
        return false;
    }
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        *error = QStringLiteral("GLM-OCR 响应缺少 choices");
        return false;
    }
    const QJsonObject message =
        choices.first().toObject().value(QStringLiteral("message")).toObject();
    *content = message.value(QStringLiteral("content")).toString();
    if (usage)
        *usage = root.value(QStringLiteral("usage")).toObject();
    return true;
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


namespace {

QRectF rectFromJson(const QJsonValue &value)
{
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (array.size() >= 4) {
            const qreal x1 = array.at(0).toDouble();
            const qreal y1 = array.at(1).toDouble();
            const qreal x2 = array.at(2).toDouble();
            const qreal y2 = array.at(3).toDouble();
            const qreal left = qMax<qreal>(0, qMin(x1, x2));
            const qreal top = qMax<qreal>(0, qMin(y1, y2));
            const qreal right = qMin<qreal>(1000, qMax(x1, x2));
            const qreal bottom = qMin<qreal>(1000, qMax(y1, y2));
            return QRectF(left, top, qMax<qreal>(0, right - left),
                          qMax<qreal>(0, bottom - top));
        }
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        const qreal x = object.value(QStringLiteral("x")).toDouble();
        const qreal y = object.value(QStringLiteral("y")).toDouble();
        const qreal w = object.value(QStringLiteral("w")).toDouble(
            object.value(QStringLiteral("width")).toDouble());
        const qreal h = object.value(QStringLiteral("h")).toDouble(
            object.value(QStringLiteral("height")).toDouble());
        return QRectF(qMax<qreal>(0, x), qMax<qreal>(0, y),
                      qMax<qreal>(0, w), qMax<qreal>(0, h));
    }
    return {};
}

QJsonArray rectToJson(const QRectF &box)
{
    return QJsonArray{box.left(), box.top(), box.right(), box.bottom()};
}

} // namespace

QJsonObject blockToJson(const Block &block)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), block.kind);
    object.insert(QStringLiteral("text"), block.text);

    QJsonArray boxes;
    for (const QRectF &box : block.boxes)
        boxes.append(rectToJson(box));
    object.insert(QStringLiteral("boxes"), boxes);

    if (!block.lines.isEmpty()) {
        QJsonArray lines;
        for (const TextLine &line : block.lines) {
            QJsonObject entry;
            entry.insert(QStringLiteral("box"), rectToJson(line.box));
            entry.insert(QStringLiteral("text"), line.text);
            entry.insert(QStringLiteral("start"), line.start);
            entry.insert(QStringLiteral("length"), line.length);
            lines.append(entry);
        }
        object.insert(QStringLiteral("lines"), lines);
    }
    return object;
}

Block blockFromJson(const QJsonObject &object)
{
    Block block;
    block.kind = object.value(QStringLiteral("kind")).toString(QStringLiteral("text"));
    block.text = object.value(QStringLiteral("text")).toString();

    const QJsonArray boxes = object.value(QStringLiteral("boxes")).toArray();
    for (const QJsonValue &value : boxes) {
        const QRectF box = rectFromJson(value);
        if (box.width() > 0 && box.height() > 0)
            block.boxes.append(box);
    }
    if (block.boxes.isEmpty()) {
        const QRectF box = rectFromJson(object.value(QStringLiteral("bbox")));
        if (box.width() > 0 && box.height() > 0)
            block.boxes.append(box);
    }

    const QJsonArray lines = object.value(QStringLiteral("lines")).toArray();
    for (const QJsonValue &value : lines) {
        const QJsonObject entry = value.toObject();
        TextLine line;
        line.box = rectFromJson(entry.value(QStringLiteral("box")));
        line.text = entry.value(QStringLiteral("text")).toString();
        line.start = entry.value(QStringLiteral("start")).toInt(0);
        line.length = entry.value(QStringLiteral("length")).toInt(line.text.size());
        if (line.box.width() > 0 && line.box.height() > 0 && !line.text.isEmpty())
            block.lines.append(line);
    }
    return block;
}

QVector<Block> blocksFromPayload(const QJsonObject &payload, QString *purpose)
{
    QVector<Block> blocks;
    const QJsonArray blockArray = payload.value(QStringLiteral("blocks")).toArray();
    for (const QJsonValue &value : blockArray) {
        const Block block = blockFromJson(value.toObject());
        if (!block.text.isEmpty() || !block.boxes.isEmpty())
            blocks.append(block);
    }

    const QJsonArray layoutBlocks = payload.value(QStringLiteral("layout_blocks")).toArray();
    if (blocks.isEmpty()) {
        for (const QJsonValue &value : layoutBlocks) {
            const Block block = blockFromJson(value.toObject());
            if (!block.text.isEmpty() || !block.boxes.isEmpty())
                blocks.append(block);
        }
    }

    if (blocks.isEmpty()) {
        const QJsonArray pages = payload.value(QStringLiteral("pages")).toArray();
        for (const QJsonValue &value : pages)
            blocks += parseBlocks(value.toObject().value(QStringLiteral("text")).toString());
    }

    // 纯文本/整页 fallback：没有坐标时给一个整页块，后续仍可用页面图像检测行。
    if (blocks.isEmpty()) {
        const QJsonArray pages = payload.value(QStringLiteral("pages")).toArray();
        if (!pages.isEmpty()) {
            const QString text = pages.first().toObject().value(QStringLiteral("text")).toString();
            if (!text.trimmed().isEmpty()) {
                Block block;
                block.kind = QStringLiteral("text");
                block.text = text;
                block.boxes.append(QRectF(20, 20, 960, 960));
                blocks.append(block);
            }
        }
    }

    if (purpose) {
        const QString stored = payload.value(QStringLiteral("purpose")).toString();
        if (!stored.isEmpty()) {
            *purpose = stored;
        } else if (payload.value(QStringLiteral("provider")).toString()
                       == QLatin1String("glmocr")
                   || payload.contains(QStringLiteral("layout"))
                   || !layoutBlocks.isEmpty()) {
            *purpose = QStringLiteral("glmocr");
        } else if (!blocks.isEmpty()) {
            *purpose = QStringLiteral("rich_layout_recognition");
        } else {
            *purpose = QStringLiteral("text_recognition");
        }
    }
    return blocks;
}

QString displayText(const QString &raw)
{
    if (raw.contains(QStringLiteral("<table"), Qt::CaseInsensitive))
        return tableToLines(raw).trimmed();
    // 纯文本/Markdown 不做 HTML 解析，否则 QTextDocumentFragment 会把换行压成空格。
    if (raw.contains(QLatin1Char('<')) && raw.contains(QLatin1Char('>')))
        return stripHtml(raw);
    return raw.trimmed();
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



// GLM 的“版面检测 + 分区 OCR”流水线。
// 版面在 QtConcurrent 线程里跑，OCR 请求回到主线程，按页合并成一个 PageResult。
struct Client::GlmPipeline : QObject {
    struct Task {
        LayoutRegion region;
        QByteArray image;
        QString prompt;
        QString text;
        QString error;
        bool imageOnly = false;
        QNetworkReply *reply = nullptr;
    };

    struct Job {
        int page = -1;
        int generation = -1;
        qint64 started = 0;
        QByteArray pageData;
        QImage pageImage;
        QVector<Task> tasks;
        QQueue<int> queue;
        QVector<QNetworkReply *> replies;
        QFutureWatcher<LayoutResult> *watcher = nullptr;
        int active = 0;
        int completed = 0;
        int promptTokens = 0;
        int completionTokens = 0;
        int totalTokens = 0;
        QString firstError;
        bool cancelled = false;
        bool finishing = false;
    };

    explicit GlmPipeline(Client *client)
        : QObject(nullptr)
        , q(client)
    {
    }

    ~GlmPipeline() override { cancel(); }

    void submit(int page, int generation, const QByteArray &data)
    {
        if (jobs.contains(page))
            cancelJob(page);

        QString layoutError;
        const std::shared_ptr<LayoutEngine> engine = q->layoutEngine(&layoutError);
        if (!engine) {
            q->submitGlmWholePage(page, generation, data, layoutError);
            return;
        }

        const QImage image = QImage::fromData(data);
        if (image.isNull()) {
            q->submitGlmWholePage(page, generation, data,
                                  QStringLiteral("无法解码页面图像"));
            return;
        }

        auto job = std::make_shared<Job>();
        job->page = page;
        job->generation = generation;
        job->started = QDateTime::currentMSecsSinceEpoch();
        job->pageData = data;
        job->pageImage = image;
        jobs.insert(page, job);

        auto *watcher = new QFutureWatcher<LayoutResult>(this);
        job->watcher = watcher;
        connect(watcher, &QFutureWatcher<LayoutResult>::finished, this,
                [this, page, generation] { onLayout(page, generation); });
        watcher->setFuture(QtConcurrent::run(
            [engine, image] { return engine->detect(image); }));
    }

    void cancel()
    {
        const QList<int> pages = jobs.keys();
        for (int page : pages)
            cancelJob(page);
    }

    void cancelJob(int page)
    {
        auto it = jobs.find(page);
        if (it == jobs.end())
            return;
        const auto job = *it;
        job->cancelled = true;
        jobs.erase(it);

        if (job->watcher) {
            job->watcher->cancel();
            job->watcher->deleteLater();
            job->watcher = nullptr;
        }
        // abort() 可能同步触发 finished()，先清空列表避免 lambda 回写时迭代失效。
        const QVector<QNetworkReply *> replies = job->replies;
        job->replies.clear();
        for (QNetworkReply *reply : replies)
            reply->abort();
    }

    void onLayout(int page, int generation)
    {
        auto it = jobs.find(page);
        if (it == jobs.end() || (*it)->generation != generation
            || (*it)->cancelled) {
            return;
        }
        const auto job = *it;
        auto *watcher = job->watcher;
        if (!watcher)
            return;
        const LayoutResult layout = watcher->result();
        job->watcher = nullptr;
        watcher->deleteLater();

        if (!layout.ok() || layout.regions.isEmpty()) {
            jobs.remove(page);
            const QString reason = layout.error.isEmpty()
                                       ? QStringLiteral("版面模型没有返回区域")
                                       : layout.error;
            q->submitGlmWholePage(page, generation, job->pageData, reason);
            return;
        }
        startTasks(job, layout);
    }

    void startTasks(const std::shared_ptr<Job> &job, const LayoutResult &layout)
    {
        int imageOnlyCount = 0;
        for (const LayoutRegion &region : layout.regions) {
            Task task;
            task.region = region;
            if (region.isImage()) {
                task.imageOnly = true;
                ++imageOnlyCount;
                job->tasks.append(task);
                continue;
            }
            const int padding = region.kind == QLatin1String("table") ? 8 : 5;
            const QImage crop = cropRegion(job->pageImage, region.box, padding);
            if (crop.isNull())
                continue;
            task.image = pngBytes(crop);
            task.prompt = promptForKind(region.kind);
            job->tasks.append(task);
        }

        if (job->tasks.isEmpty()) {
            jobs.remove(job->page);
            q->submitGlmWholePage(job->page, job->generation, job->pageData,
                                  QStringLiteral("版面检测没有可用区域"));
            return;
        }

        job->completed = imageOnlyCount;
        for (int i = 0; i < job->tasks.size(); ++i) {
            if (!job->tasks.at(i).imageOnly)
                job->queue.enqueue(i);
        }
        if (job->queue.isEmpty()) {
            finish(job);
            return;
        }
        pump(job);
    }

    void pump(const std::shared_ptr<Job> &job)
    {
        // 一页内最多 4 个区域并发；页间并发由 OcrQueue 控制。
        while (!job->cancelled && job->active < 4 && !job->queue.isEmpty()) {
            const int index = job->queue.dequeue();
            ++job->active;
            sendRegion(job, index);
        }
    }

    void sendRegion(const std::shared_ptr<Job> &job, int index)
    {
        Task &task = job->tasks[index];
        const int maxTokens = task.region.kind == QLatin1String("table") ? 8192 : 4096;
        const QJsonObject body = glmBody(q->m_glmModel, task.prompt, task.image,
                                         maxTokens);

        QNetworkRequest request(QUrl(q->m_glmEndpoint));
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
        if (!q->m_glmApiKey.isEmpty())
            request.setRawHeader("Authorization",
                                 "Bearer " + q->m_glmApiKey.toUtf8());
        request.setTransferTimeout(300000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);

        QNetworkReply *reply =
            q->m_nam.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        task.reply = reply;
        job->replies.append(reply);
        connect(reply, &QNetworkReply::finished, this,
                [this, job, index, reply] {
                    job->replies.removeOne(reply);
                    onRegion(job, index, reply);
                    reply->deleteLater();
                });
    }

    void onRegion(const std::shared_ptr<Job> &job, int index, QNetworkReply *reply)
    {
        if (job->cancelled)
            return;
        --job->active;
        ++job->completed;

        Task &task = job->tasks[index];
        const QByteArray raw = reply->readAll();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            task.error = QStringLiteral("GLM-OCR 网络错误：%1")
                             .arg(reply->errorString());
        } else {
            QString content;
            QString error;
            QJsonObject usage;
            if (parseGlmAnswer(raw, status, &content, &error, &usage)) {
                task.text = cleanRegionText(content, task.region.kind);
                job->promptTokens += usage.value(QStringLiteral("prompt_tokens")).toInt();
                job->completionTokens +=
                    usage.value(QStringLiteral("completion_tokens")).toInt();
                job->totalTokens +=
                    usage.value(QStringLiteral("total_tokens")).toInt();
            } else {
                task.error = error;
            }
        }
        if (!task.error.isEmpty() && job->firstError.isEmpty())
            job->firstError = task.error;

        pump(job);
        if (job->completed >= job->tasks.size())
            finish(job);
    }

    void finish(const std::shared_ptr<Job> &job)
    {
        if (job->finishing || job->cancelled)
            return;
        job->finishing = true;
        jobs.remove(job->page);

        PageResult result;
        result.page = job->page;
        result.purpose = QStringLiteral("glmocr");
        result.elapsedMs = QDateTime::currentMSecsSinceEpoch() - job->started;

        QStringList lines;
        QJsonArray layoutBlocks;
        for (const Task &task : job->tasks) {
            if (task.imageOnly) {
                Block block;
                block.kind = task.region.kind;
                block.boxes.append(task.region.box);
                result.blocks.append(block);
                layoutBlocks.append(QJsonObject{
                    {QStringLiteral("kind"), block.kind},
                    {QStringLiteral("text"), QString()},
                    {QStringLiteral("bbox"),
                     QJsonArray{task.region.box.left(), task.region.box.top(),
                                task.region.box.right(), task.region.box.bottom()}}});
                continue;
            }
            const QString text = task.text.trimmed();
            if (text.isEmpty())
                continue;
            Block block;
            block.kind = task.region.kind;
            block.text = text;
            block.boxes.append(task.region.box);
            result.blocks.append(block);
            layoutBlocks.append(QJsonObject{
                {QStringLiteral("kind"), block.kind},
                {QStringLiteral("text"), displayText(text).trimmed()},
                {QStringLiteral("bbox"),
                 QJsonArray{task.region.box.left(), task.region.box.top(),
                            task.region.box.right(), task.region.box.bottom()}}});
            const QString plain = displayText(text).trimmed();
            if (!plain.isEmpty())
                lines << plain;
        }

        // 有文字块才算成功；只剩图片块且所有文字区域都失败时要让队列重试。
        if (lines.isEmpty() && !job->firstError.isEmpty()) {
            result.error = job->firstError;
            emit q->finished(job->page, job->generation, result);
            return;
        }

        // 用页面图像检测真实文字行，给文本选择层和可搜索 PDF 用。
        for (Block &block : result.blocks) {
            if (block.text.isEmpty() || block.boxes.isEmpty() || isImageKind(block.kind))
                continue;
            const QString display = displayText(block.text);
            if (display.trimmed().isEmpty())
                continue;
            block.lines = textlayout::place(display, block.boxes.first(), job->pageImage,
                                            block.kind);
        }

        const QString pageText = lines.join(QLatin1Char('\n'));
        QJsonObject pageObject;
        pageObject.insert(QStringLiteral("index"), 0);
        pageObject.insert(QStringLiteral("text"), pageText);
        pageObject.insert(QStringLiteral("finish_reason"), QStringLiteral("stop"));

        QJsonObject usage;
        if (job->totalTokens > 0) {
            usage.insert(QStringLiteral("prompt_tokens"), job->promptTokens);
            usage.insert(QStringLiteral("completion_tokens"), job->completionTokens);
            usage.insert(QStringLiteral("total_tokens"), job->totalTokens);
        }

        QJsonObject payload;
        payload.insert(QStringLiteral("pages"), QJsonArray{pageObject});
        payload.insert(QStringLiteral("usage"), usage);
        payload.insert(QStringLiteral("provider"), QStringLiteral("glmocr"));
        payload.insert(QStringLiteral("layout"), QStringLiteral("pp-doclayout-v3"));
        payload.insert(QStringLiteral("layout_blocks"), layoutBlocks);
        if (!job->firstError.isEmpty()) {
            payload.insert(QStringLiteral("region_error"), job->firstError);
        }
        result.payload = payload;
        emit q->finished(job->page, job->generation, result);
    }

    Client *q = nullptr;
    QHash<int, std::shared_ptr<Job>> jobs;
};

Client::Client(QObject *parent)
    : QObject(parent)
{
    m_baseUrl = QString::fromLatin1(kDefaultBase);
    m_layoutModelPath = defaultLayoutModelPath();
    m_glmPipeline = std::make_unique<GlmPipeline>(this);
}

Client::~Client()
{
    if (m_glmPipeline)
        m_glmPipeline->cancel();
}

void Client::configure(const QString &baseUrl, const QString &apiKey)
{
    m_baseUrl = baseUrl.trimmed().isEmpty() ? QString::fromLatin1(kDefaultBase) : baseUrl.trimmed();
    m_apiKey = apiKey;
}

void Client::configureGlm(const QString &endpoint, const QString &model,
                          const QString &apiKey, const QString &layoutModel)
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

    const QString path = layoutModel.trimmed().isEmpty()
                             ? defaultLayoutModelPath()
                             : layoutModel.trimmed();
    if (path != m_layoutModelPath) {
        m_layoutEngine.reset();
        m_layoutLoadedPath.clear();
        m_layoutError.clear();
    }
    m_layoutModelPath = path;
}

std::shared_ptr<LayoutEngine> Client::layoutEngine(QString *error)
{
    const QString path = m_layoutModelPath.trimmed().isEmpty()
                             ? defaultLayoutModelPath()
                             : m_layoutModelPath.trimmed();
    if (m_layoutEngine && m_layoutLoadedPath == path)
        return m_layoutEngine;
    if (m_layoutLoadedPath == path && !m_layoutError.isEmpty()) {
        if (error)
            *error = m_layoutError;
        return {};
    }

    QString message;
    auto engine = LayoutEngine::create(path, &message);
    m_layoutLoadedPath = path;
    if (!engine) {
        m_layoutError = message;
        if (error)
            *error = message;
        return {};
    }
    m_layoutError.clear();
    m_layoutEngine = engine;
    if (error)
        error->clear();
    return engine;
}

void Client::setBackend(Backend backend)
{
    m_backend = backend;
}

void Client::cancelPending()
{
    if (m_glmPipeline)
        m_glmPipeline->cancel();
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
    m_glmPipeline->submit(page, generation, data);
}

void Client::submitGlmWholePage(int page, int generation, const QByteArray &data,
                                const QString &layoutError)
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
            [this, reply, page, generation, started, layoutError] {
                handleGlmReply(reply, page, generation, started, layoutError);
                reply->deleteLater();
            });
}

void Client::handleGlmReply(QNetworkReply *reply, int page, int generation,
                            qint64 started, const QString &layoutError)
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

    finishGlm(page, generation, root, raw, started, layoutError);
}

void Client::finishGlm(int page, int generation, const QJsonObject &response,
                       const QByteArray &raw, qint64 started,
                       const QString &layoutError)
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
        if (!layoutError.isEmpty()) {
            payload.insert(QStringLiteral("layout"), QStringLiteral("fallback"));
            payload.insert(QStringLiteral("layout_error"), layoutError);
        }
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
        // 只有图片/空 Markdown：按空结果交给 OcrQueue 判断是否空白页。
        QJsonObject pageObject;
        pageObject.insert(QStringLiteral("index"), 0);
        pageObject.insert(QStringLiteral("text"), QString());
        pageObject.insert(QStringLiteral("finish_reason"), QStringLiteral("stop"));
        QJsonObject payload;
        payload.insert(QStringLiteral("pages"), QJsonArray{pageObject});
        payload.insert(QStringLiteral("usage"), response.value(QStringLiteral("usage")));
        payload.insert(QStringLiteral("provider"), QStringLiteral("glmocr"));
        if (!layoutError.isEmpty()) {
            payload.insert(QStringLiteral("layout"), QStringLiteral("fallback"));
            payload.insert(QStringLiteral("layout_error"), layoutError);
        }
        result.payload = payload;
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
    if (!layoutError.isEmpty()) {
        payload.insert(QStringLiteral("layout"), QStringLiteral("fallback"));
        payload.insert(QStringLiteral("layout_error"), layoutError);
    }
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
