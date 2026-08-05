#!/bin/bash
# Run two local netplay instances against each other and report whether they
# stayed in sync.
#
# $1 = work dir   $2 = seconds to stay in-game (default 60)   $3 = port
#
# Both peers get:
#   * the memory card, or the game parks on "No previous SOULCALIBUR II data
#     found ..." forever and the session measures a stalled emulator;
#   * BackgroundInput=True, since Dolphin drops all input -- pipe included --
#     when the render window lacks focus, and neither of these has focus;
#   * a Pipe pad device with a writer held open for the session, because
#     Dolphin opens the FIFO once and never recovers from EOF.
# See the "driving the game" notes: each of those alone is enough to make a
# session look broken for reasons unrelated to netplay.
set -u
P=/mnt/hera/projects/soulcalibur
W="${1:-/tmp/netplay-local}"
PLAY="${2:-60}"
PORT="${3:-2626}"
PKG="$P/dist/RingOut-1.0-deck"

pre="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
if [ "$pre" != "0" ]; then
  echo "ABORT: $pre emulator(s) already running (pkill -9 -x moderngekko-run)"
  exit 1
fi

rm -rf "$W"; mkdir -p "$W"

setup_peer() {
  local name="$1"
  local d="$W/$name"
  mkdir -p "$d/user/Config" "$d/user/Pipes"
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
  printf '[Input]\nBackgroundInput = True\n' > "$d/user/Config/Dolphin.ini"
  setsid bash -c 'exec 9<>"'"$d"'/user/Pipes/ctrl"; exec sleep 7200' \
    >/dev/null 2>&1 &
  echo $! > "$d/holder.pid"
}

setup_peer host
setup_peer guest

# HASH=1 additionally runs the determinism harness on both peers and diffs the
# per-frame guest-RAM hashes afterwards. Dolphin's own desync detection compares
# only the emulated timebase, once every 60 frames; this compares all of RAM
# every frame, so it is the difference between "the clocks agree" and "the two
# machines are in the same state". It costs a 24 MB hash per frame, hence opt-in.
launch() {
  local name="$1"; shift
  local d="$W/$name"
  local -a hash_env=()
  if [ "${HASH:-0}" = "1" ]; then
    hash_env=(RINGOUT_DETERMINISM_LOG="$d/hash.log")
  fi
  ( cd "$PKG" && exec env "${hash_env[@]}" ./bin/moderngekko-run --headless \
      --user-dir "$d/user" --game ./game --module ./bin/gGRSEAF_recomp.so \
      --controller "Standard Controller" "$@" ) > "$d/log.txt" 2>&1 &
  echo $! > "$d/pid"
  echo "$name pid=$(cat "$d/pid")"
}

launch host  --netplay-host --netplay-port "$PORT" --netplay-players 2 \
             --nickname Host  --buffer 5 --netplay-timeout 90
sleep 4
launch guest --netplay-join 127.0.0.1 --netplay-port "$PORT" \
             --nickname Guest --netplay-timeout 90

# Wait for both to report booting, then let them run.
# "netplay armed" is the load-bearing string, not "booting". NetPlay_Enable
# happens inside NetPlayClient::StartGame; without it SI reads local pads and
# desync detection never arms, so two independent single-player sessions run to
# completion and report no desync -- a clean-looking pass that checked nothing.
waited=0
while [ $waited -lt 100 ]; do
  if grep -qa "netplay armed" "$W/host/log.txt" 2>/dev/null && \
     grep -qa "netplay armed" "$W/guest/log.txt" 2>/dev/null; then
    echo "both peers booted after ${waited}s"
    break
  fi
  # Bail out early on a lobby failure rather than burning the whole timeout.
  if grep -qa "no start signal\|timed out\|refusing to start\|could not connect\|did not arm\|refused to start" \
       "$W/host/log.txt" "$W/guest/log.txt" 2>/dev/null; then
    echo "lobby failed:"
    grep -ha "netplay:" "$W/host/log.txt" "$W/guest/log.txt" | tail -12
    break
  fi
  sleep 2; waited=$((waited + 2))
done

if grep -qa "netplay armed" "$W/host/log.txt" 2>/dev/null; then
  echo "running for ${PLAY}s ..."
  slept=0
  while [ $slept -lt "$PLAY" ]; do
    sleep 5; slept=$((slept + 5))
    if grep -qa "DESYNC" "$W/host/log.txt" "$W/guest/log.txt" 2>/dev/null; then
      echo "desync detected after ${slept}s in-game"
      break
    fi
  done
fi

for name in host guest; do
  d="$W/$name"
  [ -f "$d/pid" ] && kill "$(cat "$d/pid")" 2>/dev/null
done
sleep 2
for name in host guest; do
  d="$W/$name"
  [ -f "$d/pid" ] && kill -9 "$(cat "$d/pid")" 2>/dev/null
  [ -f "$d/holder.pid" ] && kill -9 "$(cat "$d/holder.pid")" 2>/dev/null
done
sleep 1
left="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
[ "$left" != "0" ] && echo "WARNING: $left emulator(s) still alive"

echo
echo "================ HOST ================"
grep -a "netplay:\|fmv-hle\|staticrecomp. shutdown" "$W/host/log.txt" 2>/dev/null
echo "================ GUEST ==============="
grep -a "netplay:\|fmv-hle\|staticrecomp. shutdown" "$W/guest/log.txt" 2>/dev/null
echo "======================================"
if [ "${HASH:-0}" = "1" ] && [ -s "$W/host/hash.log" ] && [ -s "$W/guest/hash.log" ]; then
  # Compare only the frames both peers reached; one is always killed a moment
  # before the other, and a length difference is not a state difference.
  n=$(( $(wc -l < "$W/host/hash.log") < $(wc -l < "$W/guest/hash.log") \
        ? $(wc -l < "$W/host/hash.log") : $(wc -l < "$W/guest/hash.log") ))
  head -n "$n" "$W/host/hash.log"  > "$W/host.trim"
  head -n "$n" "$W/guest/hash.log" > "$W/guest.trim"
  echo "guest-RAM hash comparison over $n frames:"
  if diff -q "$W/host.trim" "$W/guest.trim" >/dev/null; then
    echo "  IDENTICAL on every frame"
  else
    echo "  FIRST DIVERGENCE:"
    diff "$W/host.trim" "$W/guest.trim" | head -4
  fi
fi

if grep -qa "DESYNC" "$W/host/log.txt" "$W/guest/log.txt" 2>/dev/null; then
  echo "RESULT: DESYNCED"
elif grep -qa "netplay armed" "$W/host/log.txt" 2>/dev/null && \
     grep -qa "netplay armed" "$W/guest/log.txt" 2>/dev/null; then
  echo "RESULT: both peers ran netplay-armed with no desync reported"
else
  echo "RESULT: session did not start"
fi
