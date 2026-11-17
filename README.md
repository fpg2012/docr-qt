# docr-qt

`docr-qt` 是 DeepSeek OCR 的 Qt 6 桌面客户端，支持 PDF、图片和图片目录。程序逐页调用
`POST /v1/tools/ocr`，把返回的 `<|ref|>/<|box|>` 版面坐标叠加到页面上，并将结果整理为
可筛选、复制和导出的版面块卡片。界面提供连续翻页、PDF 内置大纲、从目录页 OCR 生成大纲、
框选以及跨块跨页的字符级文本选择。

同一套核心还提供 `docr-cli`，可以在无界面环境中批量识别并导出 JSON / 文本 / Markdown。

## 快速开始

**依赖**

- Qt 6：Widgets、Network、Gui、Svg
- poppler-qt6 开发包
- CMake ≥ 3.21，支持 C++20 的编译器

Debian / Ubuntu：

```bash
sudo apt install qt6-base-dev libqt6svg6-dev libpoppler-qt6-dev cmake g++
```

**构建**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

生成两个可执行文件：`build/docr-qt` 和 `build/docr-cli`。

**配置 API Key**

在 GUI 中打开「编辑 → 首选项…」（`Ctrl+,`）填写，也可以设置环境变量：

```bash
export DEEPSEEK_API_KEY=sk-...
```

未配置时还会只读回退到 `~/.dsh/.credentials.yaml`。

**运行**

```bash
./build/docr-qt
./build/docr-cli --help
```

把 PDF、图片或图片目录拖进窗口即可开始识别。命令行批量识别示例：

```bash
./build/docr-cli doc.pdf --pages 1-5 -f markdown -o out
```

**安装**

```bash
sudo cmake --install build
```

默认安装到 `/usr/local/bin`，同时安装应用图标和 desktop 文件。

## 代码结构

```text
src/
├── core/                  # 与界面无关，GUI 和 CLI 共用
│   ├── document.*         # PDF / 图片模型、按需渲染、缩略图
│   ├── ocr.*              # OCR 客户端、multipart 组包、标记解析
│   ├── ocrqueue.*         # 并发队列、取消、空结果重试
│   ├── exporters.*        # Markdown / JSON 导出
│   ├── ocrdiagnostics.*   # 空结果诊断落盘
│   └── tocparser.*        # 从 OCR 目录页解析标题与页码
├── pageview.*             # 连续页面、识别框、框选、跨页文本选择
├── blockcards.*           # 版面块卡片列表与图片裁剪
├── preferences.*          # 首选项与关于对话框
├── theme.*                # QPalette 派生颜色、SVG 图标染色
├── mainwindow.*           # 主窗口编排、大纲与状态同步
├── main.cpp
└── cli/
    └── main.cpp           # docr-cli 入口
assets/                    # SVG 图标与 desktop 文件
CMakeLists.txt
```

`src/core` 只依赖 Qt Core/Gui/Network 和 poppler-qt6，不依赖 Widgets；GUI 和 CLI 都链接
同一个 `docr-core` 静态库。
