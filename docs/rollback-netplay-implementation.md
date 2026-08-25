# Rollback netplay implementation handoff

Status: live branch integration on `codex/rollback-netplay`, originating at
commit `38d69d84` and finalized at implementation commit `6518db52` on
2026-08-25.

This document records branch-local work. It does **not** change the published
release verdict: published RingOut netplay remains fixed-delay lockstep. The
branch now has an end-to-end live rollback path—normal launcher selection,
strict fingerprint/mode negotiation, Ready-gated start, grouped SI prediction,
checkpoint restore, corrected replay, output quarantine, atomic journal commit,
and a local confirmed logical-state convergence oracle. The executable
contracts, fault grammar, exact commands, and retained live evidence are
cataloged in [the live rollback test harness](rollback-live-test-harness.md).

The production capability predicate is currently enabled and the ordinary
`--netplay-mode rollback` path has completed a real two-process correction run
without the isolated test acknowledgement. The later memory-card snapshot,
fault/cancel/destructor output-quiescence, and corrected-frontier hard-fault
changes have all landed, and the post-fix production correction rerun passed.
This is still not a final player-ready/release verdict. Full Windows/AppImage
package validation exists only as CI workflow/smoke logic in this worktree; no
tagged artifact or physical/cross-machine run validates it.

## Live integration outcome

The two-process path now exercises the real game and emulator rather than a
model:

- `NetPlayServer` queries rollback capability, selects an exact all-peer v1
  session or fixed-delay fallback, assigns a fresh nonzero generation, and
  relays only bounded RSIB packets whose pad masks belong to the sender.
- RingOut now carries its existing game/DOL/recomp-module compatibility
  fingerprint in a bounded, versioned extension to the initial NetPlay hello.
  The server validates the complete extension before allocating a player ID.
  Matching modern peers are admitted; malformed or mismatched identities are
  rejected with distinct connection errors, and the launcher maps an actual
  `CompatibilityMismatch` to its existing player-facing diagnosis. Legacy
  extension-less peers remain possible only for generic Dolphin servers which
  do not provide RingOut's identity; RingOut servers require the exact extension
  in fixed-delay and rollback modes. A requested-mode mismatch is rejected at
  connect time instead of silently downgrading. This is compatibility
  validation, not authentication;
  the plaintext fingerprint is still self-asserted by the remote process.
- RingOut lobbies opt in to an authoritative Ready/NotReady relay using the
  existing protocol IDs. Headless peers become Ready only after their own game
  status and controller mapping have landed; interactive peers use the same
  state through a lobby toggle. Start is rejected unless every mapped player is
  Ready. Game, controller mapping, input-delay/mode changes, roster joins, and
  disconnects clear prior readiness. The gate is disabled by default for
  external legacy Dolphin callers.
- RSIB uses a third ENet channel with unreliable-sequenced delivery. Lobby,
  control, saves, compatibility, and legacy `PadData` remain reliable.
- `LiveRollbackInputScheduler` samples local GC pads ahead by the negotiated
  base delay, predicts missing remote pads by repeat-last, records the real
  `(emulated_frame, poll_ordinal)` mapping, and blocks at a conservative batch
  horizon that cannot outrun the snapshot ring. Its RSIB sender now consumes
  contiguous ACKs independently for every remote pad stream. Until all streams
  acknowledge a batch, the sender retains it in a separate protocol-bounded
  512-batch history (about 4.3 seconds at the typical 120 Hz SI rate) and
  transmits the oldest unacknowledged gap alongside the newest two-batch tail.
  The retransmit window is deliberately independent of the much shorter rollback
  journal/snapshot horizon. This closes the case where every ordinary copy of one
  batch is lost but newer actual input keeps prediction moving forever.
  Stale/reordered ACKs are ignored; ACKs beyond locally produced input and
  ambiguous mixed-owner packet masks fail closed.
- `NetPlayClientRollback` owns the CPU-thread session. It captures broad Dolphin
  frame-start states, drains authoritative input before each frame boundary,
  restores the earliest affected checkpoint, replays every recorded SI poll,
  atomically acknowledges the journal generation, and publishes only after a
  complete corrected frontier. Corrections arriving between two polls extend
  through the later poll before the boundary; unbatched `RunBuffer` transfers
  are independent journal entries. While horizon-blocked it keeps
  retransmitting the redundant input tail so symmetric stalls recover.
