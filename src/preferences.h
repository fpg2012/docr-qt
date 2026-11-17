// 首选项对话框与关于对话框。
#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

struct Preferences {
    QString apiKey;
    QString baseUrl = QStringLiteral("https://api.deepseek.com/v1/tools");
    int dpi = 150;
    int concurrency = 3;
    QString purpose = QStringLiteral("rich_layout_recognition");
    QString backend = QStringLiteral("deepseek");
    QString glmEndpoint = QStringLiteral("http://127.0.0.1:8080/v1/chat/completions");
    QString glmModel = QStringLiteral("glm-ocr");
    QString glmApiKey;
    QString layoutModelPath;
    bool rememberKey = false;
};

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(const Preferences &initial, QWidget *parent = nullptr);

    Preferences values() const;

private:
    QLineEdit *m_keyEdit = nullptr;
    QLineEdit *m_baseEdit = nullptr;
    QComboBox *m_backendCombo = nullptr;
    QComboBox *m_purposeCombo = nullptr;
    QLineEdit *m_glmEndpointEdit = nullptr;
    QLineEdit *m_glmModelEdit = nullptr;
    QLineEdit *m_glmKeyEdit = nullptr;
    QLineEdit *m_layoutModelEdit = nullptr;
    QSpinBox *m_dpiSpin = nullptr;
    QSpinBox *m_concSpin = nullptr;
    QCheckBox *m_remember = nullptr;
};

void showAboutDialog(const QString &baseUrl, const QString &purpose, QWidget *parent);
