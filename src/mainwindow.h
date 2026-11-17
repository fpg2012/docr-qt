// 主窗口：页面列表、叠加视图、识别结果与设置。
#pragma once

#include <QList>
#include <QMainWindow>
#include <QPair>
#include <QQueue>
#include <QSet>

#include "document.h"
#include "ocr.h"

class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QTableWidget;
class QTimer;
class PageView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    bool loadPath(const QString &path);
    void startOcrAll();
    void startOcrCurrent();
    void stopOcr();
    void injectMockResult(const QString &jsonPath);

signals:
    void allFinished();
    void busyChanged(bool busy);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void buildUi();
    void buildActions();
    void applySettings();
    void saveSettings();
    void openSettings();

    void openFiles();
    void openDirectory();
    void resetDocument(const QString &title);
    void addPagesToUi();
    void showPage(int index);
    void refreshPageList(int index);
    void updatePanels();
    void updateStats();

    void enqueuePages(const QVector<int> &pages);
    void pumpQueue();
    void onResult(int page, int generation, const ocr::PageResult &result);
    void encodeAndSubmit(int page);
    void ensureThumbs();

    void exportMarkdown();
    void exportJson();
    void copyText();

    Document m_doc;
    ocr::Client m_client;

    QListWidget *m_pageList = nullptr;
    PageView *m_view = nullptr;
    QPlainTextEdit *m_textView = nullptr;
    QPlainTextEdit *m_jsonView = nullptr;
    QTableWidget *m_blockTable = nullptr;
    QComboBox *m_purposeBox = nullptr;
    QAction *m_stopAction = nullptr;
    QLabel *m_statsLabel = nullptr;
    QLabel *m_hintLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QTimer *m_thumbTimer = nullptr;
    QList<QPair<QAction *, QString>> m_themedActions;
    bool m_applyingTheme = false;
    void applyTheme();

    QQueue<int> m_pending;
    QSet<int> m_running;
    int m_generation = 0;
    int m_concurrency = 3;
    int m_dpi = 150;
    QString m_apiKey;
    QString m_baseUrl;
    QString m_purpose = QStringLiteral("rich_layout_recognition");
    bool m_allDoneEmitted = true;
};
