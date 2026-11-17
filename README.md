# docr-qt

`docr-qt` 是 DeepSeek OCR 的 Qt 6 桌面客户端，支持 PDF、图片和图片目录。程序逐页调用
`POST /v1/tools/ocr`，把返回的 `<|ref|>/<|box|>` 版面坐标叠加到页面上，并将结果整理为
可筛选、复制和导出的版面块卡片。界面提供连续翻页、PDF 内置大纲、从目录页 OCR 生成大纲、
框选以及跨块跨页的字符级文本选择。连续视图只渲染视口附近的页面，缩略图也在进入可见区时
才生成；批量识别时每页结果增量并入视图，打开和滚动 500 页以上的 PDF 不会一次性渲染全本。
目录页既可以按「标题 …… 页码」规则解析，也可以交给 `deepseek-flash` 整理成适合多栏、
分组标题和多级篇目的目录。图片块可以直接复制，右键还能导出为 PNG。

识别结果可以导出 Markdown / JSON，也可以生成保留原页面、带不可见 OCR 文本层和当前大纲的
可搜索 PDF。批量识别前可以用「识别 → 估算全部 OCR 费用…」按 OCR API 的页费
（1.5 元/千页）估算总费用；OCR 不按 `deepseek-flash` 的 token 计费，目录 AI 整理才按
token 另计。同一套核心还提供 `docr-cli`，可以在无界面环境中批量识别并导出 JSON / 文本 /
Markdown。

## 快速开始

**依赖**

- Qt 6：Widgets、Network、Gui、Svg
- poppler-qt6 开发包
- qpdf（overlay 规范化与混排文档 fallback）
- Rust / Cargo（增量写入 helper；pindf 已放在 `vendor/pindf`）
- CMake ≥ 3.21，支持 C++20 的编译器

Arch Linux：

```bash
sudo pacman -S --needed base-devel cmake pkgconf qt6-base qt6-svg poppler-qt6 qpdf rust
```

Debian / Ubuntu：

```bash
sudo apt install qt6-base-dev libqt6svg6-dev libpoppler-qt6-dev libqpdf-dev cmake g++ cargo rustc
```

Windows：使用 [MSYS2](https://www.msys2.org/) 的 UCRT64 环境。项目通过 pkg-config 查找
poppler-qt6，因此需要在 UCRT64 shell 中安装依赖并构建：

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-svg \
  mingw-w64-ucrt-x86_64-poppler-qt6 mingw-w64-ucrt-x86_64-qpdf \
  mingw-w64-ucrt-x86_64-rust
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
```

Windows（在同一个 UCRT64 shell 中）：

```bash
./build/docr-qt.exe
./build/docr-cli.exe --help
```

把 PDF、图片或图片目录拖进窗口即可开始识别。命令行批量识别示例：

```bash
./build/docr-cli doc.pdf --pages 1-5 -f markdown -o out
```

识别完成后，用「文件 → 导出可搜索 PDF…」可以生成保留原页面、带不可见文本层和当前大纲的
PDF。单个 PDF 会通过 `pindf-append` 走真正的增量更新：原文件字节保持不变，只在末尾追加
新对象、xref 和 trailer。图片、多个 PDF 混排或带 `/Rotate` 的页面会回退到 qpdf 完整重建。

目录页识别后，在「大纲」页点「从目录页生成…」，默认使用 `deepseek-flash` 把 OCR 文本整理成
多级目录；如果目录格式非常固定，也可以切换到离线规则解析。AI 调用会消耗少量 token，
「估算全部 OCR 费用」目前不包含这部分目录调用。

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
│   ├── ocr.*              # OCR 客户端、multipart 组包、标记解析
│   ├── ocrqueue.*         # 并发队列、取消、空结果重试
│   ├── exporters.*        # Markdown / JSON 导出
│   ├── pdfexport.*        # 可搜索 PDF：文本层、增量写入与 qpdf fallback
│   ├── ocrcost.*          # OCR 页费估算
│   ├── unicodenormalize.* # PDF 文本层的 CJK 字符归一化
│   ├── ocrdiagnostics.*   # 空结果诊断落盘
│   ├── tocparser.*        # 从 OCR 目录页解析标题与页码（规则模式）
│   └── tocagent.*         # deepseek-flash 目录结构化（多栏 / 多级）
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
