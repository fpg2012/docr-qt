#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QDateTime>
#include <QClipboard>
#include <QCollator>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextStream>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QUrl>
#include <QAbstractItemView>
#include <QPalette>

#include <algorithm>

#include "blockcards.h"
#include "pageview.h"
#include "theme.h"

namespace {

const char *kImageFilter =
    "图片 (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.tif *.tiff)";

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

// 页面是否有墨迹：用于区分“空白页”和“服务端返回空结果”。
bool pageHasInk(const QImage &image)
{
    if (image.isNull())
        return false;
    const QImage small = image.scaled(200, 200, Qt::KeepAspectRatio, Qt::FastTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    int dark = 0;
    for (int y = 0; y < small.height(); ++y) {
        const uchar *line = small.constScanLine(y);
        for (int x = 0; x < small.width(); ++x) {
            if (line[x] < 100)
                ++dark;
        }
    }
    return dark * 100 > small.width() * small.height(); // 超过 1% 像素是深色
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_client(this)
{
    buildUi();
    buildActions();
    applySettings();
    updatePageNav();
    updatePanels();
    updateStats();

    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setInterval(15);
    connect(m_thumbTimer, &QTimer::timeout, this, &MainWindow::ensureThumbs);

    connect(&m_client, &ocr::Client::finished, this, &MainWindow::onResult);

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

    // 左侧：页面缩略图
    auto *leftDock = new QDockWidget(QStringLiteral("页面"), this);
    leftDock->setObjectName(QStringLiteral("pagesDock"));
    m_pageList = new QListWidget(leftDock);
    m_pageList->setViewMode(QListView::IconMode);
    m_pageList->setIconSize(QSize(120, 160));
    m_pageList->setGridSize(QSize(150, 205));
    m_pageList->setResizeMode(QListView::Adjust);
    m_pageList->setMovement(QListView::Static);
    m_pageList->setUniformItemSizes(true);
    m_pageList->setWordWrap(true);
    m_pageList->setSpacing(4);
    leftDock->setWidget(m_pageList);
    leftDock->setMinimumWidth(180);
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
    m_progress = new QProgressBar(this);
    m_progress->setMaximumWidth(180);
    m_progress->setVisible(false);
    m_selLabel = new QLabel(this);
    m_selLabel->hide();
    m_hintLabel = new QLabel(
        QStringLiteral("拖入 PDF / 图片 / 文件夹 · Ctrl+滚轮缩放 · PgUp/PgDn 翻页"), this);
    m_hintLabel->setForegroundRole(QPalette::PlaceholderText);
    statusBar()->addWidget(m_statsLabel, 1);
    statusBar()->addPermanentWidget(m_selLabel);
    statusBar()->addPermanentWidget(m_hintLabel);
    statusBar()->addPermanentWidget(m_progress);

    connect(m_pageList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            showPage(row);
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
    connect(m_view, &PageView::textCopied, this, [this](int blocks, int chars) {
        statusBar()->showMessage(
            QStringLiteral("已复制 %1 个块 · %2 字").arg(blocks).arg(chars), 2500);
    });
    connect(m_view, &PageView::pageStepRequested, this, [this](int delta) { stepPage(delta); });
    connect(m_view, &PageView::blockClicked, this, [this](int index) {
        if (m_view->selection().contains(index))
            m_blocks->scrollToBlock(index);
    });
    connect(m_blocks, &BlockCardList::cardsSelectionChanged, this,
            [this] { syncViewFromCards(); });
    connect(m_blocks, &BlockCardList::copyRequested, this, [this] { copySelection(); });
    connect(m_blocks, &BlockCardList::blockActivated, this, [this](int index) {
        m_view->ensureBlockVisible(index);
    });
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

    auto *copySelectionAction = new QAction(QStringLiteral("复制选中文本"), this);
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
    helpMenu->addAction(QStringLiteral("关于 docr-qt"), this, [this] { showAbout(); });
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
    m_doc.setDpi(m_dpi);
    m_client.configure(m_baseUrl, m_apiKey);
    if (m_purposeBox) {
        const int index = m_purposeBox->findData(m_purpose);
        if (index >= 0) {
            QSignalBlocker blocker(m_purposeBox);
            m_purposeBox->setCurrentIndex(index);
        }
    }
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("baseUrl"), m_baseUrl);
    settings.setValue(QStringLiteral("dpi"), m_dpi);
    settings.setValue(QStringLiteral("concurrency"), m_concurrency);
    settings.setValue(QStringLiteral("purpose"), m_purpose);
    if (settings.value(QStringLiteral("rememberKey"), false).toBool())
        settings.setValue(QStringLiteral("apiKey"), m_apiKey);
    m_client.configure(m_baseUrl, m_apiKey);
    m_doc.setDpi(m_dpi);
}

void MainWindow::openPreferences()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("首选项"));
    dialog.resize(560, 0);

    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);

