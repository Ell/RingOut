# Soulcalibur II game-specific rollback program

Status: active implementation research on `codex/sc2-slippi-rollback`, based on
GPU-safety commit `d5fd9426`. The discovery, selective-storage, hook-chassis,
and continuous-sync slice is implementation commit `514f77e6`. Phase-armed
hook discovery and the first machine-verified SC2 engine boundary are
implementation commit `7ad94d48`. Exact-scope MEM1 write-footprint discovery is
implementation commit `7efcceb3`. Exact full-emulator engine-tick replay and
the bounded SI input replay journal are implementation commit `304df33a`;
bounded external-effect classification is commit `dbb1682c`; dispatch-PC
attribution is commit `86513abf`; and direct/indirect update-call attribution
is commit `0a1dae8d`. A later uncommitted-at-measurement selective object-update
transaction is recorded below. All were recorded 2026-08-26. No release is
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
an exact-DOL engine-iteration boundary. A research oracle now proves one idle
and one automated-VS engine iteration reproduce exactly when the entire
emulator state and the original resolved SI polls are restored. It does **not**
yet select the region store in live netplay and does not claim that the safe
memory region set or side-effect hooks are known.

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

### Exact-scope MEM1 write footprint

Commit `7efcceb3` adds an opt-in probe that copies MEM1 at entry to
`0x8001ba3c` and compares it page-by-page when the function returns at
`0x8002d628`. It therefore measures writes inside one SC2 engine iteration and
excludes the `0x8007aef8` work performed between iterations. Nested entries,
incomplete runs, empty profiles, wrong boundary context, or different host and
guest region sets fail the harness. Implementation anchors are
`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.cpp:245-254,533-632`,
`.github/scripts/rollback-live-real-game.sh:338-363`, and
`.github/scripts/sc2-memory-profile-diff.sh:1-80`.

Two 60-logical-tick, two-peer profiles passed:

| route | changed pages | upper bound | every-tick pages | physical rows |
| --- | ---: | ---: | ---: | ---: |
| synchronized idle/menu control | 43 | 176,128 bytes | 20 | 346 |
| automated VS gameplay | 33 | 135,168 bytes | 28 | 1,451 |

The cohort diff found 24 shared pages, 19 idle-only pages, 9 gameplay-only
pages, and a 52-page/212,992-byte union. Gameplay-only page offsets were
`0x002d5000`, `0x0034d000`, `0x003ed000`, `0x00405000`, `0x00406000`,
`0x00422000`, `0x00736000`, `0x00739000`, and `0x00770000`. These are offsets
from guest `0x80000000`, not host addresses.

Exact commands were:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-memory-idle-pass \
  --production --hook-profile --hook-idle-control \
  --hook-warmup-frames 0 --hook-sample-frames 120 \
  --hook-diagnostic-limit 4096 --expect-sc2-engine-boundary \
  --hook-memory-profile --hook-memory-profile-ticks 60 \
  --play-seconds 24 --port 28855

RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-memory-gameplay \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 120 --hook-diagnostic-limit 4096 \
  --expect-sc2-engine-boundary --hook-memory-profile \
  --hook-memory-profile-ticks 60 --play-seconds 24 --port 28856

.github/scripts/sc2-memory-profile-diff.sh \
  /tmp/ringout-live-rollback.sc2-memory-idle-pass \
  /tmp/ringout-live-rollback.sc2-memory-gameplay
