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
if ! "${run_env[@]}" bash "$REPO/.github/scripts/netplay-match.sh" \
    "$WORK" "$PLAY" "$PORT"; then
  fail "two-peer VS route failed; evidence retained in $WORK"
fi

if [ -n "$DIGEST_MISMATCH_FRAME" ]; then
  verify_digest_mismatch_logs "$WORK" "$DIGEST_MISMATCH_FRAME"
else
  verify_logs "$WORK"
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
