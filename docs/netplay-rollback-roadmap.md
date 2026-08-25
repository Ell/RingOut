# Netplay rollback and verification roadmap

Audit target: `ff0ad952980f5083afd21c3d3758208a7a093d72`

Audit date: 2026-08-25

Scope: shipped input scheduling and transport, existing rollback feasibility hooks,
historical measurements, and a staged implementation/test plan. Source paths and
line numbers are relative to the repository root and refer to the audited commit.

This report is intentionally blunt about the audited baseline: **At
`ff0ad952`, RingOut shipped fixed-delay deterministic lockstep and did not ship
rollback netcode.** The
repository has useful save/restore/replay experiments, but none is connected to
the live NetPlay input path. The user-facing feature list saying “rollback” is
incorrect (`README.md:127`); the earlier description saying fixed delay is the
accurate one (`README.md:33-35`).

## Executive verdict

| Area | Rating | Verdict at the audited commit |
| --- | ---: | --- |
| Trusted LAN/private VPN play | 7/10 | A credible fixed-delay implementation with synchronized settings and good historical determinism evidence. |
| Unstable/WAN responsiveness | 3/10 | Reliable ordered input, static delay, and blocking on an empty queue turn loss and jitter into stalls. |
| Deterministic-core evidence | 7/10 | Stronger than a typical prototype, but the best complete-match evidence is same-host historical evidence, not a current cross-platform gate. |
| Rollback state groundwork | 4/10 | In-memory snapshots and an offline replay probe exist; snapshot completeness, catch-up speed, and side effects remain open. |
| Shipping rollback | 0/10 | No prediction, frame-stamped input, snapshot ring, correction, resimulation, or output reconciliation exists in the live session. |

Recommended product wording until the staged plan is complete:

> Fixed-delay netplay for trusted peers. Rollback is experimental research and is
> not available in releases.

## Branch implementation progress after the audit (2026-08-25)

The “ships today” and staged sections below are intentionally preserved as the
historical `ff0ad952` baseline. The `codex/rollback-netplay` worktree based on
`6518db52` has since implemented the core of Stages 1, 3, 4, 5, and 6:
versioned/generation-keyed SI batches, per-pad ACK repair, bounded repeat-last
prediction, a broad Dolphin checkpoint ring, late-input restore/replay, hidden
replay output handling, explicit Ready, exact connect fingerprint/mode, and a
hard horizon. The later worktree also snapshots guest-visible memory-card
protocol/content state, holds output quarantine through fault/cancel/destructor
quiescence, and treats a failed corrected-frontier barrier as a hard rollback
fault. Current source anchors are
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayConnectProtocol.h:17-95`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:995-1045,1630-1741`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientRollback.cpp:35-270`,
and `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/LiveRollbackOutputGate.cpp:130-188,212-338`.

The branch also implements a local, opt-in Stage-2-style confirmed logical-state
oracle. It logs real MEM1, locked L1, and emulated timebase every 60 completed
logical frames only after incoming input is drained, the frame is authoritative,
no correction is pending, and replay is committed; GPU work is synchronized
before the read. The harness compares the last canonical row for matching
frames after both local processes exit. Ordinary players neither write nor
transmit this evidence
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientRollback.cpp:171-228`).

The post-fix production-path run retained at
`/tmp/ringout-live-rollback.final-correction.OHN0EzDz` used
runtime/module/DOL SHA-256 values
`3a7e27d7420ac9ae49eca997e0301a972f3a3799997dcff9a2920f098b1351ee`,
`e01d1fc7f14d41cf170fb5b036e5c754cb3062b8e5421f147258b627e2931d48`,
and `0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5`.
It captured 48,213,190 host bytes and 48,213,199 guest bytes (45.98 MiB each).
The host-only impairment delayed the host's changed input, and the guest
corrected restore/replay ranges 25/27 from batch 21 and 137/137 from batch 133.
The peers matched all eleven confirmed logical checkpoints from frame 60
through 660.
The matching confirmed files hash to
`4969a73790008801a05a27522d2508a7ffceb32d181a55be1b2f5d14caec9795`.

