# Soulcalibur II game-specific rollback program

Status: active implementation research on `codex/sc2-slippi-rollback`, based on
GPU-safety commit `d5fd9426`, recorded 2026-08-26. No release is certified by
this document.

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

The first branch slice implements the discovery and storage primitives. It does
**not** yet select the region store in live netplay and does not claim that the
exact SC2 update PC or safe memory region set is known.

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
`RINGOUT_SC2_HOOK_PROFILE=1`. It discards 120 warmup video frames, samples 600,
and reports only PCs executed exactly once in every sampled frame as strict
candidates. The table is bounded in time and absent from the normal player
path. A complete result is printed immediately at frame 720, so intentional
harness shutdown cannot lose it.

The two-peer harness accepts `--hook-profile`, binds evidence to the supported
GRSEAF revision-0 DOL SHA-256
`0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5`,
and requires complete, stable evidence on both peers. A candidate is only a
frequency result. It must still be disassembled and classified as update,
input, render, or an unrelated once-per-frame function before use.

`Sc2RollbackProfile` separately records that exact disc/DOL identity as
`DiscoveryOnly`. Its live-certification predicate also requires every update,
input, render, audio, and persistence hook plus at least one state region. The
current empty discovery record therefore fails closed by construction.

```bash
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package /path/to/private/package \
  --work /tmp/ringout-live-rollback.sc2-profile \
  --production --hook-profile --play-seconds 20 --port 28842
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

The owned SC2 image previously used by the real-game harness was not available
in this worktree on 2026-08-26, so no hook candidate, SC2 region size, or live
selective replay result is recorded yet.