    // —— API ——
    auto *apiPage = new QWidget(tabs);
    auto *apiForm = new QFormLayout(apiPage);

    auto *keyEdit = new QLineEdit(m_apiKey, apiPage);
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setPlaceholderText(QStringLiteral("留空则使用 DEEPSEEK_API_KEY / DSH 凭据库"));
    auto *showKey = new QCheckBox(QStringLiteral("显示"), apiPage);
    auto *keyRow = new QWidget(apiPage);
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->addWidget(keyEdit, 1);
    keyLayout->addWidget(showKey);
    connect(showKey, &QCheckBox::toggled, keyEdit, [keyEdit](bool on) {
        keyEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });

    auto *remember = new QCheckBox(QStringLiteral("保存到本机设置"), apiPage);
    remember->setChecked(QSettings().value(QStringLiteral("rememberKey"), false).toBool());

    auto *useDsh = new QPushButton(QStringLiteral("载入 DSH 凭据库的 Key"), apiPage);
    connect(useDsh, &QPushButton::clicked, this, [this, keyEdit] {
        const QString key = ocr::dshApiKey();
        if (key.isEmpty())
            QMessageBox::information(this, QStringLiteral("没有找到 Key"),
                                     QStringLiteral("环境变量和 ~/.dsh/.credentials.yaml 中都没有 Key。"));
        else
            keyEdit->setText(key);
    });

    auto *keyHint = new QLabel(
        QStringLiteral("留空时依次使用 DEEPSEEK_API_KEY、DSH 凭据库"), apiPage);
    keyHint->setWordWrap(true);
    keyHint->setForegroundRole(QPalette::PlaceholderText);

    auto *baseEdit = new QLineEdit(m_baseUrl, apiPage);
    baseEdit->setPlaceholderText(QStringLiteral("https://api.deepseek.com/v1/tools"));

    apiForm->addRow(QStringLiteral("API Key"), keyRow);
    apiForm->addRow(QString(), remember);
    apiForm->addRow(QString(), useDsh);
    apiForm->addRow(QString(), keyHint);
    apiForm->addRow(QStringLiteral("接口地址"), baseEdit);

    // —— 识别 ——
    auto *ocrPage = new QWidget(tabs);
    auto *ocrForm = new QFormLayout(ocrPage);
    auto *purposeCombo = new QComboBox(ocrPage);
    purposeCombo->addItem(QStringLiteral("版面分析（带坐标框）"),
                          QStringLiteral("rich_layout_recognition"));
    purposeCombo->addItem(QStringLiteral("纯文本（更快）"), QStringLiteral("text_recognition"));
    const int purposeIndex = purposeCombo->findData(m_purpose);
    if (purposeIndex >= 0)
        purposeCombo->setCurrentIndex(purposeIndex);

    auto *dpiSpin = new QSpinBox(ocrPage);
    dpiSpin->setRange(72, 400);
    dpiSpin->setValue(m_dpi);
    dpiSpin->setSuffix(QStringLiteral(" dpi"));
    auto *concSpin = new QSpinBox(ocrPage);
    concSpin->setRange(1, 8);
    concSpin->setValue(m_concurrency);
    concSpin->setToolTip(QStringLiteral("同时在途的 OCR 请求数；账号上限 15"));

    auto *ocrHint = new QLabel(
        QStringLiteral("「版面分析」返回带坐标的识别框；「纯文本」更快但没有框。"), ocrPage);
    ocrHint->setWordWrap(true);
    ocrHint->setForegroundRole(QPalette::PlaceholderText);

