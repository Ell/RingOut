#!/usr/bin/env python3
"""Summarise a per-stage census CSV: which stages are expensive, and is the
difference bigger than this machine's noise?

    vs-census-report.py <census.csv>

The second question is the one that matters. Run-to-run spread on the Deck is
~4.1% (see the CPU-affinity work, where two earlier verdicts turned out to be
artifacts in opposite directions), so a ranking whose spread sits inside that is
a ranking of noise. This prints the per-stage spread next to the between-stage
one and says which is larger, rather than leaving that comparison to the eye.
"""

import argparse
import csv
import statistics
from collections import defaultdict

# What each carousel index looks like; the game shows no stage names anywhere.
THUMBNAILS = {
    1: "marble hall, statues",
    2: "stepped pyramid",
    3: "green cavern, face",
    4: "golden wooden hall",
    5: "Thai palace exterior",
    6: "waterfall cave, ship",
    7: "desert ruins, sunset",
    8: "columned hall, statues",
    9: "gothic cathedral",
    10: "clock tower / gears",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", help="census.csv from vs-census.sh")
    args = parser.parse_args()

    by_stage: dict[int, list[float]] = defaultdict(list)
    truncated = 0
    with open(args.csv) as handle:
        for row in csv.DictReader(handle):
            # A run that stopped short did not execute the same work as the
            # others, so it is not comparable -- drop it and say so.
            if int(row["final_frame"]) < int(row["end_frame"]):
                truncated += 1
                continue
            by_stage[int(row["stage"])].append(float(row["fps"]))

    if not by_stage:
        print("no complete runs in the CSV")
        return 1

    print(f"{'stage':>5}  {'n':>2}  {'mean':>7}  {'spread':>7}  {'vs best':>8}   thumbnail")
    print("-" * 74)

    means = {stage: statistics.mean(values) for stage, values in by_stage.items()}
    best = max(means.values())
    for stage in sorted(means, key=lambda s: means[s]):
        values = by_stage[stage]
        mean = means[stage]
        spread = (max(values) - min(values)) / mean * 100 if len(values) > 1 else 0.0
        print(f"{stage:>5}  {len(values):>2}  {mean:>7.2f}  {spread:>6.1f}%  "
              f"{(mean - best) / best * 100:>7.1f}%   {THUMBNAILS.get(stage, '')}")

    slowest, fastest = min(means.values()), max(means.values())
    between = (fastest - slowest) / fastest * 100
    worst_within = max(
        ((max(v) - min(v)) / statistics.mean(v) * 100) for v in by_stage.values() if len(v) > 1
    ) if any(len(v) > 1 for v in by_stage.values()) else 0.0

    print()
    print(f"between stages: {between:.1f}%  (slowest {slowest:.2f} vs fastest {fastest:.2f} frames/s)")
    print(f"worst within-stage spread: {worst_within:.1f}%")
    if truncated:
        print(f"dropped {truncated} truncated run(s) that did not reach the end frame")
    print()
    if between <= worst_within:
        print("VERDICT: the between-stage difference does NOT clear the within-stage")
        print("spread. This ranking is noise -- add reps before believing any of it.")
    else:
        print("VERDICT: between-stage difference exceeds the within-stage spread, so")
        print("the ordering is real.")
        if "windowed" not in args.csv:
            print("This run is HEADLESS: no GPU work is included and these are not")
            print("playable fps. Confirm the extremes windowed.")
        else:
            print("Divide frames/s by 2 for displayed fps: this game raises ~2 PE finish")
            print("interrupts per displayed frame, so 120 frames/s is full 60 fps speed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
