#!/bin/bash
# Cycles per GUEST OPCODE CLASS, for a gameplay profile of the recompiled module.
#
# hot-guest-code.sh answers "which guest ADDRESS is hot", which is the right
# question only when a hotspot exists. On gameplay there is none -- the top
# single guest address is 0.44% and the profile is a flat smear -- so the useful
# aggregation is by OPCODE CLASS: a smear in which every site is individually
# cheap can still be dominated by one opcode whose expansion is bad everywhere.
#
# This exists to test a specific claim: that "emit fewer host instructions per
# guest instruction" has headroom. Answering it needs the DYNAMIC weight, not
# the emitter's static output -- static structure has mispredicted execution
# every time it has been tried in this codebase (static psq sites said 92.8%
# GQR0; the measured truth for stores was 15.3%).
#
# Two forms, because the two halves want different machines: `perf report` must
# run where the .so sits at the path perf recorded (the Deck), while the
# generated chunks carrying the `// <addr>: <mnem>` comments live on the build
# machine.
#
#   # all on one machine
#   opcode-cycles.sh perf.data work/gen_lazy4/generated [top-N]
#
#   # split: on the Deck, extract the srcline rows...
#   perf report -i perf.data --no-children --comms="CPU-GPU thread" -s srcline \
#       --stdio -g none --percent-limit 0 \
#     | grep -oE "[0-9.]+%[[:space:]]+chunk_[^[:space:]]+\.c:[0-9]+" > lines.txt
#   # ...then here, join them
#   opcode-cycles.sh lines.txt work/gen_lazy4/generated [top-N]
#
# TRAPS, both of which produced a silent empty result the first time:
#  * The comm is "CPU-GPU thread" in the headless harness, NOT "CPU thread".
#    Filtering on the wrong name drops 99% of samples and maps zero lines.
#  * The determinism harness's frame hashing (crc32_fold_pclmulqdq) was 18% of
#    one profile. It sits outside the chunk source lines so it does not disturb
#    the relative ranking, but it deflates every "% of CPU thread" figure --
#    read the "% of mapped" column when the log is enabled.
#
# Also note out-of-line helpers (mem_read/write, psq) are NOT attributed to any
# chunk source line, so the load/store and paired-single ld/st classes are
# undercounted here by exactly the helper time the perf profile bills elsewhere.
set -euo pipefail

IN="${1:?usage: opcode-cycles.sh <perf.data|lines.txt> <generated-dir> [top-N]}"
GEN="${2:?usage: opcode-cycles.sh <perf.data|lines.txt> <generated-dir> [top-N]}"
TOP="${3:-30}"

[ -d "$GEN/chunks" ] || { echo "no chunks/ under $GEN" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if head -c 8 "$IN" | grep -q PERFILE; then
    perf report -i "$IN" --no-children --comms="CPU-GPU thread" -s srcline \
        --stdio -g none --percent-limit 0 2>/dev/null \
        | grep -oE "[0-9.]+%[[:space:]]+chunk_[^[:space:]]+\.c:[0-9]+" \
        > "$WORK/lines" || true
    LINES="$WORK/lines"
else
    LINES="$IN"
fi

[ -s "$LINES" ] || {
    echo "no chunk source lines." >&2
    echo "  - module built with -DMODULE_DEBUG_LINES=ON?" >&2
    echo "  - comm really \"CPU-GPU thread\"? check: perf report --sort comm" >&2
    exit 1
}

exec python3 "$(dirname "${BASH_SOURCE[0]}")/opcode-cycles.py" "$LINES" "$GEN" "$TOP"
