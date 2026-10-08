#!/usr/bin/env bash
# Installs what an Android build needs under one directory: the Android SDK
# command-line tools, platform 34, build-tools, NDK r27c, and Qt for Android
# with the matching desktop Qt (androiddeployqt and the host tools must be the
# same version). Needs JDK 17 on PATH and python3. CI uses its own actions
# instead; this is for building locally.
#
#   scripts/android-toolchain.sh <dir> [abi...]    (default abis: x86_64 arm64_v8a)
#
# Prints the environment the android-* presets read.
set -euo pipefail
DIR=${1:?usage: android-toolchain.sh <dir> [abi...]}
shift
ABIS=("$@")
[[ ${#ABIS[@]} -gt 0 ]] || ABIS=(x86_64 arm64_v8a)
QT_VERSION=${QT_VERSION:-6.8.3}
# r27c: r26b's libc++ is too old for Botan 3 (no operator<=> it needs).
NDK_VERSION=${NDK_VERSION:-27.2.12479018}
TOOLS_ZIP=commandlinetools-linux-11076708_latest.zip

mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)
SDK="$DIR/sdk"

if [[ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]]; then
    curl -fsSLo "$DIR/$TOOLS_ZIP" "https://dl.google.com/android/repository/$TOOLS_ZIP"
    mkdir -p "$SDK/cmdline-tools"
    unzip -q -o "$DIR/$TOOLS_ZIP" -d "$SDK/cmdline-tools"
    rm -rf "$SDK/cmdline-tools/latest"
    mv "$SDK/cmdline-tools/cmdline-tools" "$SDK/cmdline-tools/latest"
    rm "$DIR/$TOOLS_ZIP"
fi
yes | "$SDK/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$SDK" --licenses >/dev/null || true
"$SDK/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$SDK" \
    "platform-tools" "platforms;android-34" "build-tools;34.0.0" "ndk;$NDK_VERSION"

if [[ ! -x "$DIR/aqt-venv/bin/aqt" ]]; then
    python3 -m venv "$DIR/aqt-venv"
    "$DIR/aqt-venv/bin/pip" -q install aqtinstall
fi
AQT="$DIR/aqt-venv/bin/aqt"
# A marker, not the directory: an interrupted install leaves a partial one
# (the desktop Qt without its ICU, whose tools then cannot start).
qt() { # <arch dir> <aqt args...>
    local done="$DIR/Qt/$QT_VERSION/$1/.e3-installed"; shift
    [[ -f "$done" ]] && return
    "$AQT" install-qt "$@" -O "$DIR/Qt"
    touch "$done"
}
qt gcc_64 linux desktop "$QT_VERSION" linux_gcc_64
for abi in "${ABIS[@]}"; do
    qt "android_$abi" all_os android "$QT_VERSION" "android_$abi"
done

# A throwaway signing key, as Android Studio makes for debug builds: the
# packages are not published, but Android installs only signed ones.
if [[ ! -f "$DIR/debug.keystore" ]]; then
    keytool -genkeypair -keystore "$DIR/debug.keystore" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=e3mail debug" >/dev/null
fi

cat <<EOF
export ANDROID_SDK_ROOT=$SDK
export ANDROID_NDK_ROOT=$SDK/ndk/$NDK_VERSION
export QT_HOST_PATH=$DIR/Qt/$QT_VERSION/gcc_64
export QT_ANDROID_ROOT=$DIR/Qt/$QT_VERSION     # the presets append android_<abi>
export QT_ANDROID_KEYSTORE_PATH=$DIR/debug.keystore QT_ANDROID_KEYSTORE_ALIAS=androiddebugkey
export QT_ANDROID_KEYSTORE_STORE_PASS=android QT_ANDROID_KEYSTORE_KEY_PASS=android
EOF
