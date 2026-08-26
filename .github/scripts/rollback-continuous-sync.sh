#!/bin/bash
# GGPO-style real-game oracle: every logical frame in the requested window is
# executed, restored, executed again with identical input, and hash-compared.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME rollback-continuous-sync.sh \
         --package <private-package> [--input <frame-script>] \
         [--start <physical-frame>] [--pairs <count>] \
         [--work <empty-output-dir>]

Defaults: input=.github/input-scripts/arcade-match.txt, start=600, pairs=600.
This is an isolated, headless oracle. It uses a disposable user directory and
the production full-state transaction store; it does not certify presentation,
audio, or selective SC2 state.
EOF
  exit 2
}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PACKAGE="${RINGOUT_ROLLBACK_PACKAGE:-}"
INPUT="$REPO/.github/input-scripts/arcade-match.txt"
START=600
PAIRS=600
WORK=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --package) [ "$#" -ge 2 ] || usage; PACKAGE="$2"; shift 2 ;;
    --input) [ "$#" -ge 2 ] || usage; INPUT="$2"; shift 2 ;;
    --start) [ "$#" -ge 2 ] || usage; START="$2"; shift 2 ;;
    --pairs) [ "$#" -ge 2 ] || usage; PAIRS="$2"; shift 2 ;;
    --work) [ "$#" -ge 2 ] || usage; WORK="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done

[ "${RINGOUT_REAL_GAME_ACK:-}" = I_OWN_THE_GAME ] ||
  fail "set RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME to opt in"
[ -n "$PACKAGE" ] || usage
[[ "$START" =~ ^[0-9]+$ ]] || fail "--start must be a non-negative integer"
[[ "$PAIRS" =~ ^[1-9][0-9]*$ ]] || fail "--pairs must be positive"
START=$((10#$START))
PAIRS=$((10#$PAIRS))
[ "$PAIRS" -le 1000000 ] || fail "--pairs exceeds the one-million-frame safety bound"
FRAMES=$((START + 2 * PAIRS + 1))
[ "$FRAMES" -gt "$START" ] || fail "requested frame range overflows"

PACKAGE="$(readlink -f "$PACKAGE")"
INPUT="$(readlink -f "$INPUT")"
[ -d "$PACKAGE" ] || fail "private package directory is unavailable"
[ -f "$INPUT" ] || fail "input script is unavailable"
RUNTIME="$PACKAGE/bin/moderngekko-run"
GAME="$PACKAGE/game"
[ -x "$RUNTIME" ] || fail "private package has no executable runtime"
[ -f "$GAME/sys/main.dol" ] || fail "private package has no extracted game"
shopt -s nullglob
modules=("$PACKAGE"/bin/g*_recomp.so)
shopt -u nullglob
[ "${#modules[@]}" -eq 1 ] || fail "private package must contain exactly one recomp module"
MODULE="${modules[0]}"

if [ -z "$WORK" ]; then
  WORK="$(mktemp -d /tmp/ringout-continuous-sync.XXXXXXXX)"
else
  WORK="$(readlink -m "$WORK")"
  case "$WORK" in
    /tmp/ringout-continuous-sync.*) ;;
    *) fail "--work must be under /tmp/ringout-continuous-sync.*" ;;
  esac
  if [ -e "$WORK" ]; then
    [ -d "$WORK" ] || fail "--work is not a directory"
    [ -z "$(find "$WORK" -mindepth 1 -maxdepth 1 -print -quit)" ] ||
      fail "--work must be empty"
  else
    mkdir -p "$WORK"
  fi
fi

USER_DIR="$WORK/user"
HASHES="$WORK/frames.log"
OUTPUT="$WORK/runtime.log"
mkdir -p "$USER_DIR"
cp -r "$PACKAGE/userdata/GC" "$USER_DIR/" 2>/dev/null || true

if ! env RINGOUT_DETERMINISM_LOG="$HASHES" \
    RINGOUT_DETERMINISM_FRAMES="$FRAMES" \
    RINGOUT_DETERMINISM_INPUT="$INPUT" \
    RINGOUT_DETERMINISM_CONTINUOUS_SYNC=HEADLESS_ISOLATED_ORACLE \
    RINGOUT_DETERMINISM_SYNC_START="$START" \
    RINGOUT_DETERMINISM_SYNC_PAIRS="$PAIRS" \
    "$RUNTIME" --headless --user-dir "$USER_DIR" --game "$GAME" \
    --module "$MODULE" >"$OUTPUT" 2>&1; then
  fail "runtime exited unsuccessfully; evidence retained in $WORK"
fi

grep -Fq "[rollback sync] active start_physical_frame=$START pairs=$PAIRS" "$OUTPUT" ||
  fail "continuous sync oracle did not activate"
grep -Fq "[rollback sync] COMPLETED pairs=$PAIRS" "$OUTPUT" ||
  fail "continuous sync oracle did not complete"
if grep -Eq '\[rollback sync\].*(FAILED|DIVERGED|refused)' "$OUTPUT"; then
  fail "continuous sync oracle reported a failure"
fi
[ -s "$HASHES" ] || fail "runtime produced no frame hashes"
[ "$(wc -l < "$HASHES")" -eq "$FRAMES" ] || fail "runtime produced an incomplete frame trace"

{
  echo "sync_start=$START"
  echo "sync_pairs=$PAIRS"
  echo "physical_frames=$FRAMES"
  echo "input_sha256=$(sha256sum "$INPUT" | cut -d' ' -f1)"
  echo "runtime_sha256=$(sha256sum "$RUNTIME" | cut -d' ' -f1)"
  echo "module_sha256=$(sha256sum "$MODULE" | cut -d' ' -f1)"
  echo "dol_sha256=$(sha256sum "$GAME/sys/main.dol" | cut -d' ' -f1)"
} > "$WORK/result.env"

echo "PASS: $PAIRS consecutive logical frames reproduced after one-frame rollback."
echo "Evidence retained in: $WORK"
