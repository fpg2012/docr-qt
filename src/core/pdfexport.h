// 导出可搜索 PDF：在原始页面图像/内容上叠加不可见 OCR 文本层，并写入大纲。
#pragma once

#include <QString>
#include <QVector>

#include <functional>

#include "document.h"

namespace pdfexport {

// 每处理完一页调用一次；返回 false 可取消导出。done 从 0 开始，total 为总页数。
using Progress = std::function<bool(int done, int total)>;

// outline 使用拼接文档的全局页下标；page < 0 的节点只作为分组，不写目标页。
// 输出会重写整个 PDF（qpdf 不支持增量写入），但不重新栅格化原始 PDF 页面。
bool writeSearchablePdf(Document &doc, const QVector<OutlineEntry> &outline,
                        const QString &path, QString *error = nullptr,
                        const Progress &progress = {});

} // namespace pdfexport
