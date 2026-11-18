// PDF 文本层的 Unicode 归一化。
#pragma once

#include <QString>

namespace unicodenormalize {

// NFC，并把 CJK 部首/笔画映射到等价的统一汉字；保留中文全角标点。
QString normalizeForPdf(const QString &text);

} // namespace unicodenormalize
