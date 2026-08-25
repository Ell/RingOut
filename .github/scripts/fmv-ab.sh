#!/bin/bash
# Re-measure the FMV paths now that the HLE actually engages.
#
# The old verdict in StaticRecompCore_Run.cpp ("takeover measured NOT faster,
# ~43 vs 47-60fps") compared two configurations that were BOTH running the
# game's software decoder, because the player could only find extracted .sfd
# files and no package had any. So it measured nothing.
#
# Two configurations, both reading movie0 from game/files/movie.afs:
#   guest     ordinary shipped path. The game performs MPEG decode and colour
#             conversion; no external player is armed and FFmpeg is not used.
#   takeover  STATICRECOMP_FMV_TAKEOVER. The MPEG decode is skipped entirely
#             and frame-ready state is synthesised, paced to 29.97fps.
#
# Metric is CPU-seconds consumed over a fixed wall-clock window during playback,
# plus the FPS the window title reports. Frame rate alone cannot answer this: at
# the 60fps cap every configuration may report the same number while consuming
# very different amounts of CPU, and headroom is the thing being bought.
set -u
P="${P:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
W="${1:-/tmp/fmv-ab}"
WINDOW="${2:-30}"     # measurement window, seconds
REPS="${3:-2}"

# Field 14 (utime) + 15 (stime) of /proc/<pid>/stat, in clock ticks. comm can
# contain spaces and parens, so everything up to the last ')' is dropped first.
cputicks() {
  local p="$1"
  [ -r "/proc/$p/stat" ] || { echo ""; return; }
  local rest; rest="$(sed 's/.*) //' "/proc/$p/stat" 2>/dev/null)" || { echo ""; return; }
  local u s
  u="$(echo "$rest" | cut -d' ' -f12)"
  s="$(echo "$rest" | cut -d' ' -f13)"
  echo "$((u + s))"
}

