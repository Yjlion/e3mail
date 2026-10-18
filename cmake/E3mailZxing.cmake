# zxing-cpp (Apache-2.0): reads the pairing QR code from the camera. Fetched
# at one pinned release for every platform, readers only. 2.3.0, not 3.x:
# 3.1.1 does not build with Xcode 15's libc++ (LocalGrid.cpp, construct_at),
# and 2.x has no switch to leave out other formats.
include(FetchContent)
set(ZXING_READERS ON CACHE BOOL "" FORCE)
set(ZXING_WRITERS OFF CACHE STRING "" FORCE)
set(ZXING_C_API OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
# A static library inside e3mail, whatever BUILD_SHARED_LIBS says elsewhere.
set(e3_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(zxing
    URL https://github.com/zxing-cpp/zxing-cpp/archive/refs/tags/v2.3.0.tar.gz
    URL_HASH SHA256=64e4139103fdbc57752698ee15b5f0b0f7af9a0331ecbdc492047e0772c417ba
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR core
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(zxing)
set(BUILD_SHARED_LIBS ${e3_shared})
# qt_standard_project_setup turns moc on for every target; zxing has a Qt
# header of its own that would then need Qt Gui.
set_target_properties(ZXing PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
