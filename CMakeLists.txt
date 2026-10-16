cmake_minimum_required(VERSION 3.21)

# The version is the release's tag: the release workflow passes
# -DE3MAIL_VERSION=X.Y.Z from it. Without that, the newest vX.Y.Z tag in git
# names the build, and without git, 0.0.0 does.
set(E3MAIL_VERSION "" CACHE STRING "Version as X.Y.Z; empty takes the newest vX.Y.Z tag")
if(E3MAIL_VERSION)
    if(NOT E3MAIL_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "E3MAIL_VERSION is \"${E3MAIL_VERSION}\"; it must be X.Y.Z.")
    endif()
    set(e3_version ${E3MAIL_VERSION})
else()
    execute_process(COMMAND git describe --tags --abbrev=0 --match "v[0-9]*.[0-9]*.[0-9]*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE e3_tag OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE e3_git ERROR_QUIET)
    string(REGEX REPLACE "^v" "" e3_version "${e3_tag}")
    if(NOT e3_git EQUAL 0 OR NOT e3_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(WARNING "No E3MAIL_VERSION and no vX.Y.Z tag in git: building as 0.0.0.")
        set(e3_version 0.0.0)
    endif()
endif()

project(e3mail
    VERSION ${e3_version}
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
option(E3MAIL_P2P "Sync devices over Iroh (needs Rust)" ON)
option(E3MAIL_CAMERA "Scan pairing QR codes with a camera (Qt Multimedia, zxing-cpp)" ON)
# OAuth 2 client IDs (ADR 0019), taken from the environment of the first
# configure (the release workflow's secrets) unless given. Without one, that
# provider is not offered; the same variables at run time work too.
set(E3MAIL_OAUTH_GOOGLE_ID "$ENV{E3MAIL_OAUTH_GOOGLE_ID}" CACHE STRING "Google OAuth client ID (desktop app)")
set(E3MAIL_OAUTH_GOOGLE_SECRET "$ENV{E3MAIL_OAUTH_GOOGLE_SECRET}" CACHE STRING
    "Google OAuth client secret (desktop apps' is not secret)")
set(E3MAIL_OAUTH_MICROSOFT_ID "$ENV{E3MAIL_OAUTH_MICROSOFT_ID}" CACHE STRING "Microsoft Entra application (client) ID")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(E3MAIL_QT_COMPONENTS Core Network Concurrent NetworkAuth)
if(E3MAIL_BUILD_APP)
    list(APPEND E3MAIL_QT_COMPONENTS Gui Qml Quick QuickControls2 Svg LinguistTools)
    # Notifications: the freedesktop service over D-Bus on Linux, Android's
    # own through JNI (QtCore), the system tray (Qt Widgets) elsewhere.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND E3MAIL_QT_COMPONENTS DBus)
    elseif(NOT ANDROID)
        list(APPEND E3MAIL_QT_COMPONENTS Widgets)
    endif()
endif()
if(E3MAIL_BUILD_TESTS)
    list(APPEND E3MAIL_QT_COMPONENTS Test)
endif()
find_package(Qt6 6.10 REQUIRED COMPONENTS ${E3MAIL_QT_COMPONENTS})
# The camera reads pairing codes, so it needs the app and device sync.
if(E3MAIL_CAMERA AND E3MAIL_BUILD_APP AND E3MAIL_P2P)
    find_package(Qt6 QUIET COMPONENTS Multimedia)
    if(NOT Qt6Multimedia_FOUND)
        message(WARNING "Qt Multimedia not found: building without scanning pairing codes (E3MAIL_CAMERA).")
        set(E3MAIL_CAMERA OFF)
    endif()
else()
    set(E3MAIL_CAMERA OFF)
endif()
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
if(E3MAIL_P2P)
    include(E3mailP2p)
endif()
if(E3MAIL_CAMERA)
    include(E3mailZxing)
endif()

add_subdirectory(src/core)
add_subdirectory(src/oauthflow)
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

# Android packages through Qt's androiddeployqt (src/app, packaging/android).
if(NOT ANDROID)
    include(E3mailPackaging)
endif()
