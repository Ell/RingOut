#!/bin/bash
# Asset-free regression test for rollback-live-real-game.sh's evidence gate.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/.github/scripts/rollback-live-real-game.sh"
WORK="$(mktemp -d /tmp/ringout-live-harness-test.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

"$HARNESS" --validate-fault-script \
  "$REPO/.github/input-scripts/rollback-fault-correction.txt" >/dev/null
"$HARNESS" --validate-fault-script \
  "$REPO/.github/input-scripts/rollback-fault-horizon.txt" >/dev/null
printf '%s\n' '  # indented comment' 'delay 7 9 # inline comment' \
  > "$WORK/commented-fault.txt"
"$HARNESS" --validate-fault-script "$WORK/commented-fault.txt" >/dev/null
printf '%s\n' 'delay 4 3' > "$WORK/backward-fault.txt"
if "$HARNESS" --validate-fault-script "$WORK/backward-fault.txt" >/dev/null 2>&1; then
  echo "backward delay unexpectedly passed" >&2
  exit 1
fi
printf '%s\n' 'drop 4' 'delay 4 8' > "$WORK/duplicate-fault.txt"
if "$HARNESS" --validate-fault-script "$WORK/duplicate-fault.txt" >/dev/null 2>&1; then
  echo "duplicate send ordinal unexpectedly passed" >&2
  exit 1
fi
printf '%s\n' 'sleep 4 8' > "$WORK/unknown-fault.txt"
if "$HARNESS" --validate-fault-script "$WORK/unknown-fault.txt" >/dev/null 2>&1; then
  echo "unknown instruction unexpectedly passed" >&2
  exit 1
fi

make_case() {
  local name="$1"
  mkdir -p "$WORK/$name/host" "$WORK/$name/guest"
  printf '%s\n' '[rollback live] negotiated mode=rollback version=1 horizon=8' \
    > "$WORK/$name/host/log.txt"
  printf '%s\n' '[rollback live] negotiated mode=rollback version=1 horizon=8' \
    > "$WORK/$name/guest/log.txt"
  printf '%s\n' \
    '60 11111111 aaaaaaaa 000000000000003c' \
    '120 22222222 bbbbbbbb 0000000000000078' \
    '180 33333333 cccccccc 00000000000000b4' \
    > "$WORK/$name/host/confirmed-state.log"
  cp "$WORK/$name/host/confirmed-state.log" \
    "$WORK/$name/guest/confirmed-state.log"
}

make_case pass
printf '%s\n' '[rollback live] correction committed' \
  >> "$WORK/pass/guest/log.txt"
"$HARNESS" --verify-existing "$WORK/pass" >/dev/null

make_case production
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  >> "$WORK/production/host/log.txt"
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  >> "$WORK/production/guest/log.txt"
"$HARNESS" --verify-existing "$WORK/production" --production >/dev/null

make_case production-correction
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  >> "$WORK/production-correction/host/log.txt"
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  '[rollback live] correction committed' \
  >> "$WORK/production-correction/guest/log.txt"
"$HARNESS" --verify-existing "$WORK/production-correction" --production \
  --fault-script "$REPO/.github/input-scripts/rollback-fault-correction.txt" >/dev/null

make_case production-missing-correction
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  >> "$WORK/production-missing-correction/host/log.txt"
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  >> "$WORK/production-missing-correction/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/production-missing-correction" --production \
    --fault-script "$REPO/.github/input-scripts/rollback-fault-correction.txt" \
    >/dev/null 2>&1; then
  echo "uncorrected production fault unexpectedly passed" >&2
  exit 1
fi

make_case production-inactive
if "$HARNESS" --verify-existing "$WORK/production-inactive" --production \
    >/dev/null 2>&1; then
  echo "inactive production rollback unexpectedly passed" >&2
  exit 1
fi

make_case received-only
printf '%s\n' '[rollback live] correction received before frame boundary' \
  >> "$WORK/received-only/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/received-only" >/dev/null 2>&1; then
  echo "uncommitted correction unexpectedly passed" >&2
  exit 1
fi

make_case missing-correction
if "$HARNESS" --verify-existing "$WORK/missing-correction" >/dev/null 2>&1; then
  echo "missing correction unexpectedly passed" >&2
  exit 1
fi

