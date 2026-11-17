#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QProgressDialog>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QRegularExpression>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QHeaderView>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QAbstractItemView>
#include <QPalette>

#include "blockcards.h"
#include "costdialog.h"
#include "core/exporters.h"
#include "core/ocrdiagnostics.h"
#include "core/ocrcost.h"
#include "core/pdfexport.h"
#include "core/outlineio.h"
#include "pageview.h"
#include "preferences.h"
#include "theme.h"

namespace {

const char *kImageFilter =
    "图片 (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.tif *.tiff)";

constexpr int kOutlinePageRole = Qt::UserRole + 1;

void appendOutlineNode(QVector<OutlineEntry> &roots,
                       QVector<QPair<int, QVector<int>>> &stack,
                       const OutlineEntry &node, int level)
{
    while (!stack.isEmpty() && stack.last().first >= level)
        stack.removeLast();
    if (stack.isEmpty()) {
        roots.append(node);
        stack.append({level, QVector<int>{int(roots.size() - 1)}});
        return;
    }
    QVector<int> path = stack.last().second;
    QVector<OutlineEntry> *parent = &roots;
    for (int index : path)
        parent = &(*parent)[index].children;
    parent->append(node);
    path.append(parent->size() - 1);
    stack.append({level, path});
}

QStringList tocLinesFromResult(const ocr::PageResult &result)
{
    QStringList lines;
    if (!result.ok())
        return lines;
    if (!result.blocks.isEmpty()) {
        for (const ocr::Block &block : result.blocks) {
            if (block.kind.contains(QStringLiteral("page_number"))
                || block.kind == QLatin1String("header")
                || block.kind == QLatin1String("footer")
                || block.kind.contains(QStringLiteral("page_footer"))) {
                continue;
            }
            const QString text = ocr::displayText(block.text);
            for (const QString &line : text.split(QLatin1Char('\n')))
                lines.append(line);
        }
    } else {
        const QString text = ocr::plainText(result.payload, result.purpose);
        for (const QString &line : text.split(QLatin1Char('\n')))
            lines.append(line);
    }
    return lines;
}

bool hasLocalFiles(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls())
        return false;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile())
            return true;
    }
    return false;
}

QStringList localPaths(const QMimeData *mime)
{
    QStringList paths;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    return paths;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_queue(&m_doc)
{
    buildUi();
    buildActions();
    applySettings();
    updatePageNav();
    updatePanels();
    updateStats();

    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setInterval(8);
    connect(m_thumbTimer, &QTimer::timeout, this, &MainWindow::ensureThumbs);

    connect(&m_queue, &OcrQueue::pageQueued, this, &MainWindow::onPageQueued);
    connect(&m_queue, &OcrQueue::pageStarted, this, &MainWindow::onPageStarted);
    connect(&m_queue, &OcrQueue::pageRetrying, this, &MainWindow::onPageRetrying);
    connect(&m_queue, &OcrQueue::pageFinished, this, &MainWindow::onPageFinished);
    connect(&m_queue, &OcrQueue::busyChanged, this, [this](bool) { updateStats(); });
    connect(&m_queue, &OcrQueue::busyChanged, this, &MainWindow::busyChanged);
    connect(&m_queue, &OcrQueue::allFinished, this, [this] {
        if (m_stopAction)
            m_stopAction->setEnabled(false);
        if (m_tocJob.active)
            parseTocPages();
        updateStats();
        emit allFinished();
    });

    setAcceptDrops(true);
    qApp->installEventFilter(this);
    resize(1480, 940);
    setWindowIcon(theme::appIcon());
    setWindowTitle(QStringLiteral("docr-qt"));
}

void MainWindow::buildUi()
{
    m_view = new PageView(this);
    setCentralWidget(m_view);

    // 左侧：页面缩略图 + 大纲
    auto *leftDock = new QDockWidget(QStringLiteral("导航"), this);
    leftDock->setObjectName(QStringLiteral("pagesDock"));
    m_leftTabs = new QTabWidget(leftDock);
    m_pageList = new QListWidget(m_leftTabs);
    m_pageList->setViewMode(QListView::IconMode);
    m_pageList->setIconSize(QSize(120, 160));
    m_pageList->setGridSize(QSize(150, 205));
    m_pageList->setResizeMode(QListView::Adjust);
    m_pageList->setMovement(QListView::Static);
    m_pageList->setUniformItemSizes(true);
    m_pageList->setWordWrap(true);
    m_pageList->setSpacing(4);
    m_leftTabs->addTab(m_pageList, QStringLiteral("页面"));

    auto *outlineTab = new QWidget(m_leftTabs);
    auto *outlineLayout = new QVBoxLayout(outlineTab);
    outlineLayout->setContentsMargins(4, 4, 4, 4);
    outlineLayout->setSpacing(4);
    m_outlineTree = new QTreeWidget(outlineTab);
    m_outlineTree->setHeaderHidden(true);
    m_outlineTree->setIndentation(14);
    m_outlineTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_outlineTree->setToolTip(QStringLiteral("点击大纲条目跳转到对应页"));
    outlineLayout->addWidget(m_outlineTree, 1);
    m_replaceOutline = new QCheckBox(QStringLiteral("用自定义目录替换内置大纲"), outlineTab);
    m_replaceOutline->setToolTip(
        QStringLiteral("同时存在内置书签和 OCR/导入目录时生效；勾选后隐藏内置书签"));
    outlineLayout->addWidget(m_replaceOutline);
    m_tocButton = new QPushButton(QStringLiteral("从目录页生成…"), outlineTab);
    m_tocButton->setToolTip(
        QStringLiteral("OCR 目录页，解析「标题 + 页码」，再映射到 PDF 实际页"));
    outlineLayout->addWidget(m_tocButton);
    m_importOutlineButton = new QPushButton(QStringLiteral("导入 JSON / YAML…"), outlineTab);
    m_importOutlineButton->setToolTip(
        QStringLiteral("导入 pdf-outline 兼容的 JSON/YAML，替换当前大纲"));
    outlineLayout->addWidget(m_importOutlineButton);
    m_leftTabs->addTab(outlineTab, QStringLiteral("大纲"));
    connect(m_leftTabs, &QTabWidget::currentChanged, this, [this](int) {
        if (m_leftTabs->currentWidget() == m_pageList)
            scheduleThumbs();
        else if (m_thumbTimer)
            m_thumbTimer->stop();
    });

    leftDock->setWidget(m_leftTabs);
    leftDock->setMinimumWidth(200);
    addDockWidget(Qt::LeftDockWidgetArea, leftDock);

    // 右侧：文本 / 版面块卡片 / 原始 JSON
    auto *rightDock = new QDockWidget(QStringLiteral("识别结果"), this);
    rightDock->setObjectName(QStringLiteral("resultDock"));
    m_tabs = new QTabWidget(rightDock);

    m_textView = new QPlainTextEdit(m_tabs);
    m_textView->setReadOnly(true);
    m_textView->setPlaceholderText(QStringLiteral("识别结果会显示在这里"));
    m_textView->setAcceptDrops(false);
    m_tabs->addTab(m_textView, QStringLiteral("文本"));

    m_blocks = new BlockCardList(m_tabs);
    m_blocks->showPlaceholder(QStringLiteral("尚未打开文档"));
    m_tabs->addTab(m_blocks, QStringLiteral("版面块"));

    m_jsonView = new QPlainTextEdit(m_tabs);
    m_jsonView->setReadOnly(true);
    m_jsonView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_jsonView->setAcceptDrops(false);
    m_tabs->addTab(m_jsonView, QStringLiteral("JSON"));

    rightDock->setWidget(m_tabs);
    rightDock->setMinimumWidth(340);
    addDockWidget(Qt::RightDockWidgetArea, rightDock);

    // 状态栏
    m_statsLabel = new QLabel(QStringLiteral("就绪"), this);
    m_statsLabel->setToolTip(
        QStringLiteral("OCR 按页费计费（1.5 元/千页）；tokens 仅统计用量，不参与 OCR 计费。"));
    m_progress = new QProgressBar(this);
    m_progress->setMaximumWidth(180);
    m_progress->setVisible(false);
    m_selLabel = new QLabel(this);
    m_selLabel->hide();
    m_retryButton = new QPushButton(QStringLiteral("重试失败页"), this);
    m_retryButton->setToolTip(QStringLiteral("重新识别失败或返回空结果的页面"));
    m_retryButton->hide();
    m_hintLabel = new QLabel(
        QStringLiteral("拖入 PDF / 图片 / 文件夹 · Ctrl+滚轮缩放 · PgUp/PgDn 滚动"), this);
    m_hintLabel->setForegroundRole(QPalette::PlaceholderText);
    statusBar()->addWidget(m_statsLabel, 1);
    statusBar()->addPermanentWidget(m_selLabel);
    statusBar()->addPermanentWidget(m_retryButton);
    statusBar()->addPermanentWidget(m_hintLabel);
    statusBar()->addPermanentWidget(m_progress);

    connect(m_outlineTree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!item)
                    return;
                const int page = item->data(0, kOutlinePageRole).toInt();
                if (page >= 0)
                    goToPage(page);
            });
    connect(m_tocButton, &QPushButton::clicked, this, &MainWindow::generateOutlineFromToc);
    connect(m_importOutlineButton, &QPushButton::clicked, this, &MainWindow::importOutline);
    connect(m_replaceOutline, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("replaceOutlineWithOcr"), on);
        rebuildOutline();
    });
    connect(m_pageList->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &MainWindow::scheduleThumbs);
    connect(m_pageList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || m_syncingPage)
            return;
        if (m_view->currentPage() == row)
            showPage(row);
        else
            m_view->setCurrentPage(row);
    });
    connect(m_view, &PageView::selectionChanged, this, [this] { syncCardsFromView(); });
    connect(m_view, &PageView::textSelectionChanged, this, [this](int blocks, int chars) {
        if (chars <= 0) {
            m_selLabel->hide();
            return;
        }
        m_selLabel->setText(QStringLiteral("已选 %1 字 · 跨 %2 个块  ").arg(chars).arg(blocks));
        m_selLabel->show();
    });
    connect(m_retryButton, &QPushButton::clicked, this, &MainWindow::retryFailedPages);
    connect(m_view, &PageView::textCopied, this, [this](int blocks, int chars) {
        statusBar()->showMessage(
            QStringLiteral("已复制 %1 个块 · %2 字").arg(blocks).arg(chars), 2500);
    });
    connect(m_view, &PageView::imageCopied, this, [this] {
        statusBar()->showMessage(QStringLiteral("已复制图片"), 2500);
    });
    connect(m_view, &PageView::imageExportRequested, this,
            [this](int page, int block) { exportBlockImage(page, block); });
    connect(m_view, &PageView::currentPageChanged, this, [this](int page) {
        if (page < 0 || page >= m_doc.count())
            return;
        m_syncingPage = true;
        if (m_pageList->currentRow() != page)
            m_pageList->setCurrentRow(page);
        if (m_pageSpin)
            m_pageSpin->setValue(page + 1);
        m_syncingPage = false;
        showPage(page);
    });
    connect(m_view, &PageView::blockClicked, this, [this](int index) {
        if (m_view->selection().contains(index))
            m_blocks->scrollToBlock(index);
    });
    connect(m_blocks, &BlockCardList::cardsSelectionChanged, this,
            [this] { syncViewFromCards(); });
    connect(m_blocks, &BlockCardList::copyRequested, this, [this] { copySelection(); });
    connect(m_blocks, &BlockCardList::imageCopyRequested, this, [this](int block) {
        copyBlockImage(m_view->currentPage(), block);
    });
    connect(m_blocks, &BlockCardList::imageExportRequested, this, [this](int block) {
        exportBlockImage(m_view->currentPage(), block);
    });
    connect(m_blocks, &BlockCardList::blockActivated, this, [this](int index) {
        m_view->ensureBlockVisible(index);
    });
    rebuildOutline();
}

