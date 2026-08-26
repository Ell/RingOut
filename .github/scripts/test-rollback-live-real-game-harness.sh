#!/bin/bash
# Asset-free regression test for rollback-live-real-game.sh's evidence gate.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/.github/scripts/rollback-live-real-game.sh"
MEMORY_DIFF="$REPO/.github/scripts/sc2-memory-profile-diff.sh"
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

make_case hook-profile
for peer in host guest; do
  printf '%s\n' \
    '[sc2-hook-profile] enabled expected_dol_sha256=0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5 warmup_frames=120 sample_frames=600 diagnostic_limit=0' \
    '[sc2-hook-profile] result observed_frames=720 profiled_frames=600 strict_candidates=1 diagnostic_candidates=12 complete=yes' \
    '[sc2-hook-profile] candidate pc=0x80012340 frames=600 once=600 hits=600 min=1 max=1 parity=300/300 first_ordinal=12..14 last_ordinal=12..14 caller_lr=0x80045678 caller_lr_stable=yes predecessor_pc=0x80023450 predecessor_stable=yes' \
    >> "$WORK/hook-profile/$peer/log.txt"
done
printf '%s\n' '[rollback live] correction committed' \
  >> "$WORK/hook-profile/guest/log.txt"
"$HARNESS" --verify-existing "$WORK/hook-profile" --hook-profile >/dev/null

cp -a "$WORK/hook-profile" "$WORK/engine-replay"
for peer in host guest; do
  printf '%s\n' \
    '[sc2-engine-replay] enabled mode=full-emulator-one-tick begin_pc=0x8001ba3c return_pc=0x8002d628' \
    '[sc2-engine-external] result reads=4 writes=0 read_sites=2 write_sites=0 fallback_instructions=0 overflow=no complete=yes' \
    '[sc2-engine-replay] captured normalized reference; restored entry for verification replay' \
    '[sc2-engine-replay] full-state-result state_match=yes cpu_match=yes tb_remainder_match=yes input_replay_match=yes input_polls=4 external_profile_complete=yes endpoint_bytes=49360152 replay_bytes=49360152 differing_state_bytes=0 first_state_difference=0x00000000 last_state_difference=0x00000000 endpoint_value=0x00 replay_value=0x00 endpoint_tb=34063786066743458 replay_tb=34063786066743458' \
    >> "$WORK/engine-replay/$peer/log.txt"
done
"$HARNESS" --verify-existing "$WORK/engine-replay" --hook-profile \
  --engine-replay-probe >/dev/null
sed -i 's/differing_state_bytes=0/differing_state_bytes=1/' \
  "$WORK/engine-replay/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/engine-replay" --hook-profile \
    --engine-replay-probe >/dev/null 2>&1; then
  echo "divergent SC2 engine replay unexpectedly passed" >&2
  exit 1
fi

make_case engine-boundary
for peer in host guest; do
  printf '%s\n' \
    '[sc2-hook-profile] enabled expected_dol_sha256=0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5 warmup_frames=120 sample_frames=600 diagnostic_limit=4096' \
    '[sc2-hook-profile] result observed_frames=720 profiled_frames=600 strict_candidates=1 diagnostic_candidates=1200 complete=yes' \
    '[sc2-hook-profile] candidate pc=0x80012340 frames=600 once=600 hits=600 min=1 max=1 parity=300/300 first_ordinal=12..14 last_ordinal=12..14 caller_lr=0x80045678 caller_lr_stable=yes predecessor_pc=0x80023450 predecessor_stable=yes' \
    '[sc2-hook-profile] diagnostic rank=200 pc=0x8001ba3c frames=300 once=300 hits=300 min=1 max=1 parity=300/0 first_ordinal=70000..73000 last_ordinal=70000..73000 caller_lr=0x8002d628 caller_lr_stable=yes predecessor_pc=0x8002d624 predecessor_stable=yes' \
    '[sc2-memory-profile] enabled begin_pc=0x8001ba3c return_pc=0x8002d628 page_bytes=4096 target_ticks=60' \
    '[sc2-memory-profile] result ticks=60 ram_bytes=25165824 page_bytes=4096 changed_pages=128 changed_bytes_upper_bound=524288 every_tick_pages=16 complete=yes' \
    '[sc2-memory-profile] region offset=0x00001000 size=0x00010000 pages=16 changed_ticks=10..60' \
    >> "$WORK/engine-boundary/$peer/log.txt"
done
printf '%s\n' '[rollback live] correction committed' \
  >> "$WORK/engine-boundary/guest/log.txt"