That rerun closes the three previously recorded fault-path blockers for the
tested Linux/source route. Clean production, isolated one-frame horizon
stall/resume, isolated frame-60 mismatch/stop, and a 2,958-row fixed-delay
regression also passed; see the live harness document for exact directories,
commands, and hashes. Stage 7 is still incomplete because complete tagged
Windows/AppImage artifacts and physical/cross-machine impairment testing remain
release gates. The later Online Room beta uses Dolphin's hosted room-code
rendezvous, but gameplay is still unauthenticated/unencrypted direct UDP for
trusted friends. Relay, authenticated rooms, encryption, and IP privacy remain
future work; see `docs/netplay-connectivity.md`.

## What ships today

### Session mode is explicitly fixed delay

RingOut forces Dolphin's network mode to `fixeddelay`
(`ModernGekko/tools/netplay_session.cpp:813-824`) and the host disables host-input
authority (`ModernGekko/tools/netplay_session.cpp:912-923`). Host and guest
therefore use the same symmetric client path; the host itself connects through a
loopback `NetPlayClient` (`ModernGekko/tools/netplay_session.cpp:924-928`).

The current flow is:

```text
SI begins a controller-poll batch
        |
        v
each client samples each locally owned pad
        |
        +-- fill its local FIFO to target + 1 with that sample
        |
        +-- send the appended records to the host
                    |
                    +-- validate sender owns each pad
                    +-- relay records to every other client
        |
        v
each client pops one queued sample for every mapped pad
        |
        +-- queue has data: advance emulation with the same delayed input
        +-- queue empty: wait; do not advance or predict
```

The relevant implementation steps are exact:

1. At the first pad in an SI batch, `GetNetPads` polls every local pad and sends
   one batched `PadData` packet when records were appended
   (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:50-67`).
2. In fixed-delay mode, `PollLocalPad` repeats the current pad state until its
   local FIFO size is greater than the target
   (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:285-305`).
   That bulk repetition seeds the initial delay; steady state normally appends one
   new sample after each old sample is consumed.
3. The server validates the pad mapping and disconnects a sender claiming another
   player's port, then relays the serialized records
   (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:682-729`).
4. A receiver pushes those records into its pad FIFO
   (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:674-694`).
5. The CPU thread waits while the next pad FIFO is empty and then pops exactly one
   sample (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:105-115`).

There is no branch in that path which guesses a missing remote input. There is no
record of predicted versus confirmed input, no snapshot selection, no rewind, and
no replay. Empty input stops progress instead of producing a speculative frame.

### The buffer unit is an SI input sample, not a 60 Hz frame

This distinction is load-bearing for both latency claims and a future rollback
protocol.

The lobby calls the control `Input buffer (frames)` and displays
`buffer * 1000 / 60` milliseconds
(`ModernGekko/tools/netplay_session.cpp:703-714`). The queue, however, is filled
and drained by controller polls. Dolphin says the SI input-update rate is
variable and typically 120 Hz
(`ModernGekko/vendor/dolphin/Source/Core/Core/HW/SI/SI.cpp:551-557`), batches the
four ports around that update (`ModernGekko/vendor/dolphin/Source/Core/Core/HW/SI/SI.cpp:551-585`),
and routes each controller read through `GetNetPads`
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1807-1816`).

Consequences:

- A target of five means approximately five **SI samples** of delay. At a typical
  120 polls/s that is roughly 41.7 ms, not the UI's 83.3 ms. The exact delay can
  vary with the guest's SI schedule.
- It is unsafe to model one network input record as one rendered/emulated frame.
  A frame can contain a variable number of SI polls.
- A future wire identifier needs at least an emulated frame number plus an
  in-frame SI-poll ordinal, or a monotonically increasing SI sample ID whose
  frame association is recorded locally.