void MainWindow::buildActions()
{
    auto *toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(22, 22));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    auto make = [this](const QString &icon, const QString &text,
                       const QKeySequence &shortcut, auto &&slot) {
        auto *action = new QAction(theme::icon(icon), text, this);
        m_themedActions.append({action, icon});
        if (!shortcut.isEmpty())
            action->setShortcut(shortcut);
        const QString tip = shortcut.isEmpty()
                                ? text
                                : QStringLiteral("%1  %2").arg(
                                      text, shortcut.toString(QKeySequence::NativeText));
        action->setToolTip(tip);
        action->setStatusTip(text);
        action->setIconVisibleInMenu(false);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };

    QAction *openPdf = make(QStringLiteral("open-pdf"), QStringLiteral("打开 PDF"),
                            QKeySequence::Open, [this] { openFiles(); });
    QAction *openImage = make(QStringLiteral("open-image"), QStringLiteral("打开图片"),
                              QKeySequence(QStringLiteral("Ctrl+Shift+O")),
                              [this] { openFiles(); });
    QAction *openFolder = make(QStringLiteral("open-folder"), QStringLiteral("打开文件夹"),
                               QKeySequence(), [this] { openDirectory(); });

    QAction *prevPage = make(QStringLiteral("page-prev"), QStringLiteral("上一页"),
                             QKeySequence(QStringLiteral("Ctrl+PgUp")),
                             [this] { stepPage(-1); });
    QAction *nextPage = make(QStringLiteral("page-next"), QStringLiteral("下一页"),
                             QKeySequence(QStringLiteral("Ctrl+PgDown")),
                             [this] { stepPage(1); });

    QAction *ocrPage = make(QStringLiteral("ocr-page"), QStringLiteral("识别本页"),
                            QKeySequence(QStringLiteral("Ctrl+R")),
                            [this] { startOcrCurrent(); });
    QAction *ocrAll = make(QStringLiteral("ocr-all"), QStringLiteral("识别全部"),
                           QKeySequence(QStringLiteral("Ctrl+Shift+R")),
                           [this] { startOcrAll(); });
    QAction *stop = make(QStringLiteral("stop"), QStringLiteral("停止"),
                         QKeySequence(Qt::Key_Escape), [this] { stopOcr(); });
    stop->setEnabled(false);
    m_stopAction = stop;

    QAction *showBoxes = make(QStringLiteral("boxes"), QStringLiteral("显示识别框"),
                              QKeySequence(), [this] {});
    showBoxes->disconnect(this);
    showBoxes->setCheckable(true);
    showBoxes->setChecked(true);
    showBoxes->setToolTip(QStringLiteral("显示/隐藏版面框"));
    connect(showBoxes, &QAction::toggled, this, [this](bool on) {
        m_showBoxes = on;
        m_view->setBoxesVisible(on);
    });

    m_textToolAction = make(QStringLiteral("text-select"), QStringLiteral("选中文本"),
                            QKeySequence(), [this](bool) {});
    m_textToolAction->setCheckable(true);
    m_textToolAction->setToolTip(QStringLiteral("选中文本：拖动可连续选块，Ctrl 加选"));
    connect(m_textToolAction, &QAction::toggled, this, &MainWindow::setTextTool);

    QAction *fit = make(QStringLiteral("fit-width"), QStringLiteral("适应宽度"),
                        QKeySequence(Qt::Key_F), [this] { m_view->fitWidth(); });
    QAction *zoomIn = make(QStringLiteral("zoom-in"), QStringLiteral("放大"),
                           QKeySequence::ZoomIn, [this] { m_view->zoomBy(1.2); });
    QAction *zoomOut = make(QStringLiteral("zoom-out"), QStringLiteral("缩小"),
                            QKeySequence::ZoomOut, [this] { m_view->zoomBy(1 / 1.2); });
    QAction *actual = make(QStringLiteral("actual-size"), QStringLiteral("原始大小"),
                           QKeySequence(QStringLiteral("Ctrl+0")),
                           [this] { m_view->resetZoom(); });
    QAction *settings = make(QStringLiteral("settings"), QStringLiteral("首选项…"),
                             QKeySequence(QStringLiteral("Ctrl+,")),
                             [this] { openPreferences(); });

    toolbar->addAction(openPdf);
    toolbar->addAction(openImage);
    toolbar->addAction(openFolder);
    toolbar->addSeparator();
    toolbar->addAction(prevPage);

    m_pageSpin = new QSpinBox(toolbar);
    m_pageSpin->setRange(1, 1);
    m_pageSpin->setValue(1);
    m_pageSpin->setKeyboardTracking(false);
    m_pageSpin->setAlignment(Qt::AlignCenter);
    m_pageSpin->setFixedWidth(96);
    m_pageSpin->setSuffix(QStringLiteral(" / 0"));
    m_pageSpin->setToolTip(QStringLiteral("当前页 / 总页数，可直接输入页码跳转"));
    toolbar->addWidget(m_pageSpin);
    connect(m_pageSpin, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_syncingPage)
            return;
        goToPage(value - 1);
    });

    toolbar->addAction(nextPage);
    toolbar->addSeparator();
    toolbar->addAction(ocrPage);
    toolbar->addAction(ocrAll);
    toolbar->addAction(stop);
    toolbar->addSeparator();
    toolbar->addAction(showBoxes);
    toolbar->addAction(m_textToolAction);

    auto *purposeSpacer = new QWidget(toolbar);
    purposeSpacer->setFixedWidth(6);
    toolbar->addWidget(purposeSpacer);

    m_purposeBox = new QComboBox(toolbar);
    m_purposeBox->addItem(QStringLiteral("版面分析（带坐标框）"),
                          QStringLiteral("rich_layout_recognition"));
    m_purposeBox->addItem(QStringLiteral("纯文本（更快）"),
                          QStringLiteral("text_recognition"));
    m_purposeBox->setToolTip(QStringLiteral(
        "rich_layout_recognition 返回 <|ref|>/<|box|> 标记与坐标；text_recognition 只返回文字"));
    toolbar->addWidget(m_purposeBox);
    connect(m_purposeBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_purpose = m_purposeBox->itemData(index).toString();
        saveSettings();
    });

    toolbar->addSeparator();
    toolbar->addAction(fit);
    toolbar->addAction(zoomIn);
    toolbar->addAction(zoomOut);
    toolbar->addAction(actual);
    toolbar->addSeparator();
    toolbar->addAction(settings);

    auto *copySelectionAction = new QAction(QStringLiteral("复制选中内容"), this);
    copySelectionAction->setShortcut(QKeySequence::Copy);
    connect(copySelectionAction, &QAction::triggered, this, [this] { copySelection(); });
    auto *copyPageAction = new QAction(QStringLiteral("复制本页文本"), this);
    copyPageAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connect(copyPageAction, &QAction::triggered, this, [this] { copyText(); });

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
    fileMenu->addAction(openPdf);
    fileMenu->addAction(openImage);
    fileMenu->addAction(openFolder);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("导出 Markdown…"), this, [this] { exportMarkdown(); });
    fileMenu->addAction(QStringLiteral("导出 JSON…"), this, [this] { exportJson(); });
    fileMenu->addAction(QStringLiteral("导出可搜索 PDF…"), this,
                        [this] { exportSearchablePdf(); });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("导入大纲 JSON / YAML…"), this,
                        [this] { importOutline(); });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("退出"), QKeySequence::Quit, this, [this] { close(); });

    auto *editMenu = menuBar()->addMenu(QStringLiteral("编辑"));
    editMenu->addAction(copySelectionAction);
    editMenu->addAction(copyPageAction);
    editMenu->addSeparator();
    editMenu->addAction(settings);

    auto *ocrMenu = menuBar()->addMenu(QStringLiteral("识别"));
    ocrMenu->addAction(ocrPage);
    ocrMenu->addAction(ocrAll);
    ocrMenu->addAction(stop);
    ocrMenu->addSeparator();
    ocrMenu->addAction(QStringLiteral("重试失败页"), this,
                       [this] { retryFailedPages(); });
    ocrMenu->addAction(QStringLiteral("估算全部 OCR 费用…"), this,
                       [this] { showOcrEstimate(); });

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
    viewMenu->addAction(prevPage);
    viewMenu->addAction(nextPage);
    viewMenu->addSeparator();
    viewMenu->addAction(m_textToolAction);
    viewMenu->addAction(showBoxes);
    viewMenu->addSeparator();
    viewMenu->addAction(fit);
    viewMenu->addAction(zoomIn);
    viewMenu->addAction(zoomOut);
    viewMenu->addAction(actual);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
    helpMenu->addAction(QStringLiteral("关于 docr-qt"), this,
                        [this] { showAboutDialog(m_baseUrl, m_purpose, this); });
    helpMenu->addAction(QStringLiteral("关于 Qt"), this, [this] { QApplication::aboutQt(); });
}