    ocrForm->addRow(QStringLiteral("识别模式"), purposeCombo);
    ocrForm->addRow(QStringLiteral("渲染精度"), dpiSpin);
    ocrForm->addRow(QStringLiteral("并发数"), concSpin);
    ocrForm->addRow(QString(), ocrHint);

    tabs->addTab(apiPage, QStringLiteral("API"));
    tabs->addTab(ocrPage, QStringLiteral("识别"));
    layout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *reset = buttons->addButton(QStringLiteral("恢复默认"), QDialogButtonBox::ResetRole);
    connect(reset, &QPushButton::clicked, &dialog,
            [keyEdit, remember, baseEdit, purposeCombo, dpiSpin, concSpin] {
        keyEdit->clear();
        remember->setChecked(false);
        baseEdit->setText(QStringLiteral("https://api.deepseek.com/v1/tools"));
        purposeCombo->setCurrentIndex(0);
        dpiSpin->setValue(150);
        concSpin->setValue(3);
    });
    layout->addWidget(buttons);
    dialog.adjustSize();
    dialog.resize(600, dialog.height());
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    m_apiKey = keyEdit->text().trimmed();
    if (m_apiKey.isEmpty())
        m_apiKey = ocr::dshApiKey();
    m_baseUrl = baseEdit->text().trimmed();
    if (m_baseUrl.isEmpty())
        m_baseUrl = QStringLiteral("https://api.deepseek.com/v1/tools");
    m_dpi = dpiSpin->value();
    m_concurrency = concSpin->value();
    m_purpose = purposeCombo->currentData().toString();

    QSettings settings;
    settings.setValue(QStringLiteral("rememberKey"), remember->isChecked());
    if (remember->isChecked())
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
    }
    statusBar()->showMessage(QStringLiteral("首选项已保存"), 2500);
}

void MainWindow::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("关于 docr-qt"));
    box.setIconPixmap(theme::appIcon().pixmap(72, 72));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral(
                    "<h3>docr-qt %1</h3>"
                    "<p>DeepSeek OCR 的 Qt 6 桌面客户端。<br>"
                    "拖入 PDF / 图片，逐页识别并把版面框叠加在页面上；"
                    "结果可复制、导出 Markdown / JSON。</p>"
                    "<p><b>Qt</b> %2<br>"
                    "<b>接口</b> %3<br>"
                    "<b>识别模式</b> %4</p>"
                    "<p>识别结果只保存在本机内存，导出时才写文件。</p>")
                    .arg(QStringLiteral(DOCR_VERSION), QString::fromLatin1(qVersion()),
                         m_baseUrl.toHtmlEscaped(),
                         m_purpose == QStringLiteral("text_recognition")
                             ? QStringLiteral("纯文本")
                             : QStringLiteral("版面分析")));
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

bool MainWindow::loadPath(const QString &path)
{
    openPaths({path});
    return m_doc.count() > 0;
}

