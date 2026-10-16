# zxing-cpp (Apache-2.0): reads the pairing QR code from the camera. Fetched
# at one pinned release for every platform, readers only, QR codes only.
include(FetchContent)
set(ZXING_READERS ON CACHE BOOL "" FORCE)
set(ZXING_WRITERS OFF CACHE STRING "" FORCE)
set(ZXING_C_API OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_DEPENDENCIES LOCAL CACHE STRING "" FORCE)
foreach(format 1D AZTEC DATAMATRIX MAXICODE PDF417)
    set(ZXING_ENABLE_${format} OFF CACHE BOOL "" FORCE)
endforeach()
set(ZXING_ENABLE_QRCODE ON CACHE BOOL "" FORCE)
# A static library inside e3mail, whatever BUILD_SHARED_LIBS says elsewhere.
set(e3_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(zxing
    URL https://github.com/zxing-cpp/zxing-cpp/archive/refs/tags/v3.1.1.tar.gz
    URL_HASH SHA256=7286b1e6ade66fe82b7c8208b4595deeb55d6486b410834fdc65702f46650542
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR core
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(zxing)
set(BUILD_SHARED_LIBS ${e3_shared})
# qt_standard_project_setup turns moc on for every target; zxing has a Qt
# header of its own that would then need Qt Gui.
set_target_properties(ZXing PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
