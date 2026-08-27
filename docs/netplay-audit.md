# RingOut netplay audit

Status: read-only audit of commit `ff0ad952980f5083afd21c3d3758208a7a093d72`, recorded 2026-08-25.

This is the durable index for the current netplay research. It distinguishes what the release actually ships from experimental rollback groundwork and from proposed work. No production source was changed as part of the audit.

## Later branch implementation checkpoint (2026-08-25)

GPU-safety update 2026-08-26: player reports of repeated `GFX FIFO: Unknown
Opcode` failures and crashes invalidate the retained 2026-08-25 runs as a
renderer/FIFO release gate. Source review found that the lobby forced rollback
back to dual-core after offline play had already adopted a single-core safety
default, and the full savestate path did not quiesce the GPU across the whole
multi-threaded state transaction. Implementation commit `35c09137` on branch
`codex/rollback-gpu-state` changes the rollback default, adds whole-snapshot GPU
quiescence, and adds a real-renderer stress gate. Until that new gate passes
with a private game package, the older results below prove input correction and
confirmed memory convergence only.
See [rollback GPU-state research](rollback-emulation-gpu-state.md).

Performance-architecture update 2026-08-26: branch
`codex/sc2-slippi-rollback`, implementation commit `514f77e6` based on
`d5fd9426`, begins the game-specific path needed for Slippi-class performance.
It adds bounded once-per-frame SC2 hook discovery and a preallocated
selective-region checkpoint ring, but neither is selected by live netplay yet.
Follow-up commit `7ad94d48` used the owned USA image to pass a 600-frame
continuous restore/replay oracle and machine-verify `0x8001ba3c` as the 30 Hz
engine-iteration boundary on both peers. State regions and side-effect hooks
remain uncertified, so live netplay still uses whole-emulator checkpoints. See
[SC2 game-specific rollback](sc2-slippi-rollback.md).

A follow-up changed-input oracle also passed on both peers. It deliberately
toggled four remote SI polls across a two-engine-tick replay, proved the change
reached module-written game state, reproduced complete corrected endpoints,
restored the original endpoint, and completed the ordinary synchronized route.
The one-tick variant changed no game-owned bytes and was correctly rejected;
SC2 latches SI input into controller-conversion state on the following engine
tick. This establishes a corrected-input pipeline and its latency, but remains
an isolated full-state oracle rather than live selective correction.

The selective boundary was then widened from object update alone to the first
input-service/controller-conversion call through the first object-update
return. After transaction-adapting the root call's hardware-owned 371-byte
postimage, both peers reproduced complete endpoints while restoring only
24,829 exact game bytes across 46 observed pages. This still reuses predicted
raw input and is not live correction; corrected scheduler input must replace
the raw pad slot before controller conversion.

The follow-up corrected selective oracle now passes on two real peers. It
injects a scheduler-resolved remote A correction at SC2's SI-copy output,
executes the game's own controller conversion and update, and reproduces two
identical corrected complete endpoints while changing 17 game-owned bytes.
This closes the mechanism proof but not live activation: the ordinary player
coordinator still invokes the broad whole-emulator replay driver.

Follow-up commit `7efcceb3` measures the exact engine function's MEM1 write
footprint. A 60-tick idle control and 60-tick automated VS route passed on both
peers with identical per-route regions; their observed union is 52 pages
(212,992 bytes). This is a short-corpus write lower bound, not a certified
selective state profile, and it is not selected by live netplay.

Follow-up commit `304df33a` adds an exact engine-tick replay oracle. The first
raw MEM1/L1/CPU attempt failed because CoreTiming continued advancing; the
first full-state gameplay attempt then exposed that repeated SI polls consumed
new live scheduler batches. A bounded CPU-thread SI journal now records the
original tick's four resolved polls and reuses them without advancing network
state. The final automated VS run reproduced the complete roughly 47 MiB
endpoint byte-for-byte on both peers and completed 1,622 physical rows with
matching confirmed-state logs. This certifies one full-state gameplay tick and
the input-renewal mechanism, not a selective or player-facing engine rollback
path; render/audio/persistence suppression and state narrowing remain open.

Commit `dbb1682c` profiles the original engine pass's external accesses. The
automated VS sample was identical in aggregate on both peers: 103 MMIO reads,
104 MMIO writes, 31/40 distinct address-size sites, no gather-pipe traffic, no
interpreter fallback, and no profiler overflow. The accesses span CP, PE, VI,
PI, DSP, SI, and EXI. This rejects treating the current boundary as a pure
memory transform: it crosses hardware/interrupt service and requires dispatch-
PC attribution plus explicit hardware/output policy before selective replay.

