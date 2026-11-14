#!/bin/sh
# The shared texture cache through the whole renderer (nui design E1, E2, E3c, E4, E5, E7, E8, E11; step
# 5b), 32-bit MinGW. Called by build_test.sh; runs alone too:
#   sh test/build_render_test.sh                  build + run test_render.exe
#   sh test/build_render_test.sh --self-test      also build the library once per NUI_FAULT_* and
#                                                           require each variant to fail its target test
#   sh test/build_render_test.sh --update-golden  rewrite golden_fmt.json (after bumping a kFmt in
#                                                           src/cache.hpp, or after a game update changed the DATs)
#
# nuitest.cpp + every src/*.cpp is built as separate module DLLs: nuitest_a/b/c (separate addons
# as far as Windows, UiHook and the cache can tell) and nuitest_z (cache major 99). test_render.exe loads
# them through the stock lua5.1.dll, frames through UiHook's patch of the unpacked FFXiMain.dll copy on a
# real D3D8 device (system d3d8; gr (grey, 1.1.0) and the bench also on the game's dgVoodoo D3D8.dll when
# NUI_DGVOODOO points at it, and gr must pass there).
set -e
cd "$(dirname "$0")"
GXX="${NUI_GXX:-i686-w64-mingw32-g++}"
OUT="${NUI_TEST_OUT:-${TEMP:-/tmp}/nui_test}/render"
FFXI_COPY="${NUI_FFXI_COPY:?set NUI_FFXI_COPY to an unpacked FFXiMain.dll}"
INSTALL="${NUI_FFXI_DIR:-C:\\Program Files (x86)\\PlayOnline\\SquareEnix\\FINAL FANTASY XI\\}"
DGV="${NUI_DGVOODOO-/c/Program Files (x86)/PlayOnline/SquareEnix/PlayOnlineViewer/D3D8.dll}"
PY="${NUI_PYTHON:-python}"
FLAGS="-std=c++17 -O2 -m32 -Wall -Wextra"
LINK="-static -static-libgcc -static-libstdc++"
export PATH="$(dirname "$GXX"):$PATH"
mkdir -p "$OUT"
cp "$FFXI_COPY" "$OUT/FFXiMain.dll"
"$PY" make_big_png.py "$OUT/big.png" > /dev/null    # pd (1.2.1): a picture that costs a real decode

