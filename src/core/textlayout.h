// 把 OCR 文本排进页面图像中的实际文字行，供文本选择层和可搜索 PDF 共用。
#pragma once

#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

#include "ocr.h"

namespace textlayout {

// 根据页面图像检测 region（0-1000 归一化坐标）内的文字行框。
QVector<QRectF> detectLines(const QImage &pageImage, const QRectF &region);

// 为一段块文本生成逐行框和字符区间。没有页面图像时退化为按行数均分。
QVector<ocr::TextLine> place(const QString &text, const QRectF &region,
                             const QImage &pageImage, const QString &kind);

} // namespace textlayout
