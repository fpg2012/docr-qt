cmake_minimum_required(VERSION 3.21)
project(docr-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Gui)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)

add_executable(docr-qt
    src/main.cpp
    src/ocr.cpp
    src/document.cpp
    src/pageview.cpp
    src/mainwindow.cpp
)

target_link_libraries(docr-qt PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Gui
    PkgConfig::POPPLER_QT6
)

install(TARGETS docr-qt RUNTIME DESTINATION bin)
