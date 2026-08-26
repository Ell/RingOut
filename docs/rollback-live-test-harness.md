# Live rollback integration test harness

Status: branch-local executable contract plus a passing ordinary production-path
real-game correction run, recorded 2026-08-25 at implementation commit
`6518db52`. The final post-fix run is correction and
confirmed-state convergence evidence for the tested Linux/source path, not a
tagged player-ready release artifact.

Update 2026-08-26: player reports of `GFX FIFO: Unknown Opcode` and crashes
invalidate the older run as a GPU/renderer safety gate. Implementation commit
`35c09137` on branch `codex/rollback-gpu-state` now holds the GPU quiesced across
each complete rollback snapshot/load, defaults rollback sessions to single-core,
and extends this harness with `--windowed`, `--dual-core`, threading/barrier
provenance, and fatal GPU/FIFO marker rejection. The older result remains
input-correction and confirmed-memory evidence only. See
`docs/rollback-emulation-gpu-state.md`.

Game-specific update 2026-08-26: the `codex/sc2-slippi-rollback` branch adds a
GGPO-style consecutive-frame oracle. It uses the production full-state
transaction store today, restores and re-executes every logical frame in the
requested window with identical scripted input, and compares low MEM1, game
MEM1, and locked L1 at each endpoint:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-continuous-sync.sh \
  --package /path/to/private/package --start 600 --pairs 600 \
  --work /tmp/ringout-continuous-sync.sc2
```

The same branch adds `--hook-profile` to
`.github/scripts/rollback-live-real-game.sh`. It collects 600 post-warmup video
frames from both peers and fails unless each has at least one dispatch PC that
appears exactly once in every frame. This is hook-discovery evidence, not a
certified SC2 hook. See [SC2 game-specific rollback](sc2-slippi-rollback.md).

## Coverage and claim boundary

`moderngekko.rollback_live_contract` is an executable model which uses the
production `RollbackSIInputProtocol`, `RollbackSIInputJournal`, and
`RollbackInputTimeline`. It covers:

- symmetric two-peer capability negotiation, fixed-delay downgrade when either
  peer opts out, and fail-closed rejection of protocol, state-format, feature,
  horizon, polling-bound, or compatibility mismatches;
- a wrong repeat-last prediction followed by late authoritative input and
  convergence between the two peer journals;
- two SI polls in one emulated frame, no poll in the next frame, and a later
  poll, proving that a network batch is not modeled as a 60 Hz frame;
- explicit `PredictionHorizonExceeded` fallback instead of unbounded
  speculation;
- every truncated length of a maximum 481-byte RSIB packet, plus malformed
  magic, version, flags, generation, batch count, pad mask, connected flag,
  trailing bytes, and too-small encode buffers.

The contract retains a small local negotiation oracle, while
`moderngekko.netplay_protocol` now separately exercises the production
query/response, fixed-delay downgrade, generation/session validation, inactive
send rejection, and multi-batch pad-ownership checks.

`.github/scripts/rollback-live-real-game.sh` runs the existing two-process VS
match route against a privately prepared package. Every rollback route passes
the explicit player CLI `--netplay-mode rollback`, waits for both peers to log
`[rollback live] active` before advancing the input script, and rejects
desync/failure markers. The correction
mode requires the exact `correction committed` marker. `--expect-horizon`
instead requires both a hard-horizon marker and a later `horizon resumed`
marker, followed by successful route completion. A stall without recovery
fails the harness.

`--windowed` is the renderer-backed GPU/FIFO gate. `--dual-core` opts into the
guarded deterministic-GPU experiment; ordinary rollback qualification omits it
and must log the single-core rollback-safe default. Both paths must log the
matching `gpu_transaction_barrier` marker on both peers. The evidence gate also
rejects unknown GPU opcodes, linked/desynced/negative/out-of-bounds FIFO
markers, process-signal markers, and sanitizer failures.

`--production` exercises the ordinary player activation path. It passes
`--netplay-mode rollback` to both peers, explicitly removes the isolated-test
acknowledgement and test-only numeric overrides, and requires both peers to report
`negotiated` and `active` before the VS route can pass. This mode is expected to
fail closed whenever `IsLiveRollbackProductionReady()` is false. The current
worktree's audited capability matrix returns true, and the retained production
run proves the player activation path can negotiate, activate, correct, and
commit without the isolated-test acknowledgement
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/LiveRollbackOutputGate.cpp:158-188`;
`ModernGekko/tools/netplay_session.cpp:905-928`).