```

The idle `rollback-result.env`, host log, and guest log hash to
`5458f6c06986b1407f7500d2a203e5792c3c8b68bd44acf31f6f518fbe0dc2fc`,
`e7b1d63156876cf860d064426e7b078140edeaa999dd7bd2737e072805be4639`,
and `dac0cbe40ac5718c25c650b6699538abbf7437771d59d7bbb28465135787d8c7`.
The gameplay equivalents hash to
`bb3ec718cab34b5811a58acc95a90ee578eb34e0f255a450e9595bcd728ccfa3`,
`b18b133e556de4d2c7bdb616c8907b56b160e3662f635a6471aeb9fa7a30345e`,
and `d549bb9189b1f04368b7ff30c3e26ab2c12575522c96093621a71734645643b3`.
The diff output SHA-256 is
`67ae89bbad854c3e8debdf5e65894ff9aefa8c24d772d1c69a1f19637fa4cd9f`.

This write union is a **lower bound, not a selective-state whitelist**. It does
not include state read but not written in these ticks, pages changed by other
menus/stages/characters/round transitions, locked L1, CPU context, or emulator
state. The next gate must replay from a deliberately overinclusive candidate
profile and compare complete corrected MEM1/L1; pages may be removed only after
long corpus evidence classifies them.

### Exact engine-tick replay oracle and SI renewal

Implementation commit `304df33a` adds a deliberately expensive correctness
oracle at the certified `0x8001ba3c` entry / `0x8002d628` return boundary. On
the CPU thread it captures a complete rollback-scoped Dolphin state while the
GPU transaction is quiesced, materializes the StaticRecomp register state, and
retains the native timebase-cycle remainder. It discards the unmatched initial
pass, then runs two passes which both begin from the same restored entry state
and compares their complete serialized endpoint, `CPUState`, timebase, and
sub-timebase remainder. Implementation anchors are
`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.cpp:651-866`
and
`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore_Run.cpp:809-812`.

The first raw experiment restored all 24 MiB of MEM1, locked L1, and resident
StaticRecomp CPU state, but not Dolphin CoreTiming or hardware. Both idle peers
failed: nine MEM1 pages differed and the replay endpoint timebase was about
675,000 ticks later. This establishes that the engine function cannot yet be
treated as an isolated pure game-state call. Restoring full emulator state
closed that timing gap.

The first gameplay full-state run then exposed a separate input-renewal bug:
the host happened to reproduce, the guest differed by one serialized hardware
byte, and both peers reported a confirmed-state desync at frame 1440. Repeating
the engine tick had advanced the live rollback input scheduler and resolved a
fresh SI poll batch. The probe now records the original tick's bounded sequence
of `{pad, batching, result, GCPadStatus}` polls and supplies exactly that
sequence to both verification passes without touching the scheduler. The
journal is CPU-thread-local, capped at 4,096 polls, checks count/order, and is
inactive outside the explicit probe. Anchors are
`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:94-200,2460-2477`.

The real-game harness exposes this as `--engine-replay-probe`, requires the
supported DOL plus `--hook-profile`, and rejects a missing, partial, size-
mismatched, timebase-mismatched, input-mismatched, or byte-different result on
either peer. It cannot be combined with the boundary or memory profiler because
the oracle deliberately adds engine iterations. The asset-free evidence-gate
test also proves a one-byte endpoint difference is rejected. Anchors are
`.github/scripts/rollback-live-real-game.sh:75-79,380-399` and
`.github/scripts/test-rollback-live-real-game-harness.sh:68-89`.

The final automated VS result retained at
`/tmp/ringout-live-rollback.sc2-engine-replay-gameplay-inputjournal` passed on
both peers. Each original engine tick resolved four SI polls. Host and guest
each produced `state_match=yes`, `cpu_match=yes`,
`tb_remainder_match=yes`, `input_replay_match=yes`, and zero differing bytes;
their endpoint sizes were 49,360,077 and 49,360,086 bytes respectively. The
route completed with 1,622 physical rows and identical confirmed-state logs.
The result, host log, guest log, and common confirmed-state SHA-256 values are
respectively
`1e90c64cfea342d5e928ff33be0971d42137d19de53ef3675398e63d47878919`,
`b63e1ddc2a95baa7d4e86b7a5269f9fb3a3eadaf724a6de8a83430f8680e3407`,
`aa0068df7e70b213f98f3f73b84fd28164a26b2945f875ea90bcb980360ebf39`,
and `b854c3919a37154e7caf4c3d09afe92a9e7fabe7ccd81614b62d9922d7fef6dd`.

Exact reproduction command:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-engine-replay-gameplay-inputjournal \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 60 --hook-diagnostic-limit 0 \
  --engine-replay-probe --play-seconds 24 --port 28864
```

This proves the boundary, full machine restore, and explicit SI renewal can
reproduce one real gameplay tick. It is not the Slippi-class runtime: each
probe endpoint is still about 47 MiB, no selective state is active, no live
correction is redirected through this engine loop, and speculative
render/audio/persistence effects are not yet bypassed. The next gate is to
classify those external effects and replace the full state with an
overinclusive selective profile while continuing to compare the complete
endpoint.

