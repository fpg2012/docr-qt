// 识别结果导出/导入：Markdown / JSON。
#pragma once

#include <QString>
#include <QVector>

#include "ocr.h"

class Document;

namespace exporters {

bool writeMarkdown(const Document &doc, const QString &path, QString *error = nullptr);
bool writeJson(const Document &doc, const QString &path, QString *error = nullptr);

// 导入本程序导出的识别结果 JSON（也兼容 layout_blocks / pages 格式）。
// pageCount 用于把 1 起的旧 CLI 页码和 0 起的页码映射到当前文档。
bool readJson(const QString &path, int pageCount, QVector<ocr::PageResult> *results,
              QString *error = nullptr);

} // namespace exporters
