#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QPainter>
#include <QPalette>
#include <QSvgRenderer>

namespace theme {

bool isDark()
{
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

QColor tone(const QColor &color)
{
    return isDark() ? color.lighter(130) : color.darker(112);
}

QIcon icon(const QString &name)
{
    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QString svg = QString::fromUtf8(file.readAll());
    const QColor ink = QApplication::palette().color(QPalette::WindowText);
    svg.replace(QLatin1String("currentColor"), ink.name(QColor::HexRgb));

    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid())
        return {};

    QIcon result;
    for (const int dpr : {1, 2, 3}) {
        const int pixels = 22 * dpr;
        QPixmap pixmap(pixels, pixels);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter, QRectF(0, 0, pixels, pixels));
        painter.end();
        pixmap.setDevicePixelRatio(dpr);
        result.addPixmap(pixmap);
    }
    return result;
}

QIcon appIcon()
{
    return QIcon(QStringLiteral(":/icons/app.svg"));
}

QString styleSheet(const QPalette &palette)
{
    const QColor separator = palette.color(QPalette::Mid);
    const QColor accent = palette.color(QPalette::Highlight);

    auto rgba = [](const QColor &color, int alpha) {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(alpha);
    };

    return QStringLiteral(R"(
QToolBar { border: none; padding: 4px 6px; spacing: 2px; }
QToolBar::separator { background: %1; width: 1px; margin: 5px 7px; }
QToolButton { border: none; border-radius: 6px; padding: 4px; }
QToolButton:hover { background: %2; }
QToolButton:pressed, QToolButton:checked { background: %3; }
QMenuBar::item { padding: 4px 9px; border-radius: 5px; }
QMenuBar::item:selected { background: %2; }
QComboBox { padding: 3px 8px; border: 1px solid %1; border-radius: 6px; }
QTabBar::tab { padding: 6px 12px; }
)")
        .arg(separator.name(), rgba(accent, 32), rgba(accent, 64));
}

} // namespace theme
