// 识别结果导出：Markdown / JSON。
#pragma once

#include <QString>

class Document;

namespace exporters {

bool writeMarkdown(const Document &doc, const QString &path, QString *error = nullptr);
bool writeJson(const Document &doc, const QString &path, QString *error = nullptr);

} // namespace exporters
