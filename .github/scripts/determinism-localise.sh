#!/bin/bash
# Turns "the runs diverge" into "they diverge at these guest addresses".
#
# determinism-check.sh reports the frame; this dumps main RAM from two runs at
# that frame and reports the differing byte ranges as GameCube addresses, which
# is what a symbol map or a memory watch can actually be pointed at.
#
#   .github/scripts/determinism-localise.sh dist/RingOut-1.0-deck 240
set -euo pipefail

PKG="$(cd "${1:?usage: determinism-localise.sh <package-dir> <frame>}" && pwd)"
FRAME="${2:?usage: determinism-localise.sh <package-dir> <frame>}"

MODULE="$(ls "$PKG"/bin/g*_recomp.so 2>/dev/null | head -1)"
[ -n "$MODULE" ] || { echo "no recompiled module in $PKG/bin" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

run() {
    local n="$1"
    mkdir -p "$WORK/user$n"
    RINGOUT_DETERMINISM_LOG="$WORK/run$n.log" \
    RINGOUT_DETERMINISM_FRAMES="$((FRAME + 1))" \
    RINGOUT_DETERMINISM_DUMP="$WORK/ram$n.bin" \
    RINGOUT_DETERMINISM_DUMP_FRAME="$FRAME" \
        "$PKG/bin/moderngekko-run" --headless \
            --user-dir "$WORK/user$n" \
            --game "$PKG/game" \
            --module "$MODULE" >"$WORK/run$n.out" 2>&1 || true
}

echo "==> run 1 of 2, dumping RAM at frame $FRAME"
run 1
echo "==> run 2 of 2"
run 2

for n in 1 2; do
    [ -s "$WORK/ram$n.bin" ] || { echo "run $n produced no dump" >&2; tail -5 "$WORK/run$n.out" >&2; exit 1; }
done

echo
echo "==> dump size: $(stat -c%s "$WORK/ram1.bin") bytes each"

if cmp -s "$WORK/ram1.bin" "$WORK/ram2.bin"; then
    echo "IDENTICAL at frame $FRAME -- the divergence is later than this frame."
    exit 0
fi

# cmp -l gives one line per differing byte, offsets 1-based. Collapsed into
# ranges here: a thousand consecutive differing bytes is one object, not a
# thousand findings, and the range is what identifies it.
cmp -l "$WORK/ram1.bin" "$WORK/ram2.bin" | awk '
{
    off = $1 - 1
    if (NR == 1) { start = off; prev = off; n = 1; next }
    # Same range while the gap stays under a cache line; a wider gap is a
    # different object.
    if (off - prev <= 32) { prev = off; n++; next }
    printf "  0x%08X - 0x%08X  (%d bytes differ)\n", 0x80000000 + start, 0x80000000 + prev, n
    ranges++
    start = off; prev = off; n = 1
}
END {
    printf "  0x%08X - 0x%08X  (%d bytes differ)\n", 0x80000000 + start, 0x80000000 + prev, n
    printf "\n%d distinct region(s)\n", ranges + 1
}' | head -40