### External-effect classification

Commit `dbb1682c` instruments every statically recompiled external MMU read and
write from the original engine pass, plus the number of interpreter fallback
instructions. It is bounded to 256 distinct read and write sites, reports and
fails overflow through the engine-replay gate, and remains inactive outside the
explicit oracle. Hooks and aggregation are at
`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore_Hooks.cpp:19-161`
and
`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.cpp:651-973`.

The automated VS sample retained at
`/tmp/ringout-live-rollback.sc2-engine-external-gameplay` completed exact
endpoint replay on both peers and retained 1,664 physical rows. Each peer
observed 103 external reads over 31 address/size sites, 104 external writes over
40 sites, zero interpreter fallbacks, no profiler overflow, and no writes to
the `0xcc008000` gather pipe. The address families were `0xcc000000`,
`0xcc001000`, `0xcc002000`, `0xcc003000`, `0xcc005000`, `0xcc006400`, and
`0xcc006800`. Dolphin maps these respectively to Command Processor, Pixel
Engine, Video Interface, Processor Interface, DSP, Serial Interface, and
Expansion Interface (`ModernGekko/vendor/dolphin/Source/Core/Core/HW/Memmap.cpp:73-82`).

The result, host log, guest log, and common confirmed-state SHA-256 values are
respectively
`85f53ba60bfdffd677481449476ca2659663cd9f4c4b77a83524e88e0ab0d1fa`,
`8e11a86ce4ea5aeff009ab5141904217b860189fdd6927106e826b26fc9514b4`,
`b0284aa0b4b85a70ee0b4accab7518f6c3a7fbce73bc1269f8425e7a3d92a0a6`,
and `e374cd74643d45c124d31b384fd029b164255e3d52f14c87e498cc269110c990`.
The reproduction command is the exact engine-replay command above with work
path `/tmp/ringout-live-rollback.sc2-engine-external-gameplay` and port 28865.

Commit `86513abf` additionally attributes every access to the active static-
recompiler dispatch PC. The gameplay sample contained 41 read block/address
pairs and 46 write pairs. Concrete blocks cover PI (`0x80183bb8`), VI
(`0x8018bcf0`), DSP (`0x8018f308`), CP/PE (`0x801a1a40`), EXI
(`0x801bd3b0`), and SI (`0x801beb94` and related blocks). These are exact PCs,
not inferred function names. The generated-C inspection remained in the
private extraction workspace and is not committed.

Commit `0a1dae8d` uses the generated module's bounded MEM1 write journal rather
than copying all 24 MiB at every call. It profiles the 39 exact direct call
edges in `0x8001ba3c`, then the seven indirect-dispatch return sites inside the
gameplay-sensitive object loop at `0x800095c0`. Missing returns, journal
ownership conflicts, more than 2,048 indirect sites, or incomplete profiles
fail the evidence gate. Written pages are conservative store footprints: a
page may be reported even if the stored value was unchanged.

The passing gameplay evidence at
`/tmp/ringout-live-rollback.sc2-engine-direct-journal-28969` found 26 executed
direct sites and 6,231 completed calls in the host tick. The wait/service call
at `0x8001bf58 -> 0x8000c1f4` accounted for 6,202 calls, 58 external reads, and
44 writes. Four other direct calls touched MMIO. Idle/menu subtraction at
`/tmp/ringout-live-rollback.sc2-engine-direct-idle-28970` retained the same 26
sites but exposed gameplay-sensitive write-footprint changes at
`0x800095c0`, the repeated `0x80020348` calls, and `0x80016d14`.

The nested passing run at
`/tmp/ringout-live-rollback.sc2-engine-indirect-28971` localized all observed
virtual updates to callsite `0x80009888`. In the host tick, clean targets
`0x80009530` and `0x800e9ab0` wrote 32 and 18 MEM1 pages respectively; targets
`0x8001af10` and `0x8001f0c0` accounted for the loop's observed MMIO. Host and
guest may arm on different physical ticks, so per-peer counts are exact self-
replay profiles and are not asserted to be equal cohorts. Both peers still
passed the full-state replay oracle. The result/host/guest SHA-256 values for
that run are
`458e28fb189eae4320be14a5e99104c5c1a21230acdf274d5636f6764ee12d85`,
`8286453837c63cd108789b14edc7821238daa034366a967fc18a55766042c2d2`,
and `fa2ba8020e3137c49392d33c8d9da1577eae9c3a65be26c693a48d4976cd5247`.

