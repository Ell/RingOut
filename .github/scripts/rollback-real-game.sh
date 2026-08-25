#!/bin/bash
# Opt-in offline rollback oracle for a privately prepared RingOut package.
#
# This script never discovers, copies, extracts, or uploads a disc image. The
# caller supplies a package which already contains its private extracted game
# and matching recomp module. Results contain hashes and logs only.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME rollback-real-game.sh \
         --package <private-package> [--input <frame-script>] \
         [--predicted-input <frame-script>] \
         [--rollback-at <frame>] [--rollback-len <frames>] \
         [--skip <state-sections>] [--work <empty-output-dir>]

Defaults: input=.github/input-scripts/arcade-match.txt, rollback-at=5200,
rollback-len=30, and a full snapshot (no skipped state sections).
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
PREDICTED_INPUT=""
ROLLBACK_AT=5200
ROLLBACK_LEN=30
SKIP=""
WORK=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --package) [ "$#" -ge 2 ] || usage; PACKAGE="$2"; shift 2 ;;
    --input) [ "$#" -ge 2 ] || usage; INPUT="$2"; shift 2 ;;
    --predicted-input) [ "$#" -ge 2 ] || usage; PREDICTED_INPUT="$2"; shift 2 ;;
    --rollback-at) [ "$#" -ge 2 ] || usage; ROLLBACK_AT="$2"; shift 2 ;;
    --rollback-len) [ "$#" -ge 2 ] || usage; ROLLBACK_LEN="$2"; shift 2 ;;
    --skip) [ "$#" -ge 2 ] || usage; SKIP="$2"; shift 2 ;;
    --work) [ "$#" -ge 2 ] || usage; WORK="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done

[ "${RINGOUT_REAL_GAME_ACK:-}" = "I_OWN_THE_GAME" ] ||
  fail "set RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME to opt in"
