#!/bin/bash
# Is the arcade-final.txt route frame-stable across runs?
#
# determinism-check.sh cannot answer this as-is: it seeds a bare user dir, so
# there is no save data (the game parks on the memory-card dialog) and no
# GameSettings, which means P1 Infinite Health is NOT active -- and without it a
# lost match silently changes the route, so the two runs would diverge for a
# reason that says nothing about the core.
#
# A profile trained on a drifting route is worse than no change at all, which is
# why this runs before any PGO work on the new route.
set -u

REPO=/mnt/hera/projects/soulcalibur
PKG="$REPO/dist/RingOut-1.0-deck"
MODULE="${MODULE:-$REPO/dist/RingOut-1.0-dist/bin/gGRSEAF_recomp.so}"
INPUT="$REPO/.github/input-scripts/arcade-final.txt"
FRAMES="${1:-72000}"
OUT="$REPO/work/route-determinism"

# The runtime only WARNS on a missing input script and then runs with no input
# at all -- which still produces a full hash log and would "pass" a determinism
# check while testing boot and an idle menu. Caught this once already, after a
# branch switch removed the file. Fail loudly instead.
[ -f "$INPUT" ] || { echo "FATAL: no input script at $INPUT" >&2; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT"

seed() {
  local d="$1"
  mkdir -p "$d/Config" "$d/GameSettings"
  cp -r "$PKG/userdata/GC" "$d/" 2>/dev/null || true
  python3 - "$PKG/userdata/GameSettings/GRSEAF.ini" "$d/GameSettings/GRSEAF.ini" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
want = ["$P1 Infinite Health", "$P1 Win 1 Round To Win"]
out = []
for line in open(src):
    out.append(line)
    if line.strip() == "[ActionReplay_Enabled]":
        out.extend(w + "\n" for w in want)
open(dst, "w").writelines(out)
PY
}

run() {
  local n="$1"
  seed "$OUT/user$n"
  local t0=$(date +%s)
  env "RINGOUT_DETERMINISM_LOG=$OUT/run$n.log" \
      "RINGOUT_DETERMINISM_FRAMES=$FRAMES" \
      "RINGOUT_DETERMINISM_INPUT=$INPUT" \
      "$PKG/bin/moderngekko-run" --headless \
        --user-dir "$OUT/user$n" --game "$PKG/game" --module "$MODULE" \
        >"$OUT/run$n.out" 2>&1 || true
  echo "run $n: $(wc -l < "$OUT/run$n.log") frames in $(( $(date +%s) - t0 ))s"
}

echo "route determinism: $FRAMES frames, module $(basename "$MODULE")"
run 1
run 2

a=$(wc -l < "$OUT/run1.log"); b=$(wc -l < "$OUT/run2.log")
echo "frames: run1=$a run2=$b (asked $FRAMES)"
if [ "$a" != "$FRAMES" ] || [ "$b" != "$FRAMES" ]; then
  echo "SHORT RUN -- a run that stopped early cannot be compared"
  tail -3 "$OUT/run1.out" "$OUT/run2.out"
fi
if cmp -s "$OUT/run1.log" "$OUT/run2.log"; then
  echo "IDENTICAL over $a frames -- route is frame-stable"
else
  echo "DIVERGED. First differing frame:"
  diff <(cat -n "$OUT/run1.log") <(cat -n "$OUT/run2.log") | head -4
fi