Exact nested gameplay reproduction command:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-engine-indirect-28971 \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 60 --hook-diagnostic-limit 0 \
  --engine-replay-probe --play-seconds 24 --port 28971
```

The idle cohort used the same command with
`--hook-idle-control`, work path
`/tmp/ringout-live-rollback.sc2-engine-direct-idle-28970`, and port `28970`.
The code gate is anchored at
`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.cpp:745-973`
and `.github/scripts/rollback-live-real-game.sh:380-399`.

This rejects the hypothesis that the broad engine iteration can be replayed as
a pure memory transform. It also narrows the implementation target: the
`0x800095c0` object-update phase is gameplay-sensitive, and its two observed
MMIO-capable handlers now need explicit read replay/write suppression. The next
oracle should replay that phase from a selective checkpoint and prove the
corrected endpoint while leaving the 6,000-call wait/service loop outside
resimulation.

### Selective object-update transaction oracle

The next oracle narrows replayed guest work to the first
`0x800095c0 -> LR 0x8001bcb0` object-update call inside the certified engine
iteration. It keeps the broad wait/service loop out of resimulation and uses
the generated module's pre-write journal to record exact CPU-written bytes,
rather than treating a changed 4 KiB page as indivisible state.

That distinction is load-bearing. Early selective attempts restored 45 to 47
complete MEM1 pages and consistently differed in roughly 470 to 487 bytes on
one otherwise unrelated page. The page moved between runs and had no game
module writer. Exact-byte restoration proved the cause was asynchronous
DSP/device activity sharing a page with update CPU stores, not divergent game
execution. The final transaction therefore:

1. retains the canonical complete hardware endpoint;
2. restores only exact module-CPU bytes written by the original update;
3. restores the update's resident CPU context;
4. substitutes captured postimages for sampled MMIO-owning handlers
   `0x8001af10` and `0x8001f0c0`, accounting for their 13 external effects;
5. executes every clean update handler normally;
6. records the union of original and extra replay CPU writes, so an unexpected
   replay write cannot be hidden; and
7. re-anchors non-game state at the canonical hardware endpoint before a
   complete serialized-state, CPU, timebase, MEM1, and locked-L1 comparison.

The real two-peer VS run retained at
`/tmp/ringout-live-rollback.sc2-selective-transaction-28981` passed on both
peers. The host transaction contained 23,179 exact game bytes over 45 pages;
the guest contained 23,134 bytes over 46 pages. Both had zero extra replay
writes, zero MEM1 and locked-L1 differences, exact CPU and sub-timebase state,
two captured SI polls, 13 accounted external effects, and byte-identical
serialized endpoints of 49,360,112 and 49,360,078 bytes respectively. The
route also completed the ordinary two-peer synchronized-match gate. Host and
guest logs hash to
`a3283d4d282a301b7086b5d5109bbf0360de347b810e32fc961b4949925ba98b`
and
`930089d51cdd7f9ba8a4b733fa229344147e4baae25053cf2992e7f4625187be`.

Exact reproduction command:

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-selective-transaction-28981 \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 60 --hook-diagnostic-limit 0 \
  --selective-update-replay-probe --play-seconds 24 --port 28981
```

This closes the same-input selective determinism gate for one sampled gameplay
object update. It does **not** yet prove corrected input: the system-handler
postimages contain the original SI-visible result, so a late input must not be
routed through this adapter until a deliberately changed-input oracle proves
the corrected value reaches game state. It also still uses a complete endpoint
snapshot to re-anchor and verify hardware. Live NetPlay therefore continues to
use the whole-emulator frame-boundary fallback at this checkpoint.

### Deliberately changed-input engine oracle

`--engine-replay-corrected-input-probe` separates deterministic resimulation
from the weaker same-input result. It captures the original resolved SI poll
journal, toggles the remote pad's A bit only in two isolated normalized replay
passes, and requires all of the following on both peers:

- at least one remote poll was actually altered;
- the corrected endpoint differs from the original at a byte written by the
  generated game module;
- the two independently corrected complete endpoints are byte-identical; and
- the untouched original endpoint is restored before ordinary play resumes.

A one-engine-tick version was rejected: SC2 consumed the changed SI polls but
had changed zero game-owned bytes at that endpoint. The input hardware result
is latched and reaches game state in the following 30 Hz iteration. Extending
the oracle to two engine ticks passed. The retained safe run at
`/tmp/ringout-live-rollback.sc2-corrected-safe-28986` captured eight polls per
pass, changed four remote polls, changed 28 module-written bytes in the host
sample, reproduced complete 49,360,062-byte corrected endpoints exactly, then
restored the original endpoint and completed the synchronized two-peer route.
The host and guest logs hash to
`0493ccd3f626193fa05b5d81246133b3844723a888ff7f9d5d29b00f045fd7ee`
and
`1296460c976941b963aa107b6286ea6f76a3a0e5d3770f6defb0bcf29d66b8f4`.

The changed bytes also identify the input pipeline. SI-side copies write the
pad-1 raw slot at guest `0x80427348` from `0x801bfa0c`. On the following tick,
the already-identified controller conversion function at `0x8002a694` writes
button/axis transition state around guest `0x802b59xx`; representative exact
writers are `0x8002a5e0`, `0x8002a654`, and `0x8002a688`. This makes the next
selective boundary concrete: capture before outer call `0x80011c80`, renew the
raw pad state from the corrected scheduler input, execute controller conversion
and the `0x800095c0` object update, and stop at LR `0x8001bcb0`.

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-corrected-safe-28986 \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 60 --hook-diagnostic-limit 0 \
  --engine-replay-corrected-input-probe --play-seconds 24 --port 28986
```

The same-input form of that wider selective boundary now passes. The first
attempt correctly failed with 24 game-byte differences and 12 extra replay
writes: root call `0x80011c80` itself performs 25 hardware reads and nine
writes, so executing it against the already-advanced canonical hardware
frontier changed its service branch. The final adapter captures the root
call's 371-byte postimage, then executes the remaining pure controller/game
work plus the previously adapted inner handlers.

The real two-peer run at
`/tmp/ringout-live-rollback.sc2-selective-input-adapted-28988` passed on both
peers. Each transaction restored 24,829 exact game bytes across 46 observed
pages, replayed the 371-byte root input-service postimage and 13 inner external
effects, produced zero extra replay writes, and matched MEM1, locked L1, CPU,
timebase remainder, and the complete roughly 49.36 MB serialized endpoint.
The host and guest logs hash to
`9ba65b44dd1662ebd2b57e8a3ff72c87a329f50ce30d2e633488003fae68df66`
and
`688076225a492ab0aa0d65ded1ad4a753645a9c8fbccb8da8418e758bc3ede02`.

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-selective-input-adapted-28988 \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 60 --hook-diagnostic-limit 0 \
  --selective-input-update-replay-probe --play-seconds 24 --port 28988
```

This closes same-input replay for the game-owned input/update half of an SC2
tick. It still reuses the predicted root-service postimage. Corrected activation
therefore remains gated on replacing the raw pad slot at `0x80427340 + 8*pad`
from the rollback scheduler and proving the existing controller conversion
produces the corrected game endpoint without replaying the hardware service.

That corrected selective gate now passes. The coarse root postimage was
replaced by a bounded external-effect transaction: SC2's SI-copy output is
replaced at dispatch `0x80183708` (returning to `0x801bfa80`) with a
`GCPadStatus` encoded using the live controller device's actual SI mode. The
game then executes its own controller mapping and first object update. Bytes
last written by the certified SI/OS service range are excluded from the game
postimage and the canonical hardware endpoint supplies them instead.

The two-peer run at
`/tmp/ringout-live-rollback.sc2-selective-input-corrected-28994` deliberately
toggled remote A and repeated the corrected selective transaction twice. Both
peers changed 17 game-owned bytes, consumed one perturbed remote poll, and
matched RAM, locked L1, CPU, timebase remainder, the captured external-effect
contract, and the complete approximately 49.36 MB serialized endpoint. The
host and guest logs hash to
`4ac355cf08ceae0b03ce8f95efd1472481f607b6b736b58112aed079e537cf66`
and
`5dbf86f22bcdfcdff84224b787e8b732737e3707610287fac34aedf2467ee830`.

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-selective-input-corrected-28994 \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 60 --hook-diagnostic-limit 0 \
  --selective-input-corrected-replay-probe --play-seconds 24 --port 28994