void MainWindow::applySettings()
{
    QSettings settings;
    m_apiKey = settings.value(QStringLiteral("apiKey")).toString();
    if (m_apiKey.isEmpty())
        m_apiKey = ocr::dshApiKey();
    m_baseUrl = settings.value(QStringLiteral("baseUrl"),
                               QStringLiteral("https://api.deepseek.com/v1/tools")).toString();
    m_dpi = settings.value(QStringLiteral("dpi"), 150).toInt();
    m_concurrency = qBound(1, settings.value(QStringLiteral("concurrency"), 3).toInt(), 8);
    m_purpose = settings.value(QStringLiteral("purpose"),
                               QStringLiteral("rich_layout_recognition")).toString();
    m_ocrBackend = settings.value(QStringLiteral("ocrBackend"),
                                  QStringLiteral("deepseek")).toString();
    m_glmEndpoint = settings.value(QStringLiteral("glmEndpoint"),
                                   ocr::defaultGlmEndpoint()).toString();
    m_glmModel = settings.value(QStringLiteral("glmModel"),
                                QStringLiteral("glm-ocr")).toString();
    m_glmApiKey = settings.value(QStringLiteral("glmApiKey")).toString();
    m_doc.setDpi(m_dpi);
    m_queue.configure(m_baseUrl, m_apiKey, m_purpose, m_concurrency);
    m_queue.configureGlm(m_glmEndpoint, m_glmModel, m_glmApiKey);
    m_queue.setBackend(ocr::backendFromString(m_ocrBackend));
    if (m_replaceOutline) {
        QSignalBlocker blocker(m_replaceOutline);
        m_replaceOutline->setChecked(
            settings.value(QStringLiteral("replaceOutlineWithOcr"), false).toBool());
    }
    if (m_purposeBox) {
        const int index = m_purposeBox->findData(m_purpose);
        if (index >= 0) {
            QSignalBlocker blocker(m_purposeBox);
            m_purposeBox->setCurrentIndex(index);
        }
        const bool localGlm = ocr::backendFromString(m_ocrBackend) == ocr::Backend::GlmOcr;
        if (m_statsLabel)
            m_statsLabel->setToolTip(
                localGlm ? QStringLiteral("本地 GLM-OCR：不产生 DeepSeek OCR 页费，tokens 仅统计用量。")
                         : QStringLiteral("OCR 按页费计费（1.5 元/千页）；tokens 仅统计用量。"));
        m_purposeBox->setEnabled(!localGlm);
        m_purposeBox->setToolTip(
            localGlm ? QStringLiteral("GLM-OCR 总是返回整页文本，不使用 DeepSeek 版面模式")
                     : QStringLiteral("DeepSeek OCR 的识别模式"));
    }
    if (m_replaceOutline)
        rebuildOutline();
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("baseUrl"), m_baseUrl);
    settings.setValue(QStringLiteral("dpi"), m_dpi);
    settings.setValue(QStringLiteral("concurrency"), m_concurrency);
    settings.setValue(QStringLiteral("purpose"), m_purpose);
    settings.setValue(QStringLiteral("ocrBackend"), m_ocrBackend);
    settings.setValue(QStringLiteral("glmEndpoint"), m_glmEndpoint);
    settings.setValue(QStringLiteral("glmModel"), m_glmModel);
    if (settings.value(QStringLiteral("rememberKey"), false).toBool()) {
        settings.setValue(QStringLiteral("apiKey"), m_apiKey);
        if (!m_glmApiKey.isEmpty())
            settings.setValue(QStringLiteral("glmApiKey"), m_glmApiKey);
    } else {
        settings.remove(QStringLiteral("apiKey"));
        settings.remove(QStringLiteral("glmApiKey"));
    }
    if (m_replaceOutline)
        settings.setValue(QStringLiteral("replaceOutlineWithOcr"),
                          m_replaceOutline->isChecked());
    m_queue.configure(m_baseUrl, m_apiKey, m_purpose, m_concurrency);
    m_queue.configureGlm(m_glmEndpoint, m_glmModel, m_glmApiKey);
    m_queue.setBackend(ocr::backendFromString(m_ocrBackend));
    m_doc.setDpi(m_dpi);
}

void MainWindow::openPreferences()
{
    Preferences initial;
    initial.apiKey = m_apiKey;
    initial.baseUrl = m_baseUrl;
    initial.dpi = m_dpi;
    initial.concurrency = m_concurrency;
    initial.purpose = m_purpose;
    initial.backend = m_ocrBackend;
    initial.glmEndpoint = m_glmEndpoint;
    initial.glmModel = m_glmModel;
    initial.glmApiKey = m_glmApiKey;
    initial.rememberKey = QSettings().value(QStringLiteral("rememberKey"), false).toBool();

    PreferencesDialog dialog(initial, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const Preferences values = dialog.values();
    m_apiKey = values.apiKey;
    if (m_apiKey.isEmpty())
        m_apiKey = ocr::dshApiKey();
    m_baseUrl = values.baseUrl;
    m_dpi = values.dpi;
    m_concurrency = values.concurrency;
    m_purpose = values.purpose;
    m_ocrBackend = values.backend;
    m_glmEndpoint = values.glmEndpoint;
    m_glmModel = values.glmModel;
    m_glmApiKey = values.glmApiKey;

    QSettings settings;
    settings.setValue(QStringLiteral("rememberKey"), values.rememberKey);
    if (values.rememberKey)
        settings.setValue(QStringLiteral("apiKey"), m_apiKey);
    else
        settings.remove(QStringLiteral("apiKey"));
    saveSettings();

    if (m_purposeBox) {
        const int index = m_purposeBox->findData(m_purpose);
        if (index >= 0) {
            QSignalBlocker blocker(m_purposeBox);
            m_purposeBox->setCurrentIndex(index);
        }
        const bool localGlm = ocr::backendFromString(m_ocrBackend) == ocr::Backend::GlmOcr;
        if (m_statsLabel)
            m_statsLabel->setToolTip(
                localGlm ? QStringLiteral("本地 GLM-OCR：不产生 DeepSeek OCR 页费，tokens 仅统计用量。")
                         : QStringLiteral("OCR 按页费计费（1.5 元/千页）；tokens 仅统计用量。"));
        m_purposeBox->setEnabled(!localGlm);
        m_purposeBox->setToolTip(
            localGlm ? QStringLiteral("GLM-OCR 总是返回整页文本，不使用 DeepSeek 版面模式")
                     : QStringLiteral("DeepSeek OCR 的识别模式"));
    }
    updateStats();
    statusBar()->showMessage(QStringLiteral("首选项已保存"), 2500);
}

bool MainWindow::loadPath(const QString &path)
{
    openPaths({path});
    return m_doc.count() > 0;
}

void MainWindow::openPaths(const QStringList &paths)
{
    const QStringList files = Document::expandPaths(paths);
    if (files.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("没有可打开的 PDF / 图片"), 4000);
        return;
    }

    m_queue.stop();
    m_retriedPages.clear();
    m_batchMode = false;
    if (m_stopAction)
        m_stopAction->setEnabled(false);

    m_doc.clear();
    m_pageList->clear();
    m_view->clearPage();
    m_pageImage = QImage();
    m_blocks->setBlocks({});
    m_textView->clear();
    m_jsonView->clear();

    DocumentLoadReport report;
    m_doc.addPaths(files, m_dpi, &report);
    if (m_doc.count() == 0) {
        QMessageBox::warning(this, QStringLiteral("打开失败"),
                             report.errors.isEmpty()
                                 ? QStringLiteral("没有可显示的页面")
                                 : report.errors.join(QLatin1Char('\n')));
        m_customOutline.clear();
        m_customOutlineLabel = QStringLiteral("OCR 目录");
        m_tocJob.active = false;
        m_tocJob.pages.clear();
        rebuildOutline();
        setWindowTitle(QStringLiteral("docr-qt"));
        updatePanels();
        updatePageNav();
        updateStats();
        return;
    }

    addPagesToUi();
    m_customOutline.clear();
    m_customOutlineLabel = QStringLiteral("OCR 目录");
    m_tocJob.active = false;
    m_tocJob.pages.clear();
    rebuildOutline();
    const QString title =
        files.size() == 1
            ? QFileInfo(files.first()).fileName()
            : QStringLiteral("%1 个文件 · %2 页").arg(files.size()).arg(m_doc.count());
    setWindowTitle(QStringLiteral("docr-qt — %1").arg(title));
    if (!report.errors.isEmpty())
        statusBar()->showMessage(
            QStringLiteral("有 %1 个文件无法打开").arg(report.errors.size()), 5000);
}