Commits `86513abf` and `0a1dae8d` close that attribution gate. The outer tick's
PI/VI/DSP/CP/PE/EXI/SI accesses now have exact dispatch PCs, and a bounded write
journal decomposes 39 direct edges plus the indirect object-update phase. A
passing gameplay sample found that `0x8001bf58 -> 0x8000c1f4` is a 6,202-call
wait/service loop, while gameplay-sensitive virtual updates converge at
`0x80009888` inside `0x800095c0`. Two handlers own the sampled MMIO; the clean
handlers own the largest gameplay write footprints. This is a narrower
research target, not a shipped selective or live-correction path.

The 2026-08-26 selective object-update oracle subsequently passed a real
two-peer VS route at `/tmp/ringout-live-rollback.sc2-selective-transaction-28981`.
It restored roughly 23 KiB of exact module-CPU writes, transactionally adapted
the two observed system handlers, preserved the canonical hardware frontier,
and reproduced complete roughly 49.36 MB endpoints byte-for-byte on both
peers. Page-level attempts exposed asynchronous DSP/device RAM ownership;
exact-byte journaling fixed that boundary without excluding differences from
the gate. This is still not a player path: changed/corrected input through the
adapted handler and live correction scheduling remain unproven. See
[SC2 game-specific rollback](sc2-slippi-rollback.md).