# lib <dir> <flags...>: every src/*.cpp, compiled in parallel, into <dir>/libnui.a
lib() {
    dir="$1"; shift
    mkdir -p "$dir/obj"
    pids=""
    for f in ../src/*.cpp; do
        b=$(basename "$f" .cpp)
        "$GXX" $FLAGS -DNUI_TEST_CLOCK "$@" -I../src -c "$f" -o "$dir/obj/$b.o" & pids="$pids $!"
    done
    rc=0
    for p in $pids; do wait "$p" || rc=1; done
    [ $rc = 0 ] || return 1
    rm -f "$dir/libnui.a"
    ar rcs "$dir/libnui.a" "$dir"/obj/*.o
}

# build <dir> <flags...>: the library twice (major 1, major 99), the four modules and the driver
build() {
    dir="$1"; shift
    mkdir -p "$dir"
    lib "$dir/m1" "$@" & p1=$!
    lib "$dir/m99" -DNUICACHE_MAJOR=99 "$@" & p2=$!
    "$GXX" $FLAGS "$@" test_render.cpp -o "$dir/test_render.exe" $LINK -lpsapi & p3=$!
    wait $p1 && wait $p2 && wait $p3
    pids=""
    for n in a b c; do
        "$GXX" $FLAGS -DNUI_TEST_CLOCK "$@" -DNUITEST_NAME="\"$n\"" -I../src -shared nuitest.cpp "$dir/m1/libnui.a" \
            -o "$dir/nuitest_$n.dll" $LINK & pids="$pids $!"
    done
    "$GXX" $FLAGS -DNUI_TEST_CLOCK -DNUICACHE_MAJOR=99 "$@" -DNUITEST_NAME='"z"' -I../src -shared nuitest.cpp "$dir/m99/libnui.a" \
        -o "$dir/nuitest_z.dll" $LINK & pids="$pids $!"
    rc=0
    for p in $pids; do wait "$p" || rc=1; done
    return $rc
}

run() {   # run <dir> [args...]
    dir="$1"; shift
    "$dir/test_render.exe" "$(cygpath -w "$dir")" "$(cygpath -w "$OUT/FFXiMain.dll")" "$INSTALL" "$(cygpath -w "$PWD/fixtures")" \
        --golden "$(cygpath -w "$PWD/golden_fmt.json")" --big "$(cygpath -w "$OUT/big.png")" "$@"
}

echo "== build test_render ($OUT)"
build "$OUT/ok"
if [ "$1" = "--update-golden" ]; then
    run "$OUT/ok" --only e7,wt --update-golden
    exit $?
fi
echo "== test_render"
run "$OUT/ok"
if [ -n "$DGV" ] && [ -f "$DGV" ]; then
    echo "== grey (gr) and bench on the game's dgVoodoo D3D8.dll"
    run "$OUT/ok" --only gr,bench --d3d8 "$(cygpath -w "$DGV")" > "$OUT/dgvoodoo.log" 2>&1 || {
        grep -E '^FAIL|^CRASH|test_render:' "$OUT/dgvoodoo.log"; echo "FAIL: gr on dgVoodoo"; exit 1; }
    grep -E '^  16 icons|^  max channel|^  icon|^FAIL|^CRASH|test_render:' "$OUT/dgvoodoo.log"
fi

[ "$1" = "--self-test" ] || exit 0

# fault : the test that must fail with it (a CRASH in that test counts: a double Release frees a live texture)
FAULTS="KEY_HAS_MODULE:e1 KEY_HAS_MODULE:e4 DETACH_KEEPS_BITS:e2 DOUBLE_RELEASE:e2 NO_GEN_CHECK:e5 MAJOR_SHARED:e3c
NO_FALLBACK:e8 ICON_PIXEL:fmt PICTURE_PIXEL:fmt WT1_CASE:wt FRAME_BELOW:fr FRAME_ALPHA:fb FRAME_BLEND:fb TITLE_OLD:ti GREY_NO_RESTORE:gr GREY_STICKY:gr NO_FPU_PRESERVE:fpu
BB_ALWAYS:bb BB_NO_FALLBACK:bb VC_NO_KEY:vc VC_NO_SCALE:vc VC_NO_TEXGEN:vc LP_IGNORE_CLIP:lp RENDER_READS:ic BUILD_IN_LOCK:lk
BUILD_IN_PLACE:lk NO_HOLD:pd HOLD_NO_BIT:pd ALWAYS_DECODE:pd SYNC_DECODE:pd NO_LUA_WAIT:pd THREAD_NO_REF:pd
NO_LOCK_LEAVE:ex CREATE_THROWS_THROUGH:ex RELEASE_ON_LUA:rt NO_FINAL_PASS:rt
STICKY_UPLOAD:rv NO_STREAM_RESTORE:rv HANDLES_RESTART:rv STOP_WAITS_DECODE:tl"
echo "== render self-test: every fault variant must fail its target test"
caught=0; missed=0; built=" "
for ft in $FAULTS; do
    f="${ft%%:*}"; t="${ft##*:}"
    dir="$OUT/fault_$f"
    case "$built" in *" $f "*) ;; *) build "$dir" "-DNUI_FAULT_$f" > "$dir.build.log" 2>&1 || { echo "build failed: $f"; cat "$dir.build.log"; exit 1; }; built="$built$f " ;; esac
    if run "$dir" --only "$t" > "$dir/$t.log" 2>&1; then
        echo "NOT CAUGHT  NUI_FAULT_$f by $t (log $dir/$t.log)"; missed=$((missed + 1))
    else
        why=$(grep -m1 -E "^FAIL \[$t\]|^CRASH" "$dir/$t.log" || true)
        if [ -z "$why" ]; then
            echo "WRONG CHECK NUI_FAULT_$f: failed, but not by [$t]: $(grep -m1 '^FAIL' "$dir/$t.log")"; missed=$((missed + 1))
        else
            echo "caught      NUI_FAULT_$f by $t: $(echo "$why" | cut -c1-150)"; caught=$((caught + 1))
        fi
    fi
done
echo "render self-test: $caught caught, $missed missed"
[ "$missed" -eq 0 ]
