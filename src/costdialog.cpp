#include "costdialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QVBoxLayout>

CostDialog::CostDialog(const ocrcost::Estimate &estimate, const QString &purpose,
                       QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("OCR 费用估算"));
    resize(500, 0);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("<h3>全部 OCR 费用估算</h3>"), this));

    const QString mode = purpose == QStringLiteral("text_recognition")
                             ? QStringLiteral("纯文本")
                             : QStringLiteral("版面分析");
    QString body = QStringLiteral(
                       "<p>文档共 <b>%1</b> 页，已完成 <b>%2</b> 页，"
                       "待识别 <b>%3</b> 页（%4）。</p>")
                       .arg(estimate.totalPages)
                       .arg(estimate.donePages)
                       .arg(estimate.remainingPages)
                       .arg(mode);

    if (!estimate.billable) {
        body += QStringLiteral(
            "<p>当前使用本地 <b>GLM-OCR（vLLM）</b>，不产生 DeepSeek OCR API 页费。"
            "本地推理的 GPU/电费由部署方承担，这里不计入。</p>");
    } else if (estimate.remainingPages == 0) {
        body += QStringLiteral("<p>所有页面都已识别，没有待估算的页。"
                               "已产生 OCR 页费约 <b>¥%1</b>。</p>")
                    .arg(ocrcost::formatCny(estimate.total));
    } else {
        body += QStringLiteral(
                    "<p>OCR API 按页计费，当前价格为 <b>1.5 元/千页</b>，"
                    "与返回 token 数无关。</p>"
                    "<table cellspacing='0' cellpadding='4'>"
                    "<tr><td>待识别页数</td><td align='right'>%1 页</td></tr>")
                    .arg(estimate.remainingPages);
        if (estimate.donePages > 0) {
            body += QStringLiteral(
                        "<tr><td>已完成页费</td><td align='right'>¥%1</td></tr>"
                        "<tr><td>待识别页费</td><td align='right'>¥%2</td></tr>")
                        .arg(ocrcost::formatCny(estimate.doneFee),
                             ocrcost::formatCny(estimate.pageFee));
        } else {
            body += QStringLiteral("<tr><td>OCR 页费</td><td align='right'>¥%1</td></tr>")
                        .arg(ocrcost::formatCny(estimate.pageFee));
        }
        body += QStringLiteral("<tr><td><b>合计</b></td><td align='right'><b>≈¥%1</b></td></tr>"
                               "</table>")
                    .arg(ocrcost::formatCny(estimate.total));
    }

    auto *details = new QLabel(body, this);
    details->setTextFormat(Qt::RichText);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->setMinimumWidth(460);
    layout->addWidget(details);

    auto *note = new QLabel(
        estimate.billable
            ? QStringLiteral("只估算 OCR API 的页费。识别失败通常不计页费，重新识别会再次计费。"
                             "最终以 DeepSeek 账单为准。")
            : QStringLiteral("本地 GLM-OCR 不产生 OCR API 页费；"
                             "实际成本取决于本机推理资源。"),
        this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(this);
    auto *close = buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
    if (estimate.remainingPages > 0) {
        auto *start = buttons->addButton(QStringLiteral("开始识别"),
                                         QDialogButtonBox::AcceptRole);
        connect(start, &QPushButton::clicked, this, [this] {
            m_startRequested = true;
            accept();
        });
    }
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttons);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    adjustSize();
    resize(qMax(500, sizeHint().width()), sizeHint().height());
}
