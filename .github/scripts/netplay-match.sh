#!/bin/bash
# Drive two netplay peers all the way into a VS Battle with scripted input, then
# verify they stayed in sync through it.
#
# This is the test netplay-local.sh cannot be: that one only proves boot, the
# intro and menus with no input at all. Character select is the first screen
# where BOTH peers must contribute input, so it is the first real exercise of
# input propagation in each direction.
#
# $1 = work dir   $2 = seconds to stay in the match (default 60)   $3 = port
#
# The menu route was found by screenshotting a live session:
#   START xN            skip the intro movie -> MODE SELECT
#   A                   enter the Original mode list (Arcade highlighted)
#   D_DOWN, A           VS Battle -> CHARACTER SELECT
#   P1 A, P2 A          lock both characters
#   P1 A, P2 A          confirm health
#   P2 A                stage select is 2P's choice
# VS Battle is greyed out unless a second controller is present, so reaching it
# at all depends on netplay's pad 2 being live.
set -u
P="${P:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
W="${1:-/tmp/netplay-match}"
PLAY="${2:-60}"
PORT="${3:-2640}"
PKG="${PKG:-$P/dist/RingOut-1.0-deck}"
MIN_HASH_FRAMES="${MIN_HASH_FRAMES:-300}"
result=0

cleanup() {
  local n pid
  set +e
  for n in host guest; do
    if [ -f "$W/$n/pid" ]; then
      read -r pid < "$W/$n/pid"
      [[ "$pid" =~ ^[1-9][0-9]*$ ]] && kill "$pid" 2>/dev/null
    fi
  done
  sleep 2
  for n in host guest; do
    for pid_file in "$W/$n/pid" "$W/$n/holder.pid"; do
      if [ -f "$pid_file" ]; then
        read -r pid < "$pid_file"
        [[ "$pid" =~ ^[1-9][0-9]*$ ]] && kill -9 "$pid" 2>/dev/null
        [[ "$pid" =~ ^[1-9][0-9]*$ ]] && wait "$pid" 2>/dev/null
      fi
    done
  done
}

trap cleanup EXIT
trap 'exit 130' INT TERM

pre="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
if [ "$pre" != "0" ]; then
  echo "ABORT: $pre emulator(s) already running (pkill -9 -x moderngekko-run)"
  exit 1
fi

rm -rf "$W"; mkdir -p "$W"

setup_peer() {
  local d="$W/$1"
  mkdir -p "$d/user/Config" "$d/user/Pipes"
  # Mandatory: without save data the game parks forever on the memory-card
  # dialog and nothing below happens.
  cp -r "$PKG/userdata/GC" "$d/user/" 2>/dev/null || true
  mkfifo "$d/user/Pipes/ctrl"
  cat > "$d/user/Config/GCPadNew.ini" <<'EOF'
[GCPad1]
Device = Pipe/0/ctrl
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/X = `Button X`
Buttons/Y = `Button Y`
Buttons/Z = `Button Z`
Buttons/Start = `Button START`
D-Pad/Up = `Button D_UP`
D-Pad/Down = `Button D_DOWN`
D-Pad/Left = `Button D_LEFT`
D-Pad/Right = `Button D_RIGHT`
Triggers/L = `Button L`
Triggers/R = `Button R`
Main Stick/Up = `Axis MAIN Y -`
Main Stick/Down = `Axis MAIN Y +`
Main Stick/Left = `Axis MAIN X -`
Main Stick/Right = `Axis MAIN X +`
EOF
  # A writer must stay open for the whole session: Dolphin opens the FIFO once
  # and never recovers from EOF.
  setsid bash -c 'exec 9<>"'"$d"'/user/Pipes/ctrl"; exec sleep 7200' \
    >/dev/null 2>&1 &
  echo $! > "$d/holder.pid"
}

setup_peer host
setup_peer guest

ensure_peers_alive() {
  local n pid
  for n in host guest; do
    [ -s "$W/$n/pid" ] || return 1
    read -r pid < "$W/$n/pid"
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
  done
}

