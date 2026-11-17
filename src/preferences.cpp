#include "preferences.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "core/layout.h"
#include "core/ocr.h"
#include "theme.h"

PreferencesDialog::PreferencesDialog(const Preferences &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("首选项"));
    resize(560, 0);

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);

    // —— API ——
    auto *apiPage = new QWidget(tabs);
    auto *apiForm = new QFormLayout(apiPage);

    m_keyEdit = new QLineEdit(initial.apiKey, apiPage);
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setPlaceholderText(QStringLiteral("留空则使用 DEEPSEEK_API_KEY / DSH 凭据库"));

    auto *showKey = new QCheckBox(QStringLiteral("显示"), apiPage);
    auto *keyRow = new QWidget(apiPage);
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->addWidget(m_keyEdit, 1);
    keyLayout->addWidget(showKey);
    connect(showKey, &QCheckBox::toggled, m_keyEdit, [this](bool on) {
        m_keyEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });

    m_remember = new QCheckBox(QStringLiteral("保存到本机设置"), apiPage);
    m_remember->setChecked(initial.rememberKey);

    auto *useDsh = new QPushButton(QStringLiteral("载入 DSH 凭据库的 Key"), apiPage);
    connect(useDsh, &QPushButton::clicked, this, [this] {
        const QString key = ocr::dshApiKey();
        if (key.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("没有找到 Key"),
                                     QStringLiteral("环境变量和 ~/.dsh/.credentials.yaml 中都没有 Key。"));
        } else {
            m_keyEdit->setText(key);
        }
    });

    auto *keyHint = new QLabel(
        QStringLiteral("留空时依次使用 DEEPSEEK_API_KEY、DSH 凭据库"), apiPage);
    keyHint->setWordWrap(true);
    keyHint->setForegroundRole(QPalette::PlaceholderText);

    m_baseEdit = new QLineEdit(initial.baseUrl, apiPage);
    m_baseEdit->setPlaceholderText(QStringLiteral("https://api.deepseek.com/v1/tools"));

    apiForm->addRow(QStringLiteral("API Key"), keyRow);
    apiForm->addRow(QString(), m_remember);
    apiForm->addRow(QString(), useDsh);
    apiForm->addRow(QString(), keyHint);
    apiForm->addRow(QStringLiteral("接口地址"), m_baseEdit);

    // —— 识别 ——
    auto *ocrPage = new QWidget(tabs);
    auto *ocrForm = new QFormLayout(ocrPage);
    m_backendCombo = new QComboBox(ocrPage);
    m_backendCombo->addItem(QStringLiteral("DeepSeek OCR API"), QStringLiteral("deepseek"));
    m_backendCombo->addItem(QStringLiteral("GLM-OCR（本地 vLLM）"), QStringLiteral("glmocr"));
    const int backendIndex = m_backendCombo->findData(initial.backend);
    if (backendIndex >= 0)
        m_backendCombo->setCurrentIndex(backendIndex);

    m_purposeCombo = new QComboBox(ocrPage);
    m_purposeCombo->addItem(QStringLiteral("版面分析（带坐标框）"),
                            QStringLiteral("rich_layout_recognition"));
    m_purposeCombo->addItem(QStringLiteral("纯文本（更快）"),
                            QStringLiteral("text_recognition"));
    const int purposeIndex = m_purposeCombo->findData(initial.purpose);
    if (purposeIndex >= 0)
        m_purposeCombo->setCurrentIndex(purposeIndex);

    m_dpiSpin = new QSpinBox(ocrPage);
    m_dpiSpin->setRange(72, 400);
    m_dpiSpin->setValue(initial.dpi);
    m_dpiSpin->setSuffix(QStringLiteral(" dpi"));

    m_concSpin = new QSpinBox(ocrPage);
    m_concSpin->setRange(1, 8);
    m_concSpin->setValue(initial.concurrency);
    m_concSpin->setToolTip(QStringLiteral("同时在途的 OCR 请求数；账号上限 15"));

    auto *ocrHint = new QLabel(
        QStringLiteral("「版面分析」返回带坐标的识别框；「纯文本」更快但没有框。"), ocrPage);
    ocrHint->setWordWrap(true);
    ocrHint->setForegroundRole(QPalette::PlaceholderText);

    m_glmEndpointEdit = new QLineEdit(initial.glmEndpoint, ocrPage);
    m_glmEndpointEdit->setPlaceholderText(
        QStringLiteral("http://127.0.0.1:8080/v1/chat/completions"));
    m_glmModelEdit = new QLineEdit(initial.glmModel, ocrPage);
    m_glmModelEdit->setPlaceholderText(QStringLiteral("glm-ocr"));
    m_glmKeyEdit = new QLineEdit(initial.glmApiKey, ocrPage);
    m_glmKeyEdit->setEchoMode(QLineEdit::Password);
    m_glmKeyEdit->setPlaceholderText(QStringLiteral("本地服务通常留空"));

    m_layoutModelEdit = new QLineEdit(
        initial.layoutModelPath.isEmpty() ? ocr::defaultLayoutModelPath()
                                          : initial.layoutModelPath,
        ocrPage);
    m_layoutModelEdit->setPlaceholderText(
        QStringLiteral("pp-doclayout-v3.onnx"));
    auto *browseLayout = new QPushButton(QStringLiteral("选择…"), ocrPage);
    auto *layoutRow = new QWidget(ocrPage);
    auto *layoutRowLayout = new QHBoxLayout(layoutRow);
    layoutRowLayout->setContentsMargins(0, 0, 0, 0);
    layoutRowLayout->addWidget(m_layoutModelEdit, 1);
    layoutRowLayout->addWidget(browseLayout);
    connect(browseLayout, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择 PP-DocLayoutV3 ONNX 模型"),
            m_layoutModelEdit->text(),
            QStringLiteral("ONNX 模型 (*.onnx);;所有文件 (*)"));
        if (!path.isEmpty())
            m_layoutModelEdit->setText(path);
    });

    auto *backendHint = new QLabel(
        QStringLiteral("GLM-OCR 走本地 OpenAI 兼容 /v1/chat/completions；"
                       "先用 PP-DocLayoutV3 检测版面，再按区域并行识别，"
                       "不产生 DeepSeek OCR 页费。"),
        ocrPage);
    backendHint->setWordWrap(true);
    backendHint->setForegroundRole(QPalette::PlaceholderText);

    ocrForm->addRow(QStringLiteral("识别后端"), m_backendCombo);
    ocrForm->addRow(QStringLiteral("DeepSeek 模式"), m_purposeCombo);
    ocrForm->addRow(QStringLiteral("渲染精度"), m_dpiSpin);
    ocrForm->addRow(QStringLiteral("并发数"), m_concSpin);
    ocrForm->addRow(QStringLiteral("GLM 服务地址"), m_glmEndpointEdit);
    ocrForm->addRow(QStringLiteral("GLM 模型"), m_glmModelEdit);
    ocrForm->addRow(QStringLiteral("GLM API Key"), m_glmKeyEdit);
    ocrForm->addRow(QStringLiteral("版面模型"), layoutRow);
    ocrForm->addRow(QString(), backendHint);
    ocrForm->addRow(QString(), ocrHint);

    auto updateBackendFields = [this, browseLayout] {
        const bool glm = m_backendCombo->currentData().toString()
                         == QStringLiteral("glmocr");
        m_purposeCombo->setEnabled(!glm);
        m_glmEndpointEdit->setEnabled(glm);
        m_glmModelEdit->setEnabled(glm);
        m_glmKeyEdit->setEnabled(glm);
        m_layoutModelEdit->setEnabled(glm);
        browseLayout->setEnabled(glm);
    };
    connect(m_backendCombo, &QComboBox::currentIndexChanged, this,
            [updateBackendFields](int) { updateBackendFields(); });
    updateBackendFields();

    tabs->addTab(apiPage, QStringLiteral("API"));
    tabs->addTab(ocrPage, QStringLiteral("识别"));
    layout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *reset = buttons->addButton(QStringLiteral("恢复默认"), QDialogButtonBox::ResetRole);
    connect(reset, &QPushButton::clicked, this, [this] {
        m_keyEdit->clear();
        m_remember->setChecked(false);
        m_baseEdit->setText(QStringLiteral("https://api.deepseek.com/v1/tools"));
        m_backendCombo->setCurrentIndex(0);
        m_purposeCombo->setCurrentIndex(0);
        m_dpiSpin->setValue(150);
        m_concSpin->setValue(3);
        m_glmEndpointEdit->setText(QStringLiteral("http://127.0.0.1:8080/v1/chat/completions"));
        m_glmModelEdit->setText(QStringLiteral("glm-ocr"));
        m_glmKeyEdit->clear();
        m_layoutModelEdit->setText(ocr::defaultLayoutModelPath());
    });
    layout->addWidget(buttons);
    adjustSize();
    resize(600, height());
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