void MainWindow::openPaths(const QStringList &paths)
{
    QStringList files;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QDir dir(path);
            QStringList names = dir.entryList(Document::imageNameFilters(), QDir::Files,
                                              QDir::NoSort);
            QCollator collator;
            collator.setNumericMode(true);
            std::sort(names.begin(), names.end(),
                      [&collator](const QString &a, const QString &b) {
                          return collator.compare(a, b) < 0;
                      });
            for (const QString &name : names)
                files << dir.filePath(name);
        } else if (info.isFile()) {
            files << info.absoluteFilePath();
        }
    }

    if (files.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("没有可打开的 PDF / 图片"), 4000);
        return;
    }

    m_generation++;
    m_pending.clear();
    m_running.clear();
    m_emptyRetries.clear();
    m_batchMode = false;
    m_allDoneEmitted = true;
    if (m_stopAction)
        m_stopAction->setEnabled(false);

    m_doc.clear();
    m_pageList->clear();
    m_view->clearPage();
    m_pageImage = QImage();
    m_blocks->setBlocks({});
    m_textView->clear();
    m_jsonView->clear();

    QStringList errors;
    for (const QString &file : files) {
        QString error;
        if (QFileInfo(file).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) {
            if (!m_doc.addPdf(file, m_dpi, &error))
                errors << error;
        } else if (!m_doc.addImage(file, &error)) {
            errors << error;
        }
    }

    if (m_doc.count() == 0) {
        QMessageBox::warning(this, QStringLiteral("打开失败"),
                             errors.isEmpty() ? QStringLiteral("没有可显示的页面")
                                              : errors.join(QLatin1Char('\n')));
        setWindowTitle(QStringLiteral("docr-qt"));
        updatePanels();
        updatePageNav();
        updateStats();
        return;
    }

    addPagesToUi();
    const QString title =
        files.size() == 1
            ? QFileInfo(files.first()).fileName()
            : QStringLiteral("%1 个文件 · %2 页").arg(files.size()).arg(m_doc.count());
    setWindowTitle(QStringLiteral("docr-qt — %1").arg(title));
    if (!errors.isEmpty())
        statusBar()->showMessage(QStringLiteral("有 %1 个文件无法打开").arg(errors.size()), 5000);
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
    m_pageList->clear();
    for (int i = 0; i < m_doc.count(); ++i) {
        auto *item = new QListWidgetItem(m_pageList);
        item->setText(QString::number(i + 1));
        item->setToolTip(m_doc.page(i).label);
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(140, 190));
    }
    m_thumbTimer->start();
    if (m_doc.count() > 0)
        m_pageList->setCurrentRow(0);
    updateStats();
}

void MainWindow::ensureThumbs()
{
    for (int i = 0; i < m_doc.count(); ++i) {
        if (m_doc.page(i).thumb.isNull()) {
            const QPixmap thumb = m_doc.ensureThumb(i);
            if (auto *item = m_pageList->item(i))
                item->setIcon(QIcon(thumb));
            return; // 一次一张，保持界面响应
        }
    }
    m_thumbTimer->stop();
}

void MainWindow::showPage(int index)
{
    if (index < 0 || index >= m_doc.count())
        return;
    const QImage image = m_doc.ensureImage(index);
    m_pageImage = image;
    const PageEntry &entry = m_doc.page(index);
    const bool rich = entry.result.ok()
                      && entry.result.purpose == QStringLiteral("rich_layout_recognition");
    const QVector<ocr::Block> noBlocks;
    m_view->setPage(image, rich ? entry.result.blocks : noBlocks, m_showBoxes);
    updatePanels();
    updatePageNav();
}

void MainWindow::goToPage(int index)
{
    if (m_doc.count() == 0)
        return;
    index = qBound(0, index, m_doc.count() - 1);
    if (m_pageList->currentRow() == index) {
        showPage(index);
        return;
    }
    m_pageList->setCurrentRow(index);
    if (auto *item = m_pageList->item(index))
        m_pageList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
}

void MainWindow::stepPage(int delta)
{
    if (m_doc.count() == 0)
        return;
    const int current = m_pageList->currentRow();
    goToPage((current < 0 ? 0 : current) + delta);
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
        if (entry.result.purpose != QStringLiteral("rich_layout_recognition")) {
            m_blocks->showPlaceholder(
                QStringLiteral("当前模式只返回纯文本\n改用「版面分析」可得到带坐标的块"));
        } else if (!ocr::hasText(entry.result.payload)) {
            if (!pageHasInk(m_pageImage)) {
                m_textView->setPlaceholderText(QStringLiteral("本页没有可识别的文字"));
                m_blocks->showPlaceholder(QStringLiteral("空白页（没有文字）"));
            } else {
                m_textView->setPlaceholderText(
                    QStringLiteral("服务端未返回文字；可点「识别本页」重试"));
                m_blocks->showPlaceholder(
                    m_emptyRetries.contains(index)
                        ? QStringLiteral("服务端未返回文字\n已自动重试一次，可再点「识别本页」")
                        : QStringLiteral("服务端未返回文字\n点「识别本页」重试"));
            }
        } else {
            m_blocks->showPlaceholder(QStringLiteral("本页没有可显示的版面块"));
        }
    }
    m_selLabel->hide();
}

