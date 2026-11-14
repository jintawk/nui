#!/bin/sh
# The shared texture cache's tests (NuiCache.h, nui design E1/E2/E3c/E3b/E4/E5/E6/E8), 32-bit MinGW.
#   sh test/build_cache_test.sh               build + run every test
#   sh test/build_cache_test.sh --self-test   also build each NUI_FAULT_* variant and
#                                                       require its target test to FAIL
#   sh test/build_cache_test.sh --dump-layout rewrite layout_nuicache.txt (only when a
#                                                       field is APPENDED in a new revision)
#
# cache_mod.cpp is built as separate modules: cache_a/b/c/d (same code, separate DLLs, like
# separate addons), cache_r2 (NUICACHE_REV 2), cache_x (NUICACHE_ENTRIES 200: an ABI mismatch that
# must fall back to private) and cache_z (NUICACHE_MAJOR 99: its own mapping). test_cache.exe loads
# and FreeLibrary's them in every order. E4/E5 real use the system d3d8 on hidden windows, and the
# game's dgVoodoo D3D8.dll as well when NUI_DGVOODOO points at it (default: the PlayOnline install,
# only read). Outputs go to NUI_TEST_OUT (default: a temp folder), never into the repo.
set -e
cd "$(dirname "$0")"
GXX="${NUI_GXX:-i686-w64-mingw32-g++}"
OUT="${NUI_TEST_OUT:-${TEMP:-/tmp}/nui_test}/cache"
DGV="${NUI_DGVOODOO-/c/Program Files (x86)/PlayOnline/SquareEnix/PlayOnlineViewer/D3D8.dll}"
FLAGS="-std=c++17 -O2 -m32 -Wall -Wextra -static -static-libgcc -static-libstdc++"
export PATH="$(dirname "$GXX"):$PATH"

# build <dir> <extra flags...>: every module and the exe, in parallel
build() {
    dir="$1"; shift
    mkdir -p "$dir"
    pids=""
    for n in a b c d; do
        "$GXX" $FLAGS "$@" -DNUITEST_NAME="\"$n\"" -shared cache_mod.cpp -o "$dir/cache_$n.dll" & pids="$pids $!"
    done
    "$GXX" $FLAGS "$@" -DNUITEST_NAME='"r2"' -DNUICACHE_REV=2 -shared cache_mod.cpp -o "$dir/cache_r2.dll" & pids="$pids $!"
    "$GXX" $FLAGS "$@" -DNUITEST_NAME='"x"' -DNUICACHE_ENTRIES=200 -shared cache_mod.cpp -o "$dir/cache_x.dll" & pids="$pids $!"
    "$GXX" $FLAGS "$@" -DNUITEST_NAME='"z"' -DNUICACHE_MAJOR=99 -shared cache_mod.cpp -o "$dir/cache_z.dll" & pids="$pids $!"
    "$GXX" $FLAGS "$@" test_cache.cpp -o "$dir/test_cache.exe" -lpsapi & pids="$pids $!"
    rc=0
    for p in $pids; do wait "$p" || rc=1; done
    return $rc
}

run() {   # run <dir> [args...]
    dir="$1"; shift
    "$dir/test_cache.exe" "$(cygpath -w "$dir")" --layout "$(cygpath -w "$PWD/layout_nuicache.txt")" "$@"
}

echo "== build ($OUT)"
build "$OUT/ok"
if [ "$1" = "--dump-layout" ]; then
    "$OUT/ok/test_cache.exe" x --dump-layout "$(cygpath -w "$PWD/layout_nuicache.txt")"
    exit 0
fi
echo "== test_cache"
if [ -n "$DGV" ] && [ -f "$DGV" ]; then run "$OUT/ok" --d3d8 "$(cygpath -w "$DGV")"; else run "$OUT/ok"; fi

[ "$1" = "--self-test" ] || exit 0

# fault : the test that must fail with it
FAULTS="KEY_HAS_MODULE:e1 KEY_HAS_MODULE:e4 SWEEP_FREES_HELD:e1 LOCK_NEVER_RELEASED:e1
DETACH_KEEPS_BITS:e2 DOUBLE_RELEASE:e2 STOP_KEEPS_UNHELD:e2 NO_HOLDER_CLEAR:evict NO_RECLAIM:evict
MAJOR_SHARED:e3c NO_GEN_CHECK:e5 NO_LOCK:e6 NO_FALLBACK:e8 LAYOUT:layout HOLD_NO_BIT:hold
DEAD_BY_LOADER:ld RENDER_LOCK_LUA:rl RENDER_RETRY_CREATE:rl DROP_UNPUBLISHED:rl"
echo "== self-test: every fault variant must fail its target test"
caught=0; missed=0; built=" "
for ft in $FAULTS; do
    f="${ft%%:*}"; t="${ft##*:}"
    dir="$OUT/fault_$f"
    case "$built" in *" $f "*) ;; *) build "$dir" "-DNUI_FAULT_$f"; built="$built$f " ;; esac
    if run "$dir" --only "$t" > "$dir/$t.log" 2>&1; then
        echo "NOT CAUGHT  NUI_FAULT_$f by $t (log $dir/$t.log)"; missed=$((missed + 1))
    else
        why=$(grep -m1 -E "FAIL $t:|CRASH" "$dir/$t.log" || true)
        echo "caught      NUI_FAULT_$f by $t: ${why# *}"; caught=$((caught + 1))
    fi
done
echo "self-test: $caught caught, $missed missed"
[ "$missed" -eq 0 ]
