# Soulcalibur II game-specific rollback program

Status: active implementation research on `codex/sc2-slippi-rollback`, based on
GPU-safety commit `d5fd9426`. The discovery, selective-storage, hook-chassis,
and continuous-sync slice is implementation commit `514f77e6`. Phase-armed
hook discovery and the first machine-verified SC2 engine boundary are
implementation commit `7ad94d48`. Both were recorded 2026-08-26. No release is
certified by this document.

## Outcome and claim boundary

RingOut's current live path performs real prediction, correction, restore, and
resimulation, but checkpoints the whole emulator at Dolphin's video frame
boundary. That path is a correctness fallback, not a Slippi-class performance
architecture: retained checkpoints are about 45.98 MiB and restore emulated GPU
state that has already been allowed to run speculatively. The 2026-08-26 GPU
transaction barrier prevents torn full states, but cannot make those states
cheap or remove the fundamental coupling between speculative gameplay and the
renderer.

Slippi-level performance for SC2 therefore means a game-specific path:

1. intercept the exact SC2 update-loop boundary before rendering;
2. snapshot only the deterministic game-owned memory and CPU context;
3. preserve host/network bookkeeping across a restore;
4. replay corrected input through SC2's update loop without rendering, audio,
   card writes, or outbound network effects;
5. render and publish only the corrected frontier; and
6. refuse production activation unless the exact DOL profile and module
   fingerprint match a continuously verified profile.

The branch implements the discovery and storage primitives and now identifies
an exact-DOL engine-iteration boundary. It does **not** yet select the region
store in live netplay and does not claim that the safe memory region set or
side-effect hooks are known.

## Primary-source comparison

