// 从 pdf-outline 兼容的 JSON / YAML 导入大纲。
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "document.h"

namespace outlineio {

// page 字段按 1 起（pdf-outline 约定）；根节点可用 "pageBase": 0 改为 0 起。
// 支持 { "chd": [...] }、节点数组，以及每项的 "page" 或 "destination": {"page": n}。
bool loadFile(const QString &path, int pageCount, QVector<OutlineEntry> *entries,
              QString *error, QStringList *warnings = nullptr);

} // namespace outlineio
