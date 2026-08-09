#!/bin/bash
# Fixed-work A/B of recompiled modules on a real gameplay workload.
#
#   module-bench.sh <package-dir> <frames> <input-script> <reps> <tag>=<so> ...
#
# Every previous perf comparison in this project measured one of two wrong
# things. The determinism harness with no input runs boot and an idle menu,
# which carries ~0.1% of a session's paired-single traffic. Watching fps during
# attract mode does reach gameplay, but the attract sequence is NOT reproducible
# across launches -- measured 2026-08-09: two runs of the same package put the
# intro movie 33 s apart and played the attract items in a different order, so
# a fixed measurement window lands on different content each time.
#
# This runs the frame-keyed input harness instead. Every arm executes the same
# emulated frames in the same order, so wall time IS the measurement and no
# window has to be aligned with anything. .github/input-scripts/arcade-match.txt
# drives an actual Arcade match; frames before ~4500 are boot and menus, so use
# a frame count large enough that gameplay dominates.
#
# Runs are alternated and the order reverses on even reps: a result that tracks
# the run order rather than the module is then visible instead of averaged away.
# Each run gets a fresh user directory seeded from the package, so no run
# inherits state the previous one wrote.
set -u

PKG="$(cd "${1:?usage: module-bench.sh <package-dir> <frames> <input> <reps> <tag>=<so>...}" && pwd)"
FRAMES="${2:?frames}"
INPUT="$(cd "$(dirname "${3:?input script}")" && pwd)/$(basename "$3")"
REPS="${4:?reps}"
shift 4
[ $# -ge 1 ] || { echo "need at least one <tag>=<so> arm" >&2; exit 1; }
ARMS=("$@")
[ -f "$INPUT" ] || { echo "no such input script: $INPUT" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

run_one() {
  local tag="$1" so="$2" rep="$3"
  local d="$WORK/$tag-$rep"
  mkdir -p "$d/user/Config"
  # Without save data the game parks forever on "No previous SOULCALIBUR II
  # data found ... Press START to continue without saving" and the run
  # benchmarks a dialog.
  cp -r "$PKG/userdata/GC" "$d/user/" 2>/dev/null || true

  local PERF=""
  if command -v perf >/dev/null 2>&1; then
    PERF="perf stat -x, -e cycles:u,instructions:u -o $d/perf.csv --"
  fi
  local t0 t1
  t0="$(date +%s.%N)"
  # CYCLES, not wall time, is the metric. Measured 2026-08-09 on this desktop:
  # the same module over the same 24000 frames took 229 s, 298 s and 352 s in
  # three runs -- a 54% spread that swamps anything a codegen or layout change
  # could do, because the machine is also being used. Retired cycles for a
  # FIXED emulated workload are immune to that and to frequency scaling, and
  # instructions:u doubles as a checksum that both arms really did do the same
  # work. :u because perf_event_paranoid is usually 2 and kernel counts are
  # then not permitted.
  $PERF env "RINGOUT_DETERMINISM_LOG=$d/hashes.log" \
      "RINGOUT_DETERMINISM_FRAMES=$FRAMES" \
      "RINGOUT_DETERMINISM_INPUT=$INPUT" \
      "$PKG/bin/moderngekko-run" --headless --user-dir "$d/user" \
        --game "$PKG/game" --module "$so" >"$d/out.txt" 2>"$d/err.txt" || true
  t1="$(date +%s.%N)"

  local got; got="$(wc -l < "$d/hashes.log" 2>/dev/null || echo 0)"
  # A run that stopped short is not a faster run. Without this check a module
  # that crashes at frame 300 wins every comparison.
  if [ "$got" != "$FRAMES" ]; then
    echo "$tag rep$rep: FRAMES MISMATCH -- hashed $got of $FRAMES; discarding"
    tail -3 "$d/out.txt" "$d/err.txt" 2>/dev/null
    return 1
  fi
  local sum; sum="$(md5sum "$d/hashes.log" | cut -c1-12)"
  local cyc="-" ins="-" ipc="-"
  if [ -s "$d/perf.csv" ]; then
    cyc="$(awk -F, '$3=="cycles:u"{print $1}' "$d/perf.csv")"
    ins="$(awk -F, '$3=="instructions:u"{print $1}' "$d/perf.csv")"
    [ -n "$cyc" ] && [ -n "$ins" ] && \
      ipc="$(awk -v c="$cyc" -v i="$ins" 'BEGIN{if(c>0)printf "%.3f", i/c; else print "-"}')"
    cyc="$(awk -v c="$cyc" 'BEGIN{printf "%.2f", c/1e9}')"
    ins="$(awk -v i="$ins" 'BEGIN{printf "%.2f", i/1e9}')"
  fi
  printf '%-9s rep%d  %10s Gcyc  %10s Ginsn  IPC %6s  %8.1fs wall  hashes=%s\n' \
    "$tag" "$rep" "$cyc" "$ins" "$ipc" \
    "$(awk -v a="$t1" -v b="$t0" 'BEGIN{print a-b}')" "$sum"
}

echo "package=$PKG frames=$FRAMES input=$(basename "$INPUT") reps=$REPS"
echo "arms: ${ARMS[*]}"
echo
for rep in $(seq 1 "$REPS"); do
  order=("${ARMS[@]}")
  if [ $((rep % 2)) -eq 0 ]; then
    order=(); for ((i=${#ARMS[@]}-1; i>=0; i--)); do order+=("${ARMS[$i]}"); done
  fi
  for arm in "${order[@]}"; do
    run_one "${arm%%=*}" "${arm#*=}" "$rep"
  done
done
echo
echo "identical hash columns mean the arms are semantically the same module;"
echo "a layout-only change (BOLT) MUST match, a codegen change need not."