void MainWindow::openFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("打开 PDF 或图片"), QDir::homePath(),
        QStringLiteral("支持的格式 (*.pdf *.png *.jpg *.jpeg *.webp *.gif *.bmp *.tif *.tiff);;PDF (*.pdf);;")
            + kImageFilter);
    if (files.isEmpty())
        return;
    openPaths(files);
}

void MainWindow::openDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择图片序列目录"));
    if (dir.isEmpty())
        return;
    openPaths({dir});
}

void MainWindow::addPagesToUi()
{
    m_thumbTimer->stop();
    m_pageList->setUpdatesEnabled(false);
    m_pageList->clear();
    for (int i = 0; i < m_doc.count(); ++i) {
        auto *item = new QListWidgetItem(m_pageList);
        item->setText(QString::number(i + 1));
        item->setToolTip(m_doc.page(i).label);
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(140, 190));
    }
    m_pageList->setUpdatesEnabled(true);
    m_view->setDocument(&m_doc, m_showBoxes);
    if (m_doc.count() > 0)
        m_pageList->setCurrentRow(0);
    updateStats();
    scheduleThumbs();
}

void MainWindow::appendOutlineItems(QTreeWidgetItem *parent,
                                     const QVector<OutlineEntry> &entries)
{
    for (const OutlineEntry &entry : entries) {
        auto *item = parent ? new QTreeWidgetItem(parent)
                            : new QTreeWidgetItem(m_outlineTree);
        item->setText(0, entry.title);
        if (entry.page >= 0) {
            item->setData(0, kOutlinePageRole, entry.page);
            item->setToolTip(0, QStringLiteral("第 %1 页").arg(entry.page + 1));
        }
        if (!entry.children.isEmpty()) {
            appendOutlineItems(item, entry.children);
        } else if (entry.page < 0) {
            item->setDisabled(true);
        }
    }
}

void MainWindow::rebuildOutline()
{
    if (!m_outlineTree || !m_leftTabs)
        return;
    m_outlineTree->clear();

    const QVector<OutlineEntry> &builtin = m_doc.outline();
    const bool hasBoth = !builtin.isEmpty() && !m_customOutline.isEmpty();
    const bool replace = hasBoth && m_replaceOutline && m_replaceOutline->isChecked();
    if (!replace)
        appendOutlineItems(nullptr, builtin);
    if (!m_customOutline.isEmpty()) {
        if (replace || builtin.isEmpty()) {
            appendOutlineItems(nullptr, m_customOutline);
        } else {
            auto *root = new QTreeWidgetItem(m_outlineTree);
            root->setText(0, m_customOutlineLabel);
            root->setFirstColumnSpanned(true);
            appendOutlineItems(root, m_customOutline);
            root->setExpanded(true);
        }
    }

    if (m_outlineTree->topLevelItemCount() == 0) {
        auto *empty = new QTreeWidgetItem(m_outlineTree);
        empty->setText(0, QStringLiteral("没有内置大纲"));
        empty->setDisabled(true);
        empty->setToolTip(
            0, QStringLiteral("可以用下方按钮 OCR 目录页生成，或导入 JSON/YAML"));
    } else {
        m_outlineTree->expandToDepth(1);
        for (int i = 0; i < m_outlineTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_outlineTree->topLevelItem(i);
            if (item->text(0) == m_customOutlineLabel)
                item->setExpanded(true);
        }
    }

    m_tocButton->setEnabled(m_doc.count() > 0 && !m_tocJob.active);
    m_tocButton->setText(m_tocJob.active ? QStringLiteral("正在识别目录页…")
                                         : QStringLiteral("从目录页生成…"));
    if (m_importOutlineButton)
        m_importOutlineButton->setEnabled(m_doc.count() > 0);
    if (m_replaceOutline)
        m_replaceOutline->setEnabled(hasBoth);
    if (!builtin.isEmpty() || !m_customOutline.isEmpty())
        m_leftTabs->setCurrentIndex(1);
    else
        m_leftTabs->setCurrentIndex(0);
}

void MainWindow::generateOutlineFromToc()
{
    if (m_doc.count() == 0) {
        statusBar()->showMessage(QStringLiteral("先打开文档"), 3000);
        return;
    }
    if (m_tocJob.active) {
        statusBar()->showMessage(QStringLiteral("正在识别目录页…"), 3000);
        return;
    }
    if (m_queue.isBusy()) {
        QMessageBox::information(this, QStringLiteral("稍候"),
                                 QStringLiteral("当前有识别任务在进行，完成后再生成目录。"));
        return;
    }

    const int current = qMax(0, m_pageList->currentRow());
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("从目录页生成大纲"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *start = new QSpinBox(&dialog);
    start->setRange(1, m_doc.count());
    start->setValue(current + 1);
    auto *end = new QSpinBox(&dialog);
    end->setRange(1, m_doc.count());
    end->setValue(qMin(m_doc.count(), current + 3));
    form->addRow(QStringLiteral("目录起始页"), start);
    form->addRow(QStringLiteral("目录结束页"), end);
    layout->addLayout(form);
    auto *hint = new QLabel(
        QStringLiteral("会先识别这些页，再解析「标题 …… 页码」。\n"
                       "已识别过的页直接复用，不会重复计费。"), &dialog);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    int from = start->value() - 1;
    int to = end->value() - 1;
    if (from > to)
        std::swap(from, to);
    QVector<int> pages;
    QVector<int> missing;
    for (int page = from; page <= to; ++page) {
        pages.append(page);
        if (!m_doc.page(page).result.ok())
            missing.append(page);
    }
    if (pages.isEmpty())
        return;

    m_tocJob.pages = pages;
    if (missing.isEmpty()) {
        m_tocJob.active = false;
        parseTocPages();
        return;
    }

    m_tocJob.active = true;
    m_tocButton->setEnabled(false);
    m_tocButton->setText(QStringLiteral("正在识别目录页…"));
    statusBar()->showMessage(QStringLiteral("正在识别目录页…"), 3000);
    if (m_queue.startPages(missing) == 0 && !m_queue.isBusy()) {
        m_tocJob.active = false;
        m_tocButton->setEnabled(true);
        m_tocButton->setText(QStringLiteral("从目录页生成…"));
        parseTocPages();
    }
}

void MainWindow::parseTocPages()
{
    if (m_tocJob.pages.isEmpty())
        return;
    const QVector<int> pages = m_tocJob.pages;
    m_tocJob.pages.clear();
    m_tocJob.active = false;
    m_tocButton->setEnabled(true);
    m_tocButton->setText(QStringLiteral("从目录页生成…"));

    QStringList lines;
    int usable = 0;
    for (int page : pages) {
        if (page < 0 || page >= m_doc.count() || !m_doc.page(page).result.ok())
            continue;
        ++usable;
        lines += tocLinesFromResult(m_doc.page(page).result);
    }
    if (usable == 0) {
        QMessageBox::information(
            this, QStringLiteral("没有目录文字"),
            QStringLiteral("这些页还没有可用的识别结果。请先识别目录页，或稍后重试。"));
        return;
    }

    const QVector<toc::Entry> entries = toc::parseLines(lines);
    if (entries.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("没解析出目录"),
            QStringLiteral("识别结果里没有找到「标题 …… 页码」格式的条目。\n"
                           "也可以把目录整理成 JSON/YAML 后从「导入 JSON / YAML…」载入。"));
        return;
    }

    // 用目录页自己的页脚页码猜一个偏移；没有就回到 0，用户可在预览里调。
    int guessedOffset = 0;
    bool guessed = false;
    for (int page : pages) {
        if (page < 0 || page >= m_doc.count() || !m_doc.page(page).result.ok())
            continue;
        for (const ocr::Block &block : m_doc.page(page).result.blocks) {
            if (!block.kind.contains(QStringLiteral("page_number"))
                && !block.kind.contains(QStringLiteral("page_footer")))
                continue;
            static const QRegularExpression digits(QStringLiteral(R"(\d{1,4})"));
            const QRegularExpressionMatch match =
                digits.match(ocr::displayText(block.text));
            if (!match.hasMatch())
                continue;
            const int printed = match.captured(0).toInt();
            if (printed > 0) {
                guessedOffset = page - printed + 1;
                guessed = true;
                break;
            }
        }
        if (guessed)
            break;
        const QString label = m_doc.pageLabel(page);
        // 没有显式页码标签时 poppler 会返回物理页号，不能拿它猜偏移。
        if (label == QString::number(page + 1))
            continue;
        bool ok = false;
        const int printed = label.toInt(&ok);
        if (ok && printed > 0) {
            guessedOffset = page - printed + 1;
            break;
        }
    }

    QVector<OutlineEntry> mapped;
    if (previewToc(entries, guessedOffset, &mapped) && !mapped.isEmpty()) {
        m_customOutline = mapped;
        m_customOutlineLabel = QStringLiteral("OCR 目录");
        rebuildOutline();
        m_leftTabs->setCurrentIndex(1);
        statusBar()->showMessage(
            QStringLiteral("已生成 OCR 大纲（%1 条目录项）").arg(entries.size()), 4000);
    }
}

