#include "layout.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace ocr {
namespace {

constexpr int kLayoutSize = 800;
constexpr float kScoreThreshold = 0.30f;
// PP-DocLayoutV3 的 id2label。顺序与模型 config.json 一致。
const char *const kLabels[] = {
    "abstract",       "algorithm",      "aside_text",   "chart",
    "content",        "formula",        "doc_title",    "figure_title",
    "footer",         "footer",         "footnote",     "formula_number",
    "header",         "header",         "image",        "formula",
    "number",         "paragraph_title", "reference",   "reference_content",
    "seal",           "table",          "text",         "text",
    "vision_footnote",
};
constexpr int kLabelCount = int(sizeof(kLabels) / sizeof(kLabels[0]));

QString labelName(int cls)
{
    if (cls < 0 || cls >= kLabelCount)
        return QStringLiteral("text");
    return QString::fromLatin1(kLabels[cls]);
}

QString kindForLabel(const QString &label)
{
    if (label == QLatin1String("doc_title")
        || label == QLatin1String("paragraph_title"))
        return QStringLiteral("title");
    if (label == QLatin1String("figure_title"))
        return QStringLiteral("caption");
    if (label == QLatin1String("table"))
        return QStringLiteral("table");
    if (label == QLatin1String("formula")
        || label == QLatin1String("formula_number"))
        return QStringLiteral("equation");
    if (label == QLatin1String("image") || label == QLatin1String("chart")
        || label == QLatin1String("seal"))
        return QStringLiteral("image");
    if (label == QLatin1String("header"))
        return QStringLiteral("header");
    if (label == QLatin1String("footer") || label == QLatin1String("number"))
        return QStringLiteral("footer");
    if (label == QLatin1String("footnote")
        || label == QLatin1String("vision_footnote"))
        return QStringLiteral("footnote");
    if (label == QLatin1String("abstract"))
        return QStringLiteral("abstract");
    if (label == QLatin1String("reference")
        || label == QLatin1String("reference_content"))
        return QStringLiteral("text");
    return QStringLiteral("text");
}

float sigmoid(float x)
{
    if (x >= 0.0f)
        return 1.0f / (1.0f + std::exp(-x));
    const float e = std::exp(x);
    return e / (1.0f + e);
}

float iou(const QRectF &a, const QRectF &b)
{
    const QRectF inter = a.intersected(b);
    if (inter.isEmpty())
        return 0.0f;
    const float interArea = float(inter.width() * inter.height());
    const float unionArea = float(a.width() * a.height() + b.width() * b.height())
                            - interArea;
    return unionArea > 0.0f ? interArea / unionArea : 0.0f;
}

// b 覆盖 a 的面积比例。
float coverage(const QRectF &a, const QRectF &b)
{
    if (a.isEmpty())
        return 0.0f;
    const QRectF inter = a.intersected(b);
    if (inter.isEmpty())
        return 0.0f;
    return float(inter.width() * inter.height())
           / float(a.width() * a.height());
}

bool preserveDuringMerge(const QString &label)
{
    return label == QLatin1String("image") || label == QLatin1String("seal")
           || label == QLatin1String("chart");
}

struct Candidate {
    int query = 0;
    int cls = 0;
    float score = 0.0f;
    int order = 0;
    QRectF box; // 0-1000
    QString label;
};

} // namespace

bool LayoutRegion::isImage() const
{
    return kind == QLatin1String("image") || kind == QLatin1String("figure");
}

QString defaultLayoutModelPath()
{
    const QByteArray env = qgetenv("DOCR_LAYOUT_MODEL");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QStringList candidates = {
        dataDir + QStringLiteral("/docr-qt/models/pp-doclayout-v3.onnx"),
        QDir::homePath()
            + QStringLiteral("/.local/share/docr-qt/models/pp-doclayout-v3.onnx"),
        QStringLiteral("/usr/share/docr-qt/models/pp-doclayout-v3.onnx"),
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return path;
    }
    return candidates.first();
}

QString promptForKind(const QString &kind)
{
    if (kind == QLatin1String("table"))
        return QStringLiteral("Table Recognition:");
    if (kind == QLatin1String("equation"))
        return QStringLiteral("Formula Recognition:");
    return QStringLiteral("Text Recognition:");
}

struct LayoutEngine::Impl {
    Impl(const QString &path)
        : env(ORT_LOGGING_LEVEL_WARNING, "docr-layout")
#ifdef _WIN32
        // Windows 下 ORTCHAR_T 是 wchar_t，只有 const wchar_t* 路径的重载。
        // 临时 std::wstring 的生命周期覆盖整个构造函数调用，c_str() 安全。
        , session(env, path.toStdWString().c_str(), sessionOptions())
#else
        , session(env, path.toLocal8Bit().constData(), sessionOptions())
#endif
    {
    }

