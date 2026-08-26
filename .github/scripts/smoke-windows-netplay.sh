#!/usr/bin/env bash
# Asset-private Windows release smoke: build the real module with the exact ZIP,
# then run two packaged Windows peers through a live rollback session under Wine.
#
# The repository and release stay game-data-free. CI may call this only when a
# legally supplied extracted game directory is available out of tree.
# Set RINGOUT_WINDOWS_NETPLAY_MODULE to a previously built Windows module when
# rerunning only the peer/session half after retaining an earlier work tree.
set -euo pipefail

ZIP="${1:?usage: smoke-windows-netplay.sh <windows-package.zip> <extracted-game> [empty-work-dir]}"
GAME="${2:?usage: smoke-windows-netplay.sh <windows-package.zip> <extracted-game> [empty-work-dir]}"
REQUESTED_WORK="${3:-}"
ROUTES="${RINGOUT_WINDOWS_NETPLAY_ROUTES:-direct}"
RUN_SECONDS="${RINGOUT_WINDOWS_NETPLAY_SECONDS:-8}"
BASE_PORT="${RINGOUT_WINDOWS_NETPLAY_PORT:-29620}"

die() {
  printf 'windows netplay smoke: %s\n' "$*" >&2
  exit 1
}

for command_name in find grep realpath sed sha256sum unzip; do
  command -v "$command_name" >/dev/null 2>&1 || \
    die "required command missing: $command_name"
done
[[ -f "$ZIP" ]] || die "package not found: $ZIP"
[[ -s "$GAME/sys/boot.bin" && -s "$GAME/sys/main.dol" && -d "$GAME/files" ]] || \
  die "expected an extracted GameCube game with sys/boot.bin, sys/main.dol, and files/: $GAME"
[[ "$RUN_SECONDS" =~ ^[1-9][0-9]*$ ]] || die "invalid run duration: $RUN_SECONDS"
[[ "$BASE_PORT" =~ ^[1-9][0-9]*$ ]] && ((BASE_PORT <= 65534)) || \
  die "invalid base port: $BASE_PORT"

if command -v wine >/dev/null 2>&1; then
  WINE=(wine)
elif command -v wine64 >/dev/null 2>&1; then
  WINE=(wine64)
elif command -v wine-stable >/dev/null 2>&1; then
  WINE=(wine-stable)
elif command -v wine64-stable >/dev/null 2>&1; then
  WINE=(wine64-stable)
else
  die "Wine is required"
fi

ZIP="$(realpath "$ZIP")"
GAME="$(realpath "$GAME")"
if [[ -n "$REQUESTED_WORK" ]]; then
  WORK="$(realpath -m "$REQUESTED_WORK")"
  mkdir -p "$WORK"
  [[ -z "$(find "$WORK" -mindepth 1 -maxdepth 1 -print -quit)" ]] || \
    die "work directory is not empty: $WORK"
else
  WORK="$(mktemp -d "${TMPDIR:-/tmp}/ringout-windows-netplay.XXXXXXXX")"
fi

EXTRACT="$WORK/extracted package"
PREFIX="$WORK/wine-prefix"
BUILD_OUTPUT="$WORK/module output"
mkdir -p "$EXTRACT" "$PREFIX"
export WINEPREFIX="$PREFIX"
export WINEARCH=win64
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"

windows_path() {
  local absolute
  absolute="$(realpath -m "$1")"
  printf 'Z:%s\n' "${absolute//\//\\}"
}

cleanup() {
  local pid
  set +e
  for pid_file in "$WORK"/*/host/pid "$WORK"/*/guest/pid; do
    [[ -f "$pid_file" ]] || continue
    read -r pid <"$pid_file"
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] && kill "$pid" 2>/dev/null
  done
  sleep 1
  if command -v wineserver >/dev/null 2>&1; then
    wineserver -k >/dev/null 2>&1
    wineserver -w >/dev/null 2>&1
  elif command -v wineserver-stable >/dev/null 2>&1; then
    wineserver-stable -k >/dev/null 2>&1
    wineserver-stable -w >/dev/null 2>&1
  fi
}
trap cleanup EXIT INT TERM