void MainWindow::importOutline()
{
    if (m_doc.count() == 0) {
        statusBar()->showMessage(QStringLiteral("先打开文档"), 3000);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入大纲"), QDir::homePath(),
        QStringLiteral("大纲 (*.json *.yaml *.yml);;JSON (*.json);;YAML (*.yaml *.yml);;"
                       "所有文件 (*)"));
    if (!path.isEmpty())
        importOutlineFile(path);
}

bool MainWindow::importOutlineFile(const QString &path)
{
    if (m_doc.count() == 0) {
        statusBar()->showMessage(QStringLiteral("先打开文档"), 3000);
        return false;
    }

    QVector<OutlineEntry> entries;
    QString error;
    QStringList warnings;
    if (!outlineio::loadFile(path, m_doc.count(), &entries, &error, &warnings)) {
        QMessageBox::warning(this, QStringLiteral("导入大纲失败"),
                             error.isEmpty() ? QStringLiteral("无法解析大纲文件") : error);
        return false;
    }

    m_customOutline = entries;
    m_customOutlineLabel = QStringLiteral("导入目录");

    // 导入的意图是替换当前大纲；有内置书签时自动勾选替换。
    if (m_replaceOutline && !m_doc.outline().isEmpty()) {
        QSignalBlocker blocker(m_replaceOutline);
        m_replaceOutline->setChecked(true);
        QSettings().setValue(QStringLiteral("replaceOutlineWithOcr"), true);
    }
    rebuildOutline();
    m_leftTabs->setCurrentIndex(1);
    statusBar()->showMessage(
        QStringLiteral("已导入大纲：%1 条顶层项（%2）").arg(entries.size()).arg(path), 6000);

    if (!warnings.isEmpty()) {
        QStringList shown = warnings.mid(0, 12);
        if (warnings.size() > shown.size())
            shown << QStringLiteral("…另有 %1 条").arg(warnings.size() - shown.size());
        QMessageBox::warning(this, QStringLiteral("大纲有警告"), shown.join(QLatin1Char('\n')));
    }
    return true;
}

bool MainWindow::previewToc(const QVector<toc::Entry> &entries, int guessedOffset,
                            QVector<OutlineEntry> *mapped)
{
    // poppler 在 PDF 没有页码标签时也会返回 "1"、"2"……，不能直接拿它当
    // 印刷页码。只把「和物理页号不同」的标签当作真正的 PDF 页码标签。
    QHash<QString, int> labels;
    for (int i = 0; i < m_doc.count(); ++i) {
        const QString label = m_doc.pageLabel(i);
        if (label.isEmpty() || label == QString::number(i + 1))
            continue;
        if (!labels.contains(label))
            labels.insert(label, i);
    }
    const bool explicitLabels = !labels.isEmpty();

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("预览 OCR 大纲"));
    dialog.resize(760, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *summary = new QLabel(
        QStringLiteral("解析到 %1 条目录项。勾选需要的条目；没有页码的组标题会保留为"
                       "不可跳转的层级节点。")
            .arg(entries.size()),
        &dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *tree = new QTreeWidget(&dialog);
    tree->setColumnCount(3);
    tree->setHeaderLabels({QStringLiteral("标题"), QStringLiteral("目录页"),
                           QStringLiteral("PDF 页")});
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    layout->addWidget(tree, 1);

    for (const toc::Entry &entry : entries) {
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, QString(2 * (entry.level - 1), QLatin1Char(' ')) + entry.title);
        item->setText(1, QString::number(entry.printedPage));
        item->setData(0, Qt::UserRole, entry.level);
        item->setData(0, Qt::UserRole + 1, entry.printedPage);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked);
    }

    auto *options = new QFormLayout;
    auto *useLabels = new QCheckBox(QStringLiteral("优先按 PDF 页码标签匹配"), &dialog);
    useLabels->setChecked(explicitLabels);
    useLabels->setEnabled(explicitLabels && !labels.isEmpty());
    options->addRow(useLabels);
    auto *offset = new QSpinBox(&dialog);
    offset->setRange(-9999, 9999);
    offset->setValue(guessedOffset);
    offset->setSuffix(QStringLiteral(" 页"));
    options->addRow(QStringLiteral("PDF 页码 = 目录页码 +"), offset);
    layout->addLayout(options);

    auto updateMapping = [&] {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = tree->topLevelItem(i);
            const int printed = item->data(0, Qt::UserRole + 1).toInt();
            int page = -1;
            if (useLabels->isChecked())
                page = labels.value(QString::number(printed), -1);
            if (page < 0)
                page = printed - 1 + offset->value();
            item->setData(0, Qt::UserRole + 2, page);
            if (page >= 0 && page < m_doc.count())
                item->setText(2, QString::number(page + 1));
            else
                item->setText(2, QStringLiteral("—"));
        }
    };
    connect(offset, &QSpinBox::valueChanged, &dialog, updateMapping);
    connect(useLabels, &QCheckBox::toggled, &dialog, updateMapping);
    updateMapping();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    while (dialog.exec() == QDialog::Accepted) {
        QVector<OutlineEntry> roots;
        QVector<QPair<int, QVector<int>>> stack;
        int skipped = 0;
        int withPage = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = tree->topLevelItem(i);
            if (item->checkState(0) != Qt::Checked)
                continue;
            const int page = item->data(0, Qt::UserRole + 2).toInt();
            if (page >= m_doc.count()) {
                ++skipped;
                continue;
            }
            OutlineEntry node;
            node.title = item->text(0).trimmed();
            // -1 的组标题仍然保留，作为不可跳转的层级节点；子项可以正常跳转。
            node.page = page >= 0 ? page : -1;
            if (node.page >= 0)
                ++withPage;
            appendOutlineNode(roots, stack, node, item->data(0, Qt::UserRole).toInt());
        }
        if (roots.isEmpty() || withPage == 0) {
            QMessageBox::warning(&dialog, QStringLiteral("没有可用的条目"),
                                 QStringLiteral("请至少勾选一条带有效 PDF 页码的目录项。"));
            continue;
        }
        if (skipped > 0)
            statusBar()->showMessage(
                QStringLiteral("有 %1 条目录项页码超出范围，已跳过").arg(skipped), 5000);
        *mapped = roots;
        return true;
    }
    return false;
}

void MainWindow::scheduleThumbs()
{
    if (!m_thumbTimer || m_doc.count() == 0)
        return;
    if (m_leftTabs && m_leftTabs->currentWidget() != m_pageList)
        return;
    if (!m_thumbTimer->isActive())
        m_thumbTimer->start();
}

void MainWindow::ensureThumbs()
{
    if (!m_pageList || m_doc.count() == 0 || !m_pageList->isVisible()) {
        m_thumbTimer->stop();
        return;
    }

    // 只生成当前可见页的缩略图。500 页文档如果打开时逐页渲染，会把主线程
    // 卡住几十秒；滚动到哪里再渲染，打开和滚动都保持响应。
    const QRect viewportRect = m_pageList->viewport()->rect();
    QElapsedTimer budget;
    budget.start();
    int generated = 0;
    for (int i = 0; i < m_doc.count(); ++i) {
        if (!m_doc.page(i).thumb.isNull())
            continue;
        QListWidgetItem *item = m_pageList->item(i);
        if (!item)
            continue;
        const QRect rect = m_pageList->visualItemRect(item);
        if (!rect.intersects(viewportRect))
            continue;

        const QImage thumb = m_doc.ensureThumb(i);
        if (!thumb.isNull())
            item->setIcon(QIcon(QPixmap::fromImage(thumb)));
        ++generated;
        if (generated >= 2 || budget.elapsed() > 24)
            return;
    }
    m_thumbTimer->stop();
}

void MainWindow::showPage(int index)
{
    if (index < 0 || index >= m_doc.count())
        return;
    m_pageImage = m_doc.ensureImage(index);
    updatePanels();
    updatePageNav();
}

