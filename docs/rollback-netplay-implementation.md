# Rollback netplay implementation handoff

Status: first implementation slice on branch `codex/rollback-netplay`, based on
commit `ff0ad952980f5083afd21c3d3758208a7a093d72`, recorded 2026-08-25.

This document records branch-local work. It does **not** change the shipped
verdict: RingOut's live netplay remains fixed-delay lockstep. The new rollback
timeline is not wired to the live input protocol, snapshot restore, replay,
audio, or video paths.

## Outcome of this iteration

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
- The user-facing feature list now calls current netplay fixed-delay and labels
  rollback as groundwork.

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

Do not promote the historical 24.34 MiB narrowed snapshot yet. Its 2.95 ms save
and 6.66 ms restore were measured in one 60-frame menu window, while omitted
video, ARAM, and JIT-clear behavior remains unproven. First run complete-match,
FMV, audio, DMA, EFB, save, and SMC/JIT correction oracles with full state.

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

1. Add a versioned, fail-closed SI batch codec and protocol-generation
   negotiation while continuing to consume it in fixed-delay mode.
2. Exchange the existing full game/module fingerprint before roster admission.
3. Add real Ready/Not Ready, mapping acknowledgement, capacity enforcement, and
   typed lobby outcomes.
4. Build the offline correction oracle for zero/one/multiple SI polls per frame,
   then run full-state restore/replay across complete gameplay/audio routes.
5. Add confirmed-frame component digests and archive the first mismatch with its
   input journal and build fingerprint.
6. Only after correctness and catch-up gates pass, wire an opt-in two-frame
   hybrid rollback mode with fixed-delay fallback.
