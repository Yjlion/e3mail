#!/usr/bin/env bash
# Builds e3mail-<version>-x86_64.AppImage from a configured build directory,
# using linuxdeploy and its Qt plugin (which deploys the QML modules the app
# imports).
#
#   packaging/linux/build-appimage.sh <build-dir>
set -euo pipefail
BUILD=${1:?usage: build-appimage.sh <build-dir>}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS=${APPIMAGE_TOOLS:-$BUILD/appimage-tools}
APPDIR=$BUILD/AppDir
mkdir -p "$TOOLS"
for tool in linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage; do
    if [[ ! -x "$TOOLS/$tool" ]]; then
        repo=${tool%-x86_64.AppImage}
        curl -fsSL -o "$TOOLS/$tool" \
            "https://github.com/linuxdeploy/$repo/releases/download/continuous/$tool"
        chmod +x "$TOOLS/$tool"
    fi
done
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr
export QML_SOURCES_PATHS="$ROOT/src/app/qml"
export EXTRA_QT_MODULES="svg;waylandcompositor"
# xcb is deployed by default; Wayland for modern desktops, offscreen for CI.
# The Wayland plugin's name depends on the Qt version (libqwayland.so since
# 6.10, libqwayland-generic.so and libqwayland-egl.so before), so take
# whichever this Qt has.
QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
PLATFORMS="$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms"
plugins="libqoffscreen.so"
for f in "$PLATFORMS"/libqwayland*.so; do
    if [[ -e $f ]]; then plugins+=";$(basename "$f")"; fi
done
export EXTRA_PLATFORM_PLUGINS="$plugins"
echo "extra platform plugins: $EXTRA_PLATFORM_PLUGINS"
export APPIMAGE_EXTRACT_AND_RUN=1
# linuxdeploy bundles an old strip that rejects newer ELF sections (.relr.dyn);
# stripping system libraries buys little, so do not.
export NO_STRIP=1
export OUTPUT="e3mail-$(grep -m1 'CMAKE_PROJECT_VERSION:' "$BUILD/CMakeCache.txt" | cut -d= -f2)-x86_64.AppImage"
"$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" \
    --desktop-file "$ROOT/packaging/linux/e3mail.desktop" \
    --icon-file "$ROOT/src/app/qml/icons/e3mail.svg" \
    --plugin qt --output appimage
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
echo "built $OUTPUT"