- Performance and user-facing telemetry must report both units: base delay in SI
  samples and observed equivalent milliseconds. “Frames” should be reserved for
  emulated frame boundaries.

The server initializes the target to five
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:167-173`).
`--buffer auto` does not measure RTT or jitter and does not adjust anything; it
merely leaves that fixed default in place
(`ModernGekko/tools/netplay_session.cpp:912-923`). Manual values are broadcast to
clients as `PadBuffer`
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServerInput.cpp:97-110`).

### Transport behavior

`PadData` records contain a pad index and controller fields, but no frame number,
SI sample ID, generation, acknowledgement, or checksum
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1186-1197`).
The protocol defines only a default channel and a chunked-data channel
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayProto.h:226-235`).
Inputs share the default channel with ordinary control traffic.

Every ENet packet is created with `ENET_PACKET_FLAG_RELIABLE`, regardless of
channel (`ModernGekko/vendor/dolphin/Source/Core/Common/ENet.cpp:40-63`). This
ensures eventual ordered delivery while connected, but it also means a lost
packet can hold later input behind retransmission. The fixed FIFO hides bounded
jitter; when it drains, the emulation thread waits. The no-acknowledgement timeout
is 30 seconds
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayCommon.h:18-21`).

A mapped non-host peer disconnect ends the game for everyone; there is no pause,
reconnect, migration, or state resynchronization
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:523-579`).
Rollback could conceal a short late-input window, but it must not predict through
an unbounded outage. A hard rollback horizon followed by an explicit pause or
disconnect is required.

Security work is a prerequisite to adding a more complex rollback protocol. The
current repeated-record parsers do not fail closed on truncated records
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:691-714`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:674-694`),
and the session has no application authentication or encryption. See
`docs/netplay-protocol-security.md` for the full trust-boundary audit. New input,
hash, checkpoint, and reconnect messages must use bounded decoders, negotiated
versions, explicit maximums, and authenticated room membership.

### Native desync detection is a weak clock check

