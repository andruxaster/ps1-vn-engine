#!/bin/sh
# Полная сборка: данные, код, образ диска.
#   PSN00BSDK_LIBS=/путь/к/sdk/lib/libpsn00b ./build.sh
#   PSN00BSDK_LIBS=... ./build.sh release
set -e
: "${PSN00BSDK_LIBS:?укажите PSN00BSDK_LIBS=/путь/к/sdk/lib/libpsn00b}"

if [ "$1" = "release" ]; then
    python3 tools/build_data.py --release
    REL=-DVN_RELEASE=ON
else
    python3 tools/build_data.py
    REL=-DVN_RELEASE=OFF
fi

cmake -S . -B build -G Ninja $REL \
      -DCMAKE_TOOLCHAIN_FILE="$PSN00BSDK_LIBS/cmake/sdk.cmake"
cmake --build build
echo "готово: build/vn.bin, build/vn.cue"
