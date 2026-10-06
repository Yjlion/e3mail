# Installation layout and CPack, per platform:
#   Linux   .deb and .tar.gz (an AppImage is built from the install tree by
#           packaging/linux/build-appimage.sh)
#   Windows NSIS installer and a portable .zip (portable.txt makes the app keep
#           its data beside itself)
#   macOS   a .dmg holding e3mail.app
# Qt and the QML modules the app uses are deployed into the install tree with
# Qt's own deployment script, so each package carries what it needs.

if(E3MAIL_BUILD_APP)
    if(LINUX)
        install(FILES packaging/linux/e3mail.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES packaging/linux/org.e3mail.e3mail.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
        install(FILES src/app/qml/icons/e3mail.svg
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    endif()

    option(E3MAIL_DEPLOY_QT "Copy Qt libraries and QML modules into the install tree" OFF)
    if(E3MAIL_DEPLOY_QT)
        qt_generate_deploy_qml_app_script(
            TARGET e3mail
            OUTPUT_SCRIPT e3mail_deploy_script
            NO_UNSUPPORTED_PLATFORM_ERROR
            DEPLOY_USER_QML_MODULES_ON_UNSUPPORTED_PLATFORM)
        install(SCRIPT ${e3mail_deploy_script})
    endif()
endif()

# Windows has no system copies of RNP and its dependencies: the release build
# names the directories holding their DLLs, and they are installed beside the
# executable.
set(E3MAIL_BUNDLE_DLLS_FROM "" CACHE STRING "Directories whose DLLs are installed beside e3mail (Windows)")
if(WIN32)
    foreach(dir ${E3MAIL_BUNDLE_DLLS_FROM})
        file(GLOB e3_dlls "${dir}/*.dll")
        install(FILES ${e3_dlls} DESTINATION ${CMAKE_INSTALL_BINDIR})
    endforeach()
endif()

install(FILES LICENSE NOTICE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(CPACK_PACKAGE_NAME "e3mail")
set(CPACK_PACKAGE_VENDOR "e3mail")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "e3mail")
set(CPACK_PACKAGE_EXECUTABLES "e3mail" "e3mail")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES ON)

if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_NSIS_DISPLAY_NAME "e3mail")
    set(CPACK_NSIS_PACKAGE_NAME "e3mail")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/e3mail.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\e3mail.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\e3mail.lnk' '$INSTDIR\\\\bin\\\\e3mail.exe'")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "e3mail")
else()
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "e3mail <noreply@e3mail.invalid>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "mail")
    find_program(E3_DPKG dpkg)
    if(E3_DPKG)
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    else()
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64) # no dpkg to ask
    endif()
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
endif()

include(CPack)
