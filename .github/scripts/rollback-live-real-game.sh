#!/bin/bash
# Opt-in, two-process real-game rollback fault-injection harness.
#
# The runtime is responsible for delaying authoritative RSIB batches according
# to RINGOUT_ROLLBACK_FAULT_SCRIPT.  This wrapper owns private-package checks,
# starts the existing two-peer VS route, and turns required live log evidence
# into a strict exit status.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME rollback-live-real-game.sh \
         --package <private-package> --fault-script <schedule> \
         [--work <empty-output-dir>] [--play-seconds <seconds>] \
         [--port <udp-port>] [--expect-horizon] [--production] \
         [--windowed] [--dual-core] \
         [--hook-profile] [--hook-warmup-frames <frames>] \
         [--hook-sample-frames <frames>] [--hook-diagnostic-limit <count>] \
         [--hook-idle-control] [--expect-sc2-engine-boundary] \
         [--hook-memory-profile] [--hook-memory-profile-ticks <ticks>] \
         [--engine-replay-probe] [--update-replay-probe] \
         [--selective-update-replay-probe] \
         [--expect-digest-mismatch <logical-frame>]

Test-only runtime contract:
  RINGOUT_ROLLBACK_FAULT_SCRIPT=<schedule>
  RINGOUT_ROLLBACK_TEST_ACK=HEADLESS_ISOLATED
  RINGOUT_ROLLBACK_BASE_DELAY_SAMPLES=2
  RINGOUT_ROLLBACK_HORIZON_FRAMES=8

Both logs must contain "[rollback live] negotiated". The correction scenario
requires "[rollback live] correction committed". --expect-horizon instead
requires both "[rollback live] horizon fallback" and a later
"[rollback live] horizon resumed" marker. Extra fields are allowed.

Fault schedule grammar (comments and blank lines are allowed):
  delay <send_ordinal> <release_after_send_ordinal>
  drop <send_ordinal>

--production runs the ordinary player activation and production output-gate
path: both peers get --netplay-mode rollback and both logs must report active
rollback. When combined with --fault-script, a separate fault-only
acknowledgement injects late input without selecting the isolated test gate.

--expect-digest-mismatch uses the isolated gate and corrupts only the guest's
periodic diagnostic report at the requested 60-frame checkpoint. Both peers
must report that exact desync and stop; guest RAM is never modified.

--windowed runs both peers through a real host renderer. It is the required
GPU/FIFO regression path; headless remains useful for faster protocol checks.
--dual-core opts into the guarded deterministic-GPU rollback experiment. The
player default stays single-core until this route passes the platform matrix.

--hook-profile enables the bounded SC2 dispatch profiler on both peers. The
warmup and sample windows are configurable so a script can profile menus or
actual gameplay; the defaults are 120 and 600 video frames. Diagnostic output
is disabled by default and bounded to at most 4096 ranked PCs when requested.
The verifier requires a complete sample and at least one PC that dispatched
exactly once in every sampled video frame. The result is discovery evidence,
not permission to enable a hook in production.

--hook-idle-control leaves two synchronized peers in the intro/menu route for
--play-seconds. It holds the two-controller/netplay configuration constant for
subtraction against a gameplay profile and requires --hook-profile.

--expect-sc2-engine-boundary requires the exact GRSEAF outer-loop call edge
0x8002d624 -> 0x8001ba3c -> LR 0x8002d628 to execute once on exactly one
video-frame parity. It requires an even sample and diagnostic limit 4096. This
certifies the measured 30 Hz engine boundary, not selective rollback safety.

--hook-memory-profile snapshots MEM1 at the certified engine entry and compares
it at the exact return edge. It emits the page-aligned union changed inside the
engine iteration. This requires --expect-sc2-engine-boundary and is conservative
discovery evidence, not a safe selective-state profile.

--engine-replay-probe restores a complete emulator snapshot at the certified
engine entry, executes the 30 Hz game-engine iteration twice from that same
state, and requires byte-identical complete endpoint states on both peers. It
requires --hook-profile and cannot be combined with the boundary or memory
profilers because its deliberate extra iterations change their hit counts.

--update-replay-probe applies the same symmetric full-state oracle only to the
first gameplay object-update call, 0x800095c0 -> LR 0x8001bcb0. It is a narrower
research gate and is mutually exclusive with --engine-replay-probe.