- The output layer suppresses hidden-replay presentation and frame dumping,
  resets/silences replay audio, suppresses rumble, achievements, movie/log
  output, persistent writes, guest-network output, and replay-derived netplay
  traffic, and uses a GPU barrier when publishing the corrected frontier. A
  session quarantine prevents speculative persistent/host effects. Fault,
  cancellation, and destruction retain that quarantine until the core/output
  producers quiesce; a failed corrected-frontier barrier faults the coordinator
  and journal rather than returning Ready.

The isolated test gate still requires headless execution and
`RINGOUT_ROLLBACK_TEST_ACK=HEADLESS_ISOLATED`. The production gate is separate:
the launcher exposes rollback only when
`NetPlay::IsLiveRollbackProductionReady()` is true, production start verifies a
GameCube-only read-only/safe-device policy, and every peer must request and
support the exact mode. No environment variable enables the ordinary player
path (`ModernGekko/tools/moderngekko_launcher.cpp:1197-1215,1253-1272`;
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1644-1697`).

### Current real-game evidence

Ordinary production activation and bounded correction:

```text
evidence:                 /tmp/ringout-live-rollback.final-correction.OHN0EzDz
session:                  generation 1, base delay 2 SI samples, horizon 8 frames
checkpoint host/guest:    48,213,190 / 48,213,199 bytes (45.98 MiB each)
correction 1:             restore 25, replay through 27, first batch 21, committed
correction 2:             restore 137, replay through 137, first batch 133, committed
physical trace rows:      717 host / 723 guest (717 canonical rows each)
confirmed logical frames: every 60 frames from 60 through 660; all equal
confirmed-file SHA-256:   4969a73790008801a05a27522d2508a7ffceb32d181a55be1b2f5d14caec9795
runtime SHA-256:          3a7e27d7420ac9ae49eca997e0301a972f3a3799997dcff9a2920f098b1351ee
module SHA-256:           e01d1fc7f14d41cf170fb5b036e5c754cb3062b8e5421f147258b627e2931d48
DOL SHA-256:              0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5
```

Exact command:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-rollback-private.mGbUYiaf/package \
  --fault-script .github/input-scripts/rollback-fault-correction.txt \
  --work /tmp/ringout-live-rollback.final-correction.OHN0EzDz \
  --play-seconds 18 --port 28841 --production
```

The CPU-thread logger drains late input before the boundary, logs only a
completed authoritative logical frame with the coordinator Ready and no replay,
and synchronizes the GPU before computing real-MEM1, locked-L1, and timebase
values (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientRollback.cpp:198-317`).
The logs are opt-in local files. Rollback protocol v2 separately sends the same
compact confirmed-state report every 60 logical frames so the server can stop a
live mismatch; it never serializes full emulator state. The shell harness
canonicalizes matching logical frame IDs and fails divergent, malformed, empty,
or short evidence.

The fault schedule applies only to the host. The host changes input first; its
late authoritative values make the guest perform both corrections. This
retained run proves that the post-fix ordinary player activation path reached
live rollback and converged after two late-authoritative corrections.
The private evidence/package are not distributable artifacts.

The same runtime/module/DOL identity also passed:

- clean ordinary production activation at
  `/tmp/ringout-live-rollback.final-clean.wPXzfTPg`, with 673 host / 674 guest
  physical rows, eleven matching confirmed checkpoints, and confirmed-log
  SHA-256
  `c46aa3945276228a7d1f5ff592487413b6bad53a2c2e42770ad110091c8a9e91`;
- isolated one-frame-horizon stall/resume at
  `/tmp/ringout-live-rollback.final-horizon.QGQ1Cu8O`, with 641 identical
  physical rows, ten matching confirmed checkpoints, confirmed-log SHA-256
  `5b71411c759c0441d42f4ccf0c022bb11697d0095afd040d80a2cce7bfdcecb2`, and
  fault-schedule SHA-256
  `fb5440b5b91778871e3d5e103b8475969489cecc3977ee1f87ddc422dea70ac3`;
- isolated confirmed-report-only corruption at frame 60 at
  `/tmp/ringout-live-rollback.final-digest.iay4Pxu8`; both peers reported `DESYNC at frame
  60` and stopped, while guest RAM was never modified; and
- fixed-delay regression at `/tmp/ringout-fixed-delay-final-3a7e`, with 2,958
  byte-identical rows and common trim SHA-256
  `bd76b76faa049e7e9e9dee0a3bf1ae3be9173b9ea15ed2f532204b5596fa3cb3`.

The following correction and horizon runs are earlier isolated-gate evidence:

Correction run:

```text
evidence:              /tmp/ringout-live-rollback.hEwKT03c
session:               generation 1, base delay 2 SI batches, horizon 8
physical hash rows:    676 host / 679 guest retained traces
result:                two restore/replay/commit cycles, no fault or desync
runtime SHA-256:       c4bdff2b1e4207850f35c3d5b8bb75492f3623ee0b8bd74ae0089c415672988e
```

The exact runtime hash and private module/DOL hashes are stored in
`rollback-result.env`; private game-derived files remain outside Git. The
distributed correction schedule spans repeated scripted START transitions and
the harness refuses to pass unless a real prediction mismatch produces a
`correction restore_frame=...` followed by `correction committed`.

Horizon/recovery run:

```text
evidence:              /tmp/ringout-live-rollback.t0JzxAlv
session:               generation 1, base delay 2 SI batches, horizon 1
physical hash rows:    686 host / 687 guest retained traces
result:                explicit hard-horizon stall and authoritative-input resume,
                       followed by restore/replay/commit, no fault or desync
```

Physical hash rows are not compared line-for-line after rollback because peers
can execute different numbers of speculative/replay frames. The retained logs,
live desync detector, explicit restore/commit markers, and current confirmed
logical-frame oracle have distinct evidence boundaries. The latter now supplies
an end-to-end real-MEM1/L1/timebase convergence assertion at authoritative
checkpoints; it is not a complete serialization of every emulator subsystem.

Rollback-disabled compatibility was rechecked with the same runtime and private
package at `/tmp/ringout-fixed-delay-final-3a7e`: 2,958 comparable guest-RAM rows
were byte-identical and the scripted VS route completed. The common trim hash is
`bd76b76faa049e7e9e9dee0a3bf1ae3be9173b9ea15ed2f532204b5596fa3cb3`;
this remains the stronger whole-run oracle available to the shipped fixed-delay
path.

### Production save/persistence policy

Rollback can load and synchronize the starting memory-card view, but it must not
commit speculative state to user storage. When the negotiated rollback session
settings arrive, the client forces `savedata_write=false` and
`allow_sd_writes=false`, disables SP1/SP2, and accepts only the production
policy's safe GameCube memory-card/device set
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1127-1146,1644-1697`;
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/LiveRollbackOutputGate.cpp:130-188`).
Achievements, recordings/logging, and guest/outbound effects are likewise
quarantined. Players must treat rollback-session save progress as temporary.

The snapshot now includes guest-visible memory-card command/protocol/content
state so a correction rewinds the emulated device even though rollback-session
changes are never copied back to disk. The write quarantine and rewind coverage
are distinct safeguards.

## Outcome of this iteration

The branch now crosses the save/restore/resimulation boundary in an offline
real-game oracle:

- `DolphinRollbackStateStore` keeps a bounded ring of broad frame-start states
  using Dolphin's real `State::SaveToBuffer` and
  `State::LoadFromBuffer`. It is CPU-thread-only and fails closed for zero
  capacity or experimental snapshot skip masks. A rollback-only scope excludes
  inaccessible MEM1 allocator padding and uses a tagged, lossless fake-VMEM
  representation: an observed all-zero window has no payload, while any
  non-zero byte selects a full copy for that checkpoint. Restoring the narrow
  MEM1 form explicitly zeroes its excluded padding. Video state, ARAM, and JIT
  invalidation are not skipped, and ordinary user savestates keep their prior
  byte format. The rollback scope now also serializes the guest-visible memory-
  card protocol/content state omitted by the earlier evidence run.
- `RollbackCoordinator` validates SI-batch-to-emulator-frame replay mappings,
  restores before the first affected emulated frame, enforces a bounded replay
  horizon and strict replay order, supports chained corrections, and requires
  an output gate before it permits state to move backwards.
- `RollbackSIInputProtocol` supplies a bounded, versioned `RSIB` codec with a
  session generation, scheduled SI batch IDs, optional contiguous ACK, and
  canonical big-endian pad serialization. Its decoder rejects malformed,
  oversized, wrong-generation, invalid-mask, invalid-flag, and non-increasing
  input. Authority and conflicting-actual checks belong to the journal layer.
- `RollbackSIInputJournal` records where each scheduled SI batch was actually
  consumed as `(emulated_frame, poll_ordinal)`, performs repeat-last remote
  prediction, and converts late authoritative corrections into coordinator
  replay ranges. Grouped SI updates and unbatched guest transfers both resolve
  the complete active-pad set; a future dynamic required-pad mask would be
  needed before partially sampled batches could be represented directly.
- `.github/scripts/rollback-real-game.sh` runs an authoritative baseline,
  withholds transitions to force a wrong prediction, restores the full emulator
  state, replays corrected input, and requires every corrected game-memory and
  L1 hash to converge to the baseline. The orchestration has an asset-free
  regression test in `.github/scripts/test-rollback-real-game-harness.sh`.

The following bullets describe the prior offline slice and remain useful as
component provenance. They are no longer the complete branch status.

## Real-game correction result

The private `GRSEAF` revision-0 game image was converted and prepared only in a
mode-700 directory under `/tmp`; no game-derived data was added to Git. The
branch-matched runtime and recompilation module ran the `arcade-match.txt` route
with a full snapshot and a 30-frame correction at frame 5200.

Observed result:

```text
snapshot size:              106.57 MiB
warm save:                  10.57 ms
cold measure/allocate/save: 18.32 ms
restore:                     8.43 ms
predicted endpoint:          927329fa
authoritative endpoint:      b45041a6
corrected endpoint:          b45041a6
pre-prediction diff:         empty
speculative mismatch diff:   non-empty
corrected replay diff:       empty
```

Exact command:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-real-game.sh \
  --package /tmp/ringout-rollback-private.mGbUYiaf/package \
  --input .github/input-scripts/arcade-match.txt \
  --rollback-at 5200 --rollback-len 30 \
  --work /tmp/ringout-rollback-private.mGbUYiaf/evidence-core-final
```

The runtime SHA-256 was
`11841e3dfda8e43a0ed4637e21319446e983dbea0dec9b2295f2fe82f6d1ec4f`.
The correction log marked save, restore, and completion with `[rollback core]`.

The 106.57 MiB result above predates the rollback-scoped safe memory encoding.
At the 2026-08-25 branch integration finalized at `6518db52`, the focused format
test verifies the one-byte all-zero representation, lossless non-zero fallback,
fail-closed invalid tag handling, and deterministic MEM1-padding reconstruction:

```bash
cmake --build /tmp/ringout-rollback-integration-build2 \
  --target moderngekko_rollback_memory_snapshot_test --parallel 8
ctest --test-dir /tmp/ringout-rollback-integration-build2 --output-on-failure \
  -R '^moderngekko\.rollback_memory_snapshot$'
```

Result: `1/1` passed. The final live run above records exact 45.98 MiB checkpoint
sizes and correction behavior. It does not record a new capture/restore timing
budget, so the old timing numbers remain historical rather than current
performance evidence.

This is executable evidence that a deliberately wrong speculative input path
can be corrected through `DolphinRollbackStateStore`, `RollbackCoordinator`,
and the journal's atomic acknowledge-and-publish path for this real match
window. The oracle's journal token is synthetic because input remains
frame-keyed rather than SI-poll-keyed. It is not evidence that live transport,
production `GetNetPads` replay, catch-up pacing, presentation, audio, or
irreversible side effects are rollback-safe.

The oracle's snapshot is taken at the end of logical frame 5200 and replays
5201 through 5230. It binds that state to coordinator checkpoint 5201—the state
immediately before frame 5201—and completes coordinator frames 5201 through
5230 while retaining the harness's existing physical hash-row numbering. This
exercises the convention at `Core::FrameUpdateOnCPUThread`; live SI integration
must preserve the same mapping for variable poll schedules.

The harness-only output gate performs no suppression. It is acceptable only
for this headless oracle with an isolated disposable user directory, and it
does not weaken the production gate's fail-closed contract.

The branch now has a tested rollback scheduling foundation and a safer lobby
view:

- `Core/NetPlay/RollbackInputTimeline.{h,cpp}` owns bounded frame-indexed input
  history for four pads. It distinguishes local, remote, and inactive pads;
  repeats the last known remote input; requires local actual input; stops at a
  hard prediction horizon; advances a contiguous confirmed frontier; reports
  the earliest incorrect prediction and replay frontier; and uses generation
  tokens so a replay acknowledgement cannot erase a newer correction.
- Timeline storage rejects conflicting actual input, wrong authority, ancient
  input, excessive future input, and history exhaustion. Confirmed input is not
  pruned until the core has resolved it at least once, and pending correction
  history is retained.
- `NetPlayClient::GetPlayers()` returns copied `Player` values instead of raw
  pointers into a network-thread-owned map. RingOut, VideoCommon, and DolphinQt
  call sites use the owned snapshot.
- Interactive hosting now requires the configured expected player count as
  well as matching game status before Start is enabled. This is not a full
  Ready protocol; per-player Ready/Not Ready still needs a versioned message.
- The local and match harnesses now exit nonzero for failure to arm, reported
  desync, hash divergence, empty/short hash evidence, or surviving emulator
  processes. The match route still needs an automatic visual/game-state progress
  assertion; identical hashes alone cannot prove it reached a match.
- At that earlier slice, the user-facing feature list called netplay fixed-delay
  and labelled rollback as groundwork; the final branch implementation
  supersedes that historical UI state.

## Research decision

Use GGPO/GGRS/GekkoNet as algorithm references, but keep the RingOut scheduler
owned by this codebase.

The [GGPO developer guide](https://github.com/pond3r/ggpo/blob/master/doc/DeveloperGuide.md)
establishes the core contract: fixed-step deterministic simulation, saved game
states, repeatable input-driven advance, bounded prediction, restore, and
re-simulation without presenting intermediate output. Current
[GGRS documentation](https://docs.rs/ggrs/latest/ggrs/) adds an explicit
confirmed frontier, prediction threshold, input delay, time-synchronization
recommendations, and a sync-test session which continuously restores and
replays.

[GekkoNet at `5924b5c`](https://github.com/HeatXD/GekkoNet/tree/5924b5c7abb5b1156c3c5609c9c36e9bede58c1c)
is a useful C++20 reference for limited saving, stress sessions, replay,
spectators, and event-driven save/load/advance requests. Its BSD-2-Clause
license is compatible, but vendoring it now would add a second transport and
serialization stack without solving RingOut's main mismatch: GekkoNet consumes
one opaque input per fixed frame, whereas RingOut has a variable number of SI
polls inside an emulated frame. A correct live packet therefore needs both an
authoritative SI sequence and its frame/poll position.

The 2026 [RMG-K](https://github.com/Jay-Day/RMG-K) emulator integration is useful
evidence that full-emulator rollback can be practical. It also demonstrates how
much integration remains outside the scheduler: safe frame boundaries, dynarec
restore, hidden replay frames, pacing, audio/video suppression, input recording,
spectator keyframes, ready synchronization, connect codes, and NAT traversal.
Its rapid pre-1.0 iteration and lack of public rollback correctness/netem gates
make it a case study, not release proof for RingOut.

## Target live architecture

### Input identity and correction

The authoritative input key should be:

```text
session_generation, si_batch_id, emulated_frame, poll_ordinal,
owned_pad_mask_and_states, contiguous_ack, previous_K_batches
```

`si_batch_id` orders input. `emulated_frame` plus `poll_ordinal` reproduces the
exact SI schedule after restoring the checkpoint immediately before the frame
containing the earliest wrong batch. Repeat the last confirmed remote state,
use neutral before the first sample, and stop prediction at the negotiated
horizon. Start conservatively with 2-4 SI samples of base delay and at most two
emulated frames of rollback; expand only after target-specific catch-up proof.

Input should use a dedicated unreliable-sequenced ENet channel with a bounded
redundant tail. Lobby, compatibility, hashes, saves, and control remain reliable
on independent channels. Reject wrong generation, ownership, size, conflicting
duplicates, ancient batches, and excessive future batches before they reach the
timeline.

### State and replay

Do not promote the historical 24.34 MiB skip-mask snapshot. Its 2.95 ms save and
6.66 ms restore were measured in one 60-frame menu window while omitting video,
ARAM, and JIT-clear behavior that remains unproven. The integrated rollback-only
memory encoding is a narrower claim: inaccessible MEM1 padding is reconstructed
as zero, and fake VMEM is omitted only when that exact checkpoint observes every
byte as zero. Any non-zero fake VMEM is preserved in full. Complete-match, FMV,
audio, DMA, EFB, save, and SMC/JIT correction oracles are still required with
video, ARAM, and normal JIT invalidation included.

Then compare a preallocated snapshot ring with confirmed-base plus speculative
checkpoints and dirty-page/preimage deltas. Every correction must suppress
intermediate presentation, restore before the earliest incorrect SI batch,
replay all poll ordinals through the old speculative frontier, and present only
the corrected frontier. File writes, achievements, rumble, replay writes, and
similar effects must be deferred as confirmed-frame intents. Audio needs
frame/event identity, deduplication, and a correction fade rather than duplicate
replay output.

### Lobby and connectivity

The lobby should be an authenticated control plane, separate from gameplay
input:

- A room has an opaque ID/token, owner, privacy, capacity, protocol generation,
  game/module fingerprint, supported checkpoint mode, rollback horizon, and
  fixed-delay fallback.
- A member has an authenticated identity, role, transport state, Ready state,
  controller mapping, compatibility result, and acknowledged start generation.
- Only the owner can change mapping/options, kick, or start. Start freezes active
  membership and requires every active player to be connected, compatible,
  mapped, Ready, and data-plane-acknowledged.
- Internet connectivity should use ICE/STUN with TURN/relay fallback rather than
  exposing a raw IP in an invite. See [RFC 8445](https://www.rfc-editor.org/rfc/rfc8445)
  and [RFC 8656](https://www.rfc-editor.org/rfc/rfc8656). Valve's
  [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
  is a later transport candidate because it supplies reliable/unreliable
  messages, encryption, ICE/custom signaling, network statistics, and lanes.

Before public rooms, the parser wedge, save path traversal, unsafe LZO bounds,
host-ID race, authority matrix, and authentication findings in
`netplay-protocol-security.md` remain release blockers.

## Verification performed

The final current-worktree integration run passed these focused tests:

```bash
ctest --test-dir /tmp/ringout-rollback-integration-build2 \
  --output-on-failure \
  -R '^moderngekko\.(rollback_memory_snapshot|live_rollback_gate|rollback_coordinator|rollback_input_timeline|rollback_si_input_journal|rollback_live_contract|rollback_live_scheduler|rollback_state_digest|rollback_live_harness|frontend_config)$'

ctest --test-dir /tmp/ringout-rollback-integration-build2 \
  --output-on-failure -R '^moderngekko\.netplay_protocol$'
```

Result: the focused set passed 10/10 in 0.26 seconds; the separate localhost
protocol route passed 1/1 in 20.54 seconds; and
`.github/scripts/test-rollback-live-real-game-harness.sh` passed without private
assets. The earlier incremental checks below remain useful provenance.

The original integration slice used:

```bash
cmake --build /tmp/ringout-rollback-integration-build2 \
  --target moderngekko_rollback_input_timeline_test \
  moderngekko_rollback_si_input_journal_test \
  moderngekko_rollback_coordinator_test \
  moderngekko_netplay_protocol_test moderngekko-run --parallel 8
ctest --test-dir /tmp/ringout-rollback-integration-build2 --output-on-failure \
  -R '^moderngekko\.(rollback_input_timeline|rollback_si_input_journal|rollback_coordinator|netplay_protocol)$'
```

Result at that earlier slice: `4/4` passed. The asset-free real-game harness regression, shell syntax
checks, and `git diff --check` also passed. The coordinator, SI journal, and
codec were additionally compiled with `-Wall -Wextra -Wpedantic -Werror
-fno-exceptions -fno-rtti`.

The ACK-driven gap-repair update was verified from a runtime-free build during
the branch integration with:

```bash
cmake -S ModernGekko -B /tmp/ringout-rollback-ack-build \
  -DMODERNGEKKO_ENABLE_DOLPHIN_RUNTIME=OFF -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/ringout-rollback-ack-build \
  --target moderngekko_rollback_live_scheduler_test \
  moderngekko_rollback_live_contract_test \
  moderngekko_rollback_si_input_journal_test -j4
ctest --test-dir /tmp/ringout-rollback-ack-build --output-on-failure \
  -R '^moderngekko\.(rollback_live_scheduler|rollback_live_contract|rollback_si_input_journal)$'
```

Result: `3/3` passed on 2026-08-25. The scheduler regression forces a local
batch outside the ordinary recent tail, proves it remains in the repair slot,
a delayed ACK remains recoverable for 64 batches despite a 16-entry rollback
journal and two-batch prediction horizon, proves the ACK then retires the TX
window, validates the explicit 512-batch bound, proves one remote pad stream
cannot retire it for another, proves all-stream ACK advancement retires it,
rejects a future ACK before input mutation, and accepts a reordered stale ACK
without regressing the watermark. The production
`LiveRollbackInputScheduler.cpp` and `NetPlayClientRollback.cpp` objects also
rebuilt successfully in `/tmp/ringout-rollback-build`.

The standalone rollback target was configured without the Dolphin runtime and
passed:

```bash
cmake -S ModernGekko -B /tmp/ringout-rollback-core-build \
  -DBUILD_TESTING=ON -DMODERNGEKKO_ENABLE_DOLPHIN_RUNTIME=OFF \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/ringout-rollback-core-build \
  --target moderngekko_rollback_input_timeline_test -j2
ctest --test-dir /tmp/ringout-rollback-core-build --output-on-failure \
  -R '^moderngekko\.rollback_input_timeline$'
```

Result: `1/1` passed. A direct GCC 16 build with `-Wall -Wextra -Wpedantic
-Werror -fno-exceptions` also passed. The no-exceptions check matters because
the production Dolphin core disables C++ exceptions; invalid timeline
configuration is therefore reported through `ConfigurationStatus` and result
codes rather than `throw`.

The migrated non-Qt lobby translation units were compiled against the existing
AppImage build flags, the relinked `moderngekko_netplay_protocol_test` exited 0,
and the relinked runner's `--help` exited 0. That protocol test now proves an
owned two-player snapshot remains valid after the live roster removes one peer.
Qt call sites were source-migrated but were not compiled because the available
AppImage profile has Qt disabled.

Repository checks:

```bash
bash -n .github/scripts/netplay-local.sh .github/scripts/netplay-match.sh
git diff --check
```

Both passed. A clean full-runtime build on this Arch/GCC 16 host initially
stopped in bundled SDL's `SDL_gtk.c`: its CMake probes failed to define
`HAVE_GETRESUID`/`HAVE_GETRESGID`, causing static fallback definitions to
conflict with glibc declarations. Reconfiguring with the probe results supplied
explicitly completed the production-core integration build and both tests:

```bash
cmake -S ModernGekko -B /tmp/ringout-rollback-integration-build2 -GNinja \
  -DCMAKE_BUILD_TYPE=Release -DLINUX_LOCAL_DEV=ON -DENABLE_QT=OFF \
  -DENABLE_TESTS=OFF -DENABLE_ANALYTICS=OFF -DENABLE_AUTOUPDATE=OFF \
  -DBUILD_TESTING=ON -DMODERNGEKKO_ENABLE_DOLPHIN_TESTS=OFF \
  -DHAVE_GETRESUID=1 -DHAVE_GETRESGID=1
cmake --build /tmp/ringout-rollback-integration-build2 \
  --target moderngekko_rollback_input_timeline_test \
  moderngekko_netplay_protocol_test --parallel 8
ctest --test-dir /tmp/ringout-rollback-integration-build2 --output-on-failure \
  -R '^moderngekko\.(rollback_input_timeline|netplay_protocol)$'
```

Result: `2/2` passed. Supplying those two SDL feature-probe values is a local
toolchain workaround, not a netplay code change.

## Next iteration

1. Build and smoke complete Windows/AppImage candidates through the workflows,
   then test two physical machines and mixed OS/CPU peers. No current tag
   artifact contains this branch.
2. Add dynamic required-pad masks and Wiimote support. The current live
   scheduler handles grouped GC `UpdateDevices` polls and unbatched guest
   transfers by resolving the complete mapped GC pad set; genuinely partial
   pad batches are not represented on the wire.
3. Add authenticated/encrypted room membership, traversal/relay, mapping
   acknowledgement, capacity enforcement, and typed lobby outcomes. The exact
   compatibility fingerprint and Ready protocol exist; they are not identity or
   transport security.
4. Extend the correction oracle across zero/one/multiple SI polls per frame,
   FMV, DMA, audio, EFB, saving, and SMC/JIT correction routes.
5. Expand the confirmed logical-state report beyond MEM1/L1/timebase and
   archive the first mismatch with its input journal and build fingerprint. Do
   not add full-state exchange without an explicit player-consent and trust
   design.
6. Add authenticated reconnect/resume, adaptive delay/pacing, and long-duration
   loss/reorder/netem tests for ACK-driven repair before a lobby-visible opt-in.