[ -n "$PACKAGE" ] || usage
[[ "$ROLLBACK_AT" =~ ^[0-9]+$ ]] || fail "--rollback-at must be an integer"
[[ "$ROLLBACK_LEN" =~ ^[1-9][0-9]*$ ]] || fail "--rollback-len must be positive"
ROLLBACK_AT=$((10#$ROLLBACK_AT))
ROLLBACK_LEN=$((10#$ROLLBACK_LEN))

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
[ "${#modules[@]}" -eq 1 ] ||
  fail "private package must contain exactly one recomp module"
MODULE="${modules[0]}"

if [ -z "$WORK" ]; then
  WORK="$(mktemp -d /tmp/ringout-real-rollback.XXXXXXXX)"
else
  WORK="$(readlink -m "$WORK")"
  if [ -e "$WORK" ]; then
    [ -d "$WORK" ] || fail "--work is not a directory"
    [ -z "$(find "$WORK" -mindepth 1 -maxdepth 1 -print -quit)" ] ||
      fail "--work must be empty"
  else
    mkdir -p "$WORK"
  fi
fi

LOGICAL_END=$((ROLLBACK_AT + ROLLBACK_LEN))
BASELINE_FRAMES=$((LOGICAL_END + 1))
CORRECTION_FRAMES=$((ROLLBACK_AT + 2 * ROLLBACK_LEN + 1))

if [ -n "$PREDICTED_INPUT" ]; then
  PREDICTED_INPUT="$(readlink -f "$PREDICTED_INPUT")"
  [ -f "$PREDICTED_INPUT" ] || fail "predicted input script is unavailable"
else
  # Model repeat-last prediction by withholding every authoritative input
  # transition in the correction window. Everything before the snapshot stays
  # byte-identical to the baseline, while replay receives the complete script.
  PREDICTED_INPUT="$WORK/predicted-input.txt"
  awk -v first="$ROLLBACK_AT" -v last="$LOGICAL_END" '
    /^[[:space:]]*#/ || /^[[:space:]]*$/ { print; next }
    {
      frame = ($1 ~ /^[Pp][1-4]$/ ? $2 : $1)
      if (frame ~ /^[0-9]+$/ && frame > first && frame <= last)
        next
      print
    }
  ' "$INPUT" > "$PREDICTED_INPUT"
fi

run_case() {
  local name="$1"
  local frames="$2"
  local rollback="$3"
  local case_input="$4"
  local user="$WORK/$name-user"
  local hashes="$WORK/$name.hashes"
  local output="$WORK/$name.out"
  local -a rollback_env=()
  local -a correction_env=()
  local -a skip_env=()
  mkdir -p "$user"
  if [ "$rollback" = "yes" ]; then
    rollback_env=(
      "RINGOUT_DETERMINISM_ROLLBACK_AT=$ROLLBACK_AT"
      "RINGOUT_DETERMINISM_ROLLBACK_LEN=$ROLLBACK_LEN"
      "RINGOUT_DETERMINISM_ROLLBACK_CORE=HEADLESS_ISOLATED_ORACLE"
    )
    correction_env=("RINGOUT_DETERMINISM_CORRECTED_INPUT=$INPUT")
    [ -z "$SKIP" ] || skip_env=("RINGOUT_ROLLBACK_SKIP=$SKIP")
  fi

  if ! env "RINGOUT_DETERMINISM_LOG=$hashes" \
      "RINGOUT_DETERMINISM_FRAMES=$frames" \
      "RINGOUT_DETERMINISM_INPUT=$case_input" \
      "${rollback_env[@]}" "${correction_env[@]}" "${skip_env[@]}" \
      "$RUNTIME" --headless --user-dir "$user" --game "$GAME" \
      --module "$MODULE" >"$output" 2>&1; then
    echo "Runtime output retained in: $output" >&2
    fail "$name runtime exited unsuccessfully"
  fi
  [ -s "$hashes" ] || fail "$name produced no hashes"
  local actual
  actual="$(wc -l < "$hashes")"
  [ "$actual" -eq "$frames" ] ||
    fail "$name produced $actual hashes; expected $frames"
  awk -v expected="$frames" '
    $1 != NR - 1 { exit 1 }
    END { if (NR != expected) exit 1 }
  ' "$hashes" || fail "$name hash log has a broken frame sequence"
}

echo "==> baseline: frames 0..$LOGICAL_END"
run_case baseline "$BASELINE_FRAMES" no "$INPUT"
echo "==> correction: predict after $ROLLBACK_AT, then restore and replay corrected input"
run_case correction "$CORRECTION_FRAMES" yes "$PREDICTED_INPUT"

grep -q '\[rollback\].*restored.*(ok)' "$WORK/correction.out" ||
  fail "correction run did not report a successful restore"
grep -q '\[rollback\] corrected replay COMPLETED:' "$WORK/correction.out" ||
  fail "runtime did not report a completed corrected replay"
if grep -q '\[rollback\].*\(FAILED\|DIVERGED\)' "$WORK/correction.out"; then
  fail "runtime reported a rollback failure"
fi

# Before prediction begins, both processes must produce the same game/L1 trace.
# The speculative window must then differ, or this run did not exercise an
# incorrect prediction and is not evidence for correction.
head -n "$BASELINE_FRAMES" "$WORK/correction.hashes" > "$WORK/first-pass.hashes"
head -n "$((ROLLBACK_AT + 1))" "$WORK/baseline.hashes" | cut -d' ' -f1,3,4 \
  > "$WORK/baseline-before-prediction.trace"
head -n "$((ROLLBACK_AT + 1))" "$WORK/first-pass.hashes" | cut -d' ' -f1,3,4 \
  > "$WORK/first-pass-before-prediction.trace"
diff -u "$WORK/baseline-before-prediction.trace" \
  "$WORK/first-pass-before-prediction.trace" > "$WORK/pre-prediction.diff" ||
  fail "baseline and predicted run diverged before prediction began"

sed -n "$((ROLLBACK_AT + 2)),$((LOGICAL_END + 1))p" \
  "$WORK/baseline.hashes" | cut -d' ' -f1,3,4 > "$WORK/baseline-speculative.trace"
sed -n "$((ROLLBACK_AT + 2)),$((LOGICAL_END + 1))p" \
  "$WORK/first-pass.hashes" | cut -d' ' -f1,3,4 > "$WORK/predicted-speculative.trace"
if diff -u "$WORK/baseline-speculative.trace" "$WORK/predicted-speculative.trace" \
    > "$WORK/speculative-mismatch.diff"; then
  fail "withheld input did not change game memory or L1; no misprediction was exercised"
fi

if ! diff -q <(head -n "$((ROLLBACK_AT + 1))" "$WORK/baseline.hashes" | cut -d' ' -f1,2) \
                  <(head -n "$((ROLLBACK_AT + 1))" "$WORK/first-pass.hashes" | \
                    cut -d' ' -f1,2) >/dev/null; then
  echo "NOTE: baseline OS-global hashes differ; game memory and L1 still match."
fi

# The snapshot is captured after hashing ROLLBACK_AT. The replay therefore
# covers logical frames ROLLBACK_AT+1 through LOGICAL_END. The correction log's
# final ROLLBACK_LEN physical rows are those same logical frames after restore.
sed -n "$((ROLLBACK_AT + 2)),$((LOGICAL_END + 1))p" \
  "$WORK/baseline.hashes" | cut -d' ' -f2-4 > "$WORK/baseline-replay-window.trace"
tail -n "$ROLLBACK_LEN" "$WORK/correction.hashes" | cut -d' ' -f2-4 \
  > "$WORK/corrected-replay-window.trace"
diff -u "$WORK/baseline-replay-window.trace" \
  "$WORK/corrected-replay-window.trace" > "$WORK/replay.diff" ||
  fail "corrected replay differs from the baseline hash trace"

{
  echo "rollback_at=$ROLLBACK_AT"
  echo "rollback_len=$ROLLBACK_LEN"
  echo "logical_end=$LOGICAL_END"
  echo "snapshot_skip=${SKIP:-none}"
  echo "input_sha256=$(sha256sum "$INPUT" | cut -d' ' -f1)"
  echo "predicted_input_sha256=$(sha256sum "$PREDICTED_INPUT" | cut -d' ' -f1)"
  echo "runtime_sha256=$(sha256sum "$RUNTIME" | cut -d' ' -f1)"
  echo "module_sha256=$(sha256sum "$MODULE" | cut -d' ' -f1)"
  echo "dol_sha256=$(sha256sum "$GAME/sys/main.dol" | cut -d' ' -f1)"
} > "$WORK/result.env"

echo "PASS: prediction diverged and corrected replay converged to the baseline."
echo "Evidence retained in: $WORK"
