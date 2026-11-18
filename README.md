# docr-qt

`docr-qt` 是 DeepSeek OCR 的 Qt 6 桌面客户端，支持 PDF、图片和图片目录。程序逐页调用
`POST /v1/tools/ocr`，把返回的 `<|ref|>/<|box|>` 版面坐标叠加到页面上，并将结果整理为
可筛选、复制和导出的版面块卡片。界面提供连续翻页、PDF 内置大纲、从目录页 OCR 生成大纲、
框选以及跨块跨页的字符级文本选择。连续视图只渲染视口附近的页面，缩略图也在进入可见区时
才生成；批量识别时每页结果增量并入视图，打开和滚动 500 页以上的 PDF 不会一次性渲染全本。
目录页既可以按「标题 …… 页码」规则解析，也可以导入 pdf-outline 兼容的 JSON/YAML 多级
大纲。图片块可以直接复制，右键还能导出为 PNG。

识别结果可以导出 Markdown / JSON，也可以把之前导出的 JSON 重新导入当前文档；可搜索 PDF
会按页面图像里检测到的真实文字行放置不可见文本层，复制/搜索时和原文字基本对齐。批量识别前
可以用「识别 → 估算全部 OCR 费用…」按 OCR API 的页费
（1.5 元/千页）估算总费用。也可以切换到本地 GLM-OCR（vLLM）后端，不产生 DeepSeek
OCR 页费。同一套核心还提供 `docr-cli`，可以在无界面环境中批量识别并导出 JSON / 文本 /
Markdown。

## 快速开始

**依赖**

- Qt 6：Widgets、Network、Gui、Svg、Concurrent
- poppler-qt6 开发包
- qpdf（overlay 规范化与混排文档 fallback）
- yaml-cpp（导入 YAML 大纲）
- ONNX Runtime（本地 GLM-OCR 的 PP-DocLayoutV3 版面检测）
- Rust / Cargo（增量写入 helper；pindf 已放在 `vendor/pindf`）
- CMake ≥ 3.21，支持 C++20 的编译器

Arch Linux：

```bash
sudo pacman -S --needed base-devel cmake pkgconf qt6-base qt6-svg poppler-qt6 qpdf yaml-cpp rust onnxruntime
```

Debian / Ubuntu：

```bash
sudo apt install qt6-base-dev libqt6svg6-dev libpoppler-qt6-dev libqpdf-dev libyaml-cpp-dev \
  cmake g++ cargo rustc libonnxruntime-dev
```