When `--production` is combined with `--fault-script`, the match wrapper adds
the separate `RINGOUT_ROLLBACK_FAULT_ACK=PRODUCTION_OUTPUT_GATE` only for fault
injection; it does not select the isolated output gate
(`.github/scripts/netplay-match.sh:121-139` and
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1413-1424,1644-1658`).

The memory-card protocol/content snapshot, teardown-latched output suppression,
and corrected-frontier publication-fault paths identified after the earlier
run are implemented and covered by the final production rerun.

Rollback replay creates additional physical-frame hash rows, so the legacy
line-for-line fixed-delay hash comparison is not a valid correction oracle in
fault mode. Both traces are retained. Rollback harness runs additionally set
`RINGOUT_ROLLBACK_CONFIRMED_LOG` to a private per-process file. At each
60-logical-frame checkpoint, the CPU thread writes real-MEM1, locked-L1, and
emulated-time-base values only if the final SI batch consumed by that completed
frame is inside the contiguous authoritative frontier, no correction is
pending, and the coordinator is Ready. Replaying the same logical frame replaces
the earlier candidate. It synchronizes GPU work before reading guest memory
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientRollback.cpp:198-317`).
The wrapper canonicalizes the last local row for each frame, requires at least
three shared confirmed frames, and fails on the first differing value. Empty,
malformed, divergent, and too-short evidence all fail the asset-free shell
contract (`.github/scripts/rollback-live-real-game.sh:156-196`).

Rollback protocol v2 also makes that compact periodic confirmed-state report
mandatory for live rollback. The server accepts exactly one report per expected
player/generation/checkpoint, bounds lead and retained frames, and either
retires a match or stops the session on mismatch/malformed/duplicate/stale/
future input
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/RollbackStateDigestProtocol.h:20-157`;
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:1057-1110,1812-1850`).
The opt-in local files remain harness evidence only; ordinary players do not
create them. The reports are error detectors, not authentication or encryption,
and they do not serialize full emulator state.

## Test-only fault contract

Fault injection is allowed only in a headless isolated test session with:

```text
RINGOUT_ROLLBACK_FAULT_SCRIPT=/absolute/path/to/schedule
RINGOUT_ROLLBACK_TEST_ACK=HEADLESS_ISOLATED
RINGOUT_ROLLBACK_BASE_DELAY_SAMPLES=2
RINGOUT_ROLLBACK_HORIZON_FRAMES=8
```

The deterministic schedule grammar is:

```text
# comments and blank lines are allowed
delay <send_ordinal> <release_after_send_ordinal>
drop <send_ordinal>
```

Ordinals count locally sent authoritative RSIB packets. A delayed packet stays
queued until the named later send ordinal. Dropped input can be recovered only
by protocol redundancy; a test which produces no correction fails. Malformed,
duplicate, backward, or impossible instructions must make the runtime fail
closed before starting netplay. The wrapper independently validates those same
properties before it launches either process.

Repository schedules are intentionally asset-free:

- `.github/input-scripts/rollback-fault-correction.txt` holds every redundant
  copy in distributed windows across real scripted input edges. The run fails
  unless at least one late value actually corrects a prediction.
- `.github/input-scripts/rollback-fault-horizon.txt` is run with a one-frame
  horizon and drops send ordinals 1 through 64. The receiver freezes after
  exhausting its single predicted batch; ordinal 65 is delivered normally and
  carries the sender's persistent oldest-unacknowledged repair, allowing both
  peers to resume. The file's SHA-256 is
  `fb5440b5b91778871e3d5e103b8475969489cecc3977ee1f87ddc422dea70ac3`.

The seam must not delay ENet control packets, handshake packets, or fixed-delay
`PadData`; it targets authoritative RSIB sends only. It must remain disabled in
ordinary sessions and release builds unless both explicit test gates are
present.