void MainWindow::goToPage(int index)
{
    if (m_doc.count() == 0)
        return;
    index = qBound(0, index, m_doc.count() - 1);
    m_view->setCurrentPage(index);
    scheduleThumbs();
    if (auto *item = m_pageList->item(index))
        m_pageList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
}

void MainWindow::stepPage(int delta)
{
    if (m_doc.count() == 0)
        return;
    const int current = m_pageList->currentRow();
    const int target = (current < 0 ? 0 : current) + delta;
    if (target < 0 || target >= m_doc.count())
        return;
    goToPage(target);
}

void MainWindow::updatePageNav()
{
    if (!m_pageSpin)
        return;
    const int count = m_doc.count();
    QSignalBlocker blocker(m_pageSpin);
    m_pageSpin->setMaximum(qMax(1, count));
    m_pageSpin->setValue(count > 0 ? m_pageList->currentRow() + 1 : 1);
    m_pageSpin->setSuffix(count > 0 ? QStringLiteral(" / %1").arg(count)
                                    : QStringLiteral(" / 0"));
    m_pageSpin->setEnabled(count > 0);
}

void MainWindow::updatePanels()
{
    const int index = m_pageList->currentRow();
    if (index < 0 || index >= m_doc.count()) {
        m_textView->clear();
        m_jsonView->clear();
        m_blocks->setBlocks({});
        m_blocks->showPlaceholder(QStringLiteral("尚未打开文档"));
        m_selLabel->hide();
        return;
    }
    const PageEntry &entry = m_doc.page(index);
    if (!entry.result.ok()) {
        m_textView->setPlainText(entry.result.error.isEmpty()
                                     ? QStringLiteral("尚未识别")
                                     : QStringLiteral("识别失败：%1").arg(entry.result.error));
        m_jsonView->clear();
        m_blocks->setBlocks({});
        m_blocks->showPlaceholder(entry.result.error.isEmpty()
                                      ? QStringLiteral("尚未识别\n识别后这里按板块列出结果")
                                      : QStringLiteral("识别失败\n可以重新识别本页"));
        m_selLabel->hide();
        return;
    }

    m_textView->setPlainText(ocr::plainText(entry.result.payload, entry.result.purpose));
    m_jsonView->setPlainText(QString::fromUtf8(
        QJsonDocument(entry.result.payload).toJson(QJsonDocument::Indented)));

    m_blocks->setBlocks(entry.result.blocks, m_pageImage);
    m_textView->setPlaceholderText(QStringLiteral("识别结果会显示在这里"));
    if (entry.result.blocks.isEmpty()) {
        if (!ocr::isLayoutPurpose(entry.result.purpose)) {
            m_blocks->showPlaceholder(
                QStringLiteral("当前模式只返回纯文本\n改用「版面分析」可得到带坐标的块"));
        } else if (!ocr::hasText(entry.result.payload)) {
            if (!OcrQueue::pageHasInk(m_pageImage)) {
                m_textView->setPlaceholderText(QStringLiteral("本页没有可识别的文字"));
                m_blocks->showPlaceholder(QStringLiteral("空白页（没有文字）"));
            } else {
                m_textView->setPlaceholderText(
                    QStringLiteral("服务端未返回文字；可点「识别本页」重试"));
                m_blocks->showPlaceholder(
                    m_retriedPages.contains(index)
                        ? QStringLiteral("服务端未返回文字\n已自动重试一次，可再点「识别本页」")
                        : QStringLiteral("服务端未返回文字\n点「识别本页」重试"));
            }
        } else {
            m_blocks->showPlaceholder(QStringLiteral("本页没有可显示的版面块"));
        }
    }
    m_selLabel->hide();
}

QVector<int> MainWindow::retryCandidates() const
{
    QVector<int> pages;
    for (int i = 0; i < m_doc.count(); ++i) {
        if (ocr::needsRetry(m_doc.page(i).result))
            pages.append(i);
    }
    return pages;
}

void MainWindow::updateRetryButton(int count)
{
    if (!m_retryButton)
        return;
    const bool busy = m_queue.isBusy();
    m_retryButton->setText(count > 0 ? QStringLiteral("重试失败页 (%1)").arg(count)
                                     : QStringLiteral("重试失败页"));
    m_retryButton->setVisible(count > 0 && !busy);
    m_retryButton->setEnabled(count > 0 && !busy);
}

void MainWindow::updateStats()
{
    int done = 0, failed = 0, emptyPages = 0, blankPages = 0, retryable = 0;
    int tokens = 0;
    double seconds = 0.0;
    for (int i = 0; i < m_doc.count(); ++i) {
        const PageEntry &entry = m_doc.page(i);
        if (entry.result.ok()) {
            ++done;
            tokens += entry.result.usage().value(QStringLiteral("total_tokens")).toInt();
            seconds += entry.result.elapsedMs / 1000.0;
            if (entry.result.blank)
                ++blankPages;
            if (ocr::needsRetry(entry.result)) {
                ++emptyPages;
                ++retryable;
            }
        } else if (!entry.result.error.isEmpty()) {
            ++failed;
            if (ocr::needsRetry(entry.result))
                ++retryable;
        }
    }
    const bool billable = ocr::backendFromString(m_ocrBackend) != ocr::Backend::GlmOcr;
    const double cost = ocrcost::actual(m_doc, billable).total;
    QString text;
    if (m_doc.count() == 0) {
        text = QStringLiteral("就绪");
    } else if (!billable) {
        text = QStringLiteral("%1/%2 页 · %3s · %4 tokens · 本地 GLM-OCR")
                   .arg(done).arg(m_doc.count())
                   .arg(seconds, 0, 'f', 1)
                   .arg(tokens);
    } else {
        text = QStringLiteral("%1/%2 页 · %3s · %4 tokens · 页费≈¥%5")
                   .arg(done).arg(m_doc.count())
                   .arg(seconds, 0, 'f', 1)
                   .arg(tokens)
                   .arg(ocrcost::formatCny(cost));
    }
    if (failed > 0)
        text += QStringLiteral(" · %1 页失败").arg(failed);
    if (emptyPages > 0)
        text += QStringLiteral(" · %1 页空结果").arg(emptyPages);
    if (blankPages > 0)
        text += QStringLiteral(" · %1 页空白").arg(blankPages);
    const bool busy = m_queue.isBusy();
    if (busy)
        text += QStringLiteral(" · 识别中");
    m_statsLabel->setText(text);
    updateRetryButton(retryable);
    m_progress->setVisible(busy);
    if (busy && !m_batchMode) {
        // 单页请求：用忙碌指示器，避免进度条看起来停在原地。
        m_progress->setRange(0, 0);
    } else {
        m_progress->setRange(0, qMax(1, m_doc.count()));
        m_progress->setValue(done);
    }
}

void MainWindow::retryFailedPages()
{
    if (m_doc.count() == 0) {
        statusBar()->showMessage(QStringLiteral("先打开文档"), 3000);
        return;
    }
    if (m_queue.isBusy()) {
        statusBar()->showMessage(QStringLiteral("当前有识别任务在进行"), 3000);
        return;
    }
    const QVector<int> pages = retryCandidates();
    if (pages.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("没有需要重试的页面"), 3000);
        updateStats();
        return;
    }
    m_batchMode = true;
    m_retriedPages.clear();
    if (m_stopAction)
        m_stopAction->setEnabled(true);
    const int count = m_queue.startPages(pages);
    if (count == 0) {
        m_batchMode = false;
        if (!m_queue.isBusy() && m_stopAction)
            m_stopAction->setEnabled(false);
        statusBar()->showMessage(QStringLiteral("这些页面已在识别队列中"), 3000);
        updateStats();
        return;
    }
    statusBar()->showMessage(QStringLiteral("开始重试 %1 个失败/空结果页…").arg(count), 4000);
    updateStats();
}

void MainWindow::showOcrEstimate()
{
    if (m_doc.count() == 0) {
        statusBar()->showMessage(QStringLiteral("先打开文档"), 3000);
        return;
    }
    const bool billable = ocr::backendFromString(m_ocrBackend) != ocr::Backend::GlmOcr;
    const ocrcost::Estimate estimate = ocrcost::estimateRemaining(m_doc, m_purpose, billable);
    CostDialog dialog(estimate, m_purpose, this);
    if (dialog.exec() == QDialog::Accepted && dialog.startRequested())
        startOcrAll();
}

void MainWindow::startOcrAll()
{
    if (m_doc.count() == 0) {
        statusBar()->showMessage(QStringLiteral("先打开文档"), 3000);
        return;
    }

    m_batchMode = true;
    // 先亮起停止按钮，避免同步失败时错过 allFinished。
    if (m_stopAction)
        m_stopAction->setEnabled(true);
    statusBar()->showMessage(QStringLiteral("开始识别…"), 2000);

    const int count = m_queue.startAll();
    if (count == 0) {
        if (!m_queue.isBusy() && m_stopAction)
            m_stopAction->setEnabled(false);
        m_batchMode = false;
        statusBar()->showMessage(
            m_queue.isBusy() ? QStringLiteral("页面已在识别队列中")
                             : QStringLiteral("所有页面都已识别"),
            3000);
        updateStats();
        return;
    }
    m_retriedPages.clear();
    const bool billable = ocr::backendFromString(m_ocrBackend) != ocr::Backend::GlmOcr;
    const ocrcost::Estimate estimate = ocrcost::estimateRemaining(m_doc, m_purpose, billable);
    statusBar()->showMessage(
        billable ? QStringLiteral("开始识别 %1 页 · 预计 ≈¥%2（OCR 页费）")
                       .arg(count)
                       .arg(ocrcost::formatCny(estimate.pageFee))
                 : QStringLiteral("开始识别 %1 页 · 本地 GLM-OCR").arg(count),
        5000);
    updateStats();
}

