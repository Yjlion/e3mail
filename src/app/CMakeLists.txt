# The composer's whitelist emitter, separate so tests can reach it.
add_library(e3richtext STATIC controllers/RichText.cpp controllers/RichText.h)
target_include_directories(e3richtext PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controllers)
target_link_libraries(e3richtext PUBLIC e3core Qt6::Gui)
e3mail_set_warnings(e3richtext)

qt_add_executable(e3mail WIN32 MACOSX_BUNDLE
    main.cpp
    NetworkBarrier.cpp NetworkBarrier.h
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
    SOURCE_TARGETS e3mail e3core
    TS_FILE_DIR translations
    PLURALS_TS_FILE translations/e3mail_en.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none)

target_include_directories(e3mail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/controllers ${CMAKE_CURRENT_SOURCE_DIR}/models)
target_link_libraries(e3mail PRIVATE
    e3core e3richtext Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
e3mail_set_warnings(e3mail)

set_target_properties(e3mail PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER org.e3mail.e3mail
    MACOSX_BUNDLE_BUNDLE_NAME e3mail
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

if(WIN32)
    target_sources(e3mail PRIVATE ${PROJECT_SOURCE_DIR}/packaging/windows/e3mail.rc)
elseif(APPLE)
    set(E3_ICNS ${PROJECT_SOURCE_DIR}/packaging/macos/e3mail.icns)
    set_source_files_properties(${E3_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(e3mail PRIVATE ${E3_ICNS})
    set_target_properties(e3mail PROPERTIES MACOSX_BUNDLE_ICON_FILE e3mail.icns)
endif()

install(TARGETS e3mail
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
