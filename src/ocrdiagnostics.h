// 空 OCR 结果的诊断落盘：原始响应、上传图与元数据。
#pragma once

#include <QString>

class QImage;
struct PageEntry;
namespace ocr {
struct PageResult;
}

namespace ocrdiag {

// 返回保存路径前缀；失败返回空字符串。
QString saveEmptyResult(int page, const PageEntry &entry, const QImage &image,
                        const ocr::PageResult &result, const QString &baseUrl, int dpi);

} // namespace ocrdiag
