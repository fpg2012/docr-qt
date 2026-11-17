// docr-cli：与桌面端共用 core 的命令行 OCR 工具。
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>

#include "core/document.h"
#include "core/ocr.h"
#include "core/ocrcost.h"
#include "core/ocrdiagnostics.h"
#include "core/ocrqueue.h"

namespace {

QTextStream &err()
{
    static QTextStream stream(stderr);
    stream.setEncoding(QStringConverter::Utf8);
    return stream;
}

QTextStream &out()
{
    static QTextStream stream(stdout);
    stream.setEncoding(QStringConverter::Utf8);
    return stream;
}

bool parsePages(const QString &spec, int count, QVector<int> *pages, QString *error)
{
    QSet<int> result;
    if (spec.trimmed().isEmpty()) {
        for (int i = 0; i < count; ++i)
            result.insert(i);
    } else {
        const QStringList tokens = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (tokens.isEmpty()) {
            *error = QStringLiteral("--pages 为空");
            return false;
        }
        for (const QString &raw : tokens) {
            const QString token = raw.trimmed();
            const int dash = token.indexOf(QLatin1Char('-'));
            bool ok1 = false;
            bool ok2 = false;
            int first = 0;
            int last = 0;
            if (dash < 0) {
                first = last = token.toInt(&ok1);
                ok2 = ok1;
            } else {
                first = token.left(dash).trimmed().toInt(&ok1);
                last = token.mid(dash + 1).trimmed().toInt(&ok2);
            }
            if (!ok1 || !ok2 || first < 1 || last < first) {
                *error = QStringLiteral("无法解析页码：%1").arg(token);
                return false;
            }
            if (last > count) {
                *error = QStringLiteral("页码超出范围：%1（共 %2 页）").arg(token).arg(count);
                return false;
            }
            for (int page = first; page <= last; ++page)
                result.insert(page - 1);
        }
    }

    QVector<int> sorted;
    for (int page : result)
        sorted.append(page);
    std::sort(sorted.begin(), sorted.end());
    *pages = sorted;
    return !sorted.isEmpty();
}

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("无法写入：%1").arg(path);
        return false;
    }
    if (file.write(data) < 0) {
        *error = QStringLiteral("写入失败：%1").arg(path);
        return false;
    }
    return true;
}

QByteArray markdownFor(const Document &doc, const QVector<int> &pages)
{
    QString text;
    QTextStream stream(&text);
    stream.setEncoding(QStringConverter::Utf8);
    if (!pages.isEmpty()) {
        const QString file = QFileInfo(doc.page(pages.first()).sourcePath).fileName();
        stream << "# " << (file.isEmpty() ? QStringLiteral("OCR 结果") : file) << "\n\n";
    }
    for (int page : pages) {
        const PageEntry &entry = doc.page(page);
        if (!entry.result.ok())
            continue;
        stream << "## 第 " << (page + 1) << " 页\n\n"
               << ocr::plainText(entry.result.payload, entry.result.purpose) << "\n\n";
    }
    return text.toUtf8();
}

QByteArray textFor(const Document &doc, const QVector<int> &pages)
{
    QString text;
    QTextStream stream(&text);
    stream.setEncoding(QStringConverter::Utf8);
    for (int page : pages) {
        const PageEntry &entry = doc.page(page);
        if (!entry.result.ok())
            continue;
        stream << "===== 第 " << (page + 1) << " 页 =====\n"
               << ocr::plainText(entry.result.payload, entry.result.purpose) << "\n\n";
    }
    return text.toUtf8();
}

