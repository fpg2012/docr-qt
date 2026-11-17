#include "outlineio.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <yaml-cpp/yaml.h>

namespace outlineio {
namespace {

struct Context {
    int pageCount = 0;
    int pageBase = 1;
    QStringList warnings;
};

int pageValue(const QJsonObject &object, Context &context, bool *present)
{
    *present = false;
    int raw = 0;
    if (object.value(QStringLiteral("page")).isDouble()) {
        raw = object.value(QStringLiteral("page")).toInt();
        *present = true;
    } else {
        const QJsonObject destination = object.value(QStringLiteral("destination")).toObject();
        if (destination.value(QStringLiteral("page")).isDouble()) {
            raw = destination.value(QStringLiteral("page")).toInt();
            *present = true;
        }
    }
    if (!*present)
        return -1;
    if (raw == -1 || raw == 0)
        return raw == -1 ? -1 : (context.pageBase == 0 ? 0 : -1);
    const int page = raw - context.pageBase;
    if (page < 0 || page >= context.pageCount) {
        context.warnings.append(
            QStringLiteral("页码 %1 超出 1-%2，已设为不可跳转")
                .arg(raw)
                .arg(context.pageCount));
        return -1;
    }
    return page;
}

void parseJsonNode(const QJsonObject &object, Context &context,
                   QVector<OutlineEntry> *out)
{
    OutlineEntry entry;
    entry.title = object.value(QStringLiteral("title")).toString().trimmed();
    if (entry.title.isEmpty())
        return;
    bool present = false;
    const int page = pageValue(object, context, &present);
    entry.page = present ? page : -1;

    const QJsonArray children = object.contains(QStringLiteral("chd"))
                                    ? object.value(QStringLiteral("chd")).toArray()
                                    : object.value(QStringLiteral("children")).toArray();
    for (const QJsonValue &value : children) {
        if (value.isObject())
            parseJsonNode(value.toObject(), context, &entry.children);
    }
    out->append(entry);
}

bool loadJson(const QByteArray &data, Context &context, QVector<OutlineEntry> *entries,
              QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("JSON 解析失败：%1（偏移 %2）")
                     .arg(parseError.errorString())
                     .arg(parseError.offset);
        return false;
    }

    const QJsonValue root = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
    if (doc.isObject()) {
        const QJsonObject object = doc.object();
        if (object.value(QStringLiteral("pageBase")).isDouble())
            context.pageBase = object.value(QStringLiteral("pageBase")).toInt() == 0 ? 0 : 1;
        if (object.contains(QStringLiteral("chd"))) {
            for (const QJsonValue &value : object.value(QStringLiteral("chd")).toArray()) {
                if (value.isObject())
                    parseJsonNode(value.toObject(), context, entries);
            }
        } else if (object.contains(QStringLiteral("title"))) {
            parseJsonNode(object, context, entries);
        } else if (object.contains(QStringLiteral("children"))) {
            for (const QJsonValue &value : object.value(QStringLiteral("children")).toArray()) {
                if (value.isObject())
                    parseJsonNode(value.toObject(), context, entries);
            }
        }
    } else if (root.isArray()) {
        for (const QJsonValue &value : root.toArray()) {
            if (value.isObject())
                parseJsonNode(value.toObject(), context, entries);
        }
    }
    if (entries->isEmpty()) {
        *error = QStringLiteral("没有找到大纲条目（需要 title / page / chd）");
        return false;
    }
    return true;
}

int yamlPage(const YAML::Node &node, Context &context, bool *present)
{
    *present = false;
    int raw = 0;
    try {
        if (node["page"] && node["page"].IsScalar()) {
            raw = node["page"].as<int>();
            *present = true;
        } else if (node["destination"] && node["destination"].IsMap()
                   && node["destination"]["page"]
                   && node["destination"]["page"].IsScalar()) {
            raw = node["destination"]["page"].as<int>();
            *present = true;
        }
    } catch (const YAML::Exception &) {
        return -1;
    }
    if (!*present)
        return -1;
    if (raw == -1 || raw == 0)
        return raw == -1 ? -1 : (context.pageBase == 0 ? 0 : -1);
    const int page = raw - context.pageBase;
    if (page < 0 || page >= context.pageCount) {
        context.warnings.append(
            QStringLiteral("页码 %1 超出 1-%2，已设为不可跳转")
                .arg(raw)
                .arg(context.pageCount));
        return -1;
    }
    return page;
}

void parseYamlNode(const YAML::Node &node, Context &context, QVector<OutlineEntry> *out)
{
    if (!node.IsMap())
        return;
    OutlineEntry entry;
    try {
        if (node["title"])
            entry.title = QString::fromStdString(node["title"].as<std::string>()).trimmed();
    } catch (const YAML::Exception &) {
        return;
    }
    if (entry.title.isEmpty())
        return;
    bool present = false;
    const int page = yamlPage(node, context, &present);
    entry.page = present ? page : -1;

    const YAML::Node children = node["chd"] ? node["chd"] : node["children"];
    if (children && children.IsSequence()) {
        for (const YAML::Node &child : children)
            parseYamlNode(child, context, &entry.children);
    }
    out->append(entry);
}

bool loadYaml(const QByteArray &data, Context &context, QVector<OutlineEntry> *entries,
              QString *error)
{
    try {
        const YAML::Node root = YAML::Load(std::string(data.constData(), data.size()));
        if (!root)
            return false;
        if (root.IsMap() && root["pageBase"]
            && root["pageBase"].as<int>() == 0)
            context.pageBase = 0;
        if (root.IsSequence()) {
            for (const YAML::Node &node : root)
                parseYamlNode(node, context, entries);
        } else if (root.IsMap() && root["chd"] && root["chd"].IsSequence()) {
            for (const YAML::Node &node : root["chd"])
                parseYamlNode(node, context, entries);
        } else if (root.IsMap() && root["children"] && root["children"].IsSequence()) {
            for (const YAML::Node &node : root["children"])
                parseYamlNode(node, context, entries);
        } else {
            parseYamlNode(root, context, entries);
        }
    } catch (const YAML::Exception &e) {
        *error = QStringLiteral("YAML 解析失败：%1").arg(QString::fromStdString(e.what()));
        return false;
    }
    if (entries->isEmpty()) {
        *error = QStringLiteral("没有找到大纲条目（需要 title / page / chd）");
        return false;
    }
    return true;
}

} // namespace

bool loadFile(const QString &path, int pageCount, QVector<OutlineEntry> *entries,
              QString *error, QStringList *warnings)
{
    entries->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取：%1").arg(path);
        return false;
    }
    const QByteArray data = file.readAll();
    Context context;
    context.pageCount = pageCount;

    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool yaml = suffix == QStringLiteral("yaml") || suffix == QStringLiteral("yml");
    const bool ok = yaml ? loadYaml(data, context, entries, error)
                         : loadJson(data, context, entries, error);
    if (warnings)
        *warnings = context.warnings;
    return ok;
}

} // namespace outlineio