Once per emulated frame, the CPU thread calls `SendTimeBase`
(`ModernGekko/vendor/dolphin/Source/Core/Core/Core.cpp:136-143`). The client only
sends when its local counter is divisible by 60; the packet contains the fake
timebase and that counter
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1623-1640`).
The server groups values by counter and reports a desync if peers' timebases differ
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:936-977`).
The client only reports the notification; it does not repair state
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:986-1003`).

Equal timebases establish only that clocks agree at that checkpoint. MEM1, L1,
DSP/ARAM, device state, GPU-visible state, and the static recompiler's metadata
can differ while the clock remains equal. This check is useful as a cheap alarm,
not proof of deterministic equality and not a rollback confirmation mechanism.

### Deterministic setup is valuable but not sufficient

RingOut does make several good choices for lockstep: stock CPU/VI clocks and
100% emulation speed are forced, strict settings sync is enabled, netplay codes
and saves are synchronized, background input is forced, and deterministic GPU
processing is used with dual core
(`ModernGekko/tools/netplay_session.cpp:778-853`). Dolphin's netplay configuration
layer synchronizes CPU, DSP, memory, exception, MMU, fastmem, and graphics
settings and disables nondeterministic AA under strict sync
(`ModernGekko/vendor/dolphin/Source/Core/Core/ConfigLoaders/NetPlayConfigLoader.cpp:33-129`).

The stronger ModernGekko compatibility fingerprint covers the revision, disc ID,
DOL hash, module/CPU ABI, `CPUState` size, recompiled ranges, SMC ranges, and chunk
hashes (`ModernGekko/tools/netplay_compatibility.cpp:67-96`). It is exercised by a
unit test but is not sent in the live handshake
(`ModernGekko/tests/netplay_protocol_test.cpp:178-198`). It must become a live,
fail-closed negotiation field before rollback relies on byte-identical execution.

## What rollback would require, and what is absent

| Rollback requirement | Audited status |
| --- | --- |
| Frame/SI-stamped local and remote input | Absent; `PadData` is an unnumbered stream. |
| Prediction policy | Absent; an empty queue blocks. |
| Confirmed-input frontier | Absent. |
| Snapshot/delta ring | Absent from live NetPlay; one offline buffer exists in the probe. |
| Restore to the earliest wrong prediction | Absent. |
| Fast resimulation without presenting speculative frames | Absent. |
| Speculative audio/video reconciliation | Absent. |
| Frame-keyed state hashes | Absent; only timebase is exchanged. |
| Bounded rollback horizon and delay fallback | Absent. |
| Rollback-aware reconnect/resync | Absent. |

The repository's rollback hooks are offline feasibility tools:

- `State::SaveToBuffer` and `LoadFromBuffer` are synchronous, uncompressed,
  CPU-thread primitives
  (`ModernGekko/vendor/dolphin/Source/Core/Core/State.h:128-136` and
  `ModernGekko/vendor/dolphin/Source/Core/Core/State.cpp:252-286`).
- `RecompDeterminism` can save at one configured frame, run forward, restore,
  rewind the scripted-input offset, replay the same number of frames, and compare
  the resulting guest-memory hash
  (`ModernGekko/vendor/dolphin/Source/Core/Core/RecompDeterminism.cpp:84-95`,
  `ModernGekko/vendor/dolphin/Source/Core/Core/RecompDeterminism.cpp:263-269`, and
  `ModernGekko/vendor/dolphin/Source/Core/Core/RecompDeterminism.cpp:462-518`).
- Environment-gated skip bits can omit fake VMEM, MEM1 padding, video state,
  ARAM, and rollback-only JIT invalidation
  (`ModernGekko/vendor/dolphin/Source/Core/Core/State.h:100-125` and
  `ModernGekko/vendor/dolphin/Source/Core/Core/State.cpp:94-115`).

Those hooks answer “can this state be restored and replayed in one controlled
experiment?” They do not schedule prediction or correction in a live session.

## Historical evidence and its boundaries

The following results are preserved in commit messages and source comments. They
are valuable measurements from the repository's development history, but they
were **not rerun as fresh performance benchmarks on the audit machine at
`ff0ad952`**. Hardware, build flags, route, and package may differ from a release
user's system.

| Commit | Historical measurement | What it proves | What it does not prove |
| --- | --- | --- | --- |
| `619767f0` | At frame 600 with a 60-frame replay: 106.57 MiB full state; 21.17 ms warm save, 70.83 ms cold save, 38.18 ms restore; replayed guest-memory hash matched. | Full-state restore plus identical scripted input can reproduce the checked RAM over that window. | A live rollback scheduler, gameplay/audio safety, cross-platform equality, or frame-budget viability. |
| `4a877ca5` | 32 MiB MEM1 arena (24 MiB real plus 8 MiB padding), 32 MiB fake VMEM, 26.23 MiB video, 16.01 MiB ARAM, 0.25 MiB L1, about 0.08 MiB other state; fake VMEM all zero and MEM1 arena 8.3% nonzero in that run. | Most serialized bytes came from sections that might be narrowed for this title. | That skipped sections never change, or that occupancy alone makes omission correct. |
| `2e93f53e` | Full 106.57 MiB / 24.42 ms save / 32.02 ms restore; narrowing through VMEM, padding, video, and ARAM reached 24.34 MiB / 3.06 ms / 21.81 ms, with each 60-frame replay matching. Restore attribution included HW 3.09 ms, IBAT 1.74 ms, DBAT 1.54 ms, and JitInterface 15.04 ms. | Narrowing solved the measured save-time problem in the sampled window and identified restore's dominant cost. | Safe omission during an entire match, streaming audio, or an efficient correction. |
| `1f16b92a` | Narrow state plus rollback-only JIT-clear skip: 24.34 MiB, 2.95 ms save, 6.66 ms restore, replay matched. Full and narrow comparison values remained 24.42/32.02 ms and 3.06/21.81 ms. | A warm snapshot and restore can individually fit within 16.7 ms on the measured machine. | Enough catch-up throughput, safe JIT metadata after SMC, or complete output reconciliation. The commit itself says the test was a 60-frame menu window. |
| `07de096e` | Two local fixed-delay peers reported 6,470 identical per-frame guest-RAM hashes. An earlier clean-looking run was rejected because NetPlay had not actually armed. | A correctly armed same-host session can remain RAM-identical for that observed route; it documents an important false-pass guard. | Rollback, physical networking, unlike CPUs/OSes, or gameplay input from both directions. |
| `e467c92a` | A scripted VS route reached 8,635 identical frames with both players contributing input and both health bars damaged. | Strong same-host end-to-end evidence for pad routing and deterministic match play. | WAN behavior or automatic regression gating. The script can print a divergence without returning failure. |

The two historical netplay commits also establish an important testing rule:
byte-identical peers can be identically wrong. `e467c92a` caught a route which
pressed Back and another whose short settle time left both clients on character
select, perfectly synchronized. State equality must be paired with screen/game
progress assertions and proof that both peers' inputs affected gameplay.

## Snapshot budget and performance math

“Each operation is under one frame” does not mean rollback fits. A live client
pays to checkpoint every emulated frame and, on correction, pays to restore and
simulate every invalid frame again.

| Stored checkpoints | Full, 106.57 MiB each | Narrow, 24.34 MiB each |
| ---: | ---: | ---: |
| 1 | 106.57 MiB | 24.34 MiB |
| 4 | 426.28 MiB | 97.36 MiB |
| 8 | 852.56 MiB | 194.72 MiB |
| 12 | 1,278.84 MiB | 292.08 MiB |

At 60 emulated frames/s, a full copy moves about 6.24 GiB/s; the narrow copy
still moves about 1.43 GiB/s. The historical 2.95 ms narrow save consumes 177 ms
of CPU-thread time per emulated second, or 17.7% of a 60 Hz timeline. A 6.66 ms
restore is another 40% of one 16.67 ms frame. Save plus restore is 9.61 ms, or
57.7% of that frame, before replay. The ring sizes exclude input/hash history,
audio/video queues, alignment, and allocator metadata.

An approximately 195 MiB eight-frame narrow ring is plausible, but a reversible
delta format should be tested: retain occasional bases, save small CPU/device
state, and record the pre-image of each page first dirtied per frame. All DMA
writers must participate. The existing module journal cannot see DVD/DSP/AI DMA
which runs from CoreTiming
(`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore_Run.cpp:659-668`).
Compare every delta restore against periodic complete restores; page faults,
write barriers, or hashing may cost more than the contiguous copy.

### JIT-clear omission needs a stronger invariant

The best restore skips JIT invalidation because a rollback state comes from the
same session (`ModernGekko/vendor/dolphin/Source/Core/Core/State.h:117-124`);
ordinary load clears it
(`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/JitInterface.cpp:50-57`).
“Same session” alone is not enough. Speculation can encounter self-modifying
code, create fallback blocks, or change static-recompiler chunk-verification
state. Restoring RAM does not replay the write hooks which invalidated those
artifacts. `StaticRecompCore::ClearCache` resets fallback and chunk state
(`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.cpp:426-444`).

Before using this skip live, checkpoint relevant JIT/chunk metadata, track
code-page generations and selectively invalidate, or find a faster conservative
clear. Tests must cross SMC and fallback events; a 60-frame menu replay is not
sufficient.

## Steam Deck catch-up risk

Rollback needs spare simulation throughput. New frames keep arriving while old
ones are replayed; at or below real time, backlog cannot close. The repository
records 45–49 displayed fps in a Deck match (`README.md:21-22`) and a historical
PC-to-Deck netplay change from 41 to 46 fps
(`ModernGekko/tools/netplay_session.cpp:786-805`). Those are not fresh results or
no-present simulation benchmarks, but they make clear that headroom cannot be
assumed. Snapshotting adds the historical 2.95 ms/frame.

Measure normal p50/p95/p99 frame time, checkpoint-only overhead, and restore plus
1/2/4/8 no-render replay frames at character select, effects-heavy gameplay, and
streaming audio, including a 20-minute thermal soak. Require at least 1.25x
sustained no-present simulation speed for a four-frame Deck horizon; 1.5x is a
safer target. Otherwise retain adaptive fixed delay on Deck.

## Staged hybrid rollback design

### Stage 0: honest fixed-delay telemetry

- Correct `README.md:127`; call the buffer SI input samples, not frames.
- Show observed SI rate, milliseconds, RTT, jitter, occupancy, and underruns.
- Make `auto` derive a bounded sample target from jitter and change it only at
  synchronization-safe points.
- Make manual probes fail nonzero and require game-progress assertions as well as
  equal hashes.

### Stage 1: version and stamp input

Negotiate a new protocol generation and send a logical batch containing:

```text
session_generation, SI_batch_id, emulated_frame, poll_ordinal,
owned_pad_mask_and_states, contiguous_ack, previous_K_batches
```

The monotonically increasing SI batch ID is the primary input key. Frame plus
poll ordinal recreates the exact SI schedule after restoring a frame checkpoint.
Increment the generation on start/reset/reconnect so stale packets cannot enter a
new game.

Run this protocol in fixed-delay mode first and require the same state trace as
the legacy path. Keep control/hash/save messages reliable, but use a dedicated
sequenced-unreliable input channel with a small redundant tail and receiver-side
deduplication. This avoids reliable head-of-line blocking while allowing the next
packet to repair isolated loss. Bound the tail by MTU and measured burst loss.
The server must still enforce ownership and reject malformed, conflicting,
future, ancient, or cross-generation batches. Negotiate the existing complete
fingerprint (`ModernGekko/tools/netplay_compatibility.cpp:67-96`) and harden/authenticate
the protocol before accepting untrusted peers.

### Stage 2: exchange confirmed-frame state evidence

The harness already hashes OS globals, other guest RAM, and L1 per frame
(`ModernGekko/vendor/dolphin/Source/Core/Core/RecompDeterminism.cpp:440-460`). Add
CPU registers, CoreTiming, SI, DSP/ARAM, EXI, guest-visible GPU state, and JIT
generations. Retain at least the rollback window plus 300 frames; exchange a
compact digest every ten confirmed frames and a component breakdown every 60.
Key it by session generation/frame and compare confirmed frames only. On mismatch,
stop prediction and preserve the first component/input trace. CRC32 is diagnostic,
not hostile integrity; authenticate the transcript separately. Symmetric peers
have no inherently correct authority, so do not silently overwrite one state.

### Stage 3: prove compact checkpoints offline

- Test complete match, character select, FMV, save access, and streaming audio.
- Compare full components after 1/2/4/8/30/60-frame restores.
- Force SMC, fallback JIT, DMA, EFB readback, and ARAM changes.
- Cross-check deltas against complete checkpoints and preallocate the ring.
- Fail on any state marker/read failure; do not continue with partial state.

Exit only with exact restore on every target plus measured memory and catch-up
budgets. No live prediction belongs in this stage.

### Stage 4: build an offline correction oracle

Deliver real scripted input late to one process. Predict last-known input within a
hard horizon, checkpoint at defined frame boundaries, and record every SI batch.
On mismatch restore before the earliest wrong batch, replay known/predicted batches
in exact SI order without presenting intermediate output, and stop at the old
speculative frontier. For every confirmed frame require:

```text
hash(inputs known on time) == hash(predicted, corrected, and replayed)
```

Fuzz duplicate, reordered, simultaneous, wraparound, reset, and disconnect cases.

### Stage 5: reconcile output and side effects

**Video:** speculative frames may render normally, but replay frames must not be
presented; show only the newest correction. The narrow probe skips the video
backend whose normal state path precedes hardware
(`ModernGekko/vendor/dolphin/Source/Core/Core/State.cpp:183-205`). Prove all
guest-visible EFB/XFB state and RAM writeback is restored/rebuilt; only host caches
may be discarded.

**Audio:** associate blocks with emulated frames. Hold them until confirmation or
use bounded speculative audio with a short correction crossfade; never emit replay
audio twice. Include ARAM initially. Replace its 16 MiB only after a complete
dirty-page scheme passes streaming-audio tests.

**Other effects:** defer or ID-deduplicate file writes, achievements, rumble, and
host callbacks. Preserve the current ban on host-timed FFmpeg takeover during
netplay/determinism
(`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore_Run.cpp:380-399`).

### Stage 6: opt-in bounded hybrid

Start with about 2–4 **SI samples** of base delay (roughly 1–2 nominal 60 Hz
frames at the typical SI rate) and a separate 2–4 **emulated-frame** rollback
horizon. Track prediction accuracy, rewind depth, restore/replay cost, audio
corrections, and backlog. Increase sample delay if p99 jitter/correction cost
threatens the horizon; decrease slowly. Fall back to higher fixed delay for the
match when a device misses its catch-up budget, and report why.

The lobby must negotiate a common protocol, fingerprint, checkpoint support, and
horizon. Add expected-player and Ready state; today the host can start with only
itself and treats `SameGame` as ready (`ModernGekko/tools/netplay_session.cpp:721-733`).
On disconnect, predict only to the horizon, then pause. Resume only through an
authenticated reconnect which proves player/session identity and a mutually
verified checkpoint; otherwise end explicitly, not after 30 seconds of invented
input.

### Stage 7: release gates

Keep fixed delay as the safety fallback. Promote rollback only after Linux,
Windows, and Deck—including mixed OS/CPU—pass the reference, impairment, output,
thermal, and multiplayer gates below.

## Reproduction and test gates

Run from the repository root. The current built protocol test was executed during
this audit with localhost networking available and returned 0:

```bash
./build-appimage/moderngekko_netplay_protocol_test
```

It covers fingerprint equality/change, invalid host, two clients, roster and
buffer round-trips, mapping, valid owner input, and rejecting an input impostor
(`ModernGekko/tests/netplay_protocol_test.cpp:178-298`). It deliberately does not
boot the core or consume the far-side pad
(`ModernGekko/tests/netplay_protocol_test.cpp:260-264`). Its target/CTest name are
defined at `ModernGekko/CMakeLists.txt:568-602`. Windows cross-builds currently
disable tests (`.github/workflows/windows-cross.yml:125-142`).

With a privately prepared package, rerun deterministic, local, and match probes:

```bash
.github/scripts/determinism-check.sh \
  dist/RingOut-1.0-deck 1200 .github/input-scripts/arcade-match.txt