The executive verdict below remains the historical result for audited commit
`ff0ad952`. It must not be read as a description of the current
`codex/rollback-netplay` worktree. At implementation commit `6518db52` on
2026-08-25, the branch has a normal-launcher-selectable
Experimental rollback path: an exact mode/fingerprint connect extension,
Ready-gated start, bounded SI input prediction and repair, broad Dolphin
checkpoints including guest-visible memory-card protocol/content state,
restore/replay, fault-latched output quarantine, and mandatory confirmed-state
comparison. The exact current
anchors are `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayConnectProtocol.h:17-95`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:1057-1110,1812-1850`,
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientRollback.cpp:198-317`,
and
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/LiveRollbackOutputGate.cpp:212-338`.

A production-path two-process run retained at
`/tmp/ringout-live-rollback.final-correction.OHN0EzDz` negotiated generation 1 with two SI
samples of base delay and an eight-frame horizon. Each peer captured a
frame-one checkpoint: 48,213,190 host bytes and 48,213,199 guest bytes (45.98
MiB each). The host's fault schedule impaired the direction in which the host
changed input; the guest corrected and
committed `restore_frame=25, replay_through_frame=27, first_batch=21`, then
`restore_frame=137, replay_through_frame=137, first_batch=133`. Both peers
produced identical confirmed logical-state rows at every 60-frame checkpoint
from 60 through 660; the complete files share SHA-256
`4969a73790008801a05a27522d2508a7ffceb32d181a55be1b2f5d14caec9795`.
Runtime, module, and DOL SHA-256 values were respectively
`3a7e27d7420ac9ae49eca997e0301a972f3a3799997dcff9a2920f098b1351ee`,
`e01d1fc7f14d41cf170fb5b036e5c754cb3062b8e5421f147258b627e2931d48`,
and `0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5`.

A 2026-08-26 instrumented correction at commit `7f7fad00`, retained at
`/tmp/ringout-live-rollback.performance-28996`, passed without a GPU crash but
measured 28.6-35.0 ms average whole-state capture and 16.4-17.0 ms restore.
The newly added sparse preimage ring benchmarks below a millisecond p95 restore
for the observed 24,829-byte SC2 transaction, but is not connected to the live
state store. This is performance evidence for changing the architecture, not
evidence that players currently receive selective rollback.

Commit `8c769987` subsequently exercised that ring through the real generated
SC2 module. Evidence at
`/tmp/ringout-live-rollback.sc2-sparse-corrected-28998` shows exact agreement
between the sparse journal and independent write profile (24,784 host / 24,829
guest bytes), a 17-byte corrected game-state change, and byte-identical full
endpoints. This closes one-transaction storage coverage only. The ordinary
player coordinator still selects whole-emulator checkpoints and remains the
only shipped live path.

Commit `c5d7ae8c` then attached exact rollback batch identity to each captured
SC2 controller transaction. In the retained two-peer run at
`/tmp/ringout-live-rollback.sc2-batch-map-28999`, both peers selected batch
1249 and passed sparse/full endpoint gates. The transaction timeline is tested
for 30/60 Hz mapping, grouped pads, unconsumed SI gaps, eviction, and invalid
identity. It is still integration groundwork rather than a shipped selective
coordinator.

Commit `864b705b` adds branchable sparse transaction ownership and a continuous
real-module shadow gate. Evidence at
`/tmp/ringout-live-rollback.sc2-transaction-history-29003` includes 565 exact
common network mappings and a 146-transaction safe epoch; incomplete fallback
spans are discarded rather than partially retained. This substantially lowers
state cost but does not alter the audit verdict: ordinary player corrections
still use broad whole-emulator restore/resimulation.

That post-fix run passed the ordinary production gate and closes the three
previously recorded memory-card, teardown-output, and corrected-frontier fault
blockers for this tested path. A clean production route retained at
`/tmp/ringout-live-rollback.final-clean.wPXzfTPg` produced 673 host / 674 guest
physical trace rows and 11 matching confirmed states. An isolated one-frame-
horizon run retained at `/tmp/ringout-live-rollback.final-horizon.QGQ1Cu8O`
stalled after a bounded 64-action drop schedule, resumed, and finished with 641
equal physical rows and 10 matching confirmed states. An isolated
report-only fault at logical frame 60 made both peers report `DESYNC` and stop
(`/tmp/ringout-live-rollback.final-digest.iay4Pxu8`). The fixed-delay final
regression retained at `/tmp/ringout-fixed-delay-final-3a7e` produced 2,958
byte-identical rows
with trim SHA-256
`bd76b76faa049e7e9e9dee0a3bf1ae3be9173b9ea15ed2f532204b5596fa3cb3`.
Public prerelease `v1.2.1-ell.11` now contains this path. Its Windows and
AppImage workflows passed their complete build, package-policy, Wine/AppImage,
and clean-host smoke gates at source commit
`a24a0cd5e0372d724a053992c11b56c1a554b087`. This supersedes the package/tag
gap recorded above, but no retained physical/cross-machine run validates the
release.

The later branch snapshot adds an Online Room beta through Dolphin's hosted
eight-character-code rendezvous, while retaining Direct IP under Advanced. The
transport remains unauthenticated, unencrypted direct UDP for mutually trusted
friends; there is no relay or IP privacy. The hosted rendezvous exchange passes,
but a complete independently routed two-machine gameplay run remains a release
gate. Authenticated ICE/TURN rooms remain proposed. See
[the implementation handoff](rollback-netplay-implementation.md) and
[live harness](rollback-live-test-harness.md) for current evidence.

The same later branch snapshot makes the desktop launcher the only visible
player entry point: the obsolete Direct-IP Host/Join/Scan rows are no longer in
the in-game System tab. A persisted detailed-diagnostics toggle enables
Dolphin's NETPLAY trace; the launcher captures it in `Logs/RingOut.log`, rotates
one `RingOut.previous.log`, exposes a Copy log path action, and warns that logs
can contain endpoint and local-machine information. This player-UX and
diagnostics checkpoint is commit
`6c89d6d4c801003fbe8cad34b1d8939998921750`, recorded 2026-08-25.

## Documents

- [Lobby architecture and UX](netplay-lobby.md): entry points, LAN discovery, direct connection, player mapping, start synchronization, and lobby defects.
- [Protocol, security, and reliability](netplay-protocol-security.md): packet flow, trust model, parser and save-transfer risks, compatibility, desync detection, and fail-closed remediation.
- [Rollback and test roadmap](netplay-rollback-roadmap.md): current fixed-delay algorithm, rollback experiments and measurements, test evidence, performance constraints, and a staged hybrid rollback design.
- [Rollback implementation handoff](rollback-netplay-implementation.md): the later branch-local live implementation, safety policy, evidence, and blockers.
- [Live rollback test harness](rollback-live-test-harness.md): exact real-game commands and confirmed logical-state comparison.
- [Rollback GPU-state research](rollback-emulation-gpu-state.md): emulated-versus-host GPU state boundary, FIFO crash root cause, transaction barrier, and renderer-backed gate.
- [SC2 game-specific rollback](sc2-slippi-rollback.md): game-loop replay architecture, automated hook discovery, selective-state groundwork, and release gates.

## Historical executive verdict at `ff0ad952`

RingOut does **not** ship rollback netcode. It ships symmetric, fixed-delay, deterministic lockstep using Dolphin's stock NetPlay protocol.

The implementation explicitly sets `NETPLAY_NETWORK_MODE` to `fixeddelay` and disables host-input authority so every peer consumes the same delayed inputs (`ModernGekko/tools/netplay_session.cpp:813-824`, `:912-920`). Missing remote input blocks the emulator (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClientInput.cpp:105-115`); it is never predicted, corrected, or replayed.