Windows：使用 [MSYS2](https://www.msys2.org/) 的 UCRT64 环境。项目通过 pkg-config 查找
poppler-qt6，因此需要在 UCRT64 shell 中安装依赖并构建：

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-svg \
  mingw-w64-ucrt-x86_64-poppler-qt6 mingw-w64-ucrt-x86_64-qpdf \
  mingw-w64-ucrt-x86_64-yaml-cpp mingw-w64-ucrt-x86_64-rust
```

**构建**

Linux：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows（UCRT64 shell）：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

生成 `build/docr-qt`、`build/docr-cli`；Windows 下为对应的 `.exe` 文件。

**配置 API Key**

在 GUI 中打开「编辑 → 首选项…」（`Ctrl+,`）填写，也可以设置环境变量：

```bash
export DEEPSEEK_API_KEY=sk-...
```

未配置时还会只读回退到 `~/.dsh/.credentials.yaml`。

**运行**

Linux：

```bash
./build/docr-qt
./build/docr-cli --help
# 也可以在启动时导入大纲，或恢复之前导出的识别结果
./build/docr-qt --open doc.pdf --outline outline.yaml
./build/docr-qt --open doc.pdf --import-results ocr.json
```

Windows（在同一个 UCRT64 shell 中）：

```bash
./build/docr-qt.exe
./build/docr-cli.exe --help
```

把 PDF、图片或图片目录拖进窗口即可开始识别。命令行批量识别示例：

```bash
./build/docr-cli doc.pdf --pages 1-5 -f markdown -o out
./build/docr-cli doc.pdf --backend glmocr \
  --glm-url http://127.0.0.1:8080/v1/chat/completions --pages 1-5 -o out
```

识别完成后，用「文件 → 导出可搜索 PDF…」可以生成保留原页面、带不可见文本层和当前大纲的
PDF。文本层会先检测每个版面块里的实际文字行，再按行设置字号和位置，避免整段文字挤在
块中央。单个 PDF 会通过 `pindf-append` 走真正的增量更新：原文件字节保持不变，只在末尾
追加新对象、xref 和 trailer。图片、多个 PDF 混排或带 `/Rotate` 的页面会回退到 qpdf
完整重建。

如果已经导出过识别结果，可以重新打开源 PDF / 图片，用「文件 → 导入识别结果 JSON…」恢复
识别文本和版面块，不需要再次调用 OCR。导入的 JSON 既可以是本程序 GUI/CLI 的新格式（含
逐行文字框），也可以是旧格式或 GLM 的 `layout_blocks`；旧格式会在显示/导出时按页面图像
补算行框。JSON 页数不必和源文档完全一致，但页码必须在源文档范围内，超出范围的条目会被忽略。

「大纲」页支持两种来源：

- 「从目录页生成…」：OCR 目录页后按「标题 …… 页码」规则解析；
- 「导入 JSON / YAML…」：直接载入 pdf-outline 兼容的多级大纲，并替换当前大纲。

JSON 结构和 pdf-outline 一致，`page` 从 1 开始；也接受 `destination.page`：

```json
{
  "chd": [
    { "title": "前言", "page": 5 },
    { "title": "牡丹亭", "destination": { "page": 14 }, "chd": [
      { "title": "第一齣 標目", "page": 14 },
      { "title": "第二齣 言懷", "page": 17 }
    ] }
  ]
}
```

YAML 使用相同字段：

```yaml
chd:
  - title: 前言
    page: 5
  - title: 牡丹亭
    page: 14
    chd:
      - title: 第一齣 標目
        page: 14
```

导入后会自动替换内置大纲；没有页码的组标题会保留为不可跳转的父节点。

批量识别时，失败或返回空结果的页面会显示在状态栏的「重试失败页 (N)」按钮里；点击只重试
这些页面，「识别全部」也会自动包含待重试页。

**本地 GLM-OCR**

「编辑 → 首选项 → 识别」可以把后端切到 **GLM-OCR（本地 vLLM）**。默认地址是
`http://127.0.0.1:8080/v1/chat/completions`，模型名 `glm-ocr`。vLLM 启动示例：

```bash
vllm serve zai-org/GLM-OCR --port 8080 --served-model-name glm-ocr
```

GLM-OCR 的 vLLM 模型只负责文字/表格/公式识别，版面坐标由独立的 PP-DocLayoutV3
模型提供。程序在本地用 ONNX Runtime 跑版面检测，再按区域并行调用 vLLM，因此 GLM
后端也有标题、正文、表格、图片、脚注等版面块和真实坐标。界面和 `docr-cli` 都支持
指定版面模型路径：

```bash
# 默认位置
~/.local/share/docr-qt/models/pp-doclayout-v3.onnx

# 或显式指定
export DOCR_LAYOUT_MODEL=/path/to/pp-doclayout-v3.onnx
./build/docr-cli --backend glmocr --layout-model /path/to/pp-doclayout-v3.onnx ...
```

也可以在「首选项 → 识别 → 版面模型」里选择 ONNX 文件。找不到模型时会退回整页文本
识别，并在 JSON 的 `layout_error` 中记录原因。模型只用于本地推理，运行时不需要 Python。
本地后端不产生 DeepSeek OCR 页费。

Windows 下把命令中的 `docr-cli` 换成 `docr-cli.exe`。在 UCRT64 shell 中运行时会自动找到
Qt 和 poppler 的 DLL；若要脱离 MSYS2 分发，需要将相关 DLL 与 exe 放在一起，可先用
`windeployqt` 复制 Qt 依赖。

**安装**

Linux：

```bash
sudo cmake --install build
```

默认安装到 `/usr/local/bin`，同时安装应用图标和 desktop 文件。Windows 直接在 UCRT64
环境中运行即可，无需执行安装步骤。

## 代码结构

```text
src/
├── core/                  # 与界面无关，GUI 和 CLI 共用
│   ├── document.*         # PDF / 图片模型、按需渲染、缩略图
│   ├── ocr.*              # OCR 客户端、multipart 组包、GLM 分区识别
│   ├── layout.*           # PP-DocLayoutV3 ONNX 版面检测
│   ├── ocrqueue.*         # 并发队列、取消、空结果重试
│   ├── exporters.*        # Markdown / JSON 导出与识别结果导入
│   ├── textlayout.*       # 按页面图像检测文字行，供选择层/PDF 文本层使用
│   ├── pdfexport.*        # 可搜索 PDF：文本层、增量写入与 qpdf fallback
│   ├── ocrcost.*          # OCR 页费估算
│   ├── unicodenormalize.* # PDF 文本层的 CJK 字符归一化
│   ├── ocrdiagnostics.*   # 空结果诊断落盘
│   ├── tocparser.*        # 从 OCR 目录页解析标题与页码
│   └── outlineio.*        # 导入 pdf-outline 兼容 JSON/YAML
├── pageview.*             # 连续页面、识别框、框选、跨页文本选择
├── blockcards.*           # 版面块卡片列表与图片裁剪
├── costdialog.*           # 全部 OCR 费用估算对话框
├── preferences.*          # 首选项与关于对话框
├── theme.*                # QPalette 派生颜色、SVG 图标染色
├── mainwindow.*           # 主窗口编排、大纲与状态同步
├── main.cpp
└── cli/
    └── main.cpp           # docr-cli 入口
tools/
└── pindf-append/          # Rust helper：导入 overlay、改页面、写大纲
vendor/
└── pindf/                 # pindf（Rust）源码，Apache-2.0
assets/                    # SVG 图标与 desktop 文件
CMakeLists.txt
```

`src/core` 只依赖 Qt Core/Gui/Network、poppler-qt6 和 qpdf，不依赖 Widgets；GUI 和 CLI 都链接
同一个 `docr-core` 静态库。`pindf-append` 由 CMake 调用 Cargo 构建，运行时从程序同目录或
`PATH` 查找。