make_case horizon
printf '%s\n' '[rollback live] horizon fallback: waiting for authoritative input' \
  '[rollback live] horizon resumed with authoritative input' \
  >> "$WORK/horizon/host/log.txt"
"$HARNESS" --verify-existing "$WORK/horizon" --expect-horizon >/dev/null

make_case stalled-only
printf '%s\n' '[rollback live] horizon fallback: waiting for authoritative input' \
  >> "$WORK/stalled-only/host/log.txt"
if "$HARNESS" --verify-existing "$WORK/stalled-only" --expect-horizon \
    >/dev/null 2>&1; then
  echo "unrecovered horizon stall unexpectedly passed" >&2
  exit 1
fi

make_case missing-horizon
printf '%s\n' '[rollback live] correction generation=4 status=committed' \
  >> "$WORK/missing-horizon/host/log.txt"
if "$HARNESS" --verify-existing "$WORK/missing-horizon" --expect-horizon \
    >/dev/null 2>&1; then
  echo "missing horizon fallback unexpectedly passed" >&2
  exit 1
fi

make_case fatal
printf '%s\n' '[rollback live] correction committed' \
  '[rollback live] correction status=FAILED reason=restore' \
  >> "$WORK/fatal/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/fatal" >/dev/null 2>&1; then
  echo "fatal rollback marker unexpectedly passed" >&2
  exit 1
fi

make_case desync
printf '%s\n' '[rollback live] correction committed' 'DESYNC at frame 600' \
  >> "$WORK/desync/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/desync" >/dev/null 2>&1; then
  echo "desync unexpectedly passed" >&2
  exit 1
fi

make_case gpu-fifo-failure
printf '%s\n' '[rollback live] correction committed' \
  '[alert] Error: GFX FIFO: Unknown Opcode (0x00)' \
  >> "$WORK/gpu-fifo-failure/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/gpu-fifo-failure" >/dev/null 2>&1; then
  echo "GPU FIFO failure unexpectedly passed" >&2
  exit 1
fi

make_case expected-digest-mismatch
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  'netplay: DESYNC at frame 60' \
  >> "$WORK/expected-digest-mismatch/host/log.txt"
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  '[rollback live] isolated digest fault injected at frame 60' \
  'netplay: DESYNC at frame 60' \
  >> "$WORK/expected-digest-mismatch/guest/log.txt"
"$HARNESS" --verify-existing "$WORK/expected-digest-mismatch" \
  --expect-digest-mismatch 60 >/dev/null

make_case one-sided-digest-mismatch
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  >> "$WORK/one-sided-digest-mismatch/host/log.txt"
printf '%s\n' '[rollback live] active generation=4 snapshot_frames=10' \
  '[rollback live] isolated digest fault injected at frame 60' \
  'netplay: DESYNC at frame 60' \
  >> "$WORK/one-sided-digest-mismatch/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/one-sided-digest-mismatch" \
    --expect-digest-mismatch 60 >/dev/null 2>&1; then
  echo "one-sided digest mismatch unexpectedly passed" >&2
  exit 1
fi

make_case faulted
printf '%s\n' '[rollback live] correction committed' \
  '[rollback live] session faulted' >> "$WORK/faulted/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/faulted" >/dev/null 2>&1; then
  echo "faulted rollback unexpectedly passed" >&2
  exit 1
fi

make_case state-diverged
printf '%s\n' '[rollback live] correction committed' \
  >> "$WORK/state-diverged/guest/log.txt"
sed -i 's/22222222/22222223/' \
  "$WORK/state-diverged/guest/confirmed-state.log"
if "$HARNESS" --verify-existing "$WORK/state-diverged" >/dev/null 2>&1; then
  echo "divergent confirmed logical state unexpectedly passed" >&2
  exit 1
fi

make_case too-few-confirmed
printf '%s\n' '[rollback live] correction committed' \
  >> "$WORK/too-few-confirmed/guest/log.txt"
head -n 2 "$WORK/too-few-confirmed/host/confirmed-state.log" \
  > "$WORK/too-few-confirmed/host/short.log"
mv "$WORK/too-few-confirmed/host/short.log" \
  "$WORK/too-few-confirmed/host/confirmed-state.log"
if "$HARNESS" --verify-existing "$WORK/too-few-confirmed" >/dev/null 2>&1; then
  echo "too few confirmed logical states unexpectedly passed" >&2
  exit 1
fi

echo "rollback live real-game evidence gate: PASS"