GGPO requires deterministic, serializable game state and an advance-frame path
that can run without rendering. Its recommended sync test rolls back one frame
on every frame and compares the result. See the
[GGPO developer guide](https://github.com/pond3r/ggpo/blob/master/doc/DeveloperGuide.md).

Slippi does not use an ordinary full Dolphin savestate for each Melee frame.
Its [savestate implementation](https://github.com/project-slippi/Ishiiruka/blob/slippi/Source/Core/Core/Slippi/SlippiSavestate.cpp)
copies selected Melee memory ranges and excludes sound and XFB/VI blocks; its
[EXI coordinator](https://github.com/project-slippi/Ishiiruka/blob/slippi/Source/Core/Core/HW/EXI_DeviceSlippi.cpp)
maintains a bounded savestate ring and coordinates capture/load with the game.
The game patch starts rollback inside the update loop, restores state, renews
inputs, and loops the engine without presenting intermediate frames
([StartEngineLoop.asm](https://github.com/project-slippi/slippi-ssbm-asm/blob/master/Online/Core/StartEngineLoop.asm),
[LoopEngineForRollback.asm](https://github.com/project-slippi/slippi-ssbm-asm/blob/master/Online/Core/LoopEngineForRollback.asm)).
It also has explicit invalidated-sound and alarm handling rather than assuming
renderer/audio state is irrelevant
([PreventDuplicateSounds.asm](https://github.com/project-slippi/slippi-ssbm-asm/blob/master/Online/Sound/PreventDuplicateSounds.asm)).

These sources support the architecture above. They do not supply SC2 addresses
or prove that Melee's selected regions are safe for SC2.

## Implemented branch slice

### Bounded game-frame hook discovery

`FrameDispatchProfiler` records statically recompiled dispatch PCs only when
`RINGOUT_SC2_HOOK_PROFILE=1`. Warmup and sample lengths are bounded and
configurable; the defaults remain 120 and 600 video frames. An optional marker
file arms both peers only after the harness reaches the requested gameplay or
two-peer idle route. The marker boundary itself is not counted, so sample zero
is the first complete post-arm frame. A complete result is printed immediately
at the configured bound, so intentional harness shutdown cannot lose it.

The profiler retains per-PC hit counts, first/last dispatch ordinals, caller
LR, preceding dispatch PC, and even/odd profiled-video-frame coverage. Strict
candidates still execute exactly once in every sample frame. Bounded diagnostic
output also retains always-multiple and partial-coverage PCs because the SC2
logical loop advances on only one of the two 60 Hz video-frame parities. The
real-game verifier requires identical strict PC sets on both peers and rejects
an incomplete, asymmetric, or misconfigured profile.
Implementation anchors at `7ad94d48` are
`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/FrameDispatchProfiler.cpp:18-139`
for aggregation/parity/context,
`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.cpp:218-244,438-519`
for bounded configuration, phase arming, and reporting, and
`.github/scripts/rollback-live-real-game.sh:281-321` for the two-peer and exact
engine-edge gates.

The two-peer harness accepts `--hook-profile`, binds evidence to the supported
GRSEAF revision-0 DOL SHA-256
`0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5`,
and requires complete, stable evidence on both peers. A candidate is only a
frequency result. It must still be disassembled and classified as update,
input, render, or an unrelated periodic function before use.
`--expect-sc2-engine-boundary` additionally requires the exact measured
outer-loop edge and parity on both peers; it does not certify state regions or
side-effect suppression.

`Sc2RollbackProfile` separately records that exact disc/DOL identity as
`DiscoveryOnly`. Its live-certification predicate also requires every update,
input, render, audio, and persistence hook plus at least one state region. The
current empty discovery record therefore fails closed by construction.

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /path/to/private/package \
  --work /tmp/ringout-live-rollback.sc2-profile \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 300 --hook-diagnostic-limit 4096 \
  --expect-sc2-engine-boundary --play-seconds 24 --port 28842
```

### Owned-image results and engine boundary

The user-provided USA RVZ was exercised locally on 2026-08-26 without adding
the image, extracted game, generated C, or compiled game module to Git. Private
identity for reproducing these measurements is:

| artifact | SHA-256 |
| --- | --- |
| `Soulcalibur II (USA).rvz` | `4b53a6013cc762c31d7a18283dc419969e89e6286864633a66213cb1e7a0fe3b` |
| extracted `sys/main.dol` | `0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5` |
| generated `gGRSEAF_recomp.so` | `3ab206bfa8ab10e0fb15aa24646d13e137578b813b76aa26e4e15e453ecc53b5` |

The continuous one-frame oracle warmed through 600 logical frames and then
restored/replayed 600 consecutive frames. It retained 1,801 physical hash rows
with no mismatch. The whole-emulator checkpoint was 69,839,907 bytes (66.60
MiB) and used the single-core GPU transaction barrier. Evidence is retained at
`/tmp/ringout-continuous-sync.sc2-real`; `result.env`, `runtime.log`, and
`frames.log` hash respectively to
`436ac2fdddfb6d80e35a6ff965ac694e0fa15325de3e24ab3b407d45743233c7`,
`6968db0c51b0779e15c84940939af7c6aa4e43a8bbc9282258c28fc98c4eca4d`,
and `1728af16af613283bc8daa570dd61c06cad388732affd11d06d6f6f232f7d37b`.

Phase-armed two-peer profiles show the SC2 engine advances at 30 logical ticks
per second inside 60 Hz video boundaries. Generated disassembly for the exact
DOL shows the outer function at `0x8002d5e8`: after an engine iteration returns,
the back-edge at `0x8002d624` calls `0x8001ba3c`, receives LR `0x8002d628`, and
loops while the return status requests another iteration. On the final 60-frame
idle-control validation, both peers measured `0x8001ba3c` exactly 30 times,
once on parity `30/0`, with stable caller LR `0x8002d628` and stable predecessor
`0x8002d624`. The wrapper now machine-checks this exact edge. The synchronized
route retained 366 physical rows and six matching confirmed-state checkpoints;
no GPU/FIFO failure was present. Evidence is retained at
`/tmp/ringout-live-rollback.sc2-parity-idle`. The profiled runtime SHA-256 is
`88d45786cef2f96d98fd6d1a821ba17744f62db781cbb5f095913a1f2cbe76ef`;
`rollback-result.env`, `host/log.txt`, and `guest/log.txt` hash respectively to
`e52cb78462b1bf3ba44f1d9c958f2929a09a0d2d21ad718d80ad1a0ee652c37e`,
`c7103f1d68d138259ff93a2b0388a7917e522205c044de98537032c8adca00df`,
and `d25b4bb68b3cc522849a58f7e54c46bcdf4192b842e7bc5b5d779e0647aee188`.

Exact local reproduction commands were:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-continuous-sync.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --start 600 --pairs 600 \
  --work /tmp/ringout-continuous-sync.sc2-real

RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-parity-idle \
  --production --hook-profile --hook-idle-control \
  --hook-warmup-frames 0 --hook-sample-frames 60 \
  --hook-diagnostic-limit 4096 --play-seconds 20 --port 28853

.github/scripts/rollback-live-real-game.sh \
  --verify-existing /tmp/ringout-live-rollback.sc2-parity-idle \
  --production --hook-profile --hook-idle-control \
  --hook-warmup-frames 0 --hook-sample-frames 60 \
  --hook-diagnostic-limit 4096 --expect-sc2-engine-boundary
```

Earlier frequency candidates were explicitly rejected: `0x8002bc3c` is a
60 Hz runtime/VI chain; `0x8002a694` is controller conversion and was exposed by
a one-controller/two-controller confound; `0x801419a4` is an inner event-record
path; and `0x8001af10` is one virtual object handler called by a wider object
loop. None is used as the engine hook.

The evidence certifies `0x8001ba3c` as the exact-DOL engine-iteration boundary
for continued research. It does **not** yet make selective replay safe: the
game-owned memory regions, controller-renewal point, render/audio suppression,
persistence suppression, and corrected-frontier publication still need to be
derived and tested before this PC can drive live netplay.

### Preallocated selective checkpoint ring

`RollbackRegionSnapshotRing` validates and sorts non-overlapping profile
regions, preallocates every history slot, copies only selected bytes plus an
opaque auxiliary CPU/game context, and restores only those bytes. Omitted gaps
remain untouched by design. It invalidates a slot before overwrite and retains
exact frame identity across modulo reuse.

This is the intended hot storage primitive after SC2 regions are certified. It
is not yet connected to `DolphinRollbackStateStore`; the whole-emulator store
remains the only live state store. `moderngekko_rollback_region_snapshot_benchmark`
is a manual 12 MiB memcpy baseline, not evidence for an SC2 region size.

On the 2026-08-26 development host, the Release benchmark copied 12,583,424
bytes (12 MiB plus 512 auxiliary bytes) for 240 capture/restore pairs:

| operation | p50 | p95 |
| --- | ---: | ---: |
| capture | 1.068 ms | 1.517 ms |
| restore | 1.035 ms | 1.449 ms |

This establishes that preallocated region copying can fit comfortably below a
16.67 ms frame on this host. It does not include SC2 update replay, does not
represent Deck/Windows memory bandwidth, and must not be used as a release
threshold by itself.

`StaticRecompDispatchHook` is the chassis seam for the eventual driver. A
certified list of PCs can be forced back through the DolRecomp dispatcher; a
CPU-thread callback may then continue normally, return to the guest link
register to suppress a side effect, or redirect the PC to repeat/finish the SC2
update loop. The branch defines and compiles this seam but installs no live
hook, so ordinary play is unchanged.

### Continuous one-frame sync oracle

`RINGOUT_DETERMINISM_CONTINUOUS_SYNC=HEADLESS_ISOLATED_ORACLE` now implements
the GGPO test shape against the production full-state transaction store. After
a configurable warm route, each logical frame is executed once, restored,
executed again with the same scripted input, and compared across OS-low MEM1,
game MEM1, and locked L1. The corrected endpoint becomes the next checkpoint.
Any mismatch, restore failure, or recapture failure is explicit. The wrapper
requires an owned private package and preserves hashes, logs, and identities:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-continuous-sync.sh \
  --package /path/to/private/package --start 600 --pairs 600 \
  --work /tmp/ringout-continuous-sync.sc2
```

This is a real emulator determinism gate, but currently remains headless and
uses expensive whole-emulator checkpoints. Once selective SC2 state is wired,
the store changes while the every-frame comparison contract stays the same.

## Certification gates before player activation

- Identify the update-loop, controller-read, render-submit, audio-event, and
  persistence boundaries for the exact supported DOL. Force each required PC
  through DolRecomp's `--dispatch-pc` and include the profile version and hooks
  in module/netplay compatibility identity.
- Derive state regions by differential capture across menus, character select,
  stages, rounds, throws, ring-outs, pause, replay, and result transitions.
  Treat omissions as unsafe until proven; never infer them from zero bytes.
- Install a game-loop rollback driver that restores before the first incorrect
  SC2 update, rewinds the input cursor, advances updates without GX/audio/card
  side effects, and publishes exactly one corrected frontier.
- Run GGPO's one-frame-every-frame sync test for long scripted corpora. Compare
  complete MEM1 and locked L1 at the corrected frontier, then reduce the
  profile only when differing bytes are classified and intentionally preserved.
- Benchmark capture, restore, and 1/2/4/7-frame replay p50/p95 on desktop,
  Steam Deck, Windows, and Linux. The seven-frame worst case must fit inside one
  display interval without sustained slowdown.
- Pass headless protocol, real Vulkan renderer, audio event, memory-card,
  asymmetric delay/jitter/loss, Windows-to-Linux, and thermal-soak gates. Any
  unknown DOL/profile or failed gate falls back to full-state rollback or
  refuses rollback; it never silently uses selective state.

## Reproduction for the asset-free slice

```bash
cmake -S ModernGekko -B /tmp/ringout-sc2-rollback-build \
  -DCMAKE_BUILD_TYPE=Release -DUSE_SYSTEM_SDL3=ON \
  -DMODERNGEKKO_ENABLE_DOLPHIN_RUNTIME=ON
cmake --build /tmp/ringout-sc2-rollback-build --target \
  moderngekko_frame_dispatch_profiler_test \
  moderngekko_rollback_region_snapshot_ring_test \
  moderngekko_sc2_rollback_profile_test \
  moderngekko_rollback_region_snapshot_benchmark core -j4
ctest --test-dir /tmp/ringout-sc2-rollback-build \
  -R 'moderngekko.(frame_dispatch_profiler|rollback_region_snapshot_ring|sc2_rollback_profile)' \
  --output-on-failure
/tmp/ringout-sc2-rollback-build/moderngekko_rollback_region_snapshot_benchmark
bash -n .github/scripts/rollback-live-real-game.sh \
  .github/scripts/rollback-continuous-sync.sh
```

The complete Linux build at implementation commit `7ad94d48` passed all 48
registered CTest tests. The asset-free log-contract harness includes negative
cases for incomplete samples, wrong caller/predecessor edges, and asymmetric
peer candidate sets. The earlier source also built the Windows release target
(`moderngekko-launcher`, producing `RingOut.exe`) with the repository MinGW
toolchain and workflow-equivalent release options. The private-image commands
above additionally passed the 600-frame continuous-sync oracle and two-peer
engine-boundary gate. Selective state size, Windows behavior at `7ad94d48`,
renderer-backed replay, and live selective replay remain unverified and are not
claimed.