void MainWindow::startOcrCurrent()
{
    const int index = m_pageList->currentRow();
    if (index < 0 || index >= m_doc.count()) {
        statusBar()->showMessage(QStringLiteral("先打开文档"), 3000);
        return;
    }

    m_batchMode = false;
    m_retriedPages.remove(index);
    if (m_stopAction)
        m_stopAction->setEnabled(true);
    statusBar()->showMessage(QStringLiteral("正在识别第 %1 页…").arg(index + 1), 2000);

    bool alreadyRunning = false;
    if (!m_queue.startPage(index, &alreadyRunning)) {
        if (alreadyRunning) {
            statusBar()->showMessage(
                QStringLiteral("第 %1 页正在识别中，请稍候…").arg(index + 1), 2000);
        } else if (!m_queue.isBusy() && m_stopAction) {
            m_stopAction->setEnabled(false);
        }
        return;
    }
    updateStats();
}

void MainWindow::onPageQueued(int page)
{
    if (auto *item = m_pageList->item(page)) {
        item->setText(QStringLiteral("%1 ⏳").arg(page + 1));
        item->setForeground(theme::tone(QColor(245, 158, 11)));
        item->setToolTip(m_doc.page(page).label);
    }
    updateStats();
}

void MainWindow::onPageStarted(int page)
{
    if (auto *item = m_pageList->item(page)) {
        item->setText(QStringLiteral("%1 ⏳").arg(page + 1));
        item->setForeground(theme::tone(QColor(245, 158, 11)));
        item->setToolTip(m_doc.page(page).label);
    }
    updateStats();
}

void MainWindow::onPageRetrying(int page)
{
    m_retriedPages.insert(page);
    if (auto *item = m_pageList->item(page)) {
        item->setText(QStringLiteral("%1 ↻").arg(page + 1));
        item->setForeground(theme::tone(QColor(245, 158, 11)));
        item->setToolTip(QStringLiteral("%1\n服务端返回空结果，正在自动重试")
                             .arg(m_doc.page(page).label));
    }
    statusBar()->showMessage(
        QStringLiteral("第 %1 页返回空结果，正在自动重试…").arg(page + 1), 5000);
    updateStats();
}

void MainWindow::onPageFinished(int page, const ocr::PageResult &result)
{
    if (page < 0 || page >= m_doc.count())
        return;

    const bool layout = ocr::isLayoutPurpose(result.purpose);
    const bool empty = result.ok() && layout && result.blocks.isEmpty()
                       && !ocr::hasText(result.payload);
    const bool hasInk = empty && !result.blank;

    if (auto *item = m_pageList->item(page)) {
        if (!result.ok()) {
            item->setText(QStringLiteral("%1 ✕").arg(page + 1));
            item->setForeground(theme::tone(QColor(239, 68, 68)));
            item->setToolTip(QStringLiteral("%1\n%2").arg(m_doc.page(page).label, result.error));
        } else if (layout && result.blocks.isEmpty() && empty && hasInk) {
            item->setText(QStringLiteral("%1 !").arg(page + 1));
            item->setForeground(theme::tone(QColor(245, 158, 11)));
            item->setToolTip(QStringLiteral("%1\n未返回文字（服务端空结果），可重试")
                                 .arg(m_doc.page(page).label));
        } else if (layout && result.blocks.isEmpty() && empty) {
            // 空白页：没有文字是正常的，不标成异常。
            item->setText(QStringLiteral("%1 ✓").arg(page + 1));
            item->setForeground(theme::tone(QColor(34, 197, 94)));
            item->setToolTip(QStringLiteral("%1\n空白页（无文字）").arg(m_doc.page(page).label));
        } else if (layout && result.blocks.isEmpty()) {
            item->setText(QStringLiteral("%1 !").arg(page + 1));
            item->setForeground(theme::tone(QColor(245, 158, 11)));
            item->setToolTip(QStringLiteral("%1\n未返回版面块").arg(m_doc.page(page).label));
        } else {
            item->setText(QStringLiteral("%1 ✓").arg(page + 1));
            item->setForeground(theme::tone(QColor(34, 197, 94)));
            item->setToolTip(m_doc.page(page).label);
        }
    }

    if (empty) {
        if (!hasInk) {
            statusBar()->showMessage(
                QStringLiteral("第 %1 页没有文字（空白页）").arg(page + 1), 3000);
        } else {
            QString message = QStringLiteral(
                                  "第 %1 页未返回文字（服务端空结果），可再点「识别本页」重试")
                                  .arg(page + 1);
            const QString endpoint =
                ocr::backendFromString(m_ocrBackend) == ocr::Backend::GlmOcr
                    ? m_glmEndpoint
                    : m_baseUrl;
            const QString diagnostics = ocrdiag::saveEmptyResult(
                page, m_doc.page(page), m_doc.ensureImage(page), result, endpoint, m_dpi);
            if (!diagnostics.isEmpty()) {
                message += QStringLiteral("；诊断：%1").arg(diagnostics);
                if (auto *item = m_pageList->item(page)) {
                    item->setToolTip(item->toolTip()
                                     + QStringLiteral("\n诊断：%1").arg(diagnostics));
                }
            }
            statusBar()->showMessage(message, 10000);
        }
    } else if (m_retriedPages.contains(page)) {
        statusBar()->showMessage(QStringLiteral("第 %1 页重试成功").arg(page + 1), 2500);
    } else if (!result.ok()) {
        statusBar()->showMessage(
            QStringLiteral("第 %1 页识别失败：%2").arg(page + 1).arg(result.error), 5000);
    } else {
        statusBar()->showMessage(
            QStringLiteral("第 %1 页识别完成%2")
                .arg(page + 1)
                .arg(layout ? QStringLiteral(" · %1 个块").arg(result.blocks.size())
                            : QString()),
            2500);
    }

    m_view->refreshPage(page);
    if (page == m_pageList->currentRow())
        showPage(page);
    updateStats();
}

void MainWindow::stopOcr()
{
    m_queue.stop();
    if (m_tocJob.active) {
        m_tocJob.active = false;
        m_tocJob.pages.clear();
        if (m_tocButton) {
            m_tocButton->setEnabled(m_doc.count() > 0);
            m_tocButton->setText(QStringLiteral("从目录页生成…"));
        }
    }
    m_retriedPages.clear();
    m_batchMode = false;
    if (m_stopAction)
        m_stopAction->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("已停止"), 2500);
    updateStats();
}