The README is internally inconsistent:

- `README.md:33-35` correctly describes fixed-delay play.
- `README.md:127` incorrectly advertises rollback.

Practical ratings for the audited revision:

| Area | Rating | Meaning |
| --- | ---: | --- |
| Trusted wired LAN or private VPN | 7/10 | Deterministic and usable, with good synchronization work |
| Lobby UX | 6/10 | Functional roster and discovery, but incomplete lifecycle handling |
| Internet/WAN responsiveness | 3/10 | Fixed delay, reliable-channel stalls, no adaptation or relay |
| Security against untrusted peers | 2/10 | Treat rooms as trusted-friend sessions until P0 hardening lands |
| Shipping rollback implementation | 0/10 | Absent; only offline research hooks exist |

## Current architecture

```text
local controller poll
        |
        +--> local N-sample FIFO ---------------------+
        |                                             |
        +--> reliable ENet --> host server --> peers  |
                                                      v
each emulator consumes the next sample for every mapped pad
                                                      |
                        missing sample --> wait; guest time does not advance
```

The host is also an ordinary local client connected through loopback. The server validates controller ownership before relaying `PadData` (`NetPlayServer.cpp:682-729`). This avoids a host-only input path and prevents one normal client from claiming another player's controller.

All ordinary NetPlay packets use reliable ENet delivery (`ModernGekko/vendor/dolphin/Source/Core/Common/ENet.cpp:40-63`). Gameplay input has no frame or sequence number. A lost packet therefore delays later input behind retransmission instead of allowing prediction or redundant recovery.

The buffer setting is measured in queued SI input samples, not video frames. Dolphin updates input at a variable SI rate, typically 120 Hz (`Core/HW/SI/SI.cpp:551-557`). The lobby labels the value as 60 Hz frames and computes milliseconds from 60 Hz (`netplay_session.cpp:703-714`), so that display is not authoritative. `auto` does not adapt: it leaves the fixed server default at five samples (`NetPlayServer.cpp:167-173`).

## What is implemented well

- Host and guests share the same client path.
- The server validates controller-port ownership.
- Pads are assigned deterministically by player ID, host first (`netplay_session.cpp:248-295`).
- Host settings, initial RTC, SRAM/save data, and enabled AR/Gecko codes are synchronized before boot.
- Strict settings synchronization is enabled and CPU/VI clocks plus emulation speed are pinned (`netplay_session.cpp:813-840`).
- A deterministic GPU mode is used with dual-core emulation, with a single-core diagnostic fallback.
- Background input is forced for netplay, avoiding silent neutral input when a peer loses window focus (`netplay_session.cpp:842-853`).
- Netplay overlay input is neutralized before it reaches the game (`NetPlayClientInput.cpp:189-217`).
- Save/load/reset/speed/rebinding/cheat mutation is locked during netplay.
- Historical manual tests reached 6,470 synchronized frames and an 8,635-frame VS match with input from both peers (`07de096e`, `e467c92a`).

These are substantial deterministic-lockstep results. They are not evidence of rollback.

## Highest-priority findings

### P0: do not treat direct Internet rooms as hostile-safe

Several packet parsers repeat while `!packet.endOfPacket()` but do not test the packet-valid flag after extraction (`NetPlayServer.cpp:691-713`; `NetPlayClient.cpp:674-694`). SFML marks a failed extraction invalid without necessarily advancing the read cursor (`Externals/SFML/SFML/src/SFML/Network/Packet.cpp:86-95`, `:572-578`). A truncated record can therefore wedge the network thread.

RingOut always enables host save synchronization. A host-provided GCI filename is concatenated without basename/path validation (`NetPlayClientSaves.cpp:111-143`), allowing writes outside the intended card folder. The shared decompressor also trusts a peer-provided `u64` allocation size and uses non-bounds-checking LZO decompression without exact output accounting (`NetPlayCommon.cpp:269-310`). These paths need fail-closed parsing, strict caps, safe decompression, and path containment before sessions with untrusted hosts are encouraged.

