#include <QApplication>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QStyleHints>
#include <QTimer>

#include "mainwindow.h"
#include "theme.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("docr-qt"));
    QApplication::setDesktopFileName(QStringLiteral("docr-qt"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
    QApplication::setOrganizationName(QStringLiteral("docr-qt"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("DeepSeek OCR 图形前端"));
    parser.addHelpOption();
    QCommandLineOption openOption({QStringLiteral("o"), QStringLiteral("open")},
                                  QStringLiteral("启动时打开 PDF / 图片 / 目录（可重复）"),
                                  QStringLiteral("path"));
    QCommandLineOption ocrOption(QStringLiteral("ocr"), QStringLiteral("启动后立即识别全部页面"));
    QCommandLineOption ocrCurrentOption(QStringLiteral("ocr-current"),
                                        QStringLiteral("启动后识别当前页"));
    QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                        QStringLiteral("识别完成后保存窗口截图并退出"),
                                        QStringLiteral("png"));
    QCommandLineOption mockOption(QStringLiteral("mock"),
                                  QStringLiteral("用已保存的 JSON 填入第 1 页（离线检查界面）"),
                                  QStringLiteral("json"));
    QCommandLineOption purposeOption(QStringLiteral("purpose"),
                                     QStringLiteral("rich_layout_recognition | text_recognition"),
                                     QStringLiteral("purpose"));
    QCommandLineOption outlineOption(QStringLiteral("outline"),
                                     QStringLiteral("启动时导入 JSON/YAML 大纲"),
                                     QStringLiteral("path"));
    QCommandLineOption selectOption(QStringLiteral("select"),
                                    QStringLiteral("调试：选中第 1 页的指定版面块（逗号分隔，隐含切到「版面块」页）"),
                                    QStringLiteral("indices"));
    QCommandLineOption textSelectOption(
        QStringLiteral("text-select"),
        QStringLiteral("调试：按字符偏移选中文本（begin,end）"), QStringLiteral("range"));
    QCommandLineOption dropHintOption(QStringLiteral("drop-hint"),
                                      QStringLiteral("调试：显示拖入提示框"));
    QCommandLineOption pageOption(QStringLiteral("page"),
                                  QStringLiteral("调试：启动后跳到指定页（1 起）"),
                                  QStringLiteral("number"));
    QCommandLineOption darkOption(QStringLiteral("dark"), QStringLiteral("强制深色方案（调试用）"));
    QCommandLineOption lightOption(QStringLiteral("light"), QStringLiteral("强制浅色方案（调试用）"));
    parser.addOptions({openOption, ocrOption, ocrCurrentOption, screenshotOption, mockOption, purposeOption,
                       selectOption, textSelectOption, dropHintOption, outlineOption,
                       pageOption, darkOption, lightOption});
    parser.process(app);

    if (parser.isSet(darkOption))
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    else if (parser.isSet(lightOption))
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);

    app.setStyleSheet(theme::styleSheet(app.palette()));

    MainWindow window;
    window.show();

    const QStringList openPaths = parser.values(openOption);
    if (!openPaths.isEmpty())
        window.openPaths(openPaths);

    if (parser.isSet(mockOption))
        window.injectMockResult(parser.value(mockOption));
    if (parser.isSet(outlineOption))
        window.importOutlineFile(parser.value(outlineOption));

    if (parser.isSet(pageOption)) {
        bool ok = false;
        const int page = parser.value(pageOption).toInt(&ok);
        if (ok) {
            QTimer::singleShot(0, &window, [&window, page] { window.goToPage(page - 1); });
        }
    }

    if (parser.isSet(selectOption)) {
        QVector<int> blocks;
        for (const QString &token : parser.value(selectOption).split(QLatin1Char(','),
                                                                    Qt::SkipEmptyParts)) {
            bool ok = false;
            const int index = token.trimmed().toInt(&ok);
            if (ok && index >= 0)
                blocks.append(index);
        }
        window.selectBlocks(blocks);
    }
    if (parser.isSet(textSelectOption)) {
        const QStringList parts = parser.value(textSelectOption).split(QLatin1Char(','));
        if (parts.size() == 2) {
            bool ok1 = false, ok2 = false;
            const int begin = parts.at(0).toInt(&ok1);
            const int end = parts.at(1).toInt(&ok2);
            if (ok1 && ok2)
                window.selectTextRange(begin, end);
        }
    }
    if (parser.isSet(dropHintOption))
        window.setDropHintVisible(true);

    const QString screenshot = parser.value(screenshotOption);
    auto scheduleShot = [&app, &window, screenshot](int delay) {
        QTimer::singleShot(delay, &app, [&app, &window, screenshot] {
            window.grab().save(screenshot);
            QApplication::quit();
        });
    };

    if (parser.isSet(ocrOption)) {
        if (!screenshot.isEmpty()) {
            QObject::connect(&window, &MainWindow::allFinished, &app,
                             [scheduleShot] { scheduleShot(400); });
        }
        QTimer::singleShot(0, &window, &MainWindow::startOcrAll);
    } else if (parser.isSet(ocrCurrentOption)) {
        if (!screenshot.isEmpty()) {
            QObject::connect(&window, &MainWindow::allFinished, &app,
                             [scheduleShot] { scheduleShot(400); });
        }
        QTimer::singleShot(0, &window, &MainWindow::startOcrCurrent);
    } else if (!screenshot.isEmpty()) {
        scheduleShot(600);
    }

    return app.exec();
}