void MainWindow::exportMarkdown()
{
    if (m_doc.count() == 0)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 Markdown"), QDir::homePath() + QStringLiteral("/ocr.md"),
        QStringLiteral("Markdown (*.md)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!exporters::writeMarkdown(m_doc, path, &error))
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
}

void MainWindow::exportJson()
{
    if (m_doc.count() == 0)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 JSON"), QDir::homePath() + QStringLiteral("/ocr.json"),
        QStringLiteral("JSON (*.json)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!exporters::writeJson(m_doc, path, &error))
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
}

QVector<OutlineEntry> MainWindow::outlineForExport() const
{
    const QVector<OutlineEntry> &builtin = m_doc.outline();
    const bool hasBoth = !builtin.isEmpty() && !m_customOutline.isEmpty();
    const bool replace = hasBoth && m_replaceOutline && m_replaceOutline->isChecked();

    QVector<OutlineEntry> result;
    if (!replace)
        result += builtin;
    if (!m_customOutline.isEmpty()) {
        if (replace || builtin.isEmpty()) {
            result += m_customOutline;
        } else {
            OutlineEntry root;
            root.title = m_customOutlineLabel;
            root.page = -1;
            root.children = m_customOutline;
            result.append(root);
        }
    }
    return result;
}

void MainWindow::exportSearchablePdf()
{
    if (m_doc.count() == 0)
        return;

    int pagesWithText = 0;
    for (int i = 0; i < m_doc.count(); ++i) {
        const PageEntry &entry = m_doc.page(i);
        if (entry.result.ok()
            && (!entry.result.blocks.isEmpty()
                || !ocr::plainText(entry.result.payload, entry.result.purpose).trimmed().isEmpty()))
            ++pagesWithText;
    }
    if (pagesWithText == 0) {
        QMessageBox::information(
            this, QStringLiteral("没有可嵌入的文本"),
            QStringLiteral("请先识别至少一页，再导出可搜索 PDF。"));
        return;
    }

    QString base = QStringLiteral("ocr");
    if (!m_doc.sourcePath().isEmpty()) {
        const QFileInfo info(m_doc.sourcePath());
        const QString name = info.isDir() ? info.fileName() : info.completeBaseName();
        if (!name.isEmpty())
            base = name;
    }
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出可搜索 PDF"),
        QDir::homePath() + QLatin1Char('/') + base + QStringLiteral("-ocr.pdf"),
        QStringLiteral("PDF (*.pdf)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
        path += QStringLiteral(".pdf");

    QProgressDialog progress(QStringLiteral("正在写入文本层和大纲…"),
                             QStringLiteral("取消"), 0, m_doc.count(), this);
    progress.setWindowTitle(QStringLiteral("导出可搜索 PDF"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.show();

    QString error;
    bool incremental = false;
    const bool ok = pdfexport::writeSearchablePdf(
        m_doc, outlineForExport(), path, &error, [&](int done, int total) {
            progress.setMaximum(total);
            progress.setValue(done);
            QCoreApplication::processEvents();
            return !progress.wasCanceled();
        }, &incremental);
    progress.close();

    if (!ok) {
        if (error == QStringLiteral("已取消")) {
            statusBar()->showMessage(QStringLiteral("已取消导出"), 3000);
        } else {
            QMessageBox::warning(this, QStringLiteral("导出失败"),
                                 error.isEmpty() ? QStringLiteral("无法写入 PDF") : error);
        }
        return;
    }
    statusBar()->showMessage(
        incremental ? QStringLiteral("已增量导出可搜索 PDF：%1").arg(path)
                    : QStringLiteral("已导出可搜索 PDF（完整重写）：%1").arg(path),
        8000);
}

void MainWindow::copyText()
{
    QApplication::clipboard()->setText(m_textView->toPlainText());
    statusBar()->showMessage(QStringLiteral("已复制本页文本"), 2000);
}

bool MainWindow::copyBlockImage(int page, int block)
{
    if (page < 0 || page >= m_doc.count())
        return false;
    const PageEntry &entry = m_doc.page(page);
    if (block < 0 || block >= entry.result.blocks.size()
        || !ocr::isImageKind(entry.result.blocks.at(block).kind))
        return false;
    const QImage image = m_doc.blockImage(page, block);
    if (image.isNull()) {
        statusBar()->showMessage(QStringLiteral("无法读取图片区域"), 3000);
        return false;
    }
    QApplication::clipboard()->setImage(image);
    statusBar()->showMessage(
        QStringLiteral("已复制图片 · %1×%2").arg(image.width()).arg(image.height()), 2500);
    return true;
}

void MainWindow::exportBlockImage(int page, int block)
{
    if (page < 0 || page >= m_doc.count())
        return;
    const PageEntry &entry = m_doc.page(page);
    if (block < 0 || block >= entry.result.blocks.size()
        || !ocr::isImageKind(entry.result.blocks.at(block).kind))
        return;

    QString base = QStringLiteral("image");
    if (!entry.sourcePath.isEmpty()) {
        const QFileInfo info(entry.sourcePath);
        const QString name = info.completeBaseName();
        if (!name.isEmpty())
            base = name;
    }
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 PNG"),
        QDir::homePath() + QLatin1Char('/') + base
            + QStringLiteral("-p%1-b%2.png").arg(page + 1).arg(block + 1),
        QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        path += QStringLiteral(".png");

    const QImage image = m_doc.blockImage(page, block);
    if (image.isNull()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法读取该图片区域"));
        return;
    }
    if (!image.save(path, "PNG")) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法写入图片：%1").arg(path));
        return;
    }
    statusBar()->showMessage(QStringLiteral("已导出 PNG：%1").arg(path), 5000);
}

void MainWindow::copySelection()
{
    if (auto *edit = qobject_cast<QPlainTextEdit *>(QApplication::focusWidget())) {
        if (edit->textCursor().hasSelection()) {
            edit->copy();
            statusBar()->showMessage(QStringLiteral("已复制所选文字"), 2000);
            return;
        }
    }
    if (m_view->hasTextSelection() || !m_view->selection().isEmpty()) {
        if (m_view->copySelection() > 0)
            return;
    }
    if (m_view->tool() == PageView::Tool::Text) {
        statusBar()->showMessage(QStringLiteral("先用鼠标拖选一段文本"), 2500);
        return;
    }
    copyText();
}

void MainWindow::showSelectionStatus()
{
    const QVector<int> selection = m_view->selection();
    if (selection.isEmpty()) {
        m_selLabel->hide();
        return;
    }
    int imageBlocks = 0;
    const int page = m_view->currentPage();
    if (page >= 0 && page < m_doc.count()) {
        const auto &blocks = m_doc.page(page).result.blocks;
        for (int index : selection) {
            if (index >= 0 && index < blocks.size() && ocr::isImageKind(blocks.at(index).kind))
                ++imageBlocks;
        }
    }
    if (imageBlocks == selection.size())
        m_selLabel->setText(QStringLiteral("已选 %1 个图片块   ").arg(imageBlocks));
    else
        m_selLabel->setText(QStringLiteral("已选 %1 个块 · %2 字   ")
                                .arg(selection.size())
                                .arg(m_view->selectedText().size()));
    m_selLabel->show();
}

void MainWindow::syncCardsFromView()
{
    if (m_syncing)
        return;
    m_syncing = true;
    const QVector<int> selection = m_view->selection();
    m_blocks->setSelectedBlocks(selection);
    m_syncing = false;

    if (!selection.isEmpty()) {
        m_tabs->setCurrentWidget(m_blocks);
        m_blocks->scrollToBlock(selection.first());
    }
    showSelectionStatus();
}

void MainWindow::syncViewFromCards()
{
    if (m_syncing)
        return;
    const QVector<int> selection = m_blocks->selectedBlocks();
    m_syncing = true;
    m_view->setSelection(selection, false);
    m_syncing = false;
    if (!selection.isEmpty())
        m_view->ensureBlockVisible(selection.first());
    showSelectionStatus();
}

void MainWindow::setTextTool(bool on)
{
    m_view->setTool(on ? PageView::Tool::Text : PageView::Tool::Pointer);
    if (on)
        statusBar()->showMessage(
            QStringLiteral("文本选择：按住左键拖动，可像文档一样跨框选中文本；Ctrl+C 复制"),
            4000);
}

void MainWindow::showDropHint(bool on)
{
    if (m_dropHint == on)
        return;
    m_dropHint = on;
    if (m_view)
        m_view->setDropActive(on);
}

void MainWindow::injectMockResult(const QString &jsonPath)
{
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("无法读取 mock JSON: %s", qPrintable(jsonPath));
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    ocr::PageResult result;
    result.page = 0;
    result.purpose = m_purpose;
    result.payload = doc.object();
    if (m_purpose == QStringLiteral("rich_layout_recognition")) {
        for (const QJsonValue &value : result.payload.value(QStringLiteral("pages")).toArray())
            result.blocks += ocr::parseBlocks(
                value.toObject().value(QStringLiteral("text")).toString());
    }
    if (m_doc.count() > 0)
        m_doc.page(0).result = result;
    refreshPageList(0);
    m_view->refreshPage(0);
    showPage(0);
    updateStats();
}

void MainWindow::refreshPageList(int index)
{
    if (auto *item = m_pageList->item(index))
        item->setText(QStringLiteral("%1 ✓").arg(index + 1));
}

void MainWindow::selectBlocks(const QVector<int> &blocks)
{
    m_view->setSelection(blocks);
    syncCardsFromView();
    showBlocksTab();
}

void MainWindow::selectTextRange(int begin, int end)
{
    if (m_textToolAction && !m_textToolAction->isChecked())
        m_textToolAction->setChecked(true);
    m_view->selectTextRange(begin, end);
}

void MainWindow::showBlocksTab()
{
    if (m_tabs && m_blocks)
        m_tabs->setCurrentWidget(m_blocks);
}

void MainWindow::setDropHintVisible(bool on)
{
    showDropHint(on);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!hasLocalFiles(event->mimeData())) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    showDropHint(true);
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (!hasLocalFiles(event->mimeData())) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event)
{
    showDropHint(false);
    QMainWindow::dragLeaveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    showDropHint(false);
    const QStringList paths = localPaths(event->mimeData());
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    openPaths(paths);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != this)
        return QMainWindow::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *drag = static_cast<QDragEnterEvent *>(event);
        if (!hasLocalFiles(drag->mimeData()))
            break;
        drag->setDropAction(Qt::CopyAction);
        drag->accept();
        showDropHint(true);
        return true;
    }
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        if (!hasLocalFiles(drag->mimeData()))
            break;
        drag->setDropAction(Qt::CopyAction);
        drag->accept();
        return true;
    }
    case QEvent::DragLeave:
        showDropHint(false);
        break;
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        if (!hasLocalFiles(drop->mimeData()))
            break;
        showDropHint(false);
        const QStringList paths = localPaths(drop->mimeData());
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        if (!paths.isEmpty())
            openPaths(paths);
        return true;
    }
    default:
        break;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_queue.stop();
    QMainWindow::closeEvent(event);
}

void MainWindow::applyTheme()
{
    if (m_applyingTheme)
        return;
    m_applyingTheme = true;

    qApp->setStyleSheet(theme::styleSheet(qApp->palette()));
    for (const auto &entry : m_themedActions)
        entry.first->setIcon(theme::icon(entry.second));

    if (m_view)
        m_view->updateBackground();
    if (m_blocks)
        m_blocks->viewport()->update();

    m_applyingTheme = false;
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::PaletteChange)
        applyTheme();
    QMainWindow::changeEvent(event);
}
