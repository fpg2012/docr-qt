// OCR 费用估算。OCR API 按页计费（1.5 元/千页），不按模型 token 计费。
#pragma once

#include <QString>

class Document;

namespace ocrcost {

struct Estimate {
    int totalPages = 0;
    int donePages = 0;
    int remainingPages = 0;
    double doneFee = 0.0;  // 已完成页的 OCR 页费
    double pageFee = 0.0;  // 待识别页的 OCR 页费
    double total = 0.0;    // doneFee + pageFee
    QString purpose;
    bool billable = true;  // 本地 GLM-OCR 为 false
};

QString formatCny(double value);

// 估算尚未识别的页面的 OCR 页费。
Estimate estimateRemaining(const Document &doc, const QString &purpose,
                           bool billable = true);

// 当前文档已经识别页面对应的 OCR 页费。
Estimate actual(const Document &doc, bool billable = true);

} // namespace ocrcost
