#!/bin/bash
# Turns "they diverge at this address" into "this is what wrote it".
#
# The third step of the harness. determinism-check.sh reports the frame,
# determinism-localise.sh reports the address, and this reports the writer:
# a single run with the module's write journal armed on one guest address,
# plus a poll of that address at every burst boundary and every dispatch.
#
# The distinction the output draws is the useful part:
#
#   "store to ... from block 0xXXXX"   recompiled guest code stored it, and
#                                      that block is the writer
#   "changed ... seen at dispatch"     no guest store did it -- the block named
#                                      asked hardware to, and a DMA landed it
#   "changed ... seen at coretiming"   a scheduled hardware event did it,
#                                      between bursts
#   "changed ... seen at frame"        something outside the run loop entirely
#
# A store and a change reported together is ordinary guest code. A change with
# no store behind it is the interesting case: it means the value did not come
# from the recompiled CPU at all, which is how the memory-card header
# timestamp behind the frame-240 divergence was found.
#
#   .github/scripts/determinism-watch.sh dist/RingOut-1.0-deck 0x809E0010 [frames]
set -euo pipefail

PKG="$(cd "${1:?usage: determinism-watch.sh <package-dir> <guest-address> [frames]}" && pwd)"
ADDR="${2:?usage: determinism-watch.sh <package-dir> <guest-address> [frames]}"
FRAMES="${3:-300}"

MODULE="$(ls "$PKG"/bin/g*_recomp.so 2>/dev/null | head -1)"
[ -n "$MODULE" ] || { echo "no recompiled module in $PKG/bin" >&2; exit 1; }
[ -d "$PKG/game" ] || { echo "no game data in $PKG/game" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/user"

echo "==> watching $ADDR over $FRAMES frames"
RINGOUT_DETERMINISM_LOG="$WORK/run.log" \
RINGOUT_DETERMINISM_FRAMES="$FRAMES" \
RINGOUT_DETERMINISM_WATCH="$ADDR" \
RINGOUT_DETERMINISM_WATCH_MAX=60 \
    "$PKG/bin/moderngekko-run" --headless \
        --user-dir "$WORK/user" \
        --game "$PKG/game" \
        --module "$MODULE" >"$WORK/run.out" 2>&1 || true

if [ ! -s "$WORK/run.log" ]; then
    echo "run produced no hashes; the runtime output was:" >&2
    tail -20 "$WORK/run.out" >&2
    exit 1
fi

echo
# -a because the runtime's output is not guaranteed to be text throughout.
if ! grep -a "^\[watch\]" "$WORK/run.out"; then
    echo "no watch output at all -- the watch never armed. The runtime says:" >&2
    tail -20 "$WORK/run.out" >&2
    exit 1
fi

echo
# "DISABLED" means the watch never armed, so the absence of stores below says
# nothing at all -- distinguish that from a genuine result.
if grep -aq "watch DISABLED" "$WORK/run.out"; then
    echo "==> the watch never armed (see above); this run measured nothing." >&2
    exit 1
fi

if grep -aq "^\[watch\] frame=.* store to " "$WORK/run.out"; then
    echo "==> recompiled guest code stores to $ADDR; the blocks above are the writers."
else
    echo "==> nothing in the module ever stored to $ADDR. Whatever set it is"
    echo "    outside the recompiled CPU -- DMA, or the chassis writing guest RAM"
    echo "    natively. The site named above says which."
fi
