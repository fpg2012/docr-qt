#include "ocrdiagnostics.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include "document.h"
#include "ocr.h"

namespace ocrdiag {

QString saveEmptyResult(int page, const PageEntry &entry, const QImage &image,
                        const ocr::PageResult &result, const QString &baseUrl, int dpi)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                        + QStringLiteral("/docr-qt/ocr-empty");
    if (!QDir().mkpath(dir))
        return {};

    const QString stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = QStringLiteral("%1/page-%2-%3").arg(dir).arg(page + 1).arg(stamp);

    QJsonObject meta;
    meta.insert(QStringLiteral("page"), page + 1);
    meta.insert(QStringLiteral("source"), entry.label);
    meta.insert(QStringLiteral("purpose"), result.purpose);
    meta.insert(QStringLiteral("baseUrl"), baseUrl);
    meta.insert(QStringLiteral("dpi"), dpi);
    meta.insert(QStringLiteral("elapsedMs"), double(result.elapsedMs));
    meta.insert(QStringLiteral("usage"), result.usage());
    meta.insert(QStringLiteral("error"), result.error);
    const QJsonArray pages = result.payload.value(QStringLiteral("pages")).toArray();
    if (!pages.isEmpty()) {
        meta.insert(QStringLiteral("finish_reason"),
                    pages.first().toObject().value(QStringLiteral("finish_reason")));
    }

    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("meta"), meta);
    wrapper.insert(QStringLiteral("response"), result.payload);
    QFile json(base + QStringLiteral(".json"));
    if (json.open(QIODevice::WriteOnly))
        json.write(QJsonDocument(wrapper).toJson(QJsonDocument::Indented));

    QFile rawFile(base + QStringLiteral(".response.json"));
    if (rawFile.open(QIODevice::WriteOnly) && !result.raw.isEmpty())
        rawFile.write(result.raw);

    if (!image.isNull())
        image.save(base + QStringLiteral(".png"), "PNG");

    return base;
}

} // namespace ocrdiag
