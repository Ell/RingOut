#!/bin/bash
# Scout what the game is showing at which EMULATED FRAME, so a frame-keyed input
# script for .github/input-scripts/ can be written against something other than
# guesswork.
#
#   scout-frames.sh <package-dir> <outdir> [input-script] [max-frames] [shot-every-s]
#
# The harness this feeds is keyed on the emulated frame, while a screenshot
# happens in wall-clock time -- and the two run at very different rates (~120
# emulated fps on a desktop). The bridge is the hash log: it gets exactly one
# line per emulated frame, so `wc -l` on it IS the current frame number. Each
# shot is named with the frame it belongs to rather than the second it was
# taken, which is what makes "MODE SELECT is up by frame 2251" a statement you
# can put in an input script.
#
# Windowed on purpose: --headless has nothing to photograph, and blind press
# schedules are what cost earlier attempts their time. Screenshot before
# theorising about where the game is stuck.
#
# Needs xdotool and ImageMagick's `import`, and an X or XWayland display.
set -u
export DISPLAY="${DISPLAY:-:0}"

PKG="$(cd "${1:?usage: scout-frames.sh <package-dir> <outdir> [input] [frames] [every]}" && pwd)"
OUT="${2:?outdir}"
INPUT="${3:-}"
FRAMES="${4:-30000}"
EVERY="${5:-4}"
[ -z "$INPUT" ] || INPUT="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"

MODULE="$(ls "$PKG"/bin/g*_recomp.so 2>/dev/null | head -1)"
[ -n "$MODULE" ] || { echo "no recompiled module in $PKG/bin" >&2; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT/user/Config"
# Without save data the game parks forever on "No previous SOULCALIBUR II data
# found ... Press START to continue without saving", and the run photographs a
# dialog instead of the game.
cp -r "$PKG/userdata/GC" "$OUT/user/" 2>/dev/null || true
cp "$PKG/userdata/config.ini" "$OUT/user/" 2>/dev/null || true
printf '[Input]\nBackgroundInput = True\n' > "$OUT/user/Config/Dolphin.ini"

env "RINGOUT_DETERMINISM_LOG=$OUT/hashes.log" \
    "RINGOUT_DETERMINISM_FRAMES=$FRAMES" \
    ${INPUT:+"RINGOUT_DETERMINISM_INPUT=$INPUT"} \
    "$PKG/bin/moderngekko-run" --user-dir "$OUT/user" --game "$PKG/game" \
        --module "$MODULE" > "$OUT/log.txt" 2>&1 &
PID=$!
echo "pid=$PID module=$(basename "$MODULE") input=${INPUT:-none}"

cleanup() {
  kill "$PID" 2>/dev/null
  w=0; while kill -0 "$PID" 2>/dev/null && [ $w -lt 20 ]; do sleep 0.5; w=$((w+1)); done
  kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
  echo "final frame: $(wc -l < "$OUT/hashes.log" 2>/dev/null || echo 0)"
  exit 0
}
trap cleanup INT TERM

while kill -0 "$PID" 2>/dev/null; do
  sleep "$EVERY"
  # Re-resolve the window every time rather than caching the id. A cached one
  # goes stale, `import` then fails on every iteration, and the run finishes
  # with no pictures at all -- indistinguishable from the game never having
  # opened a window. That cost a full scouting pass before it was noticed.
  WID="$(xdotool search --name "Ring Out" 2>/dev/null | tail -1)"
  [ -z "$WID" ] && { echo "no window yet" >&2; continue; }
  frame="$(wc -l < "$OUT/hashes.log" 2>/dev/null || echo 0)"
  if import -window "$WID" "$OUT/f$(printf '%06d' "$frame").png" 2>>"$OUT/import-err.txt"; then
    echo "frame $frame"
  else
    echo "frame $frame: import failed on window $WID" >&2
  fi
done
cleanup