The protocol is not authenticated or encrypted. There is no room secret, and the host begins listening before its local client reserves player ID 1. LAN discovery is plaintext and spoofable. Any client can request StopGame or forward the power-button event (`NetPlayServer.cpp:888-933`).

### P0: live compatibility omits the recompilation module

`ModernGekko/tools/netplay_compatibility.cpp:67-96` already computes a useful fingerprint containing the RingOut revision, disc ID and DOL SHA, module/CPU ABI versions, `CPUState` size, module ranges, and chunk hashes. Only `ModernGekko/tests/netplay_protocol_test.cpp:191-198` calls it.

The live handshake checks the SCM revision (`NetPlayClient.cpp:241-250`, `NetPlayServer.cpp:443-464`) and later compares the `main.dol` sync identifier. A swapped, damaged, or differently generated recomp module can still appear ready. The existing fingerprint should be sent and rejected before the peer joins the room.

### P0/P1: built-in desync detection is only a clock check

Every 60 frames, a client sends `GetFakeTimeBase()` (`NetPlayClient.cpp:1623-1640`). The server compares those values (`NetPlayServer.cpp:936-977`). Two peers can retain equal clocks while RAM or another deterministic subsystem has diverged.

The manual scripts optionally hash guest RAM every frame because of this exact limitation (`.github/scripts/netplay-local.sh:89-93`). A production-friendly state digest should be keyed by emulated frame and exchanged periodically, with bounded history and a useful diagnostic on mismatch.

### P1: lobby lifecycle and concurrency need tightening

- There is no Ready protocol. `SameGame` is displayed as `ready`, and an interactive host can start with only itself (`netplay_session.cpp:721-733`).
- Headless start uses a fixed 500 ms sleep instead of a pad-map acknowledgement (`netplay_session.cpp:1018-1028`).
- `NetPlayClient::GetPlayers()` returns raw pointers into a mutable map after releasing its lock (`NetPlayClient.cpp:1159-1169`); the network thread can erase one while the lobby renders it.
- All connection rejections are flattened to `HostUnavailable`, leaving detailed launcher error cases unreachable (`netplay_session.cpp:935-943`).
- Lobby connection loss and user cancellation both become successful exit code 0 (`netplay_session.cpp:953-963`).
- The no-window path claims to auto-start, but returns without requesting a start (`netplay_session.cpp:601-605`).
- The server accepts more peers than the four mapped controller slots and has no explicit spectator model.

### P1/P2: Internet usability is direct-only

RingOut disables traversal and the public index, and constructs `NetPlayServer` without UPnP forwarding (`netplay_session.cpp:823-824`, `:872-884`). LAN scan is custom UDP broadcast; outside one broadcast domain, users need a manually supplied hostname/IP, UDP forwarding, or a VPN.

QWave/DSCP omission in the Windows package is not the architectural blocker. Traffic marking may help congested networks, but it cannot supply missing prediction, sequence numbers, recovery, authentication, or NAT traversal.

## Lobby summary

There are two entry paths:

1. The desktop launcher starts the runner with host/join arguments.
2. The in-game System menu writes `netplay-request.ini`, safely stops the offline runtime, and lets the runner delete and consume the request before rebuilding a pre-boot netplay session (`RecompMenu.cpp:1714-1737`; `moderngekko_run.cpp:397-478`).

A host binds the selected direct UDP port, normally 2626, and connects its own local client. While the interactive lobby is open it broadcasts `RINGOUT1 <port> <nickname>` once per second on UDP 2627 (`netplay_session.cpp:519-595`). The in-game scanner listens for 2.5 seconds and adopts the sender IP and advertised port (`RecompMenu.cpp:881-977`).

The lobby displays names, ping, assigned pad port, and game comparison. Starting synchronizes saves/codes/settings before delivering boot data. A mapped guest disconnect disables the game for everyone; the peer timeout is 30 seconds. There is no reconnect, resynchronization, host migration, password, kick UI, chat UI, or public matchmaking service.

## Rollback research status

The repository contains useful **offline feasibility tooling**, not live rollback:

- `State::SaveToBuffer` and `LoadFromBuffer` expose synchronous in-memory state (`Core/State.h:128-136`).
- `RecompDeterminism` can save at one frame, run forward, restore, replay identical scripted input, and compare guest RAM (`Core/RecompDeterminism.cpp:462-518`).
- Snapshot skip flags can experimentally omit VMEM, MEM1 padding, video, ARAM, and rollback-only JIT invalidation (`Core/State.h:100-125`).

Historical measurements:

