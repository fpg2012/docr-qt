#include "exporters.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

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
        pages.append(wrapper);
    }
    file.write(QJsonDocument(pages).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace exporters
