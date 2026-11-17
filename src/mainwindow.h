// 主窗口：页面列表、叠加视图、识别结果与设置。
#pragma once

#include <QList>
#include <QMainWindow>
#include <QPair>
#include <QSet>
#include <QVector>

#include "core/document.h"
#include "core/ocrqueue.h"

class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QSpinBox;
class QTabWidget;
class QTimer;
class BlockCardList;
class PageView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    bool loadPath(const QString &path);
    // 打开一组路径（PDF / 图片 / 图片目录，可混合），替换当前文档。
    void openPaths(const QStringList &paths);
    void startOcrAll();
    void startOcrCurrent();
    void stopOcr();
    void injectMockResult(const QString &jsonPath);
    // 调试入口：选中当前页的若干版面块。
    void selectBlocks(const QVector<int> &blocks);
    void selectTextRange(int begin, int end);
    void showBlocksTab();
    // 调试入口：强制显示拖入提示框。
    void setDropHintVisible(bool on);
    void goToPage(int index);

signals:
    void allFinished();
    void busyChanged(bool busy);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void buildActions();
    void applySettings();
    void saveSettings();

    void openFiles();
    void openPreferences();
    void openDirectory();
    void addPagesToUi();
    void showPage(int index);
    void stepPage(int delta);
    void updatePageNav();
    void updatePanels();
    void updateStats();
    void refreshPageList(int index);

    void onPageQueued(int page);
    void onPageStarted(int page);
    void onPageRetrying(int page);
    void onPageFinished(int page, const ocr::PageResult &result);
    void ensureThumbs();

    void exportMarkdown();
    void exportJson();
    void copyText();
    void copySelection();
    void showSelectionStatus();

    void syncCardsFromView();
    void syncViewFromCards();
    void setTextTool(bool on);
    void showDropHint(bool on);

    Document m_doc;
    OcrQueue m_queue;

    QListWidget *m_pageList = nullptr;
    PageView *m_view = nullptr;
    QPlainTextEdit *m_textView = nullptr;
    QPlainTextEdit *m_jsonView = nullptr;
    QTabWidget *m_tabs = nullptr;
    BlockCardList *m_blocks = nullptr;
    QComboBox *m_purposeBox = nullptr;
    QSpinBox *m_pageSpin = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_textToolAction = nullptr;
    QLabel *m_statsLabel = nullptr;
    QLabel *m_hintLabel = nullptr;
    QLabel *m_selLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QTimer *m_thumbTimer = nullptr;
    QImage m_pageImage;
    QList<QPair<QAction *, QString>> m_themedActions;
    bool m_applyingTheme = false;
    void applyTheme();

    QSet<int> m_retriedPages;
    int m_concurrency = 3;
    int m_dpi = 150;
    QString m_apiKey;
    QString m_baseUrl;
    QString m_purpose = QStringLiteral("rich_layout_recognition");
    bool m_showBoxes = true;
    bool m_batchMode = false;
    bool m_syncing = false;
    bool m_dropHint = false;
};
