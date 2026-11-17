cmake_minimum_required(VERSION 3.21)
project(docr-qt VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(QT6SVG REQUIRED IMPORTED_TARGET Qt6Svg)

# 与界面无关的核心：文档、OCR 客户端/队列、导出、诊断。
add_library(docr-core STATIC
    src/core/ocr.cpp
    src/core/document.cpp
    src/core/ocrqueue.cpp
    src/core/exporters.cpp
    src/core/ocrdiagnostics.cpp
)
target_include_directories(docr-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(docr-core PUBLIC DOCR_VERSION="${PROJECT_VERSION}")
target_link_libraries(docr-core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    PkgConfig::POPPLER_QT6
)

add_executable(docr-qt
    src/main.cpp
    src/theme.cpp
    src/pageview.cpp
    src/blockcards.cpp
    src/preferences.cpp
    src/mainwindow.cpp
    resources.qrc
)
target_link_libraries(docr-qt PRIVATE
    docr-core
    Qt6::Widgets
    PkgConfig::QT6SVG
)

add_executable(docr-cli
    src/cli/main.cpp
)
target_link_libraries(docr-cli PRIVATE
    docr-core
    Qt6::Core
)

install(TARGETS docr-qt docr-cli RUNTIME DESTINATION bin)
install(FILES assets/app.svg DESTINATION share/icons/hicolor/scalable/apps RENAME docr-qt.svg)
install(FILES assets/docr-qt.desktop DESTINATION share/applications)