HASH=1 .github/scripts/netplay-local.sh \
  dist/RingOut-1.0-deck /tmp/ringout-netplay-local 60 2626
.github/scripts/netplay-match.sh \
  dist/RingOut-1.0-deck /tmp/ringout-netplay-match 60 2640
```

The first uses isolated users and per-frame hashes
(`.github/scripts/determinism-check.sh:35-96`). The local probe requires
`netplay armed` and can diff every shared RAM frame
(`.github/scripts/netplay-local.sh:89-148`); the match probe drives both players
(`.github/scripts/netplay-match.sh:106-151`). At this commit the latter scripts
can print failure without exiting nonzero
(`.github/scripts/netplay-local.sh:170-193`,
`.github/scripts/netplay-match.sh:163-175`), so they are diagnostic until fixed.

Run the existing rollback probe and compare once with no skip mask:

```bash
RINGOUT_AUDIT_PKG="$PWD/dist/RingOut-1.0-deck"
RINGOUT_AUDIT_WORK="$(mktemp -d /tmp/ringout-rb.XXXXXXXX)"
RINGOUT_AUDIT_MODULE="$(find "$RINGOUT_AUDIT_PKG/bin" -maxdepth 1 \
  -type f -name 'g*_recomp.so' -print -quit)"
env RINGOUT_DETERMINISM_LOG="$RINGOUT_AUDIT_WORK/hashes.log" \
  RINGOUT_DETERMINISM_FRAMES=900 \
  RINGOUT_DETERMINISM_INPUT="$PWD/.github/input-scripts/arcade-match.txt" \
  RINGOUT_DETERMINISM_ROLLBACK_AT=600 \
  RINGOUT_DETERMINISM_ROLLBACK_LEN=60 RINGOUT_STATE_BREAKDOWN=1 \
  RINGOUT_ROLLBACK_SKIP=vmem,pad,video,aram,jitclear \
  "$RINGOUT_AUDIT_PKG/bin/moderngekko-run" --headless \
  --user-dir "$RINGOUT_AUDIT_WORK/user" --game "$RINGOUT_AUDIT_PKG/game" \
  --module "$RINGOUT_AUDIT_MODULE"
