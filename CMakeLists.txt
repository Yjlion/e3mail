cmake_minimum_required(VERSION 3.21)

project(e3mail
    VERSION 0.1.1
    DESCRIPTION "An end-to-end encrypted email client with classic email functionality"
    HOMEPAGE_URL "https://github.com/Yjlion/e3mail"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(E3MAIL_BUILD_APP "Build the QML desktop application" ON)
option(E3MAIL_BUILD_CLI "Build the headless e3mail-cli" ON)
option(E3MAIL_BUILD_TESTS "Build the test suite" ON)
option(E3MAIL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(E3MAIL_QT_COMPONENTS Core Network Concurrent)
if(E3MAIL_BUILD_APP)
    list(APPEND E3MAIL_QT_COMPONENTS Gui Qml Quick QuickControls2 Svg LinguistTools)
    # Desktop notifications: the freedesktop service over D-Bus on Linux, the
    # system tray (Qt Widgets) elsewhere.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND E3MAIL_QT_COMPONENTS DBus)
    else()
        list(APPEND E3MAIL_QT_COMPONENTS Widgets)
    endif()
endif()
if(E3MAIL_BUILD_TESTS)
    list(APPEND E3MAIL_QT_COMPONENTS Test)
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS ${E3MAIL_QT_COMPONENTS})
qt_standard_project_setup(REQUIRES 6.8
    I18N_SOURCE_LANGUAGE en
    I18N_TRANSLATED_LANGUAGES de fr es zh_CN ja ru pl ar he yi)

include(E3mailRnp)
# Charsets (every one ICU knows, whatever the Qt build) and locale-aware date
# patterns. See ADR 0014.
find_package(ICU 60 REQUIRED COMPONENTS uc i18n data)
find_package(SQLite3 REQUIRED)
# CMake 4 renamed the imported target; accept either.
if(TARGET SQLite3::SQLite3)
    set(E3MAIL_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(E3MAIL_SQLITE_TARGET SQLite::SQLite3)
endif()
find_package(Qt6Keychain CONFIG QUIET)

include(GNUInstallDirs)
include(E3mailCompilerFlags)

add_subdirectory(src/core)
if(E3MAIL_BUILD_CLI)
    add_subdirectory(src/cli)
endif()
if(E3MAIL_BUILD_APP)
    add_subdirectory(src/app)
endif()
if(E3MAIL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(E3mailPackaging)