QByteArray jsonFor(const Document &doc, const QVector<int> &pages)
{
    QJsonArray array;
    for (int page : pages) {
        const PageEntry &entry = doc.page(page);
        if (entry.result.ok()) {
            QJsonObject wrapper = entry.result.payload;
            wrapper.insert(QStringLiteral("page"), page + 1);
            wrapper.insert(QStringLiteral("source"), entry.label);
            array.append(wrapper);
        } else {
            QJsonObject error;
            error.insert(QStringLiteral("page"), page + 1);
            error.insert(QStringLiteral("source"), entry.label);
            error.insert(QStringLiteral("error"),
                         entry.result.error.isEmpty()
                             ? QStringLiteral("未识别")
                             : entry.result.error);
            array.append(error);
        }
    }
    return QJsonDocument(array).toJson(QJsonDocument::Indented);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("docr-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(DOCR_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("DeepSeek OCR 命令行工具"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption outputOption({QStringLiteral("o"), QStringLiteral("output")},
                                    QStringLiteral("输出目录（默认 ./docr-out）"),
                                    QStringLiteral("dir"), QStringLiteral("docr-out"));
    QCommandLineOption purposeOption(
        {QStringLiteral("p"), QStringLiteral("purpose")},
        QStringLiteral("rich_layout_recognition | text_recognition"),
        QStringLiteral("mode"), QStringLiteral("rich_layout_recognition"));
    QCommandLineOption dpiOption({QStringLiteral("d"), QStringLiteral("dpi")},
                                 QStringLiteral("渲染精度（默认 150）"),
                                 QStringLiteral("dpi"), QStringLiteral("150"));
    QCommandLineOption concOption({QStringLiteral("c"), QStringLiteral("concurrency")},
                                  QStringLiteral("并发数（1-8，默认 3）"),
                                  QStringLiteral("n"), QStringLiteral("3"));
    QCommandLineOption formatOption(
        {QStringLiteral("f"), QStringLiteral("format")},
        QStringLiteral("json | text | markdown | all（默认 json）"),
        QStringLiteral("fmt"), QStringLiteral("json"));
    QCommandLineOption pagesOption(QStringLiteral("pages"),
                                   QStringLiteral("只识别指定页，如 1-5,8（1 起）"),
                                   QStringLiteral("range"));
    QCommandLineOption apiKeyOption(QStringLiteral("api-key"),
                                    QStringLiteral("API Key；默认读环境变量/DSH 凭据库"),
                                    QStringLiteral("key"));
    QCommandLineOption baseUrlOption(
        QStringLiteral("base-url"),
        QStringLiteral("默认 https://api.deepseek.com/v1/tools"),
        QStringLiteral("url"), QStringLiteral("https://api.deepseek.com/v1/tools"));
    QCommandLineOption quietOption({QStringLiteral("q"), QStringLiteral("quiet")},
                                   QStringLiteral("不打印逐页进度"));

    parser.addOptions({outputOption, purposeOption, dpiOption, concOption, formatOption,
                       pagesOption, apiKeyOption, baseUrlOption, quietOption});
    parser.addPositionalArgument(QStringLiteral("input"),
                                 QStringLiteral("PDF / 图片 / 目录，可以多个"));

    parser.process(app);
    const QStringList inputs = parser.positionalArguments();
    if (inputs.isEmpty())
        parser.showHelp(1);

    const bool quiet = parser.isSet(quietOption);
    const QString purpose = parser.value(purposeOption);
    if (purpose != QStringLiteral("rich_layout_recognition")
        && purpose != QStringLiteral("text_recognition")) {
        err() << "未知 purpose：" << purpose << "\n";
        return 1;
    }

    bool ok = false;
    const int dpi = parser.value(dpiOption).toInt(&ok);
    if (!ok || dpi < 72 || dpi > 400) {
        err() << "无效的 dpi\n";
        return 1;
    }
    const int concurrency = parser.value(concOption).toInt(&ok);
    if (!ok || concurrency < 1 || concurrency > 8) {
        err() << "无效的并发数（1-8）\n";
        return 1;
    }

    QString format = parser.value(formatOption).toLower();
    if (format == QStringLiteral("md"))
        format = QStringLiteral("markdown");
    if (format != QStringLiteral("json") && format != QStringLiteral("text")
        && format != QStringLiteral("markdown") && format != QStringLiteral("all")) {
        err() << "未知输出格式：" << format << "\n";
        return 1;
    }

    QString apiKey = parser.value(apiKeyOption).trimmed();
    if (apiKey.isEmpty())
        apiKey = ocr::dshApiKey();
    if (apiKey.isEmpty()) {
        err() << "缺少 API Key：用 --api-key、DEEPSEEK_API_KEY 或 ~/.dsh/.credentials.yaml\n";
        return 1;
    }

    Document doc;
    doc.setDpi(dpi);
    DocumentLoadReport load;
    if (!doc.addPaths(inputs, dpi, &load) || doc.count() == 0) {
        err() << "没有可识别的页面\n";
        for (const QString &error : load.errors)
            err() << "  " << error << "\n";
        return 1;
    }
    for (const QString &error : load.errors)
        err() << "跳过：" << error << "\n";

    QVector<int> pages;
    QString pageError;
    if (!parsePages(parser.value(pagesOption), doc.count(), &pages, &pageError)) {
        err() << pageError << "\n";
        return 1;
    }

    if (!quiet) {
        out() << "载入 " << doc.count() << " 页，识别 " << pages.size() << " 页，"
              << purpose << " @ " << dpi << " dpi\n";
    }

    OcrQueue queue(&doc);
    queue.configure(parser.value(baseUrlOption), apiKey, purpose, concurrency);

    int finished = 0;
    int failed = 0;
    int emptyResults = 0;
    QSet<int> startedPages;
    QEventLoop loop;

    QObject::connect(&queue, &OcrQueue::pageStarted, &app, [&](int page) {
        startedPages.insert(page);
        if (!quiet) {
            out() << "[" << startedPages.size() << "/" << pages.size() << "] "
                  << doc.page(page).label << " …\n";
            out().flush();
        }
    });
    QObject::connect(&queue, &OcrQueue::pageRetrying, &app, [&](int page) {
        if (!quiet) {
            err() << "第 " << (page + 1) << " 页返回空结果，正在重试…\n";
            err().flush();
        }
    });
    QObject::connect(&queue, &OcrQueue::pageFinished, &app,
                     [&](int page, const ocr::PageResult &result) {
        ++finished;
        const bool rich = result.purpose == QStringLiteral("rich_layout_recognition");
        const bool empty = result.ok() && rich && result.blocks.isEmpty()
                           && !ocr::hasText(result.payload);
        if (!result.ok()) {
            ++failed;
            if (!quiet) {
                err() << "[" << finished << "/" << pages.size() << "] 第 "
                      << (page + 1) << " 页失败：" << result.error << "\n";
                err().flush();
            }
        } else if (empty) {
            ++emptyResults;
            if (OcrQueue::pageHasInk(doc.ensureImage(page))) {
                const QString diagnostics = ocrdiag::saveEmptyResult(
                    page, doc.page(page), doc.ensureImage(page), result,
                    parser.value(baseUrlOption), dpi);
                if (!quiet) {
                    err() << "[" << finished << "/" << pages.size() << "] 第 "
                          << (page + 1) << " 页未返回文字";
                    if (!diagnostics.isEmpty())
                        err() << "；诊断：" << diagnostics;
                    err() << "\n";
                    err().flush();
                }
            }
        } else if (!quiet) {
            out() << "[" << finished << "/" << pages.size() << "] 第 "
                  << (page + 1) << " 页 ✓";
            if (rich)
                out() << " · " << result.blocks.size() << " 个块";
            out() << "\n";
            out().flush();
        }
    });
    QObject::connect(&queue, &OcrQueue::allFinished, &loop, &QEventLoop::quit);

    const int queued = queue.startPages(pages);
    if (queued == 0) {
        err() << "没有需要识别的页面\n";
        return 1;
    }
    loop.exec();

    QString outputDir = parser.value(outputOption);
    if (outputDir.isEmpty())
        outputDir = QStringLiteral("docr-out");
    if (!QDir().mkpath(outputDir)) {
        err() << "无法创建输出目录：" << outputDir << "\n";
        return 1;
    }

    const QString jsonPath = QDir(outputDir).filePath(QStringLiteral("result.json"));
    const QString textPath = QDir(outputDir).filePath(QStringLiteral("result.txt"));
    const QString markdownPath = QDir(outputDir).filePath(QStringLiteral("result.md"));
    QString writeError;

    if (format == QStringLiteral("json") || format == QStringLiteral("all")) {
        if (!writeFile(jsonPath, jsonFor(doc, pages), &writeError))
            err() << writeError << "\n";
    }
    if (format == QStringLiteral("text") || format == QStringLiteral("all")) {
        if (!writeFile(textPath, textFor(doc, pages), &writeError))
            err() << writeError << "\n";
    }
    if (format == QStringLiteral("markdown") || format == QStringLiteral("all")) {
        if (!writeFile(markdownPath, markdownFor(doc, pages), &writeError))
            err() << writeError << "\n";
    }

    int success = 0;
    int tokens = 0;
    double seconds = 0.0;
    for (int page : pages) {
        const PageEntry &entry = doc.page(page);
        if (!entry.result.ok())
            continue;
        ++success;
        tokens += entry.result.usage().value(QStringLiteral("total_tokens")).toInt();
        seconds += entry.result.elapsedMs / 1000.0;
    }

    const ocrcost::Estimate cost = ocrcost::actual(doc);
    out() << "识别完成：" << success << "/" << pages.size() << " 页 · " << tokens
          << " tokens · " << QString::number(seconds, 'f', 1) << "s · OCR 页费≈¥"
          << ocrcost::formatCny(cost.total) << "\n";
    if (failed > 0 || emptyResults > 0) {
        out() << "其中 " << failed << " 页失败、" << emptyResults << " 页空结果\n";
    }
    if (format == QStringLiteral("json") || format == QStringLiteral("all"))
        out() << "JSON: " << jsonPath << "\n";
    if (format == QStringLiteral("text") || format == QStringLiteral("all"))
        out() << "Text: " << textPath << "\n";
    if (format == QStringLiteral("markdown") || format == QStringLiteral("all"))
        out() << "Markdown: " << markdownPath << "\n";
    out().flush();

    return failed > 0 ? 2 : 0;
}
