#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTimer>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("docr-qt"));
    QApplication::setOrganizationName(QStringLiteral("docr-qt"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("DeepSeek OCR 图形前端"));
    parser.addHelpOption();
    QCommandLineOption openOption({QStringLiteral("o"), QStringLiteral("open")},
                                  QStringLiteral("启动时打开 PDF / 图片 / 目录"), QStringLiteral("path"));
    QCommandLineOption ocrOption(QStringLiteral("ocr"), QStringLiteral("启动后立即识别全部页面"));
    QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                        QStringLiteral("识别完成后保存窗口截图并退出"),
                                        QStringLiteral("png"));
    QCommandLineOption mockOption(QStringLiteral("mock"),
                                  QStringLiteral("用已保存的 JSON 填入第 1 页（离线检查界面）"),
                                  QStringLiteral("json"));
    QCommandLineOption purposeOption(QStringLiteral("purpose"),
                                     QStringLiteral("rich_layout_recognition | text_recognition"),
                                     QStringLiteral("purpose"));
    parser.addOptions({openOption, ocrOption, screenshotOption, mockOption, purposeOption});
    parser.process(app);

    MainWindow window;
    window.show();

    if (parser.isSet(openOption))
        window.loadPath(parser.value(openOption));

    const QString screenshot = parser.value(screenshotOption);
    if (parser.isSet(mockOption)) {
        window.injectMockResult(parser.value(mockOption));
        if (!screenshot.isEmpty()) {
            QTimer::singleShot(600, &app, [&app, &window, screenshot] {
                window.grab().save(screenshot);
                QApplication::quit();
            });
        }
        return app.exec();
    }

    if (parser.isSet(ocrOption)) {
        if (!screenshot.isEmpty()) {
            QObject::connect(&window, &MainWindow::allFinished, &app,
                             [&app, &window, screenshot] {
                                 QTimer::singleShot(400, &app, [&app, &window, screenshot] {
                                     window.grab().save(screenshot);
                                     QApplication::quit();
                                 });
                             });
        }
        QTimer::singleShot(0, &window, &MainWindow::startOcrAll);
    }

    return app.exec();
}
