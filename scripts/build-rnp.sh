#!/usr/bin/env bash
# Builds RNP from source and installs it under a prefix, for platforms whose
# package manager has none (Windows) or an old one. Needs Botan and json-c
# findable by CMake: pass a vcpkg toolchain in CMAKE_TOOLCHAIN_FILE if they
# come from vcpkg.
#
#   scripts/build-rnp.sh <prefix> [extra cmake args...]
set -euo pipefail
PREFIX=${1:?usage: build-rnp.sh <prefix> [cmake args...]}
shift
RNP_VERSION=${RNP_VERSION:-v0.18.1}
WORK=${RNP_WORK:-$(mktemp -d)}

git clone --depth 1 --branch "$RNP_VERSION" --recurse-submodules --shallow-submodules \
    https://github.com/rnpgp/rnp.git "$WORK/rnp"
CXXFLAGS_EXTRA=""
if [[ "$(uname -s)" == Linux* ]]; then
    # Newer GCC no longer pulls these in transitively.
    CXXFLAGS_EXTRA="-include cstring -include cstdint"
fi
cmake -S "$WORK/rnp" -B "$WORK/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS_EXTRA" \
    -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DENABLE_DOC=OFF \
    -DENABLE_SM2=OFF -DENABLE_IDEA=OFF -DCRYPTO_BACKEND="${RNP_CRYPTO_BACKEND:-botan3}" \
    "$@"
cmake --build "$WORK/build" --config Release --parallel
cmake --install "$WORK/build" --config Release
echo "RNP installed in $PREFIX"
