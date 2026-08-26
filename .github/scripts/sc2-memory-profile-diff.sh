#!/bin/bash
# Compare two exact-scope SC2 engine-iteration MEM1 footprint profiles.
set -euo pipefail

usage() {
  echo "usage: sc2-memory-profile-diff.sh <idle-evidence> <gameplay-evidence>" >&2
  exit 2
}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ "$#" -eq 2 ] || usage
IDLE="$(readlink -f "$1")"
GAMEPLAY="$(readlink -f "$2")"
WORK="$(mktemp -d /tmp/ringout-sc2-memory-diff.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

profile_lines() {
  grep -E '^\[sc2-memory-profile\] (result|region) ' "$1"
}

validate_evidence() {
  local evidence="$1" route="$2" peer log
  [ -d "$evidence" ] || fail "$route evidence directory is unavailable"
  for peer in host guest; do
    log="$evidence/$peer/log.txt"
    [ -s "$log" ] || fail "$route $peer log is unavailable"
    grep -Eq '^\[sc2-memory-profile\] result ticks=[1-9][0-9]* ram_bytes=[1-9][0-9]* page_bytes=4096 changed_pages=[1-9][0-9]* changed_bytes_upper_bound=[1-9][0-9]* every_tick_pages=[0-9]+ complete=yes$' "$log" ||
      fail "$route $peer profile is incomplete"
    grep -Eq '^\[sc2-memory-profile\] region offset=0x[0-9a-f]{8} size=0x[0-9a-f]{8} pages=[1-9][0-9]* changed_ticks=[1-9][0-9]*\.\.[1-9][0-9]*$' "$log" ||
      fail "$route $peer profile has no regions"
  done
  diff -u <(profile_lines "$evidence/host/log.txt") \
    <(profile_lines "$evidence/guest/log.txt") >/dev/null ||
    fail "$route peer profiles differ"
}

expand_pages() {
  gawk '
    $1 == "[sc2-memory-profile]" && $2 == "region" {
      split($3, offset, "=")
      split($5, pages, "=")
      base = strtonum(offset[2])
      for (i = 0; i < pages[2]; ++i)
        printf "%08x\n", base + i * 4096
    }
  ' "$1" | sort -u
}

validate_evidence "$IDLE" idle
validate_evidence "$GAMEPLAY" gameplay
expand_pages "$IDLE/host/log.txt" > "$WORK/idle.pages"
expand_pages "$GAMEPLAY/host/log.txt" > "$WORK/gameplay.pages"
comm -12 "$WORK/idle.pages" "$WORK/gameplay.pages" > "$WORK/shared.pages"
comm -23 "$WORK/idle.pages" "$WORK/gameplay.pages" > "$WORK/idle-only.pages"
comm -13 "$WORK/idle.pages" "$WORK/gameplay.pages" > "$WORK/gameplay-only.pages"
sort -u "$WORK/idle.pages" "$WORK/gameplay.pages" > "$WORK/union.pages"

idle_pages="$(wc -l < "$WORK/idle.pages")"
gameplay_pages="$(wc -l < "$WORK/gameplay.pages")"
shared_pages="$(wc -l < "$WORK/shared.pages")"
idle_only_pages="$(wc -l < "$WORK/idle-only.pages")"
gameplay_only_pages="$(wc -l < "$WORK/gameplay-only.pages")"
union_pages="$(wc -l < "$WORK/union.pages")"

echo "format=sc2-memory-profile-diff-v1"
echo "page_bytes=4096"
echo "idle_pages=$idle_pages"
echo "gameplay_pages=$gameplay_pages"
echo "shared_pages=$shared_pages"
echo "idle_only_pages=$idle_only_pages"
echo "gameplay_only_pages=$gameplay_only_pages"
echo "union_pages=$union_pages"
echo "union_bytes_upper_bound=$((union_pages * 4096))"
while read -r page; do
  [ -n "$page" ] && echo "gameplay_only_page=0x$page"
done < "$WORK/gameplay-only.pages"
