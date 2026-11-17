// 文档模型：PDF（poppler 逐页渲染）与图片序列。
#pragma once

#include <QImage>
#include <QPixmap>
#include <QString>
#include <QVector>

#include <memory>

#include <poppler-qt6.h>

#include "ocr.h"

struct PageEntry {
    QString label;      // 侧栏显示名
    QString sourcePath; // 源 PDF/图片路径
    int pdfPage = -1;   // PDF 内页码（0 起）；图片为 -1
    QImage image;       // 渲染结果（按需）
    QPixmap thumb;      // 缩略图
    ocr::PageResult result;

    bool hasResult() const { return result.ok(); }
};

class Document {
public:
    bool openPdf(const QString &path, int dpi, QString *error);
    bool addImage(const QString &path, QString *error);
    int addImageDir(const QString &dir);

    void clear();
    int count() const { return m_pages.size(); }
    int dpi() const { return m_dpi; }
    void setDpi(int dpi) { m_dpi = dpi; }
    QString sourcePath() const { return m_path; }
    bool isPdf() const { return m_pdf != nullptr; }

    PageEntry &page(int index) { return m_pages[index]; }
    const PageEntry &page(int index) const { return m_pages[index]; }

    // 渲染/读取整页图片与缩略图；失败返回空。
    QImage ensureImage(int index);
    QPixmap ensureThumb(int index);

private:
    QVector<PageEntry> m_pages;
    std::unique_ptr<Poppler::Document> m_pdf;
    QImage renderPdfPage(int pdfPage);
    int m_dpi = 150;
    QString m_path;
};