    static Ort::SessionOptions sessionOptions()
    {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        return options;
    }

    Ort::Env env;
    Ort::Session session;
};

LayoutEngine::LayoutEngine(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

LayoutEngine::~LayoutEngine() = default;

std::shared_ptr<LayoutEngine> LayoutEngine::create(const QString &modelPath,
                                                   QString *error)
{
    if (modelPath.trimmed().isEmpty() || !QFileInfo::exists(modelPath)) {
        if (error) {
            *error = QStringLiteral("找不到版面模型：%1").arg(
                modelPath.trimmed().isEmpty() ? defaultLayoutModelPath() : modelPath);
        }
        return {};
    }
    try {
        auto impl = std::make_unique<Impl>(modelPath);
        return std::shared_ptr<LayoutEngine>(new LayoutEngine(std::move(impl)));
    } catch (const std::exception &e) {
        if (error)
            *error = QStringLiteral("无法加载版面模型：%1").arg(QString::fromUtf8(e.what()));
        return {};
    }
}

LayoutResult LayoutEngine::detect(const QImage &image) const
{
    LayoutResult result;
    if (image.isNull()) {
        result.error = QStringLiteral("页面图像为空");
        return result;
    }

    const QImage scaled = image.scaled(kLayoutSize, kLayoutSize,
                                       Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB888);
    if (scaled.isNull()) {
        result.error = QStringLiteral("版面模型预处理失败");
        return result;
    }

    const size_t plane = size_t(kLayoutSize) * size_t(kLayoutSize);
    std::vector<float> pixels(plane * 3);
    for (int y = 0; y < kLayoutSize; ++y) {
        const uchar *line = scaled.constScanLine(y);
        for (int x = 0; x < kLayoutSize; ++x) {
            const uchar *rgb = line + x * 3;
            const size_t index = size_t(y) * size_t(kLayoutSize) + size_t(x);
            pixels[index] = float(rgb[0]) / 255.0f;
            pixels[plane + index] = float(rgb[1]) / 255.0f;
            pixels[plane * 2 + index] = float(rgb[2]) / 255.0f;
        }
    }

    try {
        Ort::MemoryInfo memory =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 4> shape{1, 3, kLayoutSize, kLayoutSize};
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memory, pixels.data(), pixels.size(), shape.data(), shape.size());

        const char *inputNames[] = {"pixel_values"};
        const char *outputNames[] = {"logits", "pred_boxes", "order_logits"};
        std::vector<Ort::Value> outputs = m_impl->session.Run(
            Ort::RunOptions{nullptr}, inputNames, &input, 1, outputNames, 3);
        if (outputs.size() != 3) {
            result.error = QStringLiteral("版面模型输出数量异常");
            return result;
        }

        const auto logitsShape =
            outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const auto boxesShape =
            outputs[1].GetTensorTypeAndShapeInfo().GetShape();
        const auto orderShape =
            outputs[2].GetTensorTypeAndShapeInfo().GetShape();
        if (logitsShape.size() != 3 || boxesShape.size() != 3
            || orderShape.size() != 3) {
            result.error = QStringLiteral("版面模型输出形状异常");
            return result;
        }

        const int queries = int(logitsShape[1]);
        const int classes = int(logitsShape[2]);
        if (queries <= 0 || classes <= 0 || boxesShape[1] != queries
            || orderShape[1] != queries || orderShape[2] != queries) {
            result.error = QStringLiteral("版面模型输出维度异常");
            return result;
        }

        const float *logits = outputs[0].GetTensorData<float>();
        const float *boxes = outputs[1].GetTensorData<float>();
        const float *orderLogits = outputs[2].GetTensorData<float>();

        // 阅读顺序：order_logits[i][j] 表示 i 在 j 之前的票数。
        // 注意：模型输出按列投票。votes[j] 汇总所有“i 在 j 之前”的票，
        // 以及所有“j 在 i 之前”的反对票。
        std::vector<float> votes(size_t(queries), 0.0f);
        for (int j = 0; j < queries; ++j) {
            float vote = 0.0f;
            for (int i = 0; i < j; ++i)
                vote += sigmoid(orderLogits[size_t(i) * queries + j]);
            for (int i = j + 1; i < queries; ++i)
                vote += 1.0f - sigmoid(orderLogits[size_t(j) * queries + i]);
            votes[size_t(j)] = vote;
        }
        std::vector<int> pointers(static_cast<size_t>(queries));
        for (int i = 0; i < queries; ++i)
            pointers[size_t(i)] = i;
        std::sort(pointers.begin(), pointers.end(), [&](int a, int b) {
            return votes[size_t(a)] < votes[size_t(b)];
        });
        std::vector<int> rank(size_t(queries), 0);
        for (int i = 0; i < queries; ++i)
            rank[size_t(pointers[size_t(i)])] = i;

        std::vector<Candidate> candidates;
        candidates.reserve(size_t(queries));
        for (int q = 0; q < queries; ++q) {
            int bestClass = -1;
            float bestScore = 0.0f;
            for (int c = 0; c < classes; ++c) {
                const float score = sigmoid(logits[size_t(q) * classes + c]);
                if (score > bestScore) {
                    bestScore = score;
                    bestClass = c;
                }
            }
            if (bestClass < 0 || bestScore < kScoreThreshold)
                continue;

            const float cx = boxes[size_t(q) * 4 + 0];
            const float cy = boxes[size_t(q) * 4 + 1];
            const float w = boxes[size_t(q) * 4 + 2];
            const float h = boxes[size_t(q) * 4 + 3];
            QRectF box((cx - w * 0.5f) * 1000.0f,
                       (cy - h * 0.5f) * 1000.0f,
                       w * 1000.0f, h * 1000.0f);
            box = box.normalized();
            box.setLeft(qBound(0.0, box.left(), 1000.0));
            box.setTop(qBound(0.0, box.top(), 1000.0));
            box.setRight(qBound(0.0, box.right(), 1000.0));
            box.setBottom(qBound(0.0, box.bottom(), 1000.0));
            if (box.width() < 1.0 || box.height() < 1.0)
                continue;

            Candidate candidate;
            candidate.query = q;
            candidate.cls = bestClass;
            candidate.score = bestScore;
            candidate.order = rank[size_t(q)];
            candidate.box = box;
            candidate.label = labelName(bestClass);
            candidates.push_back(candidate);
        }

        // 同类 NMS；不同类只压制几乎完全重合的框。
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate &a, const Candidate &b) {
                      return a.score > b.score;
                  });
        std::vector<Candidate> kept;
        kept.reserve(candidates.size());
        for (const Candidate &candidate : candidates) {
            bool suppressed = false;
            for (const Candidate &other : kept) {
                const float threshold =
                    candidate.cls == other.cls ? 0.60f : 0.98f;
                if (iou(candidate.box, other.box) >= threshold) {
                    suppressed = true;
                    break;
                }
            }
            if (!suppressed)
                kept.push_back(candidate);
        }

        // 去掉几乎覆盖整页的“图片”，再按大框优先去掉被包含的小框。
        const bool portrait = image.height() >= image.width();
        const float pageArea = 1000.0f * 1000.0f;
        std::vector<Candidate> filtered;
        filtered.reserve(kept.size());
        for (const Candidate &candidate : kept) {
            if (candidate.label == QLatin1String("image")) {
                const float area =
                    float(candidate.box.width() * candidate.box.height());
                const float limit = (portrait ? 0.93f : 0.82f) * pageArea;
                if (area > limit)
                    continue;
            }
            filtered.push_back(candidate);
        }
        if (filtered.empty())
            filtered = kept;

        std::vector<bool> contained(filtered.size(), false);
        for (size_t i = 0; i < filtered.size(); ++i) {
            if (preserveDuringMerge(filtered[i].label))
                continue;
            for (size_t j = 0; j < filtered.size(); ++j) {
                if (i == j)
                    continue;
                if (coverage(filtered[i].box, filtered[j].box) >= 0.80f) {
                    contained[i] = true;
                    break;
                }
            }
        }
        std::vector<Candidate> merged;
        merged.reserve(filtered.size());
        for (size_t i = 0; i < filtered.size(); ++i) {
            if (!contained[i])
                merged.push_back(filtered[i]);
        }
        if (merged.empty())
            merged = filtered;

        std::sort(merged.begin(), merged.end(),
                  [](const Candidate &a, const Candidate &b) {
                      if (a.order != b.order)
                          return a.order < b.order;
                      return a.score > b.score;
                  });

        for (const Candidate &candidate : merged) {
            LayoutRegion region;
            region.kind = kindForLabel(candidate.label);
            region.nativeKind = candidate.label;
            region.box = candidate.box;
            region.score = candidate.score;
            region.order = candidate.order;
            result.regions.append(region);
        }
    } catch (const std::exception &e) {
        result.error = QStringLiteral("版面推理失败：%1").arg(QString::fromUtf8(e.what()));
    }
    return result;
}

} // namespace ocr
