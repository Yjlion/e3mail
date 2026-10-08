# The composer's whitelist emitter, separate so tests can reach it.
add_library(e3richtext STATIC controllers/RichText.cpp controllers/RichText.h)
target_include_directories(e3richtext PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controllers)
target_link_libraries(e3richtext PUBLIC e3core Qt6::Gui)
e3mail_set_warnings(e3richtext)

# New-mail notifications: gathering and wording, separate so tests can reach
# it. The platform backends are in the app.
add_library(e3notifier STATIC controllers/Notifier.cpp controllers/Notifier.h)
target_include_directories(e3notifier PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controllers)
target_link_libraries(e3notifier PUBLIC Qt6::Core)
e3mail_set_warnings(e3notifier)

qt_add_executable(e3mail WIN32 MACOSX_BUNDLE
    main.cpp
    NetworkBarrier.cpp NetworkBarrier.h
    controllers/NotifierBackends.cpp controllers/NotifierBackends.h
)

file(GLOB E3_QML_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS
    qml/*.qml qml/components/*.qml)
file(GLOB E3_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS qml/icons/*.svg)
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(e3mail
    URI E3mail
    VERSION 1.0
    QML_FILES ${E3_QML_FILES}
    RESOURCES ${E3_ICONS}
    SOURCES
        controllers/MailApp.cpp controllers/MailApp.h
        controllers/InlineImages.cpp controllers/InlineImages.h
        controllers/Languages.cpp controllers/Languages.h
        controllers/Composer.cpp controllers/Composer.h
        controllers/SetupController.cpp controllers/SetupController.h
        models/MessageListModel.cpp models/MessageListModel.h
)

# Translations: src/app/translations/e3mail_<lang>.ts, one per language in
# I18N_TRANSLATED_LANGUAGES, plus e3mail_en.ts for English plurals. Refresh
# them from the sources with `cmake --build --preset dev --target
# update_translations`. Compiled into the binary under :/i18n.
qt_add_translations(TARGETS e3mail
    SOURCE_TARGETS e3mail e3core e3notifier
    TS_FILE_DIR translations
    PLURALS_TS_FILE translations/e3mail_en.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none)

target_include_directories(e3mail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/controllers ${CMAKE_CURRENT_SOURCE_DIR}/models)
target_link_libraries(e3mail PRIVATE
    e3core e3richtext e3notifier Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(e3mail PRIVATE Qt6::DBus)
elseif(NOT ANDROID)
    target_link_libraries(e3mail PRIVATE Qt6::Widgets)
endif()
e3mail_set_warnings(e3mail)

set_target_properties(e3mail PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER org.e3mail.e3mail
    MACOSX_BUNDLE_BUNDLE_NAME e3mail
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

if(ANDROID)
    # Version code: major, minor, patch, two digits each (0.1.1 -> 101).
    math(EXPR e3_version_code
        "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
    set_target_properties(e3mail PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR ${PROJECT_SOURCE_DIR}/packaging/android
        QT_ANDROID_PACKAGE_NAME org.e3mail.e3mail
        QT_ANDROID_APP_NAME e3mail
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 34
        QT_ANDROID_VERSION_NAME ${PROJECT_VERSION}
        QT_ANDROID_VERSION_CODE ${e3_version_code})
    # QSslSocket loads OpenSSL at run time: KDAB's prebuilt libraries, from
    # scripts/android-deps.sh.
    set(E3MAIL_ANDROID_OPENSSL "" CACHE PATH "KDAB's android_openssl checkout")
    if(NOT E3MAIL_ANDROID_OPENSSL)
        message(FATAL_ERROR "Set E3MAIL_ANDROID_OPENSSL (scripts/android-deps.sh clones it).")
    endif()
    include(${E3MAIL_ANDROID_OPENSSL}/android_openssl.cmake)
    add_android_openssl_libraries(e3mail)
elseif(WIN32)
    target_sources(e3mail PRIVATE ${PROJECT_SOURCE_DIR}/packaging/windows/e3mail.rc)
elseif(APPLE)
    set(E3_ICNS ${PROJECT_SOURCE_DIR}/packaging/macos/e3mail.icns)
    set_source_files_properties(${E3_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(e3mail PRIVATE ${E3_ICNS})
    set_target_properties(e3mail PROPERTIES MACOSX_BUNDLE_ICON_FILE e3mail.icns)
endif()

# On Android the app is a library inside the package androiddeployqt builds.
if(NOT ANDROID)
    install(TARGETS e3mail
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
