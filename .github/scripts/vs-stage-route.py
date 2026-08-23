#!/usr/bin/env python3
"""Emit a frame-keyed VS-mode route that lands on ONE CHOSEN STAGE.

Why this exists: arcade draws both the stage and the opponent at random, and
stages do not cost the same guest CPU, so arcade can compare two builds (both
arms replay identical content) but cannot say how fast the game *is*. Fixing the
characters and naming the stage makes the workload the same every run, and lets
each stage be measured on its own.

The route is keyed on the emulated frame, never on wall-clock time. A press
schedule tuned by hand on one machine drifts on another -- the Deck walks these
menus at ~46-60 fps and the desktop much faster -- so a wall-clock script that
works here silently lands on the wrong screen there. Frame-keyed playback is
reproducible by construction, on both machines.

    vs-stage-route.py <stage 0-10> [-o out.txt]

Stage 0 is RANDOM (the carousel's default position); 1-10 are the real stages,
in carousel order going down. There are no stage names anywhere in the UI, so
the index IS the identifier:

    1  marble hall, statues, blue sky      6  waterfall cave with a ship
    2  pink/white stepped pyramid          7  desert ruins at sunset, cannon
    3  green cavern, monstrous face        8  ornate columned hall, statues
    4  golden ornate wooden hall           9  gothic grey cathedral interior
    5  Thai palace exterior, blue sky     10  clock tower / gear workshop

Needs a runtime with per-port scripted input (the "P2" line prefix). Before that
existed the harness drove port 1 only, which cannot run this route at all: each
side confirms its own character and health, and the stage pick is 2P's.

Characters are whatever the cursors start on -- Talim (1P) and Raphael (2P) --
which is fine: the point is that they are the SAME in every run.
"""

import argparse
import sys

# The determinism frame counter is driven by the PE finish interrupt, NOT by
# displayed frames, and this game raises almost exactly 2.0 of them per
# displayed frame -- so ~120 per second at full speed. Every constant below is
# in those units; dividing by 120 gives seconds.
FPS = 120

# A press has to be held long enough for SI to poll it and let go before the
# next screen, so every press is a press line plus a release line.
HOLD = 6

# Menu screens have entry animations that swallow input arriving too early --
# the wall-clock version of this route lost presses at exactly these points, and
# the netplay route hit the same thing. The gaps are deliberately generous.
INTRO_FIRST = 300     # first START; earlier presses land on a black screen
INTRO_LAST = 2400     # MODE SELECT is up by ~2251
INTRO_EVERY = 60

MODE_ENTER = 2700     # A: focus moves into the Original list (Arcade first)
MODE_DOWN = 3000      # DOWN: Arcade -> VS Battle
MODE_CONFIRM = 3300   # A: -> CHARACTER SELECT

LOCK_1P = 4000        # each side locks its own character
LOCK_2P = 4400
HEALTH_1P = 5000      # "Select amount of health", again one side at a time
HEALTH_2P = 5400

STAGE_FIRST = 6000    # STAGE SELECT is up; carousel sits on RANDOM
STAGE_EVERY = 150     # one DOWN per step down the carousel
STAGE_SETTLE = 300    # let the tile stop moving before confirming
STAGE_STEPS = 10      # slots reserved for carousel steps, whether used or not

# EVERY STAGE'S ROUTE MUST BE THE SAME LENGTH, or the stages are not comparable:
# stage 10 takes 9 more carousel steps than stage 1, which at STAGE_EVERY frames
# each would start its fight ~1350 frames later. Scoring a fixed frame window
# would then compare different moments of the match. The confirm is therefore
# pinned to a constant frame and the unused steps are simply idle time.

# After the stage is confirmed the match loads, and loading is not the workload.
# Scoring must start after this many frames, not at frame 0. The pre-fight
# cutscene ("PRESS START TO SKIP") is inside this budget; it is the same
# cutscene for every stage because the character pairing is fixed.
LOAD_FRAMES = 2400