gpu_fifo_failure_seen() {
  grep -Eqa 'GFX FIFO|Unknown Opcode|FIFOs linked but out of sync|Desynced read pointers|Aux FIFO|Negative fifo\.|FIFO out of bounds|SIG(SEGV|ILL|ABRT)|AddressSanitizer' \
    "$W"/*/log.txt 2>/dev/null
}

# WINDOWED=1 shows both peers, so the run can be confirmed by looking at it.
# Headless proves the two peers agree with each other, which is NOT the same as
# proving they reached a match -- two peers agreeing on a menu screen would look
# identical in the hash comparison.
launch() {
  local name="$1"; shift
  local d="$W/$name"
  local -a mode=(--headless)
  local -a rollback_test_env=()
  local -a rollback_mode_args=()
  local -a rollback_oracle_env=()
  local -a diagnostic_args=()
  local -a hook_profile_env=()
  local -a determinism_dump_env=()
  [ "${WINDOWED:-0}" = "1" ] && mode=()
  [ "${RINGOUT_NETPLAY_DIAGNOSTICS:-0}" = "1" ] &&
    diagnostic_args=(--netplay-diagnostics)
  if [ -n "${RINGOUT_SC2_HOOK_PROFILE_ARM_ROUTE:-}" ]; then
    hook_profile_env=("RINGOUT_SC2_HOOK_PROFILE_ARM_FILE=$d/hook-profile.arm")
  fi
  if [ -n "${RINGOUT_DETERMINISM_DUMP_DIR:-}" ]; then
    [ -n "${RINGOUT_DETERMINISM_DUMP_FRAME:-}" ] || {
      echo "RINGOUT_DETERMINISM_DUMP_FRAME is required with RINGOUT_DETERMINISM_DUMP_DIR" >&2
      return 1
    }
    mkdir -p "$RINGOUT_DETERMINISM_DUMP_DIR"
    determinism_dump_env=(
      "RINGOUT_DETERMINISM_DUMP=$RINGOUT_DETERMINISM_DUMP_DIR/$name.mem1.bin"
      "RINGOUT_ROLLBACK_CONFIRMED_DUMP=$RINGOUT_DETERMINISM_DUMP_DIR/$name.confirmed.mem1.bin"
      "RINGOUT_ROLLBACK_CONFIRMED_DUMP_FRAME=$RINGOUT_DETERMINISM_DUMP_FRAME"
      "RINGOUT_ROLLBACK_MISMATCH_DUMP=$RINGOUT_DETERMINISM_DUMP_DIR/$name.mismatch.mem1.bin")
  fi
  if [ "${RINGOUT_ROLLBACK_PRODUCTION:-0}" = "1" ] ||
     [ -n "${RINGOUT_ROLLBACK_FAULT_SCRIPT:-}" ] ||
     [ -n "${RINGOUT_EXPECT_DIGEST_MISMATCH_FRAME:-}" ]; then
    # Every rollback harness route requests rollback through the same explicit
    # frontend mode as a player. The isolated acknowledgement chooses only the
    # disposable output gate and numeric overrides; it must not make a
    # nominally fixed-delay session turn into rollback behind the UI's back.
    rollback_mode_args=(--netplay-mode rollback)
    rollback_oracle_env=("RINGOUT_ROLLBACK_CONFIRMED_LOG=$d/confirmed-state.log")
  fi
  if [ -n "${RINGOUT_ROLLBACK_FAULT_SCRIPT:-}" ]; then
    rollback_oracle_env=("RINGOUT_ROLLBACK_CONFIRMED_LOG=$d/confirmed-state.log")
    if [ "${RINGOUT_ROLLBACK_PRODUCTION:-0}" = "1" ]; then
      # This acknowledgement enables only deterministic packet impairment.
      # It deliberately does not select the isolated output gate: StartGame
      # still constructs the same production gate used by ordinary players.
      rollback_test_env=("RINGOUT_ROLLBACK_FAULT_ACK=PRODUCTION_OUTPUT_GATE")
    else
      rollback_test_env=("RINGOUT_ROLLBACK_TEST_ACK=HEADLESS_ISOLATED")
    fi
    rollback_test_env+=(
      "RINGOUT_ROLLBACK_BASE_DELAY_SAMPLES=${RINGOUT_ROLLBACK_BASE_DELAY_SAMPLES:-2}"
      "RINGOUT_ROLLBACK_HORIZON_FRAMES=${RINGOUT_ROLLBACK_HORIZON_FRAMES:-8}")
    if [ "$name" = host ]; then
      # The scripted route changes host input immediately while the guest's
      # first button press is much later at character select. Impairing host
      # rollback packets therefore guarantees that a bounded early window can
      # carry a real input edge and exercise correction on the guest.
      rollback_test_env+=("RINGOUT_ROLLBACK_FAULT_SCRIPT=$RINGOUT_ROLLBACK_FAULT_SCRIPT")
      if [ "${RINGOUT_ROLLBACK_FAULT_ARM_AT_HOOK:-0}" = "1" ]; then
        rollback_test_env+=("RINGOUT_ROLLBACK_FAULT_ARM_FILE=$d/hook-profile.arm")
      fi
    fi
  fi
  if [ -n "${RINGOUT_EXPECT_DIGEST_MISMATCH_FRAME:-}" ]; then
    rollback_test_env+=("RINGOUT_ROLLBACK_TEST_ACK=HEADLESS_ISOLATED")
    if [ "$name" = guest ]; then
      rollback_test_env+=(
        "RINGOUT_ROLLBACK_DIGEST_FAULT_FRAME=$RINGOUT_EXPECT_DIGEST_MISMATCH_FRAME")
    fi
  fi
  # The orchestration variables are exported to this script, but injection is
  # deliberately one-sided. Remove any inherited per-process hook before
  # adding it back through rollback_test_env for exactly one peer only.
  ( cd "$PKG" && exec env \
      -u RINGOUT_ROLLBACK_FAULT_SCRIPT \
      -u RINGOUT_ROLLBACK_DIGEST_FAULT_FRAME \
      RINGOUT_DETERMINISM_LOG="$d/hash.log" \
      "${hook_profile_env[@]}" \
      "${determinism_dump_env[@]}" \
      "${rollback_oracle_env[@]}" \
      "${rollback_test_env[@]}" \
      ./bin/moderngekko-run "${mode[@]}" --user-dir "$d/user" --game ./game \
      --module ./bin/gGRSEAF_recomp.so --controller "Standard Controller" \
      "${rollback_mode_args[@]}" "${diagnostic_args[@]}" "$@" ) \
      > "$d/log.txt" 2>&1 &
  echo $! > "$d/pid"
}

match_buffer=5
if [ -n "${RINGOUT_ROLLBACK_FAULT_SCRIPT:-}" ] ||
   [ -n "${RINGOUT_EXPECT_DIGEST_MISMATCH_FRAME:-}" ]; then
  # Pass the same requested delay through the ordinary player CLI even when an
  # isolated test subsequently applies the explicit numeric test override.
  match_buffer="${RINGOUT_ROLLBACK_BASE_DELAY_SAMPLES:-2}"
fi
traversal_args=()
if [ "${RINGOUT_NETPLAY_TRAVERSAL:-0}" = "1" ]; then
  traversal_args=(--netplay-traversal)
  [ -n "${RINGOUT_TRAVERSAL_SERVER:-}" ] &&
    traversal_args+=(--traversal-server "$RINGOUT_TRAVERSAL_SERVER")
  [ -n "${RINGOUT_TRAVERSAL_PORT:-}" ] &&
    traversal_args+=(--traversal-port "$RINGOUT_TRAVERSAL_PORT")
  [ -n "${RINGOUT_TRAVERSAL_ALT_PORT:-}" ] &&
    traversal_args+=(--traversal-alt-port "$RINGOUT_TRAVERSAL_ALT_PORT")
fi

launch host --netplay-host --netplay-port "$PORT" --netplay-players 2 \
            --nickname Host --buffer "$match_buffer" --netplay-timeout 120 \
            "${traversal_args[@]}"

if [ "${RINGOUT_NETPLAY_TRAVERSAL:-0}" = "1" ]; then
  room_code=""
  host_ready=0
  waited_code=0
  while [ "$waited_code" -lt 30 ]; do
    room_code=$(sed -n 's/.*netplay: online room code \([0-9a-f]\{8\}\).*/\1/p' \
      "$W/host/log.txt" 2>/dev/null | tail -1)
    if [ -n "$room_code" ] &&
       grep -Fqa "netplay: connected as 'Host'" "$W/host/log.txt"; then
      host_ready=1
      break
    fi
    if ! [ -s "$W/host/pid" ] ||
       ! kill -0 "$(<"$W/host/pid")" 2>/dev/null; then
      echo "host exited before Dolphin assigned a room code"
      exit 1
    fi
    sleep 1
    waited_code=$((waited_code + 1))
  done
  if [ -z "$room_code" ] || [ "$host_ready" -ne 1 ]; then
    echo "host did not finish creating the Dolphin online room"
    grep -ha 'netplay:' "$W/host/log.txt" | tail -12
    exit 1
  fi
  echo "Dolphin hosted room assigned after ${waited_code}s"
  launch guest --netplay-join "$room_code" --netplay-port "$PORT" \
               --nickname Guest --netplay-timeout 120 \
               "${traversal_args[@]}"
else
  sleep 4
  launch guest --netplay-join 127.0.0.1 --netplay-port "$PORT" \
               --nickname Guest --netplay-timeout 120
fi

waited=0
while [ $waited -lt 120 ]; do
  grep -qa "netplay armed" "$W/host/log.txt" 2>/dev/null && \
  grep -qa "netplay armed" "$W/guest/log.txt" 2>/dev/null && break
  ensure_peers_alive || {
    echo "a netplay peer exited before the session armed"
    grep -ha 'netplay:' "$W"/*/log.txt | tail -16
    exit 1
  }
  sleep 2; waited=$((waited + 2))
done
if ! grep -qa "netplay armed" "$W/host/log.txt" 2>/dev/null || \
   ! grep -qa "netplay armed" "$W/guest/log.txt" 2>/dev/null; then
  echo "peers never armed"; grep -ha "netplay:" "$W"/*/log.txt | tail -8; exit 1
fi
echo "both peers armed after ${waited}s"

if [ "${RINGOUT_NETPLAY_TRAVERSAL:-0}" = "1" ]; then
  unique_room_codes=$(sed -n \
    's/.*netplay: online room code \([0-9a-f]\{8\}\).*/\1/p' \
    "$W/host/log.txt" | sort -u)
  [ "$(printf '%s\n' "$unique_room_codes" | sed '/^$/d' | wc -l)" -eq 1 ] || {
    echo "host did not retain exactly one traversal room code"
    exit 1
  }
  grep -Fqa "netplay: registering online room with" "$W/host/log.txt" || {
    echo "host did not use the traversal path"
    exit 1
  }
  grep -Fqa "netplay: joining online room $room_code" "$W/guest/log.txt" || {
    echo "guest did not join the host's traversal code"
    exit 1
  }
  if grep -Fqa "netplay: connecting to" "$W/guest/log.txt"; then
    echo "guest unexpectedly used the Direct IP path"
    exit 1
  fi
  echo "hosted traversal provenance verified"
fi

# `netplay armed` means the synchronized StartGame handshake completed. Live
# rollback is not actually exercising snapshots, prediction, or resimulation
# until the CPU reaches the first frame boundary and both clients report the
# active scheduler. Waiting here prevents a slow or wedged boot from being
# mistaken for a successful rollback route while the input script blindly
# advances on wall-clock time.
if [ "${RINGOUT_ROLLBACK_PRODUCTION:-0}" = "1" ] ||
   [ -n "${RINGOUT_ROLLBACK_FAULT_SCRIPT:-}" ] ||
   [ -n "${RINGOUT_EXPECT_DIGEST_MISMATCH_FRAME:-}" ]; then
  waited_active=0
  while [ "$waited_active" -lt 120 ]; do
    grep -qa '\[rollback live\] active' "$W/host/log.txt" 2>/dev/null &&
    grep -qa '\[rollback live\] active' "$W/guest/log.txt" 2>/dev/null && break
    ensure_peers_alive || {
      echo "a netplay peer exited before live rollback activated"
      exit 1
    }
    sleep 2
    waited_active=$((waited_active + 2))
  done
  if ! grep -qa '\[rollback live\] active' "$W/host/log.txt" 2>/dev/null ||
     ! grep -qa '\[rollback live\] active' "$W/guest/log.txt" 2>/dev/null; then
    echo "live rollback never activated"
    grep -ha '\[rollback live\]\|netplay:' "$W"/*/log.txt | tail -16
    exit 1
  fi
  echo "live rollback active on both peers after ${waited_active}s"
  if [ -n "${RINGOUT_ROLLBACK_DUALCORE:-}" ]; then
    threading_marker='netplay: dual-core rollback experiment with a deterministic GPU thread'
    barrier_marker='gpu_transaction_barrier=dual-core-quiesced'
  else
    threading_marker='netplay: single-core (rollback-safe default)'
    barrier_marker='gpu_transaction_barrier=single-core-quiesced'
  fi
  grep -Fqa "$threading_marker" "$W/host/log.txt" || {
    echo "host did not use the requested rollback threading policy"
    exit 1
  }
  grep -Fqa "$threading_marker" "$W/guest/log.txt" || {
    echo "guest did not use the requested rollback threading policy"
    exit 1
  }
  grep -Fqa "$barrier_marker" "$W/host/log.txt" || {
    echo "host did not checkpoint behind the rollback GPU transaction barrier"
    exit 1
  }
  grep -Fqa "$barrier_marker" "$W/guest/log.txt" || {
    echo "guest did not checkpoint behind the rollback GPU transaction barrier"
    exit 1
  }
  echo "rollback threading policy verified: $threading_marker"
fi

if [ -n "${RINGOUT_EXPECT_DIGEST_MISMATCH_FRAME:-}" ]; then
  digest_frame="$RINGOUT_EXPECT_DIGEST_MISMATCH_FRAME"
  waited_digest=0
  while [ "$waited_digest" -lt 120 ]; do
    if grep -Fqa "DESYNC at frame $digest_frame" "$W/host/log.txt" 2>/dev/null &&
       grep -Fqa "DESYNC at frame $digest_frame" "$W/guest/log.txt" 2>/dev/null; then
      break
    fi
    sleep 1
    waited_digest=$((waited_digest + 1))
  done
  cleanup
  trap - EXIT INT TERM
  if ! grep -Fqa "isolated digest fault injected at frame $digest_frame" \
       "$W/guest/log.txt" 2>/dev/null; then
    echo "FAIL: guest did not inject the acknowledged digest fault"
    exit 1
  fi
  if ! grep -Fqa "DESYNC at frame $digest_frame" "$W/host/log.txt" 2>/dev/null ||
     ! grep -Fqa "DESYNC at frame $digest_frame" "$W/guest/log.txt" 2>/dev/null; then
    echo "FAIL: both peers did not report the expected digest mismatch"
    grep -ha 'DESYNC\|digest\|rollback live' "$W"/*/log.txt | tail -24
    exit 1
  fi
  echo "RESULT: both peers rejected the injected digest mismatch at frame $digest_frame"
  exit 0
fi

press() {  # $1 = host|guest  $2 = button  [$3 = settle seconds]
  local p="$W/$1/user/Pipes/ctrl"
  ensure_peers_alive || { echo "a netplay peer exited before the route completed"; exit 1; }
  printf 'PRESS %s\n' "$2" > "$p"; sleep 0.25
  printf 'RELEASE %s\n' "$2" > "$p"; sleep "${3:-1.2}"
}

arm_hook_profile() {
  [ -n "${RINGOUT_SC2_HOOK_PROFILE_ARM_ROUTE:-}" ] || return 0
  touch "$W/host/hook-profile.arm" "$W/guest/hook-profile.arm"
  echo "SC2 hook profile armed for ${RINGOUT_SC2_HOOK_PROFILE_ARM_ROUTE}"
}

if [ "${RINGOUT_NETPLAY_IDLE_ROUTE:-0}" = "1" ]; then
  # Research control for dispatch-profile subtraction. Both peers remain in
  # the synchronized intro/menu path with the same two-controller mapping,
  # rollback policy, and runtime configuration as the gameplay route. This is
  # deliberately env-gated and never selected by players.
  arm_hook_profile
  echo "leaving both peers idle for ${PLAY}s ..."
  sleep "$PLAY"
else
  # The intro movie plays automatically and is long; press START until the game
  # leaves it. Extra presses on MODE SELECT are harmless (START is not Confirm).
  echo "skipping intro ..."
  for i in $(seq 1 25); do press host START 1.0; done

  echo "MODE SELECT -> VS Battle ..."
  press host A 1.5          # enter the Original mode list
  press host D_DOWN 1.2     # Arcade -> VS Battle
  press host A 3.0          # -> CHARACTER SELECT

  # Character select, health and stage are all "A" on one side or the other, and
  # each screen has an entry animation that eats input arriving too soon. Settles
  # are deliberately generous: the earlier version used 1.5s and the presses were
  # swallowed, leaving both peers sitting at character select in perfect sync.
  echo "character select -> health -> stage ..."
  for round in 1 2 3; do
    press host  A 3.0       # lock 1P / confirm 1P health
    press guest A 3.0       # lock 2P / confirm 2P health / pick stage
  done

  echo "loading match, then playing for ${PLAY}s ..."
  sleep 20
  arm_hook_profile
  slept=0
  while [ $slept -lt "$PLAY" ]; do
    # Trade attacks so both peers actually contribute input during gameplay.
    #
    # NEVER press B here. In this game's menus B is Back, so a loop alternating A
    # and B locks and immediately unlocks the character -- which is exactly how
    # the first version of this script got stuck at character select forever while
    # both peers stayed byte-identical and reported no desync. A and X are safe:
    # attacks in a match, and harmless confirms if a menu is somehow still up.
    press host  A 0.5
    press guest X 0.5
    press host  X 0.5
    press guest A 0.5
    slept=$((slept + 4))
    if gpu_fifo_failure_seen; then
      echo "GPU/FIFO consistency failure after ${slept}s of play"
      break
    fi
    if grep -qa "DESYNC" "$W"/*/log.txt 2>/dev/null; then
      echo "DESYNC after ${slept}s of play"; break
    fi
  done
fi

if ! ensure_peers_alive; then
  echo "FAIL: a netplay peer exited before intentional shutdown"
  result=1
fi
cleanup
trap - EXIT INT TERM
sleep 1
left="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
if [ "$left" != "0" ]; then
  echo "FAIL: $left emulator(s) still alive"
  result=1
fi

echo
grep -ha "netplay: pad map\|netplay armed\|DESYNC" "$W"/*/log.txt
if [ -s "$W/host/hash.log" ] && [ -s "$W/guest/hash.log" ]; then
  n=$(( $(wc -l < "$W/host/hash.log") < $(wc -l < "$W/guest/hash.log") \
        ? $(wc -l < "$W/host/hash.log") : $(wc -l < "$W/guest/hash.log") ))
  head -n "$n" "$W/host/hash.log"  > "$W/h.trim"
  head -n "$n" "$W/guest/hash.log" > "$W/g.trim"
  echo "guest-RAM hash over $n frames:"
  if [ "$n" -lt "$MIN_HASH_FRAMES" ]; then
    echo "  FAIL: expected at least $MIN_HASH_FRAMES comparable frames"
    result=1
  elif [ -n "${RINGOUT_ROLLBACK_FAULT_SCRIPT:-}" ] || \
       [ "${RINGOUT_ROLLBACK_PRODUCTION:-0}" = "1" ]; then
    # Rollback deliberately creates speculative physical-frame rows and then
    # replays corrected logical frames, so the legacy line-for-line trace is
    # not a valid oracle in fault mode.  Keep both traces as evidence; the
    # rollback wrapper checks negotiated/correction markers and Dolphin's live
    # desync detector.  Non-fault fixed-delay coverage remains byte-exact.
    echo "  retained (rollback physical-frame rows are not the logical-state oracle)"
  elif diff -q "$W/h.trim" "$W/g.trim" >/dev/null; then
    echo "  IDENTICAL on every frame"
  else
    echo "  FIRST DIVERGENCE:"; diff "$W/h.trim" "$W/g.trim" | head -4
    result=1
  fi
else
  echo "FAIL: one or both hash logs are empty"
  result=1
fi

if grep -qa "DESYNC" "$W"/*/log.txt 2>/dev/null; then
  result=1
fi
if gpu_fifo_failure_seen; then
  echo "FAIL: a peer reported a GPU/FIFO consistency failure"
  grep -Eha 'GFX FIFO|Unknown Opcode|FIFOs linked but out of sync|Desynced read pointers|Aux FIFO|Negative fifo\.|FIFO out of bounds|SIG(SEGV|ILL|ABRT)|AddressSanitizer' \
    "$W"/*/log.txt | tail -20
  result=1
fi

if [ "$result" -eq 0 ]; then
  echo "RESULT: synchronized match route completed"
else
  echo "RESULT: match route failed"
fi
exit "$result"
