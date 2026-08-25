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
         [--port <udp-port>] [--expect-horizon]

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

while [ "$#" -gt 0 ]; do
  case "$1" in
    --package) [ "$#" -ge 2 ] || usage; PACKAGE="$2"; shift 2 ;;
    --fault-script) [ "$#" -ge 2 ] || usage; FAULT_SCRIPT="$2"; shift 2 ;;
    --work) [ "$#" -ge 2 ] || usage; WORK="$2"; shift 2 ;;
    --play-seconds) [ "$#" -ge 2 ] || usage; PLAY="$2"; shift 2 ;;
    --port) [ "$#" -ge 2 ] || usage; PORT="$2"; shift 2 ;;
    --expect-horizon) EXPECT_HORIZON=1; shift ;;
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
        next
      }
      if ($1 == "drop" && NF == 2 && $2 ~ /^[0-9]+$/ && $2 >= 1 && !seen[$2]++) {
        instructions++
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
  if [ "$EXPECT_HORIZON" -eq 1 ]; then
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
}

# Asset-free parser/orchestration tests use this path with synthetic logs. It
# never launches a runtime and is intentionally undocumented in normal usage.
if [ -n "$VERIFY_EXISTING" ]; then
  VERIFY_EXISTING="$(readlink -f "$VERIFY_EXISTING")"
  [ -d "$VERIFY_EXISTING" ] || fail "verification directory is unavailable"
  verify_logs "$VERIFY_EXISTING"
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
[ -n "$FAULT_SCRIPT" ] || usage
[[ "$PLAY" =~ ^[1-9][0-9]*$ ]] || fail "--play-seconds must be positive"
[[ "$PORT" =~ ^[1-9][0-9]*$ ]] || fail "--port must be positive"
PLAY=$((10#$PLAY))
PORT=$((10#$PORT))
[ "$PORT" -le 65535 ] || fail "--port must be at most 65535"

PACKAGE="$(readlink -f "$PACKAGE")"
FAULT_SCRIPT="$(readlink -f "$FAULT_SCRIPT")"
[ -d "$PACKAGE" ] || fail "private package directory is unavailable"
[ -f "$FAULT_SCRIPT" ] || fail "fault schedule is unavailable"
[ -s "$FAULT_SCRIPT" ] || fail "fault schedule is empty"
validate_fault_script "$FAULT_SCRIPT"
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
if ! PKG="$PACKAGE" RINGOUT_ROLLBACK_FAULT_SCRIPT="$FAULT_SCRIPT" \
    bash "$REPO/.github/scripts/netplay-match.sh" "$WORK" "$PLAY" "$PORT"; then
  fail "two-peer VS route failed; evidence retained in $WORK"
fi

verify_logs "$WORK"

{
  echo "fault_script_sha256=$(sha256sum "$FAULT_SCRIPT" | cut -d' ' -f1)"
  echo "runtime_sha256=$(sha256sum "$PACKAGE/bin/moderngekko-run" | cut -d' ' -f1)"
  echo "module_sha256=$(sha256sum "${modules[0]}" | cut -d' ' -f1)"
  echo "dol_sha256=$(sha256sum "$PACKAGE/game/sys/main.dol" | cut -d' ' -f1)"
  echo "play_seconds=$PLAY"
  echo "port=$PORT"
  echo "expect_horizon=$EXPECT_HORIZON"
} > "$WORK/rollback-result.env"

if [ "$EXPECT_HORIZON" -eq 1 ]; then
  echo "PASS: two real-game peers stalled at the prediction horizon and resumed."
else
  echo "PASS: two real-game peers negotiated rollback and corrected injected late input."
fi
echo "Evidence retained in: $WORK"
