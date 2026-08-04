#!/bin/bash
# Answers the question netplay depends on: is the statically recompiled core
# deterministic? Runs the same game twice from the same starting state and
# compares a per-frame hash of guest RAM.
#
# Delay-based netplay only works if two machines running the same inputs stay
# byte-identical; rollback needs that AND fast savestates. If the two runs here
# diverge, no amount of transport work will make netplay sync, and the frame
# this reports is where to start looking.
#
# Headless on purpose: no window, no audio device, no host timing feeding back
# into the emulation. Each run gets its own fresh user directory so both start
# from an identical state rather than from whatever the last session left.
#
#   .github/scripts/determinism-check.sh dist/RingOut-1.0-deck [frames]
set -euo pipefail

PKG="$(cd "${1:?usage: determinism-check.sh <package-dir> [frames]}" && pwd)"
FRAMES="${2:-600}"

MODULE="$(ls "$PKG"/bin/g*_recomp.so 2>/dev/null | head -1)"
[ -n "$MODULE" ] || { echo "no recompiled module in $PKG/bin" >&2; exit 1; }
[ -d "$PKG/game" ] || { echo "no game data in $PKG/game" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

run() {
    local n="$1"
    mkdir -p "$WORK/user$n"
    # The RTC is pinned by the runtime itself whenever the harness is active,
    # not from here. Writing it into Dolphin.ini looked like it worked and did
    # nothing: the key is EnableCustomRTC, and an unrecognised one is ignored in
    # silence, so the first measurements were taken with an unpinned clock.
    RINGOUT_DETERMINISM_LOG="$WORK/run$n.log" \
    RINGOUT_DETERMINISM_FRAMES="$FRAMES" \
        "$PKG/bin/moderngekko-run" --headless \
            --user-dir "$WORK/user$n" \
            --game "$PKG/game" \
            --module "$MODULE" >"$WORK/run$n.out" 2>&1 || true
}

echo "==> run 1 of 2 ($FRAMES frames)"
run 1
echo "==> run 2 of 2"
run 2

for n in 1 2; do
    if [ ! -s "$WORK/run$n.log" ]; then
        echo "run $n produced no hashes; the runtime output was:" >&2
        tail -20 "$WORK/run$n.out" >&2
        exit 1
    fi
done

echo
echo "==> frames hashed: $(wc -l < "$WORK/run1.log") and $(wc -l < "$WORK/run2.log")"

if diff -q "$WORK/run1.log" "$WORK/run2.log" >/dev/null; then
    echo "DETERMINISTIC over $FRAMES frames -- every frame hash matched."
    echo "Delay-based netplay is viable on this evidence."
    exit 0
fi

echo "DIVERGED. First differing frame:"
diff "$WORK/run1.log" "$WORK/run2.log" | head -6
echo
echo "Columns are: frame, OS-globals hash, game-memory hash, L1 hash."
echo
# Which column moved decides what kind of problem this is, so say so rather
# than leaving it to be worked out by eye.
if diff <(cut -d' ' -f1,3,4 "$WORK/run1.log") <(cut -d' ' -f1,3,4 "$WORK/run2.log") >/dev/null; then
    echo "Only the OS-globals column differs -- game memory is identical."
    echo "That is boot-time console state, not the core. Netplay pins it across"
    echo "peers, so this is compatible with delay-based netplay."
    exit 0
fi
echo "Game memory itself differs, so this is not just boot state. Something"
echo "host-dependent is reaching the guest: uninitialised memory, a pointer or"
echo "address, thread interleaving, or timing feeding back into emulation."
exit 1
