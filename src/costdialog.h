// 「估算全部 OCR 费用」对话框。
#pragma once

#include <QDialog>

#include "core/ocrcost.h"

class CostDialog : public QDialog {
    Q_OBJECT
public:
    CostDialog(const ocrcost::Estimate &estimate, const QString &purpose,
               QWidget *parent = nullptr);

    bool startRequested() const { return m_startRequested; }

private:
    bool m_startRequested = false;
};