Preferences PreferencesDialog::values() const
{
    Preferences result;
    result.apiKey = m_keyEdit->text().trimmed();
    result.baseUrl = m_baseEdit->text().trimmed();
    if (result.baseUrl.isEmpty())
        result.baseUrl = QStringLiteral("https://api.deepseek.com/v1/tools");
    result.purpose = m_purposeCombo->currentData().toString();
    result.backend = m_backendCombo->currentData().toString();
    result.glmEndpoint = m_glmEndpointEdit->text().trimmed();
    if (result.glmEndpoint.isEmpty())
        result.glmEndpoint = QStringLiteral("http://127.0.0.1:8080/v1/chat/completions");
    result.glmModel = m_glmModelEdit->text().trimmed();
    if (result.glmModel.isEmpty())
        result.glmModel = QStringLiteral("glm-ocr");
    result.glmApiKey = m_glmKeyEdit->text();
    result.layoutModelPath = m_layoutModelEdit->text().trimmed();
    if (result.layoutModelPath.isEmpty())
        result.layoutModelPath = ocr::defaultLayoutModelPath();
    result.dpi = m_dpiSpin->value();
    result.concurrency = m_concSpin->value();
    result.rememberKey = m_remember->isChecked();
    return result;
}

void showAboutDialog(const QString &baseUrl, const QString &purpose, QWidget *parent)
{
    QMessageBox box(parent);
    box.setWindowTitle(QStringLiteral("关于 docr-qt"));
    box.setIconPixmap(theme::appIcon().pixmap(72, 72));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral(
                    "<h3>docr-qt %1</h3>"
                    "<p>DeepSeek OCR 的 Qt 6 桌面客户端。<br>"
                    "拖入 PDF / 图片，逐页识别并把版面框叠加在页面上；"
                    "结果可复制文本/图片，导出 Markdown / JSON / 可搜索 PDF。<br>"
                    "支持 DeepSeek OCR 与本地 GLM-OCR，可导入 JSON/YAML 大纲；"
                    "批量识别前可估算 OCR 页费。</p>"
                    "<p><b>Qt</b> %2<br>"
                    "<b>接口</b> %3<br>"
                    "<b>识别模式</b> %4</p>"
                    "<p>识别结果只保存在本机内存，导出时才写文件。</p>")
                    .arg(QStringLiteral(DOCR_VERSION), QString::fromLatin1(qVersion()),
                         baseUrl.toHtmlEscaped(),
                         purpose == QStringLiteral("text_recognition")
                             ? QStringLiteral("纯文本")
                             : QStringLiteral("版面分析")));
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}