--selective-update-replay-probe leaves canonical hardware at the update
frontier, restores the exact module-CPU-written MEM1 bytes plus CPU entry
state, replaces the two sampled system handlers with captured transactions,
and compares the complete re-anchored endpoint. It is a correctness oracle,
not a live late-input correction mode.
EOF
  exit 2
}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PACKAGE="${RINGOUT_ROLLBACK_PACKAGE:-}"
FAULT_SCRIPT=""
WORK=""
PLAY=16
PORT=2671
EXPECT_HORIZON=0
VERIFY_EXISTING=""
VALIDATE_ONLY=""
PRODUCTION=0
DIGEST_MISMATCH_FRAME=""
WINDOWED_RUN=0
DUAL_CORE_RUN=0
HOOK_PROFILE=0
HOOK_PROFILE_WARMUP=120
HOOK_PROFILE_SAMPLE=600
HOOK_PROFILE_DIAGNOSTIC_LIMIT=0
HOOK_IDLE_CONTROL=0
EXPECT_SC2_ENGINE_BOUNDARY=0
HOOK_MEMORY_PROFILE=0
HOOK_MEMORY_PROFILE_TICKS=60
ENGINE_REPLAY_PROBE=0
UPDATE_REPLAY_PROBE=0
SELECTIVE_UPDATE_REPLAY_PROBE=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --package) [ "$#" -ge 2 ] || usage; PACKAGE="$2"; shift 2 ;;
    --fault-script) [ "$#" -ge 2 ] || usage; FAULT_SCRIPT="$2"; shift 2 ;;
    --work) [ "$#" -ge 2 ] || usage; WORK="$2"; shift 2 ;;
    --play-seconds) [ "$#" -ge 2 ] || usage; PLAY="$2"; shift 2 ;;
    --port) [ "$#" -ge 2 ] || usage; PORT="$2"; shift 2 ;;
    --expect-horizon) EXPECT_HORIZON=1; shift ;;
    --production) PRODUCTION=1; shift ;;
    --windowed) WINDOWED_RUN=1; shift ;;
    --dual-core) DUAL_CORE_RUN=1; shift ;;
    --hook-profile) HOOK_PROFILE=1; shift ;;
    --hook-warmup-frames)
      [ "$#" -ge 2 ] || usage
      HOOK_PROFILE_WARMUP="$2"
      shift 2
      ;;
    --hook-sample-frames)
      [ "$#" -ge 2 ] || usage
      HOOK_PROFILE_SAMPLE="$2"
      shift 2
      ;;
    --hook-diagnostic-limit)
      [ "$#" -ge 2 ] || usage
      HOOK_PROFILE_DIAGNOSTIC_LIMIT="$2"
      shift 2
      ;;
    --hook-idle-control) HOOK_IDLE_CONTROL=1; shift ;;
    --expect-sc2-engine-boundary) EXPECT_SC2_ENGINE_BOUNDARY=1; shift ;;
    --hook-memory-profile) HOOK_MEMORY_PROFILE=1; shift ;;
    --hook-memory-profile-ticks)
      [ "$#" -ge 2 ] || usage
      HOOK_MEMORY_PROFILE_TICKS="$2"
      shift 2
      ;;
    --engine-replay-probe) ENGINE_REPLAY_PROBE=1; shift ;;
    --update-replay-probe) UPDATE_REPLAY_PROBE=1; shift ;;
    --selective-update-replay-probe) SELECTIVE_UPDATE_REPLAY_PROBE=1; shift ;;
    --expect-digest-mismatch)
      [ "$#" -ge 2 ] || usage
      DIGEST_MISMATCH_FRAME="$2"
      shift 2
      ;;
    --verify-existing) [ "$#" -ge 2 ] || usage; VERIFY_EXISTING="$2"; shift 2 ;;
    --validate-fault-script) [ "$#" -ge 2 ] || usage; VALIDATE_ONLY="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done