# Same, for the one thread that runs recompiled guest code.
cputhread() {
  local p="$1" t
  for t in /proc/$p/task/*/; do
    [ -r "$t/comm" ] || continue
    case "$(cat "$t/comm" 2>/dev/null)" in
    "CPU thread"|"CPU-GPU thread")
      local rest; rest="$(sed 's/.*) //' "$t/stat" 2>/dev/null)" || continue
      echo "$(( $(echo "$rest" | cut -d' ' -f12) + $(echo "$rest" | cut -d' ' -f13) ))"
      return
      ;;
    esac
  done
  echo ""
}

HZ="$(getconf CLK_TCK)"

run_one() {
  local tag="$1" rep="$2"
  local d="$W/$tag$rep"
  rm -rf "$d"; mkdir -p "$d/user/Config" "$d/user/Pipes"

  # The save file is mandatory: without it the game parks forever on "No
  # previous SOULCALIBUR II data found ... Press START to continue without
  # saving" and the run measures a stalled emulator.
  cp -r "$P/dist/RingOut-1.0-deck/userdata/GC" "$d/user/" 2>/dev/null || true
  cp "$P/dist/RingOut-1.0-deck/userdata/config.ini" "$d/user/" 2>/dev/null || true
  printf '[Input]\nBackgroundInput = True\n' > "$d/user/Config/Dolphin.ini"

  local -a env_extra=()
  case "$tag" in
    guest)    env_extra=() ;;
    takeover) env_extra=(STATICRECOMP_FMV_TAKEOVER=1) ;;
  esac

  # cd into the package so the loose work/fmv files are not visible and the
  # archive path is what gets exercised.
  #
  # NOT setsid, and the subshell execs: $! must be the emulator itself. Going
  # through setsid made $! the wrapper's pid, so teardown killed a process that
  # had already exited, every run stayed alive, and six emulators ended up
  # competing for the machine -- which is what the first attempt at this
  # measurement actually measured (59.9 -> 39.4 -> 27.1 -> 21.2 -> 12.5 -> 10.1
  # FPS across the sequence, i.e. pure contention).
  ( cd "$P/dist/RingOut-1.0-deck" && \
    exec env "${env_extra[@]}" ./bin/moderngekko-run --user-dir "$d/user" \
      --game ./game --module ./bin/gGRSEAF_recomp.so ) > "$d/log.txt" 2>&1 &
  local pid=$!
  echo "$pid" > "$d/pid"
  # The pid must name a live emulator or every sample below is fiction.
  sleep 2
  if [ -z "$(cputicks "$pid")" ]; then
    echo "$tag$rep: pid $pid is not readable in /proc -- aborting"
    return 1
  fi

  if [ "$tag" = takeover ]; then
    # The explicit HLE path logs its start, so synchronise precisely there.
    local waited=0
    while [ $waited -lt 90 ]; do
      grep -qa "mwPlyStartAfs" "$d/log.txt" 2>/dev/null && break
      kill -0 "$pid" 2>/dev/null || { echo "$tag$rep: died early"; return; }
      sleep 2; waited=$((waited + 2))
    done
    if ! grep -qa "mwPlyStartAfs" "$d/log.txt" 2>/dev/null; then
      echo "$tag$rep: no movie after ${waited}s"
      kill "$pid" 2>/dev/null; return
    fi
  else
    # Normal playback deliberately has no FMV-HLE log or dispatch hook. The
    # intro movie starts automatically; keep its boot delay explicit and
    # configurable instead of pretending an HLE marker exists in this arm.
    sleep "${GUEST_MOVIE_DELAY:-20}"
  fi
  sleep 5   # let playback settle before sampling

  local id; id="$(xdotool search --name "Ring Out" 2>/dev/null | head -1)"
  local t0 c0 h0; t0="$(cputicks "$pid")"; h0="$(cputhread "$pid")"
  local fps_sum=0 fps_n=0 elapsed=0
  while [ $elapsed -lt "$WINDOW" ]; do
    sleep 3; elapsed=$((elapsed + 3))
    local f
    f="$(xdotool getwindowname "$id" 2>/dev/null | grep -o '[0-9.]* FPS' | cut -d' ' -f1)"
    if [ -n "$f" ]; then
      fps_sum="$(awk -v a="$fps_sum" -v b="$f" 'BEGIN{printf "%.2f", a+b}')"
      fps_n=$((fps_n + 1))
    fi
  done
  local t1 h1; t1="$(cputicks "$pid")"; h1="$(cputhread "$pid")"

  local src; src="$(grep -a 'fmv-hle. player' "$d/log.txt" | sed 's/.*src=//' | head -1)"

  # Teardown that is actually verified. A run that outlives its measurement
  # poisons every run after it.
  kill "$pid" 2>/dev/null
  local w=0
  while kill -0 "$pid" 2>/dev/null && [ $w -lt 10 ]; do sleep 0.5; w=$((w + 1)); done
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  local leftover; leftover="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
  if [ "$leftover" != "0" ]; then
    echo "WARNING: $leftover emulator(s) still alive after $tag$rep -- results below are contended"
  fi

  local total cpu_thread mean_fps
  total="$(awk -v a="$t1" -v b="$t0" -v hz="$HZ" 'BEGIN{printf "%.2f", (a-b)/hz}')"
  cpu_thread="$(awk -v a="$h1" -v b="$h0" -v hz="$HZ" 'BEGIN{printf "%.2f", (a-b)/hz}')"
  if [ "$fps_n" -gt 0 ]; then
    mean_fps="$(awk -v s="$fps_sum" -v n="$fps_n" 'BEGIN{printf "%.1f", s/n}')"
  else
    mean_fps="n/a"
  fi
  printf '%-9s rep%d  cpu_total=%6ss  cpu_thread=%6ss  fps=%5s  src=%s\n' \
    "$tag" "$rep" "$total" "$cpu_thread" "$mean_fps" "${src:-<none: guest decoder>}"
}

# Refuse to start dirty: a leftover emulator from an earlier run would make the
# first configuration measured look worse than it is.
pre="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
if [ "$pre" != "0" ]; then
  echo "ABORT: $pre emulator(s) already running; kill them first (pkill -9 -x moderngekko-run)"
  exit 1
fi

echo "window=${WINDOW}s reps=$REPS HZ=$HZ load=$(cut -d' ' -f1-3 /proc/loadavg)"
for rep in $(seq 1 "$REPS"); do
  for tag in guest takeover; do
    run_one "$tag" "$rep"
  done
done
