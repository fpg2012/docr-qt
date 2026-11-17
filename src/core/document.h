// 文档模型：PDF（poppler 逐页渲染）与图片序列。
#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

#include <poppler-qt6.h>

#include "ocr.h"

struct DocumentLoadReport {
    QStringList files;
    QStringList errors;
};

struct PageEntry {
    QString label;      // 侧栏显示名
    QString sourcePath; // 源 PDF/图片路径
    int pdfPage = -1;   // PDF 内页码（0 起）；图片为 -1
    std::shared_ptr<Poppler::Document> pdf; // 非空则从该 PDF 渲染本页
    QImage image;       // 渲染结果（按需）
    QImage thumb;       // 缩略图（UI 再转 QPixmap）
    ocr::PageResult result;

    bool hasResult() const { return result.ok(); }
};

class Document {
public:
    // 追加一个 PDF 的全部页面（不清空已有页面）。
    bool addPdf(const QString &path, int dpi, QString *error);
    bool addImage(const QString &path, QString *error);
    int addImageDir(const QString &dir);

    // 把 PDF / 图片 / 图片目录展开成文件列表（目录按自然序）。
    static QStringList expandPaths(const QStringList &paths);
    // 追加一组路径；report 返回实际展开的文件和打不开的错误。
    bool addPaths(const QStringList &paths, int dpi, DocumentLoadReport *report = nullptr);

    void clear();
    int count() const { return m_pages.size(); }
    int dpi() const { return m_dpi; }
    void setDpi(int dpi) { m_dpi = dpi; }
    QString sourcePath() const { return m_path; }

    PageEntry &page(int index) { return m_pages[index]; }
    const PageEntry &page(int index) const { return m_pages[index]; }

    // 渲染/读取整页图片与缩略图；失败返回空。
    QImage ensureImage(int index);
    QImage ensureThumb(int index);

    // 图片序列可用的后缀（供目录展开与文件对话框复用）。
    static QStringList imageNameFilters();

private:
    QVector<PageEntry> m_pages;
    QImage renderPdfPage(const std::shared_ptr<Poppler::Document> &pdf, int page);
    int m_dpi = 150;
    QString m_path;
};
