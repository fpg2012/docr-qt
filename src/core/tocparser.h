// 从 OCR 出来的目录页文本里提取「标题 + 页码」条目。
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace toc {

struct Entry {
    QString title;
    int printedPage = -1; // 目录上印的页码
    int level = 1;        // 1 起，按“第 X 章 / 1.2”粗略分级
};

// lines 应当是去掉页眉、页脚和页码块后的目录正文行。
QVector<Entry> parseLines(const QStringList &lines);

} // namespace toc
