#!/bin/sh
# The layer build path of nui 1.2.0 against the reference (the code of before 1.2.0, kept in src/ under
# NUI_TEST_REFERENCE), 32-bit MinGW. Called by build_test.sh; runs alone too:
#   sh test/build_build_test.sh               build, compare, pixels
#   sh test/build_build_test.sh --self-test   also every NUI_FAULT_* of the build path must fail
#   sh test/build_build_test.sh --bench       also us per layer, before and after
#
#   test_build.cpp       parse / quad / draw records of the library equal to the reference's byte for
#                        byte; the merge's invariants and the phase-1 prototype; the numeric fast paths
#   test_build_pixels.lua  five captured HUD states (fixtures/layers) drawn through the real host on a
#                        real D3D8 device (cr_testhost, frames through the patched wrapper) by a module
#                        built from the reference and one from the library: the back buffers must be
#                        equal, at scale 1 and 2; and a module with NUI_FAULT_MERGE_OVERLAP must differ
#                        (the pixel check can fail)
# Outputs in NUI_TEST_OUT/build (default %TEMP%/nui_test/build); the game install is only read.
set -e
cd "$(dirname "$0")"
GXX="${NUI_GXX:-i686-w64-mingw32-g++}"
LUA="${NUI_LUA:-/c/Program Files (x86)/Lua/5.1/lua.exe}"
TOP="${NUI_TEST_OUT:-${TEMP:-/tmp}/nui_test}"
OUT="$TOP/build"
FFXI_COPY="${NUI_FFXI_COPY:?set NUI_FFXI_COPY to an unpacked FFXiMain.dll}"
INSTALL="${NUI_FFXI_DIR:-C:\\Program Files (x86)\\PlayOnline\\SquareEnix\\FINAL FANTASY XI\\}"
FLAGS="-std=c++17 -O2 -m32 -Wall -Wextra"
LINK="-static -static-libgcc -static-libstdc++"
export PATH="$(dirname "$GXX"):$PATH"
mkdir -p "$OUT"
cp "$FFXI_COPY" "$OUT/FFXiMain.dll"
FIX_W="$(cygpath -w "$PWD/fixtures")"