The wrapper sets the acknowledgement, base delay, and horizon on both peers.
Only the host receives the fault-script path. The scripted route changes host
input early, so delayed host authority forces correction on the guest; the
direction is unambiguous in retained evidence
(`.github/scripts/netplay-match.sh:131-150`).

## Reproduction

The live concepts originated at commit `38d69d84`; current-worktree anchors are:

- normal/test activation and host configuration in
  `ModernGekko/tools/netplay_session.cpp:62-84,905-928,1040-1160`;
- channel validation, session parsing, bounded RSIB decode, and match reset in
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:363-368,829-890,1527-1545`;
- frame-boundary restore/replay/commit, confirmed logging, and grouped/unbatched
  SI consumption in
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientRollback.cpp:35-270,279-465`;
- mid-frame correction-frontier extension in
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/RollbackInputTimeline.cpp:85-140`;
- the production/test gate, session quarantine, and corrected-frontier barrier
  in `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/LiveRollbackOutputGate.cpp:43-141,143-188,212-338`.

Configure/build and run all focused asset-free contracts:

```bash
ctest --test-dir /tmp/ringout-rollback-integration-build2 \
  --output-on-failure \
  -R '^moderngekko\.(rollback_memory_snapshot|live_rollback_gate|rollback_coordinator|rollback_input_timeline|rollback_si_input_journal|rollback_live_contract|rollback_live_scheduler|rollback_state_digest|rollback_live_harness|frontend_config)$'

ctest --test-dir /tmp/ringout-rollback-integration-build2 \
  --output-on-failure -R '^moderngekko\.netplay_protocol$'
```

The final results were 10/10 focused tests in 0.26 seconds, followed by the
localhost protocol test 1/1 in 20.54 seconds.

Test the shell evidence gate without private assets:

```bash
.github/scripts/test-rollback-live-real-game-harness.sh
```

### Current production-path evidence

The retained 2026-08-25 command was:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-rollback-private.mGbUYiaf/package \
  --fault-script .github/input-scripts/rollback-fault-correction.txt \
  --work /tmp/ringout-live-rollback.final-correction.OHN0EzDz \
  --play-seconds 18 --port 28841 --production
```

`rollback-result.env` records `production_path=1`, base runtime SHA-256
`3a7e27d7420ac9ae49eca997e0301a972f3a3799997dcff9a2920f098b1351ee`,
module SHA-256
`e01d1fc7f14d41cf170fb5b036e5c754cb3062b8e5421f147258b627e2931d48`,
DOL SHA-256
`0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5`,
and correction-schedule SHA-256
`e33efd000a38e2f84ce0f410f154086368fd6f0fc2f3394c1d9577ca7f467957`.
Both peers negotiated generation 1, base delay 2 SI samples, and horizon 8.
The frame-one checkpoints were 48,213,190 host bytes and 48,213,199 guest
bytes (45.98 MiB each).

The guest recorded and committed two corrections:

```text
restore_frame=25 replay_through_frame=27 first_batch=21
restore_frame=137 replay_through_frame=137 first_batch=133
```

Host and guest retained 717 and 723 physical rows respectively; canonical
comparison retained 717 rows from each. Their confirmed-state logs are
byte-identical (SHA-256
`4969a73790008801a05a27522d2508a7ffceb32d181a55be1b2f5d14caec9795`)
and match at all eleven reported logical frames, every 60 frames from 60 through
660. Each row covers real MEM1 CRC, locked-L1 CRC, and emulated timebase. The
different physical hash traces are expected because peers execute different
speculative/replay work; the logical rows are the correction convergence
oracle.

The exact canonical rows on both peers were:

```text
60  7b906e04 e20eea22 0079016684b56f61
120 54ed18a3 e20eea22 0079016685eabed5
180 e47a48a0 e20eea22 007901668720101f
240 5eaac30d e20eea22 0079016688556c0f
300 e03010da e20eea22 00790166898aca13
360 ab33367a e20eea22 007901668ac0022b
420 49d94a91 e20eea22 007901668bf58cfb
480 0257a9c4 e20eea22 007901668d2ad487
540 4f90877b e20eea22 007901668e602191
600 1c8411d4 e20eea22 007901668f956df7
660 7768dbd2 e20eea22 0079016690caba61
```

