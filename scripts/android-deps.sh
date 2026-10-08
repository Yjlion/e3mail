#!/usr/bin/env bash
# Builds the native dependencies for one Android ABI under a prefix:
#   - vcpkg's botan, json-c, zlib, bzip2, icu and sqlite3 (static, release
#     only, from vcpkg.json) in <prefix>/vcpkg_installed/<triplet>;
#   - RNP, static, in <prefix>;
#   - QtKeychain (the Android Keystore), in <prefix>;
#   - KDAB's prebuilt OpenSSL in <prefix>/android_openssl. Qt loads
#     libssl_3.so at run time, so it must be shared, which vcpkg's Android
#     triplets do not build.
#
#   scripts/android-deps.sh <arm64-v8a|x86_64> <prefix>
#
# Needs ANDROID_NDK_ROOT, QT_HOST_PATH and QT_ANDROID (Qt for this ABI), as
# printed by scripts/android-toolchain.sh.
set -euo pipefail
ABI=${1:?usage: android-deps.sh <arm64-v8a|x86_64> <prefix>}
PREFIX=${2:?usage: android-deps.sh <arm64-v8a|x86_64> <prefix>}
: "${ANDROID_NDK_ROOT:?}" "${QT_HOST_PATH:?}" "${QT_ANDROID:?}"
RNP_VERSION=${RNP_VERSION:-v0.18.1}
QTKEYCHAIN_VERSION=${QTKEYCHAIN_VERSION:-0.15.0}
# The same pin as .github/actions/windows-deps (trap 23).
VCPKG_COMMIT=${VCPKG_COMMIT:-f451d04d496aa089e294a1a2d799a269788d47ba}
API=28

case "$ABI" in
    arm64-v8a) TRIPLET=arm64-android-release ;;
    x86_64) TRIPLET=x64-android-release ;;
    *) echo "unknown ABI $ABI" >&2; exit 2 ;;
esac
SRC=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
WORK=${E3_WORK:-$PREFIX/work}
mkdir -p "$WORK"
NDK_TOOLCHAIN="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"

# vcpkg, pinned.
VCPKG=${E3_VCPKG:-$WORK/vcpkg}
if [[ ! -x "$VCPKG/vcpkg" ]]; then
    git init -q "$VCPKG"
    git -C "$VCPKG" fetch -q --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_COMMIT"
    git -C "$VCPKG" checkout -q FETCH_HEAD
    "$VCPKG/bootstrap-vcpkg.sh" -disableMetrics
fi
export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"
INSTALLED="$PREFIX/vcpkg_installed"
"$VCPKG/vcpkg" install --vcpkg-root "$VCPKG" --x-manifest-root="$SRC" \
    --x-install-root="$INSTALLED" --overlay-triplets="$SRC/packaging/android/vcpkg-triplets" \
    --triplet "$TRIPLET" --host-triplet x64-linux-release
DEPS="$INSTALLED/$TRIPLET"

# RNP, against those packages. The NDK's clang needs none of the Linux GCC
# workarounds in build-rnp.sh, which apply only to native Linux builds.
if [[ ! -f "$PREFIX/lib/librnp.a" ]]; then
    RNP_WORK="$WORK/rnp-$ABI" E3_CROSS=1 RNP_VERSION="$RNP_VERSION" BUILD_SHARED=OFF \
        "$SRC/scripts/build-rnp.sh" "$PREFIX" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK_TOOLCHAIN" -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_FIND_ROOT_PATH="$DEPS" -DCMAKE_PREFIX_PATH="$DEPS" \
        -DENABLE_RNP_TOOLS=OFF
fi

# QtKeychain, built with Qt for Android.
if [[ ! -d "$PREFIX/lib/cmake/Qt6Keychain" ]]; then
    rm -rf "$WORK/qtkeychain"
    git clone -q --depth 1 --branch "$QTKEYCHAIN_VERSION" https://github.com/frankosterfeld/qtkeychain.git \
        "$WORK/qtkeychain"
    "$QT_ANDROID/bin/qt-cmake" -S "$WORK/qtkeychain" -B "$WORK/qtkeychain-build-$ABI" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DQT_HOST_PATH="$QT_HOST_PATH" -DANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-}" \
        -DANDROID_NDK_ROOT="$ANDROID_NDK_ROOT" \
        -DBUILD_WITH_QT6=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_TRANSLATIONS=OFF -DBUILD_TEST_APPLICATION=OFF
    cmake --build "$WORK/qtkeychain-build-$ABI"
    cmake --install "$WORK/qtkeychain-build-$ABI"
fi

# OpenSSL for QSslSocket.
if [[ ! -d "$PREFIX/android_openssl" ]]; then
    git clone -q --depth 1 https://github.com/KDAB/android_openssl.git "$PREFIX/android_openssl"
fi

echo "Android dependencies for $ABI are in $PREFIX"