```

This is still a deliberately armed one-transaction oracle, not the player
path. It proves the selective corrected-input mechanism which the live
coordinator can invoke; the remaining integration gate is replacing the live
whole-emulator restore/resimulation loop with this SC2 transaction driver.

Commit `8c769987` replaces that oracle's first selective MEM1 reset with the
sparse preimage ring. The real generated-module callback retained exactly the
independently profiled transaction set on both peers: 24,784 host bytes and
24,829 guest bytes in the retained run. Corrected input changed 17 game-owned
bytes; the verification replay finished with zero MEM1 and locked-L1
differences and a byte-identical complete serialized endpoint on both peers.
The strengthened harness now requires sparse-journal bytes to equal the
profiled write-set bytes, in addition to the complete endpoint contract.

Evidence is retained at
`/tmp/ringout-live-rollback.sc2-sparse-corrected-28998`. Host and guest log
SHA-256 values are respectively
`4cfbe80ce2c3a800e6b263836a86610919ca1998bcebd767d2d4194729a97a70`
and
`05b78edd75da9fb0ebe0c3af730d1d37721894c17e762bd1171d20e2710ccca7`.
The oracle reloads a canonical hardware endpoint between its two comparison
passes, which deliberately breaks undo-log lineage; its second pass therefore
uses the retained entry postimage. A live selective transaction does not make
that intervening full-state load. Applying an undo log across an unrelated
load is explicitly rejected as an invalid architecture.

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-sparse-corrected-28998 \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 60 --hook-diagnostic-limit 0 \
  --selective-input-corrected-replay-probe --play-seconds 24 --port 28998
```

Commit `c5d7ae8c` removes the video-frame guess from correction ownership.
`Sc2RollbackTransactionTimeline` is a bounded identity-checked ring which maps
each 30 Hz SC2 input/update transaction to the exact SI batch IDs it consumed.
Its tests cover transactions spanning two 60 Hz frames, grouped pads sharing a
batch, gaps for hardware polls the game did not consume, active-transaction
rejection, history eviction, and nonsequential identity. A correction to a
hardware-only poll now produces no game rewind; an evicted consumed batch fails
closed.

The real two-peer run at `/tmp/ringout-live-rollback.sc2-batch-map-28999`
proved that both peers mapped the sampled SC2 transaction to exactly batch
1249, while retaining exact sparse coverage and complete corrected endpoints.
The harness requires one concrete batch and equality across peers. Host and
guest log SHA-256 values are
`817bb1a76fe87a756f9dd18a2f7ed2630a9b6e9d5453ba53c8967aec8a374501`
and
`befae7f93bfc33b4c95f378e41e3e5014c2b82543292e1308231e195a921c26b`.
This establishes correction-to-transaction selection; it does not yet make
the multi-transaction driver the player-selected state store.

Commit `864b705b` makes that history branchable. The transaction store owns the
input timeline and sparse preimage ring together: restore rewinds both to the
same transaction, invalidates speculative descendants, replays the corrected
target in place, and rebuilds later identities. Unit coverage performs two
successive multi-transaction rewinds with changed corrected branches and
restores exact RAM plus opaque CPU auxiliary state each time.

The shadow real-module gate at
`/tmp/ringout-live-rollback.sc2-transaction-history-29003` retained 827 host /
828 guest transactions, including 565/566 network-owned transactions. Both
peers agreed on 565 exact `(video frame, SI batch, unique-byte set)` mappings;
the longest uninterrupted safe epoch was 146 transactions, well above the
ten-slot configured horizon. Write sets ranged from 4,301 to 145,913 bytes.
The measured transaction interval (game work plus journaling, not journal
overhead alone) averaged 2.85 ms host / 2.81 ms guest and peaked at 8.30 / 9.91
ms. Transactions which entered uncovered fallback code were rejected and the
incomplete sparse epoch was discarded; capture resumed at the next boundary.