"$HARNESS" --verify-existing "$WORK/engine-boundary" --hook-profile \
  --hook-diagnostic-limit 4096 --expect-sc2-engine-boundary >/dev/null
"$HARNESS" --verify-existing "$WORK/engine-boundary" --hook-profile \
  --hook-diagnostic-limit 4096 --expect-sc2-engine-boundary \
  --hook-memory-profile --hook-memory-profile-ticks 60 >/dev/null

sed -i 's/offset=0x00001000/offset=0x00002000/' \
  "$WORK/engine-boundary/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/engine-boundary" --hook-profile \
    --hook-diagnostic-limit 4096 --expect-sc2-engine-boundary \
    --hook-memory-profile --hook-memory-profile-ticks 60 >/dev/null 2>&1; then
  echo "asymmetric SC2 memory profile unexpectedly passed" >&2
  exit 1
fi
sed -i 's/offset=0x00002000/offset=0x00001000/' \
  "$WORK/engine-boundary/guest/log.txt"

cp -a "$WORK/engine-boundary" "$WORK/engine-memory-gameplay"
for peer in host guest; do
  sed -i 's/offset=0x00001000 size=0x00010000 pages=16/offset=0x00009000 size=0x00010000 pages=16/' \
    "$WORK/engine-memory-gameplay/$peer/log.txt"
done
memory_diff="$($MEMORY_DIFF "$WORK/engine-boundary" "$WORK/engine-memory-gameplay")"
printf '%s\n' "$memory_diff" | grep -Fqx 'idle_pages=16'
printf '%s\n' "$memory_diff" | grep -Fqx 'gameplay_pages=16'
printf '%s\n' "$memory_diff" | grep -Fqx 'shared_pages=8'
printf '%s\n' "$memory_diff" | grep -Fqx 'union_pages=24'
printf '%s\n' "$memory_diff" | grep -Fqx 'union_bytes_upper_bound=98304'

sed -i 's/predecessor_pc=0x8002d624/predecessor_pc=0x8002d620/' \
  "$WORK/engine-boundary/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/engine-boundary" --hook-profile \
    --hook-diagnostic-limit 4096 --expect-sc2-engine-boundary >/dev/null 2>&1; then
  echo "wrong SC2 engine boundary unexpectedly passed" >&2
  exit 1
fi

make_case asymmetric-hook-profile
for peer in host guest; do
  pc=80012340
  [ "$peer" = host ] || pc=80012344
  printf '%s\n' \
    '[sc2-hook-profile] enabled expected_dol_sha256=0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5 warmup_frames=120 sample_frames=600 diagnostic_limit=0' \
    '[sc2-hook-profile] result observed_frames=720 profiled_frames=600 strict_candidates=1 diagnostic_candidates=12 complete=yes' \
    "[sc2-hook-profile] candidate pc=0x${pc} frames=600 once=600 hits=600 min=1 max=1 parity=300/300 first_ordinal=12..14 last_ordinal=12..14 caller_lr=0x80045678 caller_lr_stable=yes predecessor_pc=0x80023450 predecessor_stable=yes" \
    >> "$WORK/asymmetric-hook-profile/$peer/log.txt"
done
printf '%s\n' '[rollback live] correction committed' \
  >> "$WORK/asymmetric-hook-profile/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/asymmetric-hook-profile" --hook-profile \
    >/dev/null 2>&1; then
  echo "asymmetric SC2 hook profile unexpectedly passed" >&2
  exit 1
fi

make_case incomplete-hook-profile
for peer in host guest; do
  printf '%s\n' \
    '[sc2-hook-profile] enabled expected_dol_sha256=0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5 warmup_frames=120 sample_frames=600 diagnostic_limit=0' \
    '[sc2-hook-profile] result observed_frames=719 profiled_frames=599 strict_candidates=1 diagnostic_candidates=12 complete=no' \
    '[sc2-hook-profile] candidate pc=0x80012340 frames=599 once=599 hits=599 min=1 max=1 parity=300/299 first_ordinal=12..14 last_ordinal=12..14 caller_lr=0x80045678 caller_lr_stable=yes predecessor_pc=0x80023450 predecessor_stable=yes' \
    >> "$WORK/incomplete-hook-profile/$peer/log.txt"
done
printf '%s\n' '[rollback live] correction committed' \
  >> "$WORK/incomplete-hook-profile/guest/log.txt"
if "$HARNESS" --verify-existing "$WORK/incomplete-hook-profile" --hook-profile \
    >/dev/null 2>&1; then
  echo "incomplete SC2 hook profile unexpectedly passed" >&2
  exit 1
fi

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
