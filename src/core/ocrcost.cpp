#include "ocrcost.h"

#include "document.h"

namespace ocrcost {
namespace {

// DeepSeek OCR 官方人民币价：1.5 元/千页。
constexpr double kPageCny = 0.0015;

} // namespace

QString formatCny(double value)
{
    if (value < 0.01)
        return QString::number(value, 'f', 4);
    if (value < 1.0)
        return QString::number(value, 'f', 3);
    return QString::number(value, 'f', 2);
}

Estimate estimateRemaining(const Document &doc, const QString &purpose, bool billable)
{
    Estimate result;
    result.purpose = purpose;
    result.billable = billable;
    result.totalPages = doc.count();
    for (int i = 0; i < doc.count(); ++i) {
        if (doc.page(i).result.ok())
            ++result.donePages;
    }
    result.remainingPages = qMax(0, result.totalPages - result.donePages);
    if (billable) {
        result.doneFee = result.donePages * kPageCny;
        result.pageFee = result.remainingPages * kPageCny;
        result.total = result.doneFee + result.pageFee;
    }
    return result;
}

Estimate actual(const Document &doc, bool billable)
{
    Estimate result;
    result.billable = billable;
    result.totalPages = doc.count();
    for (int i = 0; i < doc.count(); ++i) {
        if (doc.page(i).result.ok())
            ++result.donePages;
    }
    result.remainingPages = qMax(0, result.totalPages - result.donePages);
    if (billable) {
        result.doneFee = result.donePages * kPageCny;
        result.total = result.doneFee;
    }
    return result;
}

} // namespace ocrcost