void MainWindow::updateStats()
{
    int done = 0, failed = 0, emptyPages = 0, tokens = 0;
    double seconds = 0.0;
    for (int i = 0; i < m_doc.count(); ++i) {
        const PageEntry &entry = m_doc.page(i);
        if (entry.result.ok()) {
            ++done;
            tokens += entry.result.usage().value(QStringLiteral("total_tokens")).toInt();
            seconds += entry.result.elapsedMs / 1000.0;
            if (entry.result.purpose == QStringLiteral("rich_layout_recognition")
                && entry.result.blocks.isEmpty() && !ocr::hasText(entry.result.payload)) {
                ++emptyPages;
            }
        } else if (!entry.result.error.isEmpty()) {
            ++failed;
        }
    }
    const double cost = done * 0.0015;
    QString text = m_doc.count() == 0
                       ? QStringLiteral("就绪")
                       : QStringLiteral("%1/%2 页 · %3s · %4 tokens · ≈¥%5")
                             .arg(done).arg(m_doc.count())
                             .arg(seconds, 0, 'f', 1)
                             .arg(tokens)
                             .arg(cost, 0, 'f', 4);
    if (failed > 0)
        text += QStringLiteral(" · %1 页失败").arg(failed);
    if (emptyPages > 0)
        text += QStringLiteral(" · %1 页空结果").arg(emptyPages);
    const bool busy = !m_pending.isEmpty() || !m_running.isEmpty();
    if (busy)
        text += QStringLiteral(" · 识别中");
    m_statsLabel->setText(text);
    m_progress->setVisible(busy);
    if (busy && !m_batchMode) {
        // 单页请求：用忙碌指示器，避免进度条看起来停在原地。
        m_progress->setRange(0, 0);
    } else {
        m_progress->setRange(0, qMax(1, m_doc.count()));
        m_progress->setValue(done);
    }
}

void MainWindow::startOcrAll()
{
    if (m_doc.count() == 0) {
        statusBar()->showMessage(QStringLiteral("先打开文档"), 3000);
        return;
    }
    QVector<int> pages;
    for (int i = 0; i < m_doc.count(); ++i) {
        if (!m_doc.page(i).hasResult())
            pages.append(i);
    }
    if (pages.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("所有页面都已识别"), 3000);
        return;
    }
    m_batchMode = true;
    m_emptyRetries.clear();
    statusBar()->showMessage(QStringLiteral("开始识别 %1 页…").arg(pages.size()), 3000);
    enqueuePages(pages);
}

void MainWindow::startOcrCurrent()
{
    const int index = m_pageList->currentRow();
    if (index < 0 || index >= m_doc.count()) {
        statusBar()->showMessage(QStringLiteral("先打开文档"), 3000);
        return;
    }

    m_batchMode = false;
    m_emptyRetries.remove(index);
    m_allDoneEmitted = false;
    if (m_stopAction)
        m_stopAction->setEnabled(true);

    if (m_running.contains(index)) {
        statusBar()->showMessage(
            QStringLiteral("第 %1 页正在识别中，请稍候…").arg(index + 1), 2000);
        return;
    }

    // 立刻给出反馈，避免网络请求期间看起来“没反应”。
    m_doc.page(index).result = ocr::PageResult();
    if (auto *item = m_pageList->item(index)) {
        item->setText(QStringLiteral("%1 ⏳").arg(index + 1));
        item->setForeground(theme::tone(QColor(245, 158, 11)));
        item->setToolTip(m_doc.page(index).label);
    }
    if (m_pending.contains(index)) {
        // 已经在队列里：提到队首，稍后马上处理。
        QQueue<int> reordered;
        reordered.enqueue(index);
        for (int pending : m_pending) {
            if (pending != index)
                reordered.enqueue(pending);
        }
        m_pending = reordered;
    } else {
        m_pending.enqueue(index);
    }
    statusBar()->showMessage(QStringLiteral("正在识别第 %1 页…").arg(index + 1), 2000);
    pumpQueue();
    updateStats();
}

