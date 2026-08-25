#!/bin/bash
# Asset-free orchestration test for rollback-real-game.sh.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/.github/scripts/rollback-real-game.sh"
WORK="$(mktemp -d /tmp/ringout-rollback-harness-test.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PKG="$WORK/package"
mkdir -p "$PKG/bin" "$PKG/game/sys"
printf 'synthetic dol\n' > "$PKG/game/sys/main.dol"
printf 'synthetic module\n' > "$PKG/bin/gTEST01_recomp.so"
printf '0 -\n' > "$WORK/input.txt"

cat > "$PKG/bin/moderngekko-run" <<'FAKE'
#!/bin/bash
set -euo pipefail
log="${RINGOUT_DETERMINISM_LOG:?}"
frames="${RINGOUT_DETERMINISM_FRAMES:?}"
at="${RINGOUT_DETERMINISM_ROLLBACK_AT:-}"
len="${RINGOUT_DETERMINISM_ROLLBACK_LEN:-0}"
: > "$log"
for ((physical = 0; physical < frames; ++physical)); do
  logical=$physical
  if [ -n "$at" ] && ((physical > at + len)); then
    logical=$((physical - len))
  fi
  game=$(printf '%08x' "$((logical * 17 + 3))")
  l1=$(printf '%08x' "$((logical * 31 + 7))")
  if [ -n "${RINGOUT_DETERMINISM_CORRECTED_INPUT:-}" ] && \
     ((physical > at && physical <= at + len)); then
    game=$(printf '%08x' "$((logical * 17 + 4))")
  fi
  if [ "${RINGOUT_TEST_FAKE_DIVERGE:-0}" = 1 ] && \
     [ -n "$at" ] && ((physical == frames - 1)); then
    game=ffffffff
  fi
  printf '%d %08x %s %s\n' "$physical" "$((logical + 1))" "$game" "$l1" >> "$log"
done
if [ -n "$at" ]; then
  echo "[rollback] frame $((at + len)): restored in 1.00 ms (ok); replaying $len frames" >&2
  echo "[rollback] corrected replay COMPLETED: game-memory endpoint 00000000 (predicted endpoint was 00000001)" >&2
fi
FAKE
chmod +x "$PKG/bin/moderngekko-run"

if "$HARNESS" --package "$PKG" --input "$WORK/input.txt" \
    --rollback-at 4 --rollback-len 3 --work "$WORK/no-ack" >/dev/null 2>&1; then
  echo "missing opt-in acknowledgement unexpectedly succeeded" >&2
  exit 1
fi

RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  "$HARNESS" --package "$PKG" --input "$WORK/input.txt" \
  --rollback-at 4 --rollback-len 3 --work "$WORK/pass" >/dev/null
test -s "$WORK/pass/result.env"
test ! -s "$WORK/pass/replay.diff"

if RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME RINGOUT_TEST_FAKE_DIVERGE=1 \
    "$HARNESS" --package "$PKG" --input "$WORK/input.txt" \
    --rollback-at 4 --rollback-len 3 --work "$WORK/diverge" >/dev/null 2>&1; then
  echo "divergent corrected replay unexpectedly succeeded" >&2
  exit 1
fi
test -s "$WORK/diverge/replay.diff"

echo "rollback real-game harness orchestration: PASS"