# lib <dir> <flags...>: every src/*.cpp into <dir>/libnui.a
lib() {
    dir="$1"; shift
    mkdir -p "$dir/obj"
    pids=""
    for src in ../src/*.cpp; do
        obj=$(basename "$src" .cpp)
        "$GXX" $FLAGS "$@" -I../src -c "$src" -o "$dir/obj/$obj.o" & pids="$pids $!"
    done
    rc=0
    for p in $pids; do wait "$p" || rc=1; done
    [ $rc = 0 ] || return 1
    rm -f "$dir/libnui.a"
    ar rcs "$dir/libnui.a" "$dir"/obj/*.o
}
# exe <dir> <flags...>: lib + test_build.exe + the module the pixel check loads (luaopen__nuitest)
exe() {
    dir="$1"; shift
    rm -rf "$dir"
    lib "$dir" "$@" || return 1
    "$GXX" $FLAGS "$@" -I../src test_build.cpp "$dir/libnui.a" -o "$dir/test_build.exe" $LINK -lpsapi & p1=$!
    "$GXX" $FLAGS "$@" '-DNUITEST_NAME="b"' -I../src -shared nuitest.cpp "$dir/libnui.a" -o "$dir/nuitest.dll" $LINK & p2=$!
    rc=0
    wait $p1 || rc=1
    wait $p2 || rc=1
    return $rc
}

echo "== build test_build (reference and library) ($OUT)"
exe "$OUT/ref" -DNUI_TEST_REFERENCE & r1=$!
exe "$OUT/new" & r2=$!
"$GXX" $FLAGS -shared testhost.cpp -o "$OUT/cr_testhost.dll" $LINK & r3=$!
rc=0
wait $r1 || rc=1
wait $r2 || rc=1
wait $r3 || rc=1
[ $rc = 0 ] || { echo "FAIL [build] test_build did not build"; exit 1; }
echo "== test_build: the reference's records"
"$OUT/ref/test_build.exe" dump "$INSTALL" "$FIX_W" "$(cygpath -w "$OUT/ref.bin")"
echo "== test_build: the library against them"
"$OUT/new/test_build.exe" check "$INSTALL" "$FIX_W" "$(cygpath -w "$OUT/ref.bin")"

# pixels <dir a> <dir b> <log>: the five states drawn by both modules, back buffers compared
pixels() {
    rm -rf "$OUT/px"; mkdir -p "$OUT/px"
    for side in a b; do
        if [ $side = a ]; then d="$1"; else d="$2"; fi
        "$LUA" test_build_pixels.lua "$(cygpath -m "$OUT/FFXiMain.dll")" "$(cygpath -m "$OUT")" "$(cygpath -m "$d/nuitest.dll")" \
            "$(cygpath -m "$PWD/fixtures")" "$INSTALL" "$(cygpath -m "$OUT/px")/$side" || return 2
    done
    same=0; differ=0
    for f in "$OUT"/px/a_*.raw; do
        g="$OUT/px/b_${f##*/a_}"
        if cmp -s "$f" "$g"; then same=$((same + 1)); else differ=$((differ + 1)); echo "  PIXELS DIFFER ${f##*/a_}"; fi
    done
    echo "  back buffers: $same equal, $differ differ"
    rm -f "$OUT"/px/*.raw
    [ $differ = 0 ]
}
echo "== pixels: the reference and the library draw the same back buffers"
pixels "$OUT/ref" "$OUT/new" || { echo "FAIL [pixels] the library's frames differ from the reference's"; exit 1; }

if [ "$1" = "--bench" ]; then
    echo "== bench"
    "$OUT/ref/test_build.exe" bench "$INSTALL" "$FIX_W"
    "$OUT/new/test_build.exe" bench "$INSTALL" "$FIX_W"
fi

[ "$1" = "--self-test" ] || { echo "build path tests: all pass"; exit 0; }

# 1.2.6: the code review's fixes in pure functions (test_build's review_checks)
FAULTS="FAST_CLIPPED:quad PARSE_NO_TERM:parse MERGE_OVERLAP:merge MERGE_GREY:merge
ERR_SUBSTR:decode NO_SIZE_CHECK:decode NO_SIGNATURE:sig SUM32:pic LUA_READS_ALL:read NO_CLAMP:clamp
TITLE_NO_GUARD:title RECIPE_LAST:recipe NO_WIDTH_CACHE:assets HANDLES_RESTART:handles
DXT_ONLY:fmt ANSI_PATHS:fmt"
echo "== build self-test: every fault variant must fail its check"
caught=0; missed=0
for ft in $FAULTS; do
    f="${ft%%:*}"; t="${ft##*:}"
    dir="$OUT/fault_$f"
    exe "$dir" "-DNUI_FAULT_$f" > "$dir.build.log" 2>&1 || { echo "build failed: $f"; cat "$dir.build.log"; exit 1; }
    if "$dir/test_build.exe" check "$INSTALL" "$FIX_W" "$(cygpath -w "$OUT/ref.bin")" > "$dir/check.log" 2>&1; then
        echo "NOT CAUGHT  NUI_FAULT_$f by [$t] (log $dir/check.log)"; missed=$((missed + 1))
    else
        why=$(grep -m1 "^FAIL \[$t\]" "$dir/check.log" || true)
        if [ -z "$why" ]; then
            echo "WRONG CHECK NUI_FAULT_$f: failed, but not by [$t]: $(grep -m1 '^FAIL' "$dir/check.log")"; missed=$((missed + 1))
        else
            echo "caught      NUI_FAULT_$f by $t: $(echo "$why" | cut -c1-150)"; caught=$((caught + 1))
        fi
    fi
done
# the pixel check itself must be able to fail: a merge that ignores overlaps changes frames
if pixels "$OUT/ref" "$OUT/fault_MERGE_OVERLAP" > "$OUT/fault_pixels.log" 2>&1; then
    echo "NOT CAUGHT  NUI_FAULT_MERGE_OVERLAP by [pixels] (log $OUT/fault_pixels.log)"; missed=$((missed + 1))
else
    echo "caught      NUI_FAULT_MERGE_OVERLAP by pixels: $(grep -m1 'back buffers:' "$OUT/fault_pixels.log")"; caught=$((caught + 1))
fi
echo "build self-test: $caught caught, $missed missed"
[ "$missed" -eq 0 ]
