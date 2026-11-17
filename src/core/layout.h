// PP-DocLayoutV3 版面检测（ONNX Runtime，纯 C++）。
#pragma once

#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

#include <memory>

namespace ocr {

// 一页中的一个版面区域。box 为 0-1000 归一化坐标。
struct LayoutRegion {
    QString kind;       // docr 内部类型：title/text/table/equation/image/...
    QString nativeKind; // 模型原始标签：paragraph_title/vision_footnote/...
    QRectF box;
    float score = 0.0f;
    int order = 0;

    bool isImage() const;
};

struct LayoutResult {
    QVector<LayoutRegion> regions;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

// 默认模型位置：$DOCR_LAYOUT_MODEL，或 ~/.local/share/docr-qt/models/...
QString defaultLayoutModelPath();

// 版面区域对应的 GLM-OCR prompt。
QString promptForKind(const QString &kind);

class LayoutEngine {
public:
    // 失败时返回空指针并写入 error；模型只在首次创建时加载。
    static std::shared_ptr<LayoutEngine> create(const QString &modelPath,
                                                QString *error = nullptr);
    ~LayoutEngine();

    LayoutResult detect(const QImage &image) const;

private:
    struct Impl;
    explicit LayoutEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace ocr
