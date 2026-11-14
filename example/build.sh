#!/bin/sh
# Build _nuidemo.dll: the example module plus the whole nui library, one static 32-bit DLL.
#
#   sh example/build.sh                     -> example/out/_nuidemo.dll
#   NUI_GXX=<g++> sh example/build.sh       pick the compiler
#   NUI_OUT=<path> sh example/build.sh      build somewhere else (the game locks a loaded DLL, so
#                                           build to another path, unload the addon, copy over)
#
# Copy this file into your own addon and change three things: the glue file, the .def file and the
# output name. Nothing else about the build is addon-specific.
set -e
cd "$(dirname "$0")"
GXX="${NUI_GXX:-i686-w64-mingw32-g++}"
OUT="${NUI_OUT:-out/_nuidemo.dll}"
SRC="${NUI_SRC:-../src}"            # in a real addon: the vendored copy, plugin/nui
mkdir -p "$(dirname "$OUT")"
"$GXX" -std=c++17 -O2 -m32 -shared -Wall -Wextra -I"$SRC" \
    DemoModule.cpp "$SRC"/*.cpp exports.def \
    -o "$OUT" -s -static -static-libgcc -static-libstdc++
echo "built $OUT"
