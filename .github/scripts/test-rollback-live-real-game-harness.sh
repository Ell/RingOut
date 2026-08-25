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
}

make_case pass
printf '%s\n' '[rollback live] correction committed' \
  >> "$WORK/pass/guest/log.txt"
"$HARNESS" --verify-existing "$WORK/pass" >/dev/null

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

make_case faulted
printf '%s\n' '[rollback live] correction committed' \
  '[rollback live] session faulted' >> "$WORK/faulted/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/faulted" >/dev/null 2>&1; then
  echo "faulted rollback unexpectedly passed" >&2
  exit 1
fi

echo "rollback live real-game evidence gate: PASS"
