#!/bin/bash
# Turns a perf profile of the recompiled module into hot GUEST addresses.
#
# Every chunk is a single enormous C function, so a plain `perf report` can only
# say "func_8000D940 is 24% of the thread" -- a 16 KB span holding dozens of
# guest functions. That was the blind spot behind every optimisation decision
# made against this core.
#
# The module built with -DMODULE_DEBUG_LINES=ON carries line tables, so perf can
# place each sample on a line of the generated .c. The emitter writes one
# `label_<guest address>:` per guest instruction, so the nearest preceding label
# turns that line back into a guest PC. Codegen is identical with and without
# -g1 (the .text bytes match), so the profile is representative.
#
#   .github/scripts/hot-guest-code.sh <perf.data> <generated-dir> [top-N]
#   .github/scripts/hot-guest-code.sh perf.data work/gen_newabi/generated 25
set -euo pipefail

DATA="${1:?usage: hot-guest-code.sh <perf.data> <generated-dir> [top-N]}"
GEN="${2:?usage: hot-guest-code.sh <perf.data> <generated-dir> [top-N]}"
TOP="${3:-25}"

[ -f "$DATA" ] || { echo "no such perf data: $DATA" >&2; exit 1; }
[ -d "$GEN/chunks" ] || { echo "no chunks/ under $GEN" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# "CPU-GPU thread" as well as "CPU thread": the determinism harness runs the
# GPU synchronously on the emulation thread, so profiles taken through it carry
# the merged name and a filter on "CPU thread" alone silently matches nothing --
# which reads exactly like a module built without -DMODULE_DEBUG_LINES=ON.
perf report -i "$DATA" --no-children --comms="CPU thread,CPU-GPU thread" -s srcline \
    --stdio -g none --percent-limit 0 2>/dev/null \
    | grep -oE "[0-9.]+%[[:space:]]+chunk_[^[:space:]]+\.c:[0-9]+" > "$WORK/lines" || true

if [ ! -s "$WORK/lines" ]; then
    echo "No chunk source lines in this profile." >&2
    echo "Either the module was built without -DMODULE_DEBUG_LINES=ON, or the" >&2
    echo "profile predates it -- perf needs the line tables of the exact .so it" >&2
    echo "sampled." >&2
    exit 1
fi

python3 - "$WORK/lines" "$GEN" "$TOP" <<'PY'
import bisect, collections, re, sys
lines_file, gen, top = sys.argv[1], sys.argv[2], int(sys.argv[3])

# For each chunk file, the sorted list of (source line -> guest address) from the
# emitted `label_<addr>:` markers.
cache = {}
def labels(chunk):
    if chunk not in cache:
        nums, addrs = [], []
        try:
            with open(f"{gen}/chunks/{chunk}") as fh:
                for n, line in enumerate(fh, 1):
                    m = re.match(r"label_([0-9A-F]{8}):", line)
                    if m:
                        nums.append(n); addrs.append(int(m.group(1), 16))
        except OSError:
            pass
        cache[chunk] = (nums, addrs)
    return cache[chunk]

by_addr = collections.Counter()
unmapped = 0.0
for raw in open(lines_file):
    m = re.match(r"\s*([0-9.]+)%\s+(chunk_[^\s]+\.c):(\d+)", raw)
    if not m:
        continue
    pct, chunk, num = float(m.group(1)), m.group(2), int(m.group(3))
    nums, addrs = labels(chunk)
    if not nums:
        unmapped += pct; continue
    i = bisect.bisect_right(nums, num) - 1
    if i < 0:
        # Above the first label: the dispatch switch at the top of the chunk.
        by_addr["(chunk entry switch)"] += pct
    else:
        by_addr[f"0x{addrs[i]:08X}"] += pct

print(f"{'guest addr':<22}{'% of CPU thread':>16}")
print("-" * 40)
for addr, pct in by_addr.most_common(top):
    print(f"{addr:<22}{pct:>15.2f}%")
tot = sum(by_addr.values())
print("-" * 40)
print(f"{'mapped total':<22}{tot:>15.2f}%")
if unmapped:
    print(f"{'unmapped':<22}{unmapped:>15.2f}%")
print()
print("Addresses are the nearest preceding guest instruction, so they name the")
print("block a sample landed in. '(chunk entry switch)' is the per-dispatch PC")
print("switch at the top of every chunk -- dispatcher cost, not guest work.")
PY