Host and guest log SHA-256 values are
`678c8ff58deaf8fc0e7162ee47299faee51600ff65e4b4ea1132b000731d3c50`
and
`c8c23f074cedbb0949c3fab57c7a280ada329b7882660321b6064299eadf358a`.
The strengthened harness requires at least 100 matching network transaction
mappings and one full ten-transaction safe epoch. This run still used the broad
player state store for actual corrections; external-effect and corrected-input
contracts must move into each retained slot before selective replay can be
selected.

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.sc2-transaction-history-29003 \
  --production --hook-profile --hook-warmup-frames 0 \
  --hook-sample-frames 120 --hook-diagnostic-limit 0 \
  --transaction-history-probe --play-seconds 24 --port 29003
```

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

Commit `7f7fad00` adds the complementary sparse preimage form,
`RollbackUndoSnapshotRing`. The generated-module and fallback journals call it
before covered writes; it retains the first value of each byte written during
a transaction, so rewinding cost scales with actual mutation rather than a
guessed enclosing region. The ring preallocates its slots, rejects incomplete
history before changing RAM, invalidates a transaction on capacity overflow,
and cannot advance past an invalid transaction. These properties make an
unknown write set a fail-closed fallback condition instead of a partially
restored game.

The synthetic Release benchmark uses the 24,829 unique bytes observed in the
real corrected SC2 input/update transaction, 512 auxiliary bytes, ten history
slots, and 240 record/restore pairs. Four runs on the 2026-08-26 development
host produced capture p50 0.85-1.77 ms, capture p95 2.26-3.80 ms, restore p50
0.22-0.52 ms, and restore p95 0.57-0.95 ms. This measures deliberately
worst-shaped one-byte journal calls and is not a live-game latency claim.

The same commit instruments the still-active whole-emulator store. A real
two-peer correction run retained at
`/tmp/ringout-live-rollback.performance-28996` measured approximately 45.98
MiB per retained checkpoint. Host capture averaged 28.9 ms (81.3 ms maximum)
and restore took 16.4-16.6 ms; guest capture averaged 28.6-35.0 ms (74.8-82.7
ms maximum) and restore took 16.9-17.0 ms. The run passed and did not reproduce
the historical GPU crash, but those costs rule out Slippi-class catch-up with
the broad store. The sparse ring remains unselected until real-module journal
coverage and corrected two-peer convergence pass.

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /tmp/ringout-sc2-private.z5XlEc1C/package \
  --work /tmp/ringout-live-rollback.performance-28996 \
  --production --fault-script .github/input-scripts/rollback-fault-correction.txt \
  --play-seconds 12 --port 28996
/tmp/ringout-sc2-rollback-build/moderngekko_rollback_undo_snapshot_benchmark
```

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
  moderngekko_rollback_undo_snapshot_ring_test \
  moderngekko_sc2_rollback_profile_test \
  moderngekko_rollback_region_snapshot_benchmark \
  moderngekko_rollback_undo_snapshot_benchmark core -j4
ctest --test-dir /tmp/ringout-sc2-rollback-build \
  -R 'moderngekko.(frame_dispatch_profiler|rollback_(region|undo)_snapshot_ring|sc2_rollback_profile)' \
  --output-on-failure
/tmp/ringout-sc2-rollback-build/moderngekko_rollback_region_snapshot_benchmark
/tmp/ringout-sc2-rollback-build/moderngekko_rollback_undo_snapshot_benchmark
bash -n .github/scripts/rollback-live-real-game.sh \
  .github/scripts/rollback-continuous-sync.sh
```

The complete Linux build at implementation commit `7f7fad00` passed all 49
registered CTest tests. The asset-free log-contract harness includes negative
cases for incomplete samples, wrong caller/predecessor edges, and asymmetric
peer candidate sets. The earlier source also built the Windows release target
(`moderngekko-launcher`, producing `RingOut.exe`) with the repository MinGW
toolchain and workflow-equivalent release options. The private-image commands
above additionally passed the 600-frame continuous-sync oracle and two-peer
engine-boundary gate. Selective state size, Windows behavior at `7ad94d48`,
renderer-backed replay, and live selective replay remain unverified and are not
claimed.
