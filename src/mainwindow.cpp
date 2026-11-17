#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
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
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QUrl>

#include "pageview.h"

namespace {

const char *kImageFilter =
    "图片 (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.tif *.tiff)";

QString kindLabel(const QString &kind)
{
    if (kind == QLatin1String("title")) return QStringLiteral("标题");
    if (kind == QLatin1String("text")) return QStringLiteral("正文");
    if (kind == QLatin1String("table")) return QStringLiteral("表格");
    if (kind.contains(QLatin1String("caption"))) return QStringLiteral("题注");
    if (kind.contains(QLatin1String("figure"))) return QStringLiteral("图片");
    if (kind.contains(QLatin1String("footnote"))) return QStringLiteral("脚注");
    if (kind == QLatin1String("equation")) return QStringLiteral("公式");
    return kind;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_client(this)
{
    buildUi();
    buildActions();
    applySettings();

    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setInterval(15);
    connect(m_thumbTimer, &QTimer::timeout, this, &MainWindow::ensureThumbs);

    connect(&m_client, &ocr::Client::finished, this, &MainWindow::onResult);

    setAcceptDrops(true);
    resize(1480, 940);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
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

    // 右侧：文本 / 块 / 原始 JSON
    auto *rightDock = new QDockWidget(QStringLiteral("识别结果"), this);
    rightDock->setObjectName(QStringLiteral("resultDock"));
    auto *tabs = new QTabWidget(rightDock);

    m_textView = new QPlainTextEdit(tabs);
    m_textView->setReadOnly(true);
    m_textView->setPlaceholderText(QStringLiteral("识别结果会显示在这里"));
    tabs->addTab(m_textView, QStringLiteral("文本"));

    m_blockTable = new QTableWidget(0, 3, tabs);
    m_blockTable->setHorizontalHeaderLabels(
        {QStringLiteral("类型"), QStringLiteral("位置"), QStringLiteral("文本")});
    m_blockTable->horizontalHeader()->setStretchLastSection(true);
    m_blockTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_blockTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_blockTable->verticalHeader()->setVisible(false);
    m_blockTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_blockTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_blockTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tabs->addTab(m_blockTable, QStringLiteral("版面块"));

    m_jsonView = new QPlainTextEdit(tabs);
    m_jsonView->setReadOnly(true);
    m_jsonView->setLineWrapMode(QPlainTextEdit::NoWrap);
    tabs->addTab(m_jsonView, QStringLiteral("JSON"));

    rightDock->setWidget(tabs);
    rightDock->setMinimumWidth(340);
    addDockWidget(Qt::RightDockWidgetArea, rightDock);

    // 状态栏
    m_statsLabel = new QLabel(QStringLiteral("就绪"), this);
    m_progress = new QProgressBar(this);
    m_progress->setMaximumWidth(180);
    m_progress->setVisible(false);
    m_hintLabel = new QLabel(
        QStringLiteral("拖入 PDF / 图片或文件夹 · Ctrl+滚轮缩放"), this);
    m_hintLabel->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
    statusBar()->addWidget(m_statsLabel, 1);
    statusBar()->addPermanentWidget(m_hintLabel);
    statusBar()->addPermanentWidget(m_progress);

    connect(m_pageList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            showPage(row);
    });
    connect(m_blockTable, &QTableWidget::cellClicked, this,
            [this](int row, int) { m_view->selectBlock(row); });
    connect(m_view, &PageView::blockClicked, this, [this](int index) {
        if (index >= 0 && index < m_blockTable->rowCount())
            m_blockTable->selectRow(index);
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
        auto *action = new QAction(QIcon(QStringLiteral(":/icons/%1.svg").arg(icon)), text, this);
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
        m_view->setBoxesVisible(on);
    });

    QAction *fit = make(QStringLiteral("fit-width"), QStringLiteral("适应宽度"),
                        QKeySequence(Qt::Key_F), [this] { m_view->fitWidth(); });
    QAction *zoomIn = make(QStringLiteral("zoom-in"), QStringLiteral("放大"),
                           QKeySequence::ZoomIn, [this] { m_view->zoomBy(1.2); });
    QAction *zoomOut = make(QStringLiteral("zoom-out"), QStringLiteral("缩小"),
                            QKeySequence::ZoomOut, [this] { m_view->zoomBy(1 / 1.2); });
    QAction *actual = make(QStringLiteral("actual-size"), QStringLiteral("原始大小"),
                           QKeySequence(), [this] { m_view->resetZoom(); });
    QAction *settings = make(QStringLiteral("settings"), QStringLiteral("设置"),
                             QKeySequence(QStringLiteral("Ctrl+,")),
                             [this] { openSettings(); });

    toolbar->addAction(openPdf);
    toolbar->addAction(openImage);
    toolbar->addAction(openFolder);
    toolbar->addSeparator();
    toolbar->addAction(ocrPage);
    toolbar->addAction(ocrAll);
    toolbar->addAction(stop);
    toolbar->addSeparator();
    toolbar->addAction(showBoxes);

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

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
    fileMenu->addAction(openPdf);
    fileMenu->addAction(openImage);
    fileMenu->addAction(openFolder);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("导出 Markdown…"), this, [this] { exportMarkdown(); });
    fileMenu->addAction(QStringLiteral("导出 JSON…"), this, [this] { exportJson(); });
    fileMenu->addAction(QStringLiteral("复制本页文本"), QKeySequence::Copy, this,
                        [this] { copyText(); });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("退出"), QKeySequence::Quit, this, [this] { close(); });

    auto *ocrMenu = menuBar()->addMenu(QStringLiteral("识别"));
    ocrMenu->addAction(ocrPage);
    ocrMenu->addAction(ocrAll);
    ocrMenu->addAction(stop);
    ocrMenu->addSeparator();
    ocrMenu->addAction(settings);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
    viewMenu->addAction(fit);
    viewMenu->addAction(zoomIn);
    viewMenu->addAction(zoomOut);
    viewMenu->addAction(actual);
    viewMenu->addSeparator();
    viewMenu->addAction(showBoxes);
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

