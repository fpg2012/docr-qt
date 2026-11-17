#include "tocparser.h"

#include <QRegularExpression>

namespace toc {
namespace {

QString normalize(const QString &line)
{
    QString text = line;
    text.replace(QChar(0x00a0), QLatin1Char(' '));
    text.replace(QChar(0x3000), QLatin1Char(' '));
    return text.simplified();
}

bool isNoise(const QString &title)
{
    const QString key = title.toLower().simplified();
    static const QStringList noise = {
        QStringLiteral("目录"), QStringLiteral("目 录"), QStringLiteral("contents"),
        QStringLiteral("table of contents"), QStringLiteral("章节"),
        QStringLiteral("章节 页码"), QStringLiteral("标题 页码"),
        QStringLiteral("chapter"), QStringLiteral("chapter page"),
        QStringLiteral("页码"), QStringLiteral("page"),
    };
    return noise.contains(key);
}

QString cleanTitle(QString title)
{
    title = title.trimmed();
    while (!title.isEmpty()) {
        const QChar last = title.back();
        if (last.isSpace() || last == QLatin1Char('.') || last == QChar(0x00b7)
            || last == QChar(0x2026) || last == QLatin1Char('-')
            || last == QLatin1Char('_') || last == QChar(0x2014)) {
            title.chop(1);
        } else {
            break;
        }
    }
    return title.trimmed();
}

int levelFor(const QString &title)
{
    static const QRegularExpression numeric(
        QStringLiteral(R"(^(\d+(?:\.\d+)*)[\s、.．)）])"));
    const QRegularExpressionMatch match = numeric.match(title);
    if (match.hasMatch()) {
        const QString number = match.captured(1);
        const int dots = number.count(QLatin1Char('.'));
        return qBound(1, dots + 1, 4);
    }
    if (title.startsWith(QStringLiteral("第"))) {
        if (title.contains(QStringLiteral("节")))
            return 2;
        if (title.contains(QStringLiteral("章")) || title.contains(QStringLiteral("篇"))
            || title.contains(QStringLiteral("部")) || title.contains(QStringLiteral("编")))
            return 1;
    }
    if (title.startsWith(QStringLiteral("附录")) || title.startsWith(QStringLiteral("后记"))
        || title.startsWith(QStringLiteral("参考文献")) || title.startsWith(QStringLiteral("索引")))
        return 1;
    return 1;
}

bool splitEntry(const QString &line, QString *title, int *page)
{
    static const QRegularExpression pipe(
        QStringLiteral(R"(^(.+?)\s*[|｜]\s*(\d{1,4})\s*$)"));
    static const QRegularExpression leaders(
        QStringLiteral(R"(^(.+?)[\s.·…．。•]{2,}(\d{1,4})\s*$)"));
    static const QRegularExpression spaced(
        QStringLiteral(R"(^(.{2,}?)\s+(\d{1,4})\s*$)"));
    static const QRegularExpression adjacent(
        QStringLiteral(R"(^(.+[\p{Han}）\)】\]”"])(\d{1,4})\s*$)"));

    for (const QRegularExpression &re : {pipe, leaders, spaced, adjacent}) {
        const QRegularExpressionMatch match = re.match(line);
        if (!match.hasMatch())
            continue;
        const QString candidate = cleanTitle(match.captured(1));
        bool ok = false;
        const int number = match.captured(2).toInt(&ok);
        if (!ok || number <= 0 || candidate.isEmpty() || isNoise(candidate))
            continue;
        if (candidate.at(0).isDigit() && candidate.toInt(&ok) && ok)
            continue;
        *title = candidate;
        *page = number;
        return true;
    }
    return false;
}

bool onlyNumber(const QString &line, int *page)
{
    static const QRegularExpression re(QStringLiteral(R"(^\s*(\d{1,4})\s*$)"));
    const QRegularExpressionMatch match = re.match(line);
    if (!match.hasMatch())
        return false;
    bool ok = false;
    const int number = match.captured(1).toInt(&ok);
    if (!ok || number <= 0)
        return false;
    *page = number;
    return true;
}

} // namespace

QVector<Entry> parseLines(const QStringList &lines)
{
    QVector<Entry> entries;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = normalize(lines.at(i));
        if (line.isEmpty())
            continue;

        QString title;
        int page = -1;
        if (splitEntry(line, &title, &page)) {
            // 同一页眉/页脚重复出现时去重。
            bool duplicate = false;
            for (const Entry &existing : entries) {
                if (existing.title == title && existing.printedPage == page) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                entries.append({title, page, levelFor(title)});
            continue;
        }

        // 标题较长折行、页码单独占一行。
        if (line.size() >= 2 && !isNoise(line) && i + 1 < lines.size()) {
            int nextPage = -1;
            if (onlyNumber(normalize(lines.at(i + 1)), &nextPage)) {
                entries.append({cleanTitle(line), nextPage, levelFor(line)});
                ++i;
            }
        }
    }
    return entries;
}

} // namespace toc