| Commit | Result |
| --- | --- |
| `619767f0` | Full state 106.57 MiB; warm save 21.17 ms; restore 38.18 ms; 60-frame replay matched |
| `4a877ca5` | State breakdown: 32 MiB MEM1 arena, 32 MiB unused fake VMEM, 26.23 MiB video, 16.01 MiB ARAM |
| `2e93f53e` | Narrowed state 24.34 MiB; save 3.06 ms; restore 21.81 ms; replay matched |
| `1f16b92a` | With rollback-only JIT-clear skip: save 2.95 ms; restore 6.66 ms; replay matched |

The final result is encouraging, but its proof was a 60-frame menu window. It does not establish that omitting video and ARAM is safe during a live match or streaming audio. At 24.34 MiB, an eight-frame uncompressed state ring is about 195 MiB. Saving every frame costs roughly 3 ms, and a misprediction adds restore plus re-simulation. Deck builds currently have little obvious catch-up headroom.

True rollback therefore remains a separate emulator-core project: frame-numbered inputs, prediction, confirmed frames, a snapshot or delta ring, state-hash exchange, restore and fast re-simulation, render suppression, audio reconciliation, side-effect control, and cross-platform/network-impairment tests.

## Evidence and test boundaries

The current `moderngekko_netplay_protocol_test` passed for the audited commit when run with localhost networking available. It verifies:

- invalid-host failure,
- two local clients and roster exchange,
- pad-buffer message round trip,
- pad mapping,
- disconnecting a peer that sends input for a port it does not own.

It explicitly does **not** boot the core or assert that remote pad data reaches gameplay (`netplay_protocol_test.cpp:260-264`).

The manual scripts provide stronger observational evidence, including full guest-RAM hashes and a two-input VS route, but they currently print failure rather than returning nonzero (`netplay-local.sh:170-193`; `netplay-match.sh:163-175`). Workflows do not invoke them. Linux and Deck run the CTest suite; Windows cross-builds configure tests off and the Wine smoke test only loads the runtime far enough to print `--help` before exercising synthetic recompilation.

Not yet proven for this exact release:

- two physical Windows machines,
- a Windows/Linux or Windows/Deck live session,
- induced latency, loss, duplication, and reordering,
- NAT/firewall behavior,
- three- or four-player sessions,
- disconnect/reconnect behavior under load,
- hostile or fuzzed protocol inputs,
- narrowed rollback snapshots during a complete match with audio.

## Recommended implementation order

1. **Safety:** fail-closed packet decoding; sanitize and contain save paths; cap all peer-controlled sizes; use safe decompression with exact output accounting.
2. **Identity:** reserve the local host identity, add a random room token, authenticate the handshake, and restrict host-only operations.
3. **Compatibility:** put the existing game/module fingerprint into the live handshake and return a precise rejection reason.
4. **Lobby correctness:** value-based player snapshots, typed cancel/error outcomes, expected-player count, Ready state, four-slot/spectator policy, and explicit mapping/start acknowledgements.
5. **Current netcode:** frame-stamp inputs, isolate the input channel, send redundant recent samples, measure SI rate/RTT/jitter, and make delay selection genuinely adaptive.
6. **Detection:** exchange periodic frame-keyed deterministic state hashes and retain bounded diagnostic history.
7. **Test gates:** make manual scripts fail hard; add packet fuzzing, sanitizers, synthetic boot/input integration, Windows execution, and nightly `netem` impairment tests.
8. **Connectivity:** add invite strings plus authenticated traversal/relay or, at minimum, UPnP and explicit forwarding diagnostics.
9. **Rollback prototype:** validate narrowed snapshots in match/audio, measure catch-up speed on every target, then implement a bounded hybrid delay-plus-rollback mode while keeping fixed delay as a fallback.

## Reproduction pointers

Run the protocol test from a configured Linux build with localhost networking enabled:

```bash
./build-appimage/moderngekko_netplay_protocol_test
```

Inspect the historical rollback measurements:

```bash
git show -s --format=fuller 619767f0 4a877ca5 2e93f53e 1f16b92a
git show -s --format=fuller 07de096e e467c92a
```

With a prepared package at `dist/RingOut-1.0-deck`, the existing manual harnesses are:

```bash
HASH=1 .github/scripts/netplay-local.sh /tmp/netplay-local 60 2626
.github/scripts/netplay-match.sh /tmp/netplay-match 60 2640
```

These commands are diagnostic until the scripts are changed to return nonzero on every failed assertion.