void MainWindow::openSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("设置"));

    auto *layout = new QFormLayout(&dialog);
    auto *keyEdit = new QLineEdit(m_apiKey, &dialog);
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setPlaceholderText(QStringLiteral("留空则使用 DEEPSEEK_API_KEY / DSH 凭据库"));
    auto *remember = new QCheckBox(QStringLiteral("保存到本机设置"), &dialog);
    remember->setChecked(QSettings().value(QStringLiteral("rememberKey"), false).toBool());
    auto *baseEdit = new QLineEdit(m_baseUrl, &dialog);
    auto *dpiSpin = new QSpinBox(&dialog);
    dpiSpin->setRange(72, 400);
    dpiSpin->setValue(m_dpi);
    dpiSpin->setSuffix(QStringLiteral(" dpi"));
    auto *concSpin = new QSpinBox(&dialog);
    concSpin->setRange(1, 8);
    concSpin->setValue(m_concurrency);
    concSpin->setToolTip(QStringLiteral("同时在途的 OCR 请求数；账号上限 15"));

    layout->addRow(QStringLiteral("API Key"), keyEdit);
    layout->addRow(QString(), remember);
    layout->addRow(QStringLiteral("接口地址"), baseEdit);
    layout->addRow(QStringLiteral("渲染精度"), dpiSpin);
    layout->addRow(QStringLiteral("并发数"), concSpin);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    m_apiKey = keyEdit->text().trimmed();
    if (m_apiKey.isEmpty())
        m_apiKey = ocr::dshApiKey();
    m_baseUrl = baseEdit->text().trimmed();
    m_dpi = dpiSpin->value();
    m_concurrency = concSpin->value();
    QSettings().setValue(QStringLiteral("rememberKey"), remember->isChecked());
    saveSettings();
    m_client.configure(m_baseUrl, m_apiKey);
    statusBar()->showMessage(QStringLiteral("设置已保存"), 2500);
}

bool MainWindow::loadPath(const QString &path)
{
    const QFileInfo info(path);
    if (info.isDir())
        return m_doc.addImageDir(path) > 0 && (addPagesToUi(), true);
    if (info.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) {
        QString error;
        if (!m_doc.openPdf(path, m_dpi, &error)) {
            QMessageBox::warning(this, QStringLiteral("打开失败"), error);
            return false;
        }
        addPagesToUi();
        setWindowTitle(QStringLiteral("docr-qt — %1").arg(info.fileName()));
        return true;
    }
    if (m_doc.isPdf() || m_doc.count() == 0)
        m_doc.clear();
    QString error;
    if (!m_doc.addImage(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), error);
        return false;
    }
    addPagesToUi();
    setWindowTitle(QStringLiteral("docr-qt — %1").arg(info.fileName()));
    return true;
}

void MainWindow::openFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("打开 PDF 或图片"), QDir::homePath(),
        QStringLiteral("支持的格式 (*.pdf *.png *.jpg *.jpeg *.webp *.gif *.bmp *.tif *.tiff);;PDF (*.pdf);;") + kImageFilter);
    if (files.isEmpty())
        return;
    if (files.size() == 1) {
        loadPath(files.first());
        return;
    }
    m_doc.clear();
    for (const QString &file : files)
        loadPath(file);
}

void MainWindow::openDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择图片序列目录"));
    if (dir.isEmpty())
        return;
    m_doc.clear();
    if (m_doc.addImageDir(dir) == 0) {
        QMessageBox::information(this, QStringLiteral("没有图片"),
                                 QStringLiteral("该目录下没有支持的图片"));
        return;
    }
    addPagesToUi();
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

void MainWindow::resetDocument(const QString &title)
{
    m_generation++;
    m_pending.clear();
    m_running.clear();
    m_doc.clear();
    m_pageList->clear();
    m_view->clearPage();
    m_blockTable->setRowCount(0);
    m_textView->clear();
    m_jsonView->clear();
    setWindowTitle(QStringLiteral("docr-qt — %1").arg(title));
}