printf '==> extracting and verifying exact Windows ZIP\n'
unzip -q "$ZIP" -d "$EXTRACT"
shopt -s nullglob
package_roots=("$EXTRACT"/RingOut-*-windows-x86_64)
shopt -u nullglob
[[ ${#package_roots[@]} -eq 1 && -d "${package_roots[0]}" ]] || \
  die "expected one RingOut Windows package root"
PACKAGE="$(realpath "${package_roots[0]}")"
(
  cd "$PACKAGE"
  sha256sum -c MANIFEST.sha256 >/dev/null
)

PORT="$PACKAGE/tools/moderngekko-port.exe"
RUNTIME="$PACKAGE/bin/moderngekko-run.exe"
[[ -s "$PORT" && -s "$RUNTIME" ]] || die "package is missing its setup helper or runtime"

if [[ -n "${RINGOUT_WINDOWS_NETPLAY_MODULE:-}" ]]; then
  MODULE="$(realpath "$RINGOUT_WINDOWS_NETPLAY_MODULE")"
  [[ -s "$MODULE" && "${MODULE##*/}" == gGRSEAF_recomp.dll ]] || \
    die "prebuilt Windows module is missing or misnamed: $MODULE"
  printf '==> reusing retained real Windows module: %s\n' "$MODULE"
else
  printf '==> building real Windows module through the packaged setup helper\n'
  PORT_LOG="$WORK/module-build.log"
  "${WINE[@]}" "$PORT" build "$(windows_path "$GAME")" \
    --output "$(windows_path "$BUILD_OUTPUT")" --setup-progress \
    >"$PORT_LOG" 2>&1 || {
      tail -80 "$PORT_LOG" >&2
      die "packaged setup helper could not build the real module"
    }
  for phase in inspect translate configure compile publish; do
    grep -Fq "[ringout-setup] phase=$phase" "$PORT_LOG" || \
      die "real module build did not reach phase: $phase"
  done
  MODULE="$(find "$BUILD_OUTPUT" -type f -name 'gGRSEAF_recomp.dll' -print -quit)"
  [[ -n "$MODULE" && -s "$MODULE" ]] || die "real gGRSEAF_recomp.dll was not produced"
fi

peer_alive() {
  local route=$1 role=$2 pid
  read -r pid <"$WORK/$route/$role/pid" || return 1
  [[ "$pid" =~ ^[1-9][0-9]*$ ]] && kill -0 "$pid" 2>/dev/null
}

launch_peer() {
  local route=$1 role=$2
  shift 2
  local peer_dir="$WORK/$route/$role"
  mkdir -p "$peer_dir/user"
  (
    cd "$PACKAGE"
    exec "${WINE[@]}" "$RUNTIME" --headless \
      --user-dir "$(windows_path "$peer_dir/user")" \
      --game "$(windows_path "$GAME")" \
      --module "$(windows_path "$MODULE")" \
      --controller Keyboard --netplay-mode rollback "$@"
  ) >"$peer_dir/log.txt" 2>&1 &
  printf '%s\n' "$!" >"$peer_dir/pid"
}

wait_for_marker() {
  local route=$1 marker=$2 timeout=$3 elapsed=0
  while ((elapsed < timeout)); do
    if grep -Fqa "$marker" "$WORK/$route/host/log.txt" 2>/dev/null &&
       grep -Fqa "$marker" "$WORK/$route/guest/log.txt" 2>/dev/null; then
      return 0
    fi
    peer_alive "$route" host && peer_alive "$route" guest || return 1
    sleep 1
    elapsed=$((elapsed + 1))
  done
  return 1
}

stop_route() {
  local route=$1 role pid
  for role in host guest; do
    if [[ -f "$WORK/$route/$role/pid" ]]; then
      read -r pid <"$WORK/$route/$role/pid"
      [[ "$pid" =~ ^[1-9][0-9]*$ ]] && kill "$pid" 2>/dev/null || true
    fi
  done
  sleep 1
}

run_route() {
  local route=$1 index=$2
  local port=$((BASE_PORT + index)) room_code="" elapsed=0
  local -a traversal=()
  mkdir -p "$WORK/$route"
  if [[ "$route" == traversal ]]; then
    traversal=(--netplay-traversal)
  elif [[ "$route" != direct ]]; then
    die "unknown route '$route' (expected direct or traversal)"
  fi

  printf '==> starting Windows rollback peers (%s)\n' "$route"
  launch_peer "$route" host --netplay-host --netplay-port "$port" \
    --netplay-players 2 --nickname Host --buffer 2 --netplay-timeout 120 \
    "${traversal[@]}"

  if [[ "$route" == traversal ]]; then
    while ((elapsed < 45)); do
      room_code="$(sed -n 's/.*netplay: online room code \([0-9a-f]\{8\}\).*/\1/p' \
        "$WORK/$route/host/log.txt" 2>/dev/null | tail -1)"
      [[ -n "$room_code" ]] && break
      peer_alive "$route" host || break
      sleep 1
      elapsed=$((elapsed + 1))
    done
    [[ -n "$room_code" ]] || {
      tail -60 "$WORK/$route/host/log.txt" >&2
      die "Dolphin traversal did not assign a room code"
    }
    launch_peer "$route" guest --netplay-join "$room_code" \
      --netplay-port "$port" --nickname Guest --netplay-timeout 120 \
      "${traversal[@]}"
  else
    sleep 3
    launch_peer "$route" guest --netplay-join 127.0.0.1 \
      --netplay-port "$port" --nickname Guest --netplay-timeout 120
  fi

  wait_for_marker "$route" 'netplay armed; booting' 150 || {
    tail -80 "$WORK/$route/host/log.txt" >&2
    tail -80 "$WORK/$route/guest/log.txt" >&2
    die "$route peers did not complete the synchronized StartGame handshake"
  }
  wait_for_marker "$route" '[rollback live] negotiated' 60 || \
    die "$route peers did not negotiate rollback"
  wait_for_marker "$route" '[rollback live] active' 120 || \
    die "$route peers did not activate snapshots, prediction, and replay"
  sleep "$RUN_SECONDS"
  peer_alive "$route" host && peer_alive "$route" guest || \
    die "$route peer exited during the active rollback window"
  if grep -Eqi 'DESYNC|\[rollback live\].*(failed|fatal|aborted|faulted|cancelled|diverged|refused)' \
      "$WORK/$route/host/log.txt" "$WORK/$route/guest/log.txt"; then
    grep -Eai 'DESYNC|\[rollback live\]|netplay:' \
      "$WORK/$route/host/log.txt" "$WORK/$route/guest/log.txt" | tail -80 >&2
    die "$route session reported rollback/netplay failure"
  fi
  if [[ "$route" == traversal ]]; then
    grep -Fqa 'netplay: registering online room with' "$WORK/$route/host/log.txt" || \
      die "host did not use Dolphin traversal"
    grep -Fqa "netplay: joining online room $room_code" "$WORK/$route/guest/log.txt" || \
      die "guest did not join the assigned room code"
  fi
  grep -Eha 'netplay:.*(connected|armed|room code)|\[rollback live\].*(negotiated|active)' \
    "$WORK/$route/host/log.txt" "$WORK/$route/guest/log.txt" | tail -24
  stop_route "$route"
  printf 'PASS: packaged Windows peers completed %s rollback netplay\n' "$route"
}

route_index=0
for route in $ROUTES; do
  run_route "$route" "$route_index"
  route_index=$((route_index + 1))
done

trap - EXIT INT TERM
cleanup
printf 'Windows ZIP real-game two-peer netplay smoke passed\n'
printf 'Smoke work directory: %s\n' "$WORK"