```

`MATCHED` checks the probe's guest-memory hash, not skipped audio/video or every
subsystem. Recover historical measurement text with:

```bash
git show -s --format=fuller \
  619767f0 4a877ca5 2e93f53e 1f16b92a 07de096e e467c92a
```

### Impairment and cross-platform matrix

On a disposable Linux runner, apply one impairment at a time; preserve the seed
and traces. A smoke profile is:

```bash
sudo tc qdisc replace dev lo root netem delay 40ms 10ms \
  loss random 1% duplicate 0.2% reorder 1% 25%
trap 'sudo tc qdisc del dev lo root 2>/dev/null || true' EXIT
HASH=1 .github/scripts/netplay-local.sh \
  dist/RingOut-1.0-deck /tmp/ringout-netem 90 2660
```

Matrix points: one-way delay 0/20/40/80/120 ms; jitter 0/5/15/30 ms; random loss
0/0.1/0.5/1/3/5%; bursts of 2/4/8; reorder and duplicate 0/1/5%; outages
100/250/500/1000 ms and beyond the horizon; concurrent save/chunk traffic. Use
network namespaces for asymmetric profiles. Assert confirmed-state equality,
correction depth/catch-up deadline, no duplicate output, and explicit fallback.

Run Linux↔Linux per PR; native Windows↔Windows and Windows↔Linux nightly; and
physical/self-hosted Deck ARM64↔x86-64 with thermal soak. Add three/four-player
mapping and disconnect cases. Archive input batches/frontiers, component hashes,
rollback timings, occupancy/RTT/jitter/loss, screen-progress assertions, audio
event IDs, both logs, artifact fingerprints, and the impairment seed. Synthetic
game/module fixtures should gate parsers/scheduling without private assets; fuzz
new packet decoders under ASan/UBSan.

## Claim boundaries

- Current behavior is source-audited only at
  `ff0ad952980f5083afd21c3d3758208a7a093d72`.
- The localhost protocol test passed during this audit, but does not boot a game.
- The six commit results are historical records, not freshly reproduced
  benchmarks; their exact machine/build environment was not reconstructed.
- Existing `MATCHED` means one checked RAM hash matched after one replay. It does
  not prove skipped video, ARAM, JIT metadata, output, or complete state.
- The 6,470/8,635-frame runs were same-host historical tests, not impaired or
  cross-platform sessions. Deck fps is likewise historical; no catch-up benchmark
  was run in this audit.
- Memory math is exact arithmetic from recorded sizes; delta design, transport,
  thresholds, and CI plans are proposals until implemented and measured.
- This audit changes documentation only. It does not add rollback to releases.