The evidence directory contains private derived game/module/user data and must
not be published. It remains valid evidence for the correction semantics just
described and includes the post-fix rerun, but it is not a release artifact.

Exercise ordinary player activation with a private prepared package (no test
fault environment):

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package "$RINGOUT_PRIVATE_PACKAGE" --production
```

The final retained matrix used the same runtime/module/DOL hashes as the
production correction run:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-rollback-private.mGbUYiaf/package \
  --work /tmp/ringout-live-rollback.final-clean.wPXzfTPg \
  --play-seconds 18 --port 28842 --production

RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  RINGOUT_ROLLBACK_HORIZON_FRAMES=1 \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-rollback-private.mGbUYiaf/package \
  --fault-script .github/input-scripts/rollback-fault-horizon.txt \
  --work /tmp/ringout-live-rollback.final-horizon.QGQ1Cu8O \
  --play-seconds 12 --port 28847 --expect-horizon

RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-rollback-private.mGbUYiaf/package \
  --work /tmp/ringout-live-rollback.final-digest.iay4Pxu8 \
  --play-seconds 4 --port 28848 --expect-digest-mismatch 60
```

The clean production route produced 673 host / 674 guest physical rows and
eleven equal confirmed checkpoints with SHA-256
`c46aa3945276228a7d1f5ff592487413b6bad53a2c2e42770ad110091c8a9e91`.
The one-frame-horizon route produced 641 byte-identical physical rows, ten
equal confirmed checkpoints with SHA-256
`5b71411c759c0441d42f4ccf0c022bb11697d0095afd040d80a2cce7bfdcecb2`,
logged fallback and resume on both peers, and used schedule
SHA-256
`fb5440b5b91778871e3d5e103b8475969489cecc3977ee1f87ddc422dea70ac3`.
The report-only fault flipped the guest's frame-60 MEM1 CRC report without
changing RAM; both peers logged `DESYNC at frame 60` and stopped. The final
fixed-delay run at `/tmp/ringout-fixed-delay-final-3a7e` retained 2,958 byte-identical
rows; `h.trim` and `g.trim` both hash to
`bd76b76faa049e7e9e9dee0a3bf1ae3be9173b9ea15ed2f532204b5596fa3cb3`.

After building a private package from a disc owned by the tester, exercise a
bounded correction:

```bash
read -r -p "Private prepared package: " RINGOUT_PRIVATE_PACKAGE
export RINGOUT_PRIVATE_PACKAGE
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package "$RINGOUT_PRIVATE_PACKAGE" \
  --fault-script .github/input-scripts/rollback-fault-correction.txt
unset RINGOUT_PRIVATE_PACKAGE
```

Then exercise horizon fallback:

```bash
read -r -p "Private prepared package: " RINGOUT_PRIVATE_PACKAGE
export RINGOUT_PRIVATE_PACKAGE
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  RINGOUT_ROLLBACK_HORIZON_FRAMES=1 \
  .github/scripts/rollback-live-real-game.sh \
  --package "$RINGOUT_PRIVATE_PACKAGE" \
  --fault-script .github/input-scripts/rollback-fault-horizon.txt \
  --expect-horizon
unset RINGOUT_PRIVATE_PACKAGE
```

Earlier 2026-08-25 branch-matching real-game results are retained privately at
`/tmp/ringout-live-rollback.hEwKT03c` (correction, 676/679 physical rows) and
`/tmp/ringout-live-rollback.t0JzxAlv` (horizon recovery, 686/687 physical rows).
Both use runtime SHA-256
`c4bdff2b1e4207850f35c3d5b8bb75492f3623ee0b8bd74ae0089c415672988e`.
The harness never discovers, copies, extracts, or uploads a disc image. Its
private `/tmp` evidence directory contains peer logs, hash traces, generated
controller configuration, isolated user data copied from the prepared private
package, PIDs, and artifact SHA-256 values. Treat the entire directory as
private test material.

The older rollback-disabled fallback at
`/tmp/ringout-fixed-regression-20260825` produced 2,452 byte-identical rows; it
is superseded by the 2,958-row final fixed-delay result above.
