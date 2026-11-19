cmake_minimum_required(VERSION 3.21)
project(docr-qt VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets Concurrent)
find_package(qpdf REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(onnxruntime REQUIRED)
find_program(CARGO_EXECUTABLE cargo REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(QT6SVG REQUIRED IMPORTED_TARGET Qt6Svg)

# 与界面无关的核心：文档、OCR 客户端/队列、导出、诊断。
add_library(docr-core STATIC
    src/core/ocr.cpp
    src/core/layout.cpp
    src/core/textlayout.cpp
    src/core/document.cpp
    src/core/ocrqueue.cpp
    src/core/exporters.cpp
    src/core/ocrdiagnostics.cpp
    src/core/tocparser.cpp
    src/core/outlineio.cpp
    src/core/ocrcost.cpp
    src/core/pdfexport.cpp
    src/core/unicodenormalize.cpp
)
target_include_directories(docr-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(docr-core PUBLIC DOCR_VERSION="${PROJECT_VERSION}")
target_link_libraries(docr-core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Concurrent
    PkgConfig::POPPLER_QT6
    qpdf::libqpdf
    yaml-cpp::yaml-cpp
    onnxruntime::onnxruntime
)

# pindf 增量写入 helper（Rust）。产物与 docr-qt 放在同一目录，便于运行时查找。
file(GLOB_RECURSE PINDF_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/pindf/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/pindf-append/src/*.rs)
set(PINDF_CARGO_TARGET_DIR ${CMAKE_BINARY_DIR}/pindf-target)
set(PINDF_APPEND_BIN ${CMAKE_BINARY_DIR}/pindf-append${CMAKE_EXECUTABLE_SUFFIX})
add_custom_command(
    OUTPUT ${PINDF_APPEND_BIN}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${PINDF_CARGO_TARGET_DIR}
            ${CARGO_EXECUTABLE} build --release --locked
            --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/tools/pindf-append/Cargo.toml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PINDF_CARGO_TARGET_DIR}/release/pindf-append${CMAKE_EXECUTABLE_SUFFIX}
            ${PINDF_APPEND_BIN}
    DEPENDS ${PINDF_SOURCES}
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/pindf/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/pindf/Cargo.lock
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/pindf-append/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/pindf-append/Cargo.lock
    COMMENT "Building pindf-append (Rust)"
    VERBATIM)
add_custom_target(pindf-append-build ALL DEPENDS ${PINDF_APPEND_BIN})

add_executable(docr-qt
    src/main.cpp
    src/theme.cpp
    src/pageview.cpp
    src/blockcards.cpp
    src/preferences.cpp
    src/costdialog.cpp
    src/mainwindow.cpp
    resources.qrc
)
target_link_libraries(docr-qt PRIVATE
    docr-core
    Qt6::Widgets
    PkgConfig::QT6SVG
)

add_dependencies(docr-qt pindf-append-build)

# Windows 下按 GUI 子系统链接，避免启动时额外弹出一个控制台窗口。
# Qt 的入口点目标在不同安装里名字不同，按可用性挑一个。
if(WIN32)
    set_target_properties(docr-qt PROPERTIES WIN32_EXECUTABLE TRUE)
    foreach(_ep Qt6::EntryPoint Qt6::EntryPointPrivate Qt6::EntryPointImplementation)
        if(TARGET ${_ep})
            target_link_libraries(docr-qt PRIVATE ${_ep})
            break()
        endif()
    endforeach()
endif()

add_executable(docr-cli
    src/cli/main.cpp
)
target_link_libraries(docr-cli PRIVATE
    docr-core
    Qt6::Core
)

install(TARGETS docr-qt docr-cli RUNTIME DESTINATION bin)
install(PROGRAMS ${PINDF_APPEND_BIN} DESTINATION bin)
install(FILES assets/app.svg DESTINATION share/icons/hicolor/scalable/apps RENAME docr-qt.svg)
install(FILES assets/docr-qt.desktop DESTINATION share/applications)