if [ "$HOOK_PROFILE" -eq 1 ]; then
  [[ "$HOOK_PROFILE_WARMUP" =~ ^[0-9]+$ ]] || fail "hook warmup must be an integer"
  [[ "$HOOK_PROFILE_SAMPLE" =~ ^[0-9]+$ ]] || fail "hook sample must be an integer"
  [[ "$HOOK_PROFILE_DIAGNOSTIC_LIMIT" =~ ^[0-9]+$ ]] ||
    fail "hook diagnostic limit must be an integer"
  HOOK_PROFILE_WARMUP=$((10#$HOOK_PROFILE_WARMUP))
  HOOK_PROFILE_SAMPLE=$((10#$HOOK_PROFILE_SAMPLE))
  HOOK_PROFILE_DIAGNOSTIC_LIMIT=$((10#$HOOK_PROFILE_DIAGNOSTIC_LIMIT))
  [ "$HOOK_PROFILE_WARMUP" -le 36000 ] || fail "hook warmup must be at most 36000"
  [ "$HOOK_PROFILE_SAMPLE" -ge 60 ] && [ "$HOOK_PROFILE_SAMPLE" -le 3600 ] ||
    fail "hook sample must be between 60 and 3600"
  [ "$HOOK_PROFILE_DIAGNOSTIC_LIMIT" -le 4096 ] ||
    fail "hook diagnostic limit must be at most 4096"
fi
[ "$HOOK_IDLE_CONTROL" -eq 0 ] || [ "$HOOK_PROFILE" -eq 1 ] ||
  fail "--hook-idle-control requires --hook-profile"
[ "$EXPECT_SC2_ENGINE_BOUNDARY" -eq 0 ] || {
  [ "$HOOK_PROFILE" -eq 1 ] || fail "--expect-sc2-engine-boundary requires --hook-profile"
  [ $((HOOK_PROFILE_SAMPLE % 2)) -eq 0 ] ||
    fail "SC2 engine-boundary evidence requires an even hook sample"
  [ "$HOOK_PROFILE_DIAGNOSTIC_LIMIT" -eq 4096 ] ||
    fail "SC2 engine-boundary evidence requires --hook-diagnostic-limit 4096"
}
if [ "$HOOK_MEMORY_PROFILE" -eq 1 ]; then
  [ "$EXPECT_SC2_ENGINE_BOUNDARY" -eq 1 ] ||
    fail "--hook-memory-profile requires --expect-sc2-engine-boundary"
  [[ "$HOOK_MEMORY_PROFILE_TICKS" =~ ^[1-9][0-9]*$ ]] ||
    fail "hook memory profile ticks must be positive"
  HOOK_MEMORY_PROFILE_TICKS=$((10#$HOOK_MEMORY_PROFILE_TICKS))
  [ "$HOOK_MEMORY_PROFILE_TICKS" -le 600 ] ||
    fail "hook memory profile ticks must be at most 600"
  [ "$HOOK_PROFILE_SAMPLE" -ge $((HOOK_MEMORY_PROFILE_TICKS * 2)) ] ||
    fail "hook sample must cover at least twice the requested SC2 memory ticks"
fi
if [ "$ENGINE_REPLAY_PROBE" -eq 1 ] || [ "$UPDATE_REPLAY_PROBE" -eq 1 ] ||
   [ "$SELECTIVE_UPDATE_REPLAY_PROBE" -eq 1 ]; then
  [ "$HOOK_PROFILE" -eq 1 ] || fail "replay probes require --hook-profile"
  [ $((ENGINE_REPLAY_PROBE + UPDATE_REPLAY_PROBE + SELECTIVE_UPDATE_REPLAY_PROBE)) -eq 1 ] ||
    fail "SC2 replay probes are mutually exclusive"
  [ "$EXPECT_SC2_ENGINE_BOUNDARY" -eq 0 ] ||
    fail "--engine-replay-probe cannot be combined with --expect-sc2-engine-boundary"
  [ "$HOOK_MEMORY_PROFILE" -eq 0 ] ||
    fail "--engine-replay-probe cannot be combined with --hook-memory-profile"
fi

validate_fault_script() {
  local schedule="$1"
  awk '
    {
      sub(/[[:space:]]*#.*/, "")
      if (NF == 0)
        next
      if ($1 == "delay" && NF == 3 && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ &&
          $2 >= 1 && $3 > $2 && !seen[$2]++) {
        instructions++
        if (instructions > 64) exit 1
        next
      }
      if ($1 == "drop" && NF == 2 && $2 ~ /^[0-9]+$/ && $2 >= 1 && !seen[$2]++) {
        instructions++
        if (instructions > 64) exit 1
        next
      }
      exit 1
    }
    END { if (instructions == 0) exit 1 }
  ' "$schedule" || fail "fault schedule is malformed or contradictory"
}

verify_logs() {
  local evidence="$1"
  local host_log="$evidence/host/log.txt"
  local guest_log="$evidence/guest/log.txt"
  [ -s "$host_log" ] && [ -s "$guest_log" ] || fail "one or both peer logs are empty"
  grep -Fq '[rollback live] negotiated' "$host_log" || fail "host did not negotiate rollback"
  grep -Fq '[rollback live] negotiated' "$guest_log" || fail "guest did not negotiate rollback"
  if [ "$PRODUCTION" -eq 1 ]; then
    grep -Fq '[rollback live] active' "$host_log" || fail "host did not activate rollback"
    grep -Fq '[rollback live] active' "$guest_log" || fail "guest did not activate rollback"
    if [ -n "$FAULT_SCRIPT" ]; then
      grep -Fq '[rollback live] correction committed' "$host_log" "$guest_log" ||
        fail "production path did not commit an injected correction"
    fi
  elif [ "$EXPECT_HORIZON" -eq 1 ]; then
    grep -Fq '[rollback live] horizon fallback' "$host_log" "$guest_log" ||
      fail "expected horizon fallback was not reported"
    grep -Fq '[rollback live] horizon resumed' "$host_log" "$guest_log" ||
      fail "prediction horizon did not resume with authoritative input"
  else
    grep -Fq '[rollback live] correction committed' "$host_log" "$guest_log" ||
      fail "fault schedule produced no committed live correction"
  fi
  if grep -Eqi 'DESYNC|\[rollback live\].*(failed|fatal|aborted|faulted|cancelled|diverged|refused)' \
      "$host_log" "$guest_log"; then
    fail "peer reported a desync or rollback failure"
  fi
  if grep -Eqi 'GFX FIFO|Unknown Opcode|FIFOs linked but out of sync|Desynced read pointers|Aux FIFO|Negative fifo\.|FIFO out of bounds|SIG(SEGV|ILL|ABRT)|AddressSanitizer' \
      "$host_log" "$guest_log"; then
    fail "peer reported a GPU/FIFO consistency failure or process crash"
  fi

  verify_confirmed_state_logs "$evidence"
}

verify_digest_mismatch_logs() {
  local evidence="$1"
  local frame="$2"
  local host_log="$evidence/host/log.txt"
  local guest_log="$evidence/guest/log.txt"
  [ -s "$host_log" ] && [ -s "$guest_log" ] || fail "one or both peer logs are empty"
  grep -Fq '[rollback live] negotiated' "$host_log" || fail "host did not negotiate rollback"
  grep -Fq '[rollback live] negotiated' "$guest_log" || fail "guest did not negotiate rollback"
  grep -Fq '[rollback live] active' "$host_log" || fail "host did not activate rollback"
  grep -Fq '[rollback live] active' "$guest_log" || fail "guest did not activate rollback"
  grep -Fq "[rollback live] isolated digest fault injected at frame $frame" "$guest_log" ||
    fail "guest did not inject the acknowledged digest fault"
  grep -Fq "DESYNC at frame $frame" "$host_log" ||
    fail "host did not report the expected digest mismatch"
  grep -Fq "DESYNC at frame $frame" "$guest_log" ||
    fail "guest did not report the expected digest mismatch"
}

verify_confirmed_state_logs() {
  local evidence="$1"
  local host_state="$evidence/host/confirmed-state.log"
  local guest_state="$evidence/guest/confirmed-state.log"
  local minimum="${MIN_CONFIRMED_DIGESTS:-3}"
  [[ "$minimum" =~ ^[1-9][0-9]*$ ]] || fail "MIN_CONFIRMED_DIGESTS must be positive"
  [ -s "$host_state" ] && [ -s "$guest_state" ] ||
    fail "one or both confirmed logical-state logs are empty"

  local compare_dir
  compare_dir="$(mktemp -d /tmp/ringout-confirmed-compare.XXXXXXXX)"
  canonicalize_confirmed_log() {
    awk '
      NF != 4 || $1 !~ /^[0-9]+$/ || $1 == 0 || $1 % 60 != 0 ||
      length($2) != 8 || $2 !~ /^[0-9a-f]+$/ ||
      length($3) != 8 || $3 !~ /^[0-9a-f]+$/ ||
      length($4) != 16 || $4 !~ /^[0-9a-f]+$/ { exit 1 }
      { latest[$1] = $0 }
      END { for (frame in latest) print latest[frame] }
    ' "$1" | sort -n -k1,1
  }
  if ! canonicalize_confirmed_log "$host_state" > "$compare_dir/host" ||
     ! canonicalize_confirmed_log "$guest_state" > "$compare_dir/guest"; then
    rm -rf "$compare_dir"
    fail "confirmed logical-state log is malformed"
  fi
  join -j 1 "$compare_dir/host" "$compare_dir/guest" > "$compare_dir/common"
  local count
  count="$(wc -l < "$compare_dir/common")"
  if [ "$count" -lt "$minimum" ]; then
    rm -rf "$compare_dir"
    fail "expected at least $minimum matching confirmed logical frames; found $count"
  fi
  if ! awk '$2 != $5 || $3 != $6 || $4 != $7 { exit 1 }' "$compare_dir/common"; then
    local mismatch
    mismatch="$(awk '$2 != $5 || $3 != $6 || $4 != $7 { print; exit }' \
      "$compare_dir/common")"
    rm -rf "$compare_dir"
    fail "confirmed logical state diverged: $mismatch"
  fi
  rm -rf "$compare_dir"
}

verify_hook_profiles() {
  local evidence="$1"
  local peer log result expected_observed strict_count actual_count
  expected_observed=$((HOOK_PROFILE_WARMUP + HOOK_PROFILE_SAMPLE))
  canonical_strict_candidates() {
    grep -E '^\[sc2-hook-profile\] candidate pc=0x[0-9a-f]{8} ' "$1" |
      sed -E 's/^\[sc2-hook-profile\] candidate pc=(0x[0-9a-f]{8}).*/\1/' |
      sort -u
  }
  for peer in host guest; do
    log="$evidence/$peer/log.txt"
    grep -Fq "[sc2-hook-profile] enabled expected_dol_sha256=0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5 warmup_frames=${HOOK_PROFILE_WARMUP} sample_frames=${HOOK_PROFILE_SAMPLE} diagnostic_limit=${HOOK_PROFILE_DIAGNOSTIC_LIMIT}" "$log" ||
      fail "$peer did not profile the supported GRSEAF revision"
    result="$(grep -F '[sc2-hook-profile] result ' "$log" | tail -1)"
    [ -n "$result" ] || fail "$peer produced no hook profile result"
    printf '%s\n' "$result" | grep -Eq "observed_frames=${expected_observed} profiled_frames=${HOOK_PROFILE_SAMPLE} strict_candidates=[1-9][0-9]* .*complete=yes" ||
      fail "$peer hook profile is incomplete or has no strict candidate"
    grep -Eq "^\[sc2-hook-profile\] candidate pc=0x[0-9a-f]{8} frames=${HOOK_PROFILE_SAMPLE} once=${HOOK_PROFILE_SAMPLE} hits=${HOOK_PROFILE_SAMPLE} min=1 max=1 parity=[0-9]+/[0-9]+ first_ordinal=[0-9]+\.\.[0-9]+ last_ordinal=[0-9]+\.\.[0-9]+ caller_lr=0x[0-9a-f]{8} caller_lr_stable=(yes|no) predecessor_pc=0x[0-9a-f]{8} predecessor_stable=(yes|no)$" "$log" ||
      fail "$peer produced no stable once-per-frame candidate"
    strict_count="$(printf '%s\n' "$result" |
      sed -E 's/.* strict_candidates=([0-9]+) .*/\1/')"
    actual_count="$(canonical_strict_candidates "$log" | wc -l)"
    [ "$actual_count" -eq "$strict_count" ] ||
      fail "$peer strict candidate count does not match its result"
  done
  local difference
  if ! difference="$(diff -u \
      <(canonical_strict_candidates "$evidence/host/log.txt") \
      <(canonical_strict_candidates "$evidence/guest/log.txt"))"; then
    fail "peer strict hook candidate sets differ: $difference"
  fi

  if [ "$EXPECT_SC2_ENGINE_BOUNDARY" -eq 1 ]; then
    local half_sample=$((HOOK_PROFILE_SAMPLE / 2)) parity
    parity="(${half_sample}/0|0/${half_sample})"
    for peer in host guest; do
      log="$evidence/$peer/log.txt"
      grep -Eq "^\[sc2-hook-profile\] diagnostic rank=[0-9]+ pc=0x8001ba3c frames=${half_sample} once=${half_sample} hits=${half_sample} min=1 max=1 parity=${parity} first_ordinal=[0-9]+\.\.[0-9]+ last_ordinal=[0-9]+\.\.[0-9]+ caller_lr=0x8002d628 caller_lr_stable=yes predecessor_pc=0x8002d624 predecessor_stable=yes$" "$log" ||
        fail "$peer did not reproduce the SC2 30 Hz engine boundary"
    done
  fi
  if [ "$HOOK_MEMORY_PROFILE" -eq 1 ]; then
    for peer in host guest; do
      log="$evidence/$peer/log.txt"
      grep -Fq "[sc2-memory-profile] enabled begin_pc=0x8001ba3c return_pc=0x8002d628 page_bytes=4096 target_ticks=${HOOK_MEMORY_PROFILE_TICKS}" "$log" ||
        fail "$peer did not enable the scoped SC2 memory profile"
      grep -Eq "^\[sc2-memory-profile\] result ticks=${HOOK_MEMORY_PROFILE_TICKS} ram_bytes=[1-9][0-9]* page_bytes=4096 changed_pages=[1-9][0-9]* changed_bytes_upper_bound=[1-9][0-9]* every_tick_pages=[0-9]+ complete=yes$" "$log" ||
        fail "$peer did not complete a non-empty SC2 memory profile"
      grep -Eq '^\[sc2-memory-profile\] region offset=0x[0-9a-f]{8} size=0x[0-9a-f]{8} pages=[1-9][0-9]* changed_ticks=[1-9][0-9]*\.\.[1-9][0-9]*$' "$log" ||
        fail "$peer SC2 memory profile has no changed region"
    done
    local memory_difference
    if ! memory_difference="$(diff -u \
        <(grep -E '^\[sc2-memory-profile\] (result|region) ' "$evidence/host/log.txt") \
        <(grep -E '^\[sc2-memory-profile\] (result|region) ' "$evidence/guest/log.txt"))"; then
      fail "peer SC2 memory profiles differ: $memory_difference"
    fi
  fi
  if [ "$ENGINE_REPLAY_PROBE" -eq 1 ] || [ "$UPDATE_REPLAY_PROBE" -eq 1 ] ||
     [ "$SELECTIVE_UPDATE_REPLAY_PROBE" -eq 1 ]; then
    for peer in host guest; do
      log="$evidence/$peer/log.txt"
      if [ "$SELECTIVE_UPDATE_REPLAY_PROBE" -eq 1 ]; then
        grep -Fq '[sc2-engine-replay] enabled mode=selective-update-call begin_pc=0x800095c0 return_pc=0x8001bcb0' "$log" ||
          fail "$peer did not enable the selective SC2 update replay probe"
      elif [ "$UPDATE_REPLAY_PROBE" -eq 1 ]; then
        grep -Fq '[sc2-engine-replay] enabled mode=full-emulator-update-call begin_pc=0x800095c0 return_pc=0x8001bcb0' "$log" ||
          fail "$peer did not enable the full-emulator SC2 update replay probe"
      else
        grep -Fq '[sc2-engine-replay] enabled mode=full-emulator-one-tick begin_pc=0x8001ba3c return_pc=0x8002d628' "$log" ||
          fail "$peer did not enable the full-emulator SC2 engine replay probe"
      fi
      if [ "$SELECTIVE_UPDATE_REPLAY_PROBE" -eq 0 ]; then
        grep -Fq '[sc2-engine-replay] captured normalized reference; restored entry for verification replay' "$log" ||
          fail "$peer did not execute the symmetric SC2 engine replay sequence"
      fi
      grep -Eq '^\[sc2-engine-external\] result reads=[0-9]+ writes=[0-9]+ read_sites=[0-9]+ write_sites=[0-9]+ read_block_sites=[0-9]+ write_block_sites=[0-9]+ fallback_instructions=[0-9]+ overflow=no complete=yes$' "$log" ||
        fail "$peer did not complete the SC2 engine external-effect profile"
      grep -Eq '^\[sc2-engine-calls\] result sites=[1-9][0-9]* completed=[1-9][0-9]* overflow=no complete=yes$' "$log" ||
        fail "$peer did not complete the SC2 engine direct-call profile"
      grep -Eq '^\[sc2-engine-indirect\] result sites=[1-9][0-9]* completed=[1-9][0-9]* overflow=no complete=yes$' "$log" ||
        fail "$peer did not complete the SC2 engine indirect-call profile"
      result="$(grep -F '[sc2-engine-replay] full-state-result ' "$log" | tail -1)"
      [ -n "$result" ] || fail "$peer produced no SC2 engine replay result"
      printf '%s\n' "$result" | grep -Eq 'state_match=yes cpu_match=yes tb_remainder_match=yes input_replay_match=yes input_polls=[0-9]+ external_profile_complete=yes external_replay_match=yes external_effects=[0-9]+ endpoint_bytes=[1-9][0-9]* replay_bytes=[1-9][0-9]* differing_state_bytes=0 first_state_difference=0x00000000 last_state_difference=0x00000000 endpoint_value=0x00 replay_value=0x00 endpoint_tb=[0-9]+ replay_tb=[0-9]+$' ||
        fail "$peer SC2 engine replay endpoint was not exact"
      local endpoint_bytes replay_bytes endpoint_tb replay_tb
      endpoint_bytes="$(printf '%s\n' "$result" | sed -E 's/.* endpoint_bytes=([0-9]+).*/\1/')"
      replay_bytes="$(printf '%s\n' "$result" | sed -E 's/.* replay_bytes=([0-9]+).*/\1/')"
      endpoint_tb="$(printf '%s\n' "$result" | sed -E 's/.* endpoint_tb=([0-9]+).*/\1/')"
      replay_tb="$(printf '%s\n' "$result" | sed -E 's/.* replay_tb=([0-9]+).*/\1/')"
      [ "$endpoint_bytes" = "$replay_bytes" ] || fail "$peer replay state size changed"
      [ "$endpoint_tb" = "$replay_tb" ] || fail "$peer replay timebase changed"
    done
  fi
}

# Asset-free parser/orchestration tests use this path with synthetic logs. It
# never launches a runtime and is intentionally undocumented in normal usage.
if [ -n "$VERIFY_EXISTING" ]; then
  VERIFY_EXISTING="$(readlink -f "$VERIFY_EXISTING")"
  [ -d "$VERIFY_EXISTING" ] || fail "verification directory is unavailable"
  if [ -n "$DIGEST_MISMATCH_FRAME" ]; then
    verify_digest_mismatch_logs "$VERIFY_EXISTING" "$DIGEST_MISMATCH_FRAME"
  else
    verify_logs "$VERIFY_EXISTING"
  fi
  if [ "$HOOK_PROFILE" -eq 1 ]; then
    verify_hook_profiles "$VERIFY_EXISTING"
  fi
  echo "PASS: live rollback evidence satisfies the log contract."
  exit 0
fi

if [ -n "$VALIDATE_ONLY" ]; then
  VALIDATE_ONLY="$(readlink -f "$VALIDATE_ONLY")"
  [ -s "$VALIDATE_ONLY" ] || fail "fault schedule is unavailable or empty"
  validate_fault_script "$VALIDATE_ONLY"
  echo "PASS: rollback fault schedule is valid."
  exit 0
fi

[ "${RINGOUT_REAL_GAME_ACK:-}" = I_OWN_THE_GAME ] ||
  fail "set RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME to opt in"
[ -n "$PACKAGE" ] || usage
[ "$PRODUCTION" -eq 1 ] || [ -n "$FAULT_SCRIPT" ] ||
  [ -n "$DIGEST_MISMATCH_FRAME" ] || usage
[ "$PRODUCTION" -eq 0 ] || [ "$EXPECT_HORIZON" -eq 0 ] || \
  fail "--production and --expect-horizon are mutually exclusive"
[ -z "$DIGEST_MISMATCH_FRAME" ] || {
  [ "$PRODUCTION" -eq 0 ] || fail "digest mismatch test cannot use --production"
  [ "$EXPECT_HORIZON" -eq 0 ] || fail "digest mismatch and horizon tests are mutually exclusive"
  [ -z "$FAULT_SCRIPT" ] || fail "digest mismatch and input-fault tests are mutually exclusive"
  [[ "$DIGEST_MISMATCH_FRAME" =~ ^[1-9][0-9]*$ ]] ||
    fail "digest mismatch frame must be positive"
  DIGEST_MISMATCH_FRAME=$((10#$DIGEST_MISMATCH_FRAME))
  [ $((DIGEST_MISMATCH_FRAME % 60)) -eq 0 ] ||
    fail "digest mismatch frame must be a 60-frame checkpoint"
}
[[ "$PLAY" =~ ^[1-9][0-9]*$ ]] || fail "--play-seconds must be positive"
[[ "$PORT" =~ ^[1-9][0-9]*$ ]] || fail "--port must be positive"
PLAY=$((10#$PLAY))
PORT=$((10#$PORT))
[ "$PORT" -le 65535 ] || fail "--port must be at most 65535"

PACKAGE="$(readlink -f "$PACKAGE")"
[ -d "$PACKAGE" ] || fail "private package directory is unavailable"
if [ -n "$FAULT_SCRIPT" ]; then
  FAULT_SCRIPT="$(readlink -f "$FAULT_SCRIPT")"
  [ -f "$FAULT_SCRIPT" ] || fail "fault schedule is unavailable"
  [ -s "$FAULT_SCRIPT" ] || fail "fault schedule is empty"
  validate_fault_script "$FAULT_SCRIPT"
fi
[ -x "$PACKAGE/bin/moderngekko-run" ] || fail "private package has no executable runtime"
[ -f "$PACKAGE/game/sys/main.dol" ] || fail "private package has no extracted game"
if [ "$HOOK_PROFILE" -eq 1 ]; then
  [ "$(sha256sum "$PACKAGE/game/sys/main.dol" | cut -d' ' -f1)" = \
    0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5 ] ||
    fail "hook profiling requires the supported unmodified GRSEAF revision-0 DOL"
fi
shopt -s nullglob
modules=("$PACKAGE"/bin/gGRSEAF_recomp.so)
shopt -u nullglob
[ "${#modules[@]}" -eq 1 ] || fail "private package must contain gGRSEAF_recomp.so"

if [ -z "$WORK" ]; then
  WORK="$(mktemp -d /tmp/ringout-live-rollback.XXXXXXXX)"
else
  WORK="$(readlink -m "$WORK")"
  case "$WORK" in
    /tmp/ringout-live-rollback.*) ;;
    *) fail "--work must be under /tmp/ringout-live-rollback.*" ;;
  esac
  if [ -e "$WORK" ]; then
    [ -d "$WORK" ] || fail "--work is not a directory"
    [ -z "$(find "$WORK" -mindepth 1 -maxdepth 1 -print -quit)" ] ||
      fail "--work must be empty"
  fi
fi

# netplay-match.sh creates and owns WORK. Its normal fixed-delay mode retains
# exact physical-frame comparison; fault mode retains those traces but uses the
# live correction/desync evidence below because replay adds physical rows.
if [ "$PRODUCTION" -eq 1 ]; then
  run_env=(env -u RINGOUT_ROLLBACK_TEST_ACK -u RINGOUT_ROLLBACK_FAULT_ACK \
    -u RINGOUT_ROLLBACK_FAULT_SCRIPT \
    -u RINGOUT_ROLLBACK_BASE_DELAY_SAMPLES -u RINGOUT_ROLLBACK_HORIZON_FRAMES \
    PKG="$PACKAGE" RINGOUT_ROLLBACK_PRODUCTION=1)
  if [ -n "$FAULT_SCRIPT" ]; then
    run_env+=(RINGOUT_ROLLBACK_FAULT_SCRIPT="$FAULT_SCRIPT")
  fi
elif [ -n "$DIGEST_MISMATCH_FRAME" ]; then
  run_env=(env PKG="$PACKAGE"
    RINGOUT_EXPECT_DIGEST_MISMATCH_FRAME="$DIGEST_MISMATCH_FRAME")
else
  run_env=(env PKG="$PACKAGE" RINGOUT_ROLLBACK_FAULT_SCRIPT="$FAULT_SCRIPT")
fi
if [ "$WINDOWED_RUN" -eq 1 ]; then
  run_env+=(WINDOWED=1)
fi
if [ "$DUAL_CORE_RUN" -eq 1 ]; then
  run_env+=(RINGOUT_ROLLBACK_DUALCORE=1)
fi
if [ "$HOOK_PROFILE" -eq 1 ]; then
  run_env+=(RINGOUT_SC2_HOOK_PROFILE=1
    RINGOUT_SC2_HOOK_PROFILE_WARMUP_FRAMES="$HOOK_PROFILE_WARMUP"
    RINGOUT_SC2_HOOK_PROFILE_SAMPLE_FRAMES="$HOOK_PROFILE_SAMPLE"
    RINGOUT_SC2_HOOK_PROFILE_DIAGNOSTIC_LIMIT="$HOOK_PROFILE_DIAGNOSTIC_LIMIT"
    RINGOUT_SC2_HOOK_PROFILE_ARM_ROUTE="$([ "$HOOK_IDLE_CONTROL" -eq 1 ] && echo idle-control || echo gameplay)")
fi
if [ "$HOOK_MEMORY_PROFILE" -eq 1 ]; then
  run_env+=(RINGOUT_SC2_MEMORY_PROFILE=1
    RINGOUT_SC2_MEMORY_PROFILE_TICKS="$HOOK_MEMORY_PROFILE_TICKS")
fi
if [ "$ENGINE_REPLAY_PROBE" -eq 1 ]; then
  run_env+=(RINGOUT_SC2_ENGINE_REPLAY_PROBE=FULL_EMULATOR_ONE_TICK)
elif [ "$UPDATE_REPLAY_PROBE" -eq 1 ]; then
  run_env+=(RINGOUT_SC2_ENGINE_REPLAY_PROBE=FULL_EMULATOR_UPDATE_CALL)
elif [ "$SELECTIVE_UPDATE_REPLAY_PROBE" -eq 1 ]; then
  run_env+=(RINGOUT_SC2_ENGINE_REPLAY_PROBE=SELECTIVE_UPDATE_CALL)
fi
if [ "$HOOK_IDLE_CONTROL" -eq 1 ]; then
  run_env+=(RINGOUT_NETPLAY_IDLE_ROUTE=1)
fi
if ! "${run_env[@]}" bash "$REPO/.github/scripts/netplay-match.sh" \
    "$WORK" "$PLAY" "$PORT"; then
  fail "two-peer VS route failed; evidence retained in $WORK"
fi

if [ -n "$DIGEST_MISMATCH_FRAME" ]; then
  verify_digest_mismatch_logs "$WORK" "$DIGEST_MISMATCH_FRAME"
else
  verify_logs "$WORK"
fi
if [ "$HOOK_PROFILE" -eq 1 ]; then
  verify_hook_profiles "$WORK"
fi

{
  if [ -n "$FAULT_SCRIPT" ]; then
    echo "fault_script_sha256=$(sha256sum "$FAULT_SCRIPT" | cut -d' ' -f1)"
  fi
  echo "runtime_sha256=$(sha256sum "$PACKAGE/bin/moderngekko-run" | cut -d' ' -f1)"
  echo "module_sha256=$(sha256sum "${modules[0]}" | cut -d' ' -f1)"
  echo "dol_sha256=$(sha256sum "$PACKAGE/game/sys/main.dol" | cut -d' ' -f1)"
  echo "play_seconds=$PLAY"
  echo "port=$PORT"
  echo "expect_horizon=$EXPECT_HORIZON"
  echo "production_path=$PRODUCTION"
  echo "renderer_path=$([ "$WINDOWED_RUN" -eq 1 ] && echo windowed || echo headless)"
  echo "threading_path=$([ "$DUAL_CORE_RUN" -eq 1 ] && echo dual-core-experiment || echo rollback-safe-default)"
  echo "sc2_hook_profile=$([ "$HOOK_PROFILE" -eq 1 ] && echo complete || echo disabled)"
  if [ "$HOOK_PROFILE" -eq 1 ]; then
    echo "sc2_hook_profile_warmup_frames=$HOOK_PROFILE_WARMUP"
    echo "sc2_hook_profile_sample_frames=$HOOK_PROFILE_SAMPLE"
    echo "sc2_hook_profile_diagnostic_limit=$HOOK_PROFILE_DIAGNOSTIC_LIMIT"
    echo "sc2_hook_profile_route=$([ "$HOOK_IDLE_CONTROL" -eq 1 ] && echo idle-control || echo gameplay)"
    echo "sc2_engine_boundary=$([ "$EXPECT_SC2_ENGINE_BOUNDARY" -eq 1 ] && echo verified-30hz || echo unchecked)"
    echo "sc2_memory_profile=$([ "$HOOK_MEMORY_PROFILE" -eq 1 ] && echo complete || echo disabled)"
    if [ "$HOOK_MEMORY_PROFILE" -eq 1 ]; then
      echo "sc2_memory_profile_ticks=$HOOK_MEMORY_PROFILE_TICKS"
    fi
    if [ "$SELECTIVE_UPDATE_REPLAY_PROBE" -eq 1 ]; then
      echo "sc2_engine_replay_probe=selective-update-call"
    elif [ "$UPDATE_REPLAY_PROBE" -eq 1 ]; then
      echo "sc2_engine_replay_probe=exact-full-emulator-update-call"
    else
      echo "sc2_engine_replay_probe=$([ "$ENGINE_REPLAY_PROBE" -eq 1 ] && echo exact-full-emulator || echo disabled)"
    fi
  fi
  if [ -n "$DIGEST_MISMATCH_FRAME" ]; then
    echo "expected_digest_mismatch_frame=$DIGEST_MISMATCH_FRAME"
  fi
} > "$WORK/rollback-result.env"

if [ -n "$DIGEST_MISMATCH_FRAME" ]; then
  echo "PASS: two real-game peers rejected the injected confirmed-state digest mismatch."
elif [ "$PRODUCTION" -eq 1 ] && [ -n "$FAULT_SCRIPT" ]; then
  echo "PASS: two real-game peers corrected late input through the production rollback gate."
elif [ "$PRODUCTION" -eq 1 ]; then
  echo "PASS: two real-game peers activated ordinary player rollback and completed the route."
elif [ "$EXPECT_HORIZON" -eq 1 ]; then
  echo "PASS: two real-game peers stalled at the prediction horizon and resumed."
else
  echo "PASS: two real-game peers negotiated rollback and corrected injected late input."
fi
echo "Evidence retained in: $WORK"
