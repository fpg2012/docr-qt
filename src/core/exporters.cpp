#include "exporters.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>

#include "document.h"
#include "ocr.h"

namespace exporters {

bool writeMarkdown(const Document &doc, const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("无法写入文件：%1").arg(path);
        return false;
    }

    QTextStream out(&file);
    for (int i = 0; i < doc.count(); ++i) {
        const PageEntry &entry = doc.page(i);
        if (!entry.result.ok())
            continue;
        out << "## " << (i + 1) << "\n\n"
            << ocr::plainText(entry.result.payload, entry.result.purpose) << "\n\n";
    }
    return true;
}

bool writeJson(const Document &doc, const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入文件：%1").arg(path);
        return false;
    }

    QJsonArray pages;
    for (int i = 0; i < doc.count(); ++i) {
        const PageEntry &entry = doc.page(i);
        if (!entry.result.ok())
            continue;
        QJsonObject wrapper = entry.result.payload;
        wrapper.insert(QStringLiteral("page"), i);
        wrapper.insert(QStringLiteral("page_index"), i);
        wrapper.insert(QStringLiteral("purpose"), entry.result.purpose);
        wrapper.insert(QStringLiteral("source"), entry.sourcePath);
        if (!entry.result.blocks.isEmpty()) {
            QJsonArray blocks;
            for (const ocr::Block &block : entry.result.blocks)
                blocks.append(ocr::blockToJson(block));
            wrapper.insert(QStringLiteral("blocks"), blocks);
        }
        pages.append(wrapper);
    }
    file.write(QJsonDocument(pages).toJson(QJsonDocument::Indented));
    return true;
}

namespace {

QVector<QJsonObject> pageObjects(const QJsonDocument &doc)
{
    QVector<QJsonObject> objects;
    const auto append = [&objects](const QJsonValue &value) {
        if (value.isObject())
            objects.append(value.toObject());
    };

    if (doc.isArray()) {
        for (const QJsonValue &value : doc.array())
            append(value);
        return objects;
    }
    if (!doc.isObject())
        return objects;

    const QJsonObject root = doc.object();
    for (const QString &key : {QStringLiteral("results"), QStringLiteral("pages")}) {
        const QJsonArray array = root.value(key).toArray();
        if (!array.isEmpty() && array.first().isObject()
            && (array.first().toObject().contains(QStringLiteral("page"))
                || array.first().toObject().contains(QStringLiteral("page_index"))
                || array.first().toObject().contains(QStringLiteral("blocks"))
                || array.first().toObject().contains(QStringLiteral("layout_blocks")))) {
            for (const QJsonValue &value : array)
                append(value);
            return objects;
        }
    }
    // 单页 OCR payload 直接导入为一页。
    objects.append(root);
    return objects;
}

} // namespace

bool readJson(const QString &path, int pageCount, QVector<ocr::PageResult> *results,
              QString *error)
{
    if (error)
        error->clear();
    if (!results)
        return false;
    results->clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取 JSON：%1").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("JSON 解析失败：%1（偏移 %2）")
                         .arg(parseError.errorString())
                         .arg(parseError.offset);
        return false;
    }

    const QVector<QJsonObject> objects = pageObjects(doc);
    if (objects.isEmpty()) {
        if (error)
            *error = QStringLiteral("JSON 里没有页面识别结果");
        return false;
    }

    // page_index 是本程序新格式的 0 起页码；page 兼容旧格式。
    bool hasPageIndex = false;
    bool anyZero = false;
    int maxPage = -1;
    bool anyPage = false;
    for (const QJsonObject &object : objects) {
        if (object.contains(QStringLiteral("page_index"))) {
            hasPageIndex = true;
            anyPage = true;
            maxPage = qMax(maxPage, object.value(QStringLiteral("page_index")).toInt(-1));
        } else if (object.contains(QStringLiteral("page"))) {
            anyPage = true;
            const int page = object.value(QStringLiteral("page")).toInt(-1);
            maxPage = qMax(maxPage, page);
            if (page == 0)
                anyZero = true;
        }
    }
    // 旧 CLI 导出用的是 1 起页码；GUI 导出一直用 0 起。
    const bool oneBased = anyPage && !hasPageIndex && !anyZero
                          && pageCount > 0 && maxPage == pageCount;

    QVector<ocr::PageResult> imported;
    imported.reserve(objects.size());
    int fallbackIndex = 0;
    for (const QJsonObject &object : objects) {
        if (!object.value(QStringLiteral("error")).toString().isEmpty())
            continue;

        int index = -1;
        if (object.contains(QStringLiteral("page_index")))
            index = object.value(QStringLiteral("page_index")).toInt(-1);
        else if (object.contains(QStringLiteral("page")))
            index = object.value(QStringLiteral("page")).toInt(-1) - (oneBased ? 1 : 0);
        if (index < 0 || (pageCount > 0 && index >= pageCount))
            index = fallbackIndex;
        ++fallbackIndex;
        if (index < 0 || (pageCount > 0 && index >= pageCount))
            continue;

        ocr::PageResult result;
        result.page = index;
        result.payload = object;
        result.purpose = object.value(QStringLiteral("purpose")).toString();
        result.blocks = ocr::blocksFromPayload(object, &result.purpose);
        if (result.purpose.isEmpty())
            result.purpose = QStringLiteral("rich_layout_recognition");
        result.elapsedMs = object.value(QStringLiteral("elapsed_ms")).toInteger(0);
        imported.append(result);
    }

    if (imported.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有可导入的页面（页码超出当前文档范围？）");
        return false;
    }

    std::stable_sort(imported.begin(), imported.end(),
                     [](const ocr::PageResult &a, const ocr::PageResult &b) {
                         return a.page < b.page;
                     });
    // 同一页出现多次时保留最后一条。
    QVector<ocr::PageResult> unique;
    for (const ocr::PageResult &result : imported) {
        if (!unique.isEmpty() && unique.last().page == result.page)
            unique.last() = result;
        else
            unique.append(result);
    }
    *results = unique;
    return true;
}

} // namespace exporters
