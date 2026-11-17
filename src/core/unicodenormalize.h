// PDF 文本层的 Unicode 归一化。
#pragma once

#include <QString>

namespace unicodenormalize {

// NFKC，并把 CJK 部首/笔画映射到等价的统一汉字。
QString normalizeForPdf(const QString &text);

} // namespace unicodenormalize
