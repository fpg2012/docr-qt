cmake_minimum_required(VERSION 3.21)
project(docr-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Gui)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(QT6SVG REQUIRED IMPORTED_TARGET Qt6Svg)

add_executable(docr-qt
    src/main.cpp
    src/ocr.cpp
    src/theme.cpp
    src/document.cpp
    src/pageview.cpp
    src/blockcards.cpp
    src/mainwindow.cpp
    resources.qrc
)

target_link_libraries(docr-qt PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Gui
    PkgConfig::POPPLER_QT6
    PkgConfig::QT6SVG
)

install(TARGETS docr-qt RUNTIME DESTINATION bin)
install(FILES assets/app.svg DESTINATION share/icons/hicolor/scalable/apps RENAME docr-qt.svg)
install(FILES assets/docr-qt.desktop DESTINATION share/applications)