void MainWindow::showPage(int index)
{
    if (index < 0 || index >= m_doc.count())
        return;
    const QImage image = m_doc.ensureImage(index);
    const PageEntry &entry = m_doc.page(index);
    const QVector<ocr::Block> empty;
    const bool rich = entry.result.ok()
                      && entry.result.purpose == QStringLiteral("rich_layout_recognition");
    m_view->setPage(image, rich ? entry.result.blocks : empty, true);
    updatePanels();
}

void MainWindow::updatePanels()
{
    const int index = m_pageList->currentRow();
    if (index < 0 || index >= m_doc.count()) {
        m_textView->clear();
        m_jsonView->clear();
        m_blockTable->setRowCount(0);
        return;
    }
    const PageEntry &entry = m_doc.page(index);
    if (!entry.result.ok()) {
        m_textView->setPlainText(entry.result.error.isEmpty()
                                     ? QStringLiteral("尚未识别")
                                     : QStringLiteral("识别失败：%1").arg(entry.result.error));
        m_jsonView->clear();
        m_blockTable->setRowCount(0);
        return;
    }

    m_textView->setPlainText(ocr::plainText(entry.result.payload, entry.result.purpose));
    m_jsonView->setPlainText(QString::fromUtf8(
        QJsonDocument(entry.result.payload).toJson(QJsonDocument::Indented)));

    const QVector<ocr::Block> &blocks = entry.result.blocks;
    m_blockTable->setRowCount(blocks.size());
    for (int i = 0; i < blocks.size(); ++i) {
        const ocr::Block &block = blocks.at(i);
        auto *kindItem = new QTableWidgetItem(kindLabel(block.kind));
        QColor color = ocr::colorForKind(block.kind);
        kindItem->setForeground(color.darker(140));
        m_blockTable->setItem(i, 0, kindItem);

        QString position;
        if (!block.boxes.isEmpty()) {
            const QRectF &box = block.boxes.first();
            position = QStringLiteral("%1,%2 → %3,%4")
                           .arg(qRound(box.left())).arg(qRound(box.top()))
                           .arg(qRound(box.right())).arg(qRound(box.bottom()));
        }
        m_blockTable->setItem(i, 1, new QTableWidgetItem(position));

        QString text = block.text;
        text.replace(QLatin1Char('\n'), QLatin1Char(' '));
        m_blockTable->setItem(i, 2, new QTableWidgetItem(text.left(400)));
    }
    m_blockTable->resizeRowsToContents();
}

void MainWindow::updateStats()
{
    int done = 0, failed = 0, tokens = 0;
    double seconds = 0.0;
    for (int i = 0; i < m_doc.count(); ++i) {
        const PageEntry &entry = m_doc.page(i);
        if (entry.result.ok()) {
            ++done;
            tokens += entry.result.usage().value(QStringLiteral("total_tokens")).toInt();
            seconds += entry.result.elapsedMs / 1000.0;
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
    m_statsLabel->setText(text);
    m_progress->setVisible(m_pending.size() + m_running.size() > 0);
    m_progress->setRange(0, qMax(1, m_doc.count()));
    m_progress->setValue(done);
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
    enqueuePages(pages);
}

void MainWindow::startOcrCurrent()
{
    const int index = m_pageList->currentRow();
    if (index < 0)
        return;
    m_doc.page(index).result = ocr::PageResult();
    enqueuePages({index});
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
            item->setForeground(QColor(245, 158, 11));
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
    const QString name = QStringLiteral("page-%1.png").arg(page + 1);
    m_client.submit(page, m_generation, bytes, name, m_purpose);
}

void MainWindow::onResult(int page, int generation, const ocr::PageResult &result)
{
    if (generation != m_generation)
        return;
    m_running.remove(page);
    if (page >= 0 && page < m_doc.count())
        m_doc.page(page).result = result;

    if (auto *item = m_pageList->item(page)) {
        if (result.ok()) {
            item->setText(QStringLiteral("%1 ✓").arg(page + 1));
            item->setForeground(QColor(34, 197, 94));
        } else {
            item->setText(QStringLiteral("%1 ✕").arg(page + 1));
            item->setForeground(QColor(239, 68, 68));
            item->setToolTip(QStringLiteral("%1\n%2").arg(m_doc.page(page).label, result.error));
        }
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

void MainWindow::copyText()
{
    QApplication::clipboard()->setText(m_textView->toPlainText());
    statusBar()->showMessage(QStringLiteral("已复制本页文本"), 2000);
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

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty())
        return;
    if (urls.size() == 1 && QFileInfo(urls.first().toLocalFile()).isDir()) {
        m_doc.clear();
        if (m_doc.addImageDir(urls.first().toLocalFile()) > 0)
            addPagesToUi();
        return;
    }
    m_doc.clear();
    for (const QUrl &url : urls)
        loadPath(url.toLocalFile());
    event->acceptProposedAction();
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
