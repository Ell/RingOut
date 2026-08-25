# Real-game rollback oracle harness

Status: opt-in offline test harness on branch `codex/rollback-netplay`, recorded
2026-08-25. This is test infrastructure, not live rollback netplay.

## Purpose and claim boundary

`.github/scripts/rollback-real-game.sh` runs a privately prepared RingOut
package twice with the existing frame-keyed determinism hook:

1. an on-time baseline through logical frame `rollback_at + rollback_len`;
2. a correction run which saves at `rollback_at`, withholds input transitions
   through the prediction window, reaches the speculative frontier, restores,
   and replays with the complete authoritative input script.

The harness requires both runs to match before prediction, requires the
speculative trace to differ, requires the runtime restore and corrected replay
to complete, and then compares every OS-global, game-memory, and L1 hash in the
corrected replay window with the corresponding baseline logical frame. It fails
nonzero on a missing frame, broken frame sequence, runtime error, failed restore,
divergence, or hash mismatch.

This proves that the snapshot/restore path can replace an incorrect predicted
input window and converge to the on-time baseline for one scripted real-game
window. With no experimental `--skip` mask, the correction run exercises
`DolphinRollbackStateStore`, `RollbackCoordinator`, and the journal's atomic
acknowledge-and-publish path at the CPU-thread frame boundary intended for live
integration. Its rollback-only memory representation omits only inaccessible
MEM1 padding and an exactly all-zero fake-VMEM payload; any non-zero fake VMEM,
plus video state, ARAM, and normal JIT load handling, remains serialized. The
journal token is synthetic because this oracle's input is frame-keyed rather
than SI-poll-keyed. It does not exercise packet
transport, production `GetNetPads` replay, resimulation output suppression, or
audio reconciliation. Published `ell.6` netplay remains fixed-delay lockstep;
implementation commit `6518db52` has the separate live rollback path documented
in `rollback-live-test-harness.md`.

The coordinator uses a harness-only output gate. It is acceptable solely
because the run is headless, uses an isolated disposable user directory, and
does not claim presentation, audio, rumble, or host-side-effect correctness.
The gate performs no suppression and is not suitable for live netplay.

## Private-asset boundary

The harness does not search for, copy, extract, rename, or upload a disc image.
It accepts only a package already prepared locally from a disc the tester owns.
The package must contain:

- `bin/moderngekko-run` from the branch under test;
- `game/sys/main.dol` and the extracted game tree;
- exactly one matching `bin/g*_recomp.so` module.

Do not commit the prepared package or the retained output directory. Runtime
logs can contain local filesystem paths, and `result.env` intentionally records
artifact SHA-256 values for provenance. The script never prints the private
package path, although the local runtime process necessarily receives its game
and module paths as command-line arguments.

The repository's existing `dist/RingOut-1.0-dist/setup.sh` prepares such a
private package from a plain ISO/WBFS when its runtime and `dolrecomp` tools have
first been populated by the packaging workflow. Compressed RVZ/GCZ/NKit images
must be converted to a supported plain image outside this harness. Never add the
converted image to the repository.

## Prerequisites

- A private prepared package matching this branch.
- Bash, GNU `awk`, `diff`, `sed`, `sha256sum`, and `readlink`.
- Enough time for two deterministic real-game runs and enough free space for
  isolated user directories and logs. The default route reaches a match before
  the snapshot, so it is much stronger and slower than a boot-menu probe.
- The production-safe rollback representation is the default. Unsafe
  narrow-state experiments must be explicit through `--skip` and retain their
  exact skip list in `result.env`. The production state store deliberately
  rejects those skipped sections, so `--skip` retains the historical
  direct-buffer oracle path and is not evidence for the production rollback
  core.

## Exact commands

First test the orchestration without private assets:

```bash
.github/scripts/test-rollback-real-game-harness.sh
```

Then run the production-safe real-game oracle. Keep the private package path in
a local environment variable rather than recording it in shell history:

```bash
read -r -p "Private prepared package: " RINGOUT_PRIVATE_PACKAGE
export RINGOUT_PRIVATE_PACKAGE
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-real-game.sh \
  --package "$RINGOUT_PRIVATE_PACKAGE" \
  --input .github/input-scripts/arcade-match.txt \
  --rollback-at 5200 \
  --rollback-len 30
unset RINGOUT_PRIVATE_PACKAGE
```

Only after the production-safe snapshot passes, test the historical unsafe
skip-mask snapshot if comparison data is specifically needed:

```bash
read -r -p "Private prepared package: " RINGOUT_PRIVATE_PACKAGE
export RINGOUT_PRIVATE_PACKAGE
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-real-game.sh \
  --package "$RINGOUT_PRIVATE_PACKAGE" \
  --input .github/input-scripts/arcade-match.txt \
  --rollback-at 5200 \
  --rollback-len 30 \
  --skip vmem,pad,video,aram,jitclear
unset RINGOUT_PRIVATE_PACKAGE
```

The script prints its evidence directory. Retain `baseline.hashes`,
`correction.hashes`, the two runtime logs, comparison traces/diffs, and
`result.env` together. A zero exit status and an empty `replay.diff` are required
for a pass. `speculative-mismatch.diff` must be non-empty: it proves that the
first pass was actually wrong before correction.

## Asset-free regression test

The orchestration test constructs only tiny synthetic placeholder files and a
fake runtime. It checks the explicit ownership acknowledgement, a successful
logical-frame remap, evidence generation, and rejection of a deliberately
divergent replay. It does not validate emulator state serialization; the private
real-game command above is the executable evidence for that layer.
