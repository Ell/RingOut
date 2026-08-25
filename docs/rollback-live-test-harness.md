# Live rollback integration test harness

Status: branch-local executable contract and passing opt-in real-game harness,
recorded 2026-08-25 against implementation commit `38d69d84`.

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
match route against a privately prepared package. It requires both peers to log
`[rollback live] negotiated` and rejects desync/failure markers. The correction
mode requires the exact `correction committed` marker. `--expect-horizon`
instead requires both a hard-horizon marker and a later `horizon resumed`
marker, followed by successful route completion. A stall without recovery
fails the harness.

Rollback replay creates additional physical-frame hash rows, so the legacy
line-for-line fixed-delay hash comparison is not a valid correction oracle in
fault mode. Both traces are retained. The live harness currently combines the
negotiation/correction evidence with Dolphin's live desync detector; it does not
claim byte-identical final confirmed state until the live integration emits a
frame-keyed confirmed-state hash suitable for comparing peers.

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
  horizon and retains four consecutive sends until ordinal 37. The receiver
  freezes guest execution, retransmits while stalled, and resumes when the
  retained packets release.

The seam must not delay ENet control packets, handshake packets, or fixed-delay
`PadData`; it targets authoritative RSIB sends only. It must remain disabled in
ordinary sessions and release builds unless both explicit test gates are
present.

The wrapper sets the acknowledgement, base delay, and horizon on both peers.
Only the guest receives the fault-script path, so the delayed authoritative
direction is unambiguous in retained evidence.

## Reproduction

The key implementation anchors at commit `38d69d84` are:

- headless-only opt-in and host configuration in
  `ModernGekko/tools/netplay_session.cpp:76-99,912-973`;
- channel validation, session parsing, bounded RSIB decode, and match reset in
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:363-368,829-890,1527-1545`;
- frame-boundary restore/replay/commit and grouped/unbatched SI consumption in
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientRollback.cpp:37-220,225-382`;
- mid-frame correction-frontier extension in
  `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/RollbackInputTimeline.cpp:85-140`;
- the deliberately incomplete production gate and isolated complete test gate
  in `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/LiveRollbackOutputGate.cpp:48-68`.

Configure/build and run all focused asset-free contracts:

```bash
ctest --test-dir /tmp/ringout-rollback-integration-build2 \
  --output-on-failure \
  -R '^moderngekko\.(live_rollback_gate|rollback_coordinator|rollback_input_timeline|rollback_si_input_journal|rollback_live_contract|rollback_live_scheduler|rollback_live_harness|netplay_protocol)$'
```

Test the shell evidence gate without private assets:

```bash
.github/scripts/test-rollback-live-real-game-harness.sh
```

The exact live commands used for the evidence below were:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-live-package.USuoYFcK \
  --fault-script .github/input-scripts/rollback-fault-correction.txt \
  --play-seconds 1 --port 2681

RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  RINGOUT_ROLLBACK_HORIZON_FRAMES=1 \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-live-package.USuoYFcK \
  --fault-script .github/input-scripts/rollback-fault-horizon.txt \
  --play-seconds 1 --port 2682 --expect-horizon

PKG=/tmp/ringout-live-package.USuoYFcK \
  .github/scripts/netplay-match.sh \
  /tmp/ringout-fixed-regression-20260825 1 2683
```

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

The 2026-08-25 branch-matching real-game results are retained privately at
`/tmp/ringout-live-rollback.hEwKT03c` (correction, 676/679 physical rows) and
`/tmp/ringout-live-rollback.t0JzxAlv` (horizon recovery, 686/687 physical rows).
Both use runtime SHA-256
`c4bdff2b1e4207850f35c3d5b8bb75492f3623ee0b8bd74ae0089c415672988e`.
The harness never discovers, copies, extracts, or uploads a disc image. Its
private `/tmp` evidence directory contains peer logs, hash traces, generated
controller configuration, isolated user data copied from the prepared private
package, PIDs, and artifact SHA-256 values. Treat the entire directory as
private test material.

The rollback-disabled fallback was also rerun at
`/tmp/ringout-fixed-regression-20260825`: the two peers completed the VS route
with 2,452 byte-identical comparable guest-RAM hashes.