# Both fighters trade blows for the rest of the run. An idle stance would still
# be a valid comparison -- it is identical on every stage -- but it is not
# representative gameplay, and the animation and collision work is part of what
# a stage costs. NEVER press B: in this game's menus B is Back, and if a menu is
# somehow still up, an A/B loop locks and unlocks a selection forever.
FIGHT_EVERY = 60
FIGHT_BUTTONS = (("A", 1), ("X", 2), ("X", 1), ("A", 2))


def build(stage: int, until: int) -> tuple[list[tuple[int, int, str]], int]:
    """Return (events, first_scorable_frame). Each event is (frame, port, buttons)."""
    events: list[tuple[int, int, str]] = []

    def press(frame: int, port: int, button: str) -> None:
        events.append((frame, port, button))
        events.append((frame + HOLD, port, "-"))

    # Logos and the intro movie. START is a skip here and harmless on MODE
    # SELECT (it is not Confirm), so overshooting the mash costs nothing.
    for frame in range(INTRO_FIRST, INTRO_LAST + 1, INTRO_EVERY):
        press(frame, 1, "START")

    # NOTE the button names: this harness spells the d-pad "DOWN", not the Pipe
    # backend's "D_DOWN". An unrecognised name used to be dropped in silence,
    # which sent an early version of this route into Arcade instead of VS.
    press(MODE_ENTER, 1, "A")
    press(MODE_DOWN, 1, "DOWN")
    press(MODE_CONFIRM, 1, "A")

    press(LOCK_1P, 1, "A")
    press(LOCK_2P, 2, "A")
    press(HEALTH_1P, 1, "A")
    press(HEALTH_2P, 2, "A")

    # The carousel is circular and starts ON RANDOM, so stage N is exactly N
    # steps down. Stage 0 leaves it on RANDOM, which is the wrong thing for a
    # census but useful as a control.
    frame = STAGE_FIRST
    for _ in range(stage):
        press(frame, 2, "DOWN")
        frame += STAGE_EVERY

    # Pinned, not `frame + settle`: see STAGE_STEPS above.
    confirm = STAGE_FIRST + STAGE_STEPS * STAGE_EVERY + STAGE_SETTLE
    press(confirm, 2, "A")

    scorable = confirm + HOLD + LOAD_FRAMES
    for index, frame in enumerate(range(scorable, until, FIGHT_EVERY)):
        button, port = FIGHT_BUTTONS[index % len(FIGHT_BUTTONS)]
        press(frame, port, button)

    return events, scorable


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("stage", type=int, help="0 = RANDOM, 1-10 = carousel order")
    parser.add_argument("-o", "--out", default="-", help="output file (default stdout)")
    parser.add_argument("--until", type=int, default=24000,
                        help="emit fight input out to this frame (default 24000)")
    args = parser.parse_args()

    if not 0 <= args.stage <= 10:
        parser.error("stage must be 0-10 (0 = RANDOM); the carousel has 11 positions")

    events, scorable = build(args.stage, args.until)
    events.sort(key=lambda e: (e[0], e[1]))

    lines = [
        f"# VS-mode route, stage {args.stage} -- generated by vs-stage-route.py.",
        "# Talim (1P) vs Raphael (2P), default health, fixed stage.",
        "#",
        "# Frames are determinism frames (PE finish interrupts, ~2 per displayed",
        f"# frame, ~{FPS}/s). Port is the leading P1/P2.",
        "#",
        f"# Loading ends around frame {scorable}: score AFTER that, never from 0.",
        "#",
        "# frame  buttons",
        "P1 0 -",
        "P2 0 -",
    ]
    for frame, port, buttons in events:
        lines.append(f"P{port} {frame} {buttons}")
    text = "\n".join(lines) + "\n"

    if args.out == "-":
        sys.stdout.write(text)
    else:
        with open(args.out, "w") as handle:
            handle.write(text)
        print(f"{args.out}: stage {args.stage}, {len(events)} pad states, "
              f"scorable from frame {scorable}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