void MainWindow::enqueuePages(const QVector<int> &pages)
{
    if (pages.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("没有需要识别的页面"), 3000);
        return;
    }
    m_allDoneEmitted = false;
    if (m_stopAction)
        m_stopAction->setEnabled(true);
    for (int page : pages) {
        if (!m_pending.contains(page) && !m_running.contains(page))
            m_pending.enqueue(page);
    }
    pumpQueue();
    updateStats();
}

void MainWindow::pumpQueue()
{
    while (m_running.size() < m_concurrency && !m_pending.isEmpty()) {
        const int page = m_pending.dequeue();
        if (m_running.contains(page))
            continue;
        m_running.insert(page);
        if (auto *item = m_pageList->item(page)) {
            item->setText(QStringLiteral("%1 ⏳").arg(page + 1));
            item->setForeground(theme::tone(QColor(245, 158, 11)));
        }
        encodeAndSubmit(page);
    }
    if (m_pending.isEmpty() && m_running.isEmpty() && !m_allDoneEmitted) {
        m_allDoneEmitted = true;
        if (m_stopAction)
            m_stopAction->setEnabled(false);
        updateStats();
        emit allFinished();
    }
}

void MainWindow::encodeAndSubmit(int page)
{
    const QImage image = m_doc.ensureImage(page);
    if (image.isNull()) {
        ocr::PageResult result;
        result.page = page;
        result.purpose = m_purpose;
        result.error = QStringLiteral("渲染失败");
        onResult(page, m_generation, result);
        return;
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    const bool saved = image.save(&buffer, "PNG");
    Q_UNUSED(saved);
    const QString name = QStringLiteral("page-%1.png").arg(page + 1);
    m_client.submit(page, m_generation, bytes, name, m_purpose);
}

void MainWindow::onResult(int page, int generation, const ocr::PageResult &result)
{
    if (generation != m_generation)
        return;
    m_running.remove(page);
    if (page < 0 || page >= m_doc.count()) {
        pumpQueue();
        return;
    }

    const bool rich = result.purpose == QStringLiteral("rich_layout_recognition");
    const bool empty = result.ok() && rich && result.blocks.isEmpty()
                       && !ocr::hasText(result.payload);
    const bool hasInk = pageHasInk(m_doc.ensureImage(page));

    // 页面明明有内容、服务端却没返回任何文字：多半是偶发的空响应，自动重试一次。
    if (empty && !m_emptyRetries.contains(page) && hasInk) {
        m_emptyRetries.insert(page);
        m_doc.page(page).result = ocr::PageResult();
        if (auto *item = m_pageList->item(page)) {
            item->setText(QStringLiteral("%1 ↻").arg(page + 1));
            item->setForeground(theme::tone(QColor(245, 158, 11)));
            item->setToolTip(QStringLiteral("%1\n服务端返回空结果，正在自动重试")
                                 .arg(m_doc.page(page).label));
        }
        statusBar()->showMessage(
            QStringLiteral("第 %1 页返回空结果，正在自动重试…").arg(page + 1), 5000);
        m_pending.enqueue(page);
        updateStats();
        pumpQueue();
        return;
    }

    m_doc.page(page).result = result;

    if (auto *item = m_pageList->item(page)) {
        if (!result.ok()) {
            item->setText(QStringLiteral("%1 ✕").arg(page + 1));
            item->setForeground(theme::tone(QColor(239, 68, 68)));
            item->setToolTip(QStringLiteral("%1\n%2").arg(m_doc.page(page).label, result.error));
        } else if (rich && result.blocks.isEmpty() && empty && hasInk) {
            item->setText(QStringLiteral("%1 !").arg(page + 1));
            item->setForeground(theme::tone(QColor(245, 158, 11)));
            item->setToolTip(QStringLiteral("%1\n未返回文字（服务端空结果），可重试")
                                 .arg(m_doc.page(page).label));
        } else if (rich && result.blocks.isEmpty() && empty) {
            // 空白页：没有文字是正常的，不标成异常。
            item->setText(QStringLiteral("%1 ✓").arg(page + 1));
            item->setForeground(theme::tone(QColor(34, 197, 94)));
            item->setToolTip(QStringLiteral("%1\n空白页（无文字）").arg(m_doc.page(page).label));
        } else if (rich && result.blocks.isEmpty()) {
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
            const QString diagnostics = saveEmptyDiagnostics(page, result);
            if (!diagnostics.isEmpty()) {
                message += QStringLiteral("；诊断：%1").arg(diagnostics);
                if (auto *item = m_pageList->item(page)) {
                    item->setToolTip(item->toolTip()
                                     + QStringLiteral("\n诊断：%1").arg(diagnostics));
                }
            }
            statusBar()->showMessage(message, 10000);
        }
    } else if (m_emptyRetries.contains(page)) {
        statusBar()->showMessage(QStringLiteral("第 %1 页重试成功").arg(page + 1), 2500);
    } else if (!result.ok()) {
        statusBar()->showMessage(
            QStringLiteral("第 %1 页识别失败：%2").arg(page + 1).arg(result.error), 5000);
    } else {
        statusBar()->showMessage(
            QStringLiteral("第 %1 页识别完成%2")
                .arg(page + 1)
                .arg(rich ? QStringLiteral(" · %1 个块").arg(result.blocks.size())
                          : QString()),
            2500);
    }
    if (page == m_pageList->currentRow())
        showPage(page);
    updateStats();
    pumpQueue();
}

void MainWindow::stopOcr()
{
    m_generation++;
    m_pending.clear();
    m_running.clear();
    m_emptyRetries.clear();
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
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream out(&file);
    for (int i = 0; i < m_doc.count(); ++i) {
        const PageEntry &entry = m_doc.page(i);
        if (!entry.result.ok())
            continue;
        out << "## " << (i + 1) << "\n\n"
            << ocr::plainText(entry.result.payload, entry.result.purpose) << "\n\n";
    }
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
    QJsonArray pages;
    for (int i = 0; i < m_doc.count(); ++i) {
        const PageEntry &entry = m_doc.page(i);
        if (!entry.result.ok())
            continue;
        QJsonObject wrapper = entry.result.payload;
        wrapper.insert(QStringLiteral("page"), i);
        pages.append(wrapper);
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(pages).toJson(QJsonDocument::Indented));
}

QString MainWindow::saveEmptyDiagnostics(int page, const ocr::PageResult &result)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                        + QStringLiteral("/docr-qt/ocr-empty");
    if (!QDir().mkpath(dir))
        return {};

    const QString stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = QStringLiteral("%1/page-%2-%3").arg(dir).arg(page + 1).arg(stamp);

    QJsonObject meta;
    meta.insert(QStringLiteral("page"), page + 1);
    meta.insert(QStringLiteral("source"), m_doc.page(page).label);
    meta.insert(QStringLiteral("purpose"), result.purpose);
    meta.insert(QStringLiteral("baseUrl"), m_baseUrl);
    meta.insert(QStringLiteral("dpi"), m_dpi);
    meta.insert(QStringLiteral("elapsedMs"), double(result.elapsedMs));
    meta.insert(QStringLiteral("usage"), result.usage());
    meta.insert(QStringLiteral("error"), result.error);
    const QJsonArray pages = result.payload.value(QStringLiteral("pages")).toArray();
    if (!pages.isEmpty())
        meta.insert(QStringLiteral("finish_reason"),
                    pages.first().toObject().value(QStringLiteral("finish_reason")));

    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("meta"), meta);
    wrapper.insert(QStringLiteral("response"), result.payload);
    QFile json(base + QStringLiteral(".json"));
    if (json.open(QIODevice::WriteOnly))
        json.write(QJsonDocument(wrapper).toJson(QJsonDocument::Indented));

    QFile rawFile(base + QStringLiteral(".response.json"));
    if (rawFile.open(QIODevice::WriteOnly) && !result.raw.isEmpty())
        rawFile.write(result.raw);

    const QImage image = m_doc.ensureImage(page);
    if (!image.isNull())
        image.save(base + QStringLiteral(".png"), "PNG");

    return base;
}

void MainWindow::copyText()
{
    QApplication::clipboard()->setText(m_textView->toPlainText());
    statusBar()->showMessage(QStringLiteral("已复制本页文本"), 2000);
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
    if (!m_pending.isEmpty() || !m_running.isEmpty()) {
        m_generation++;
        m_pending.clear();
        m_running.clear();
    }
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
