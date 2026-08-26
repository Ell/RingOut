# Emulation rollback: GPU state boundary and FIFO failure research

Research target: `a80c2336` plus branch `codex/rollback-gpu-state`

Research date: 2026-08-26 UTC

Implementation commit: `35c09137` (`fix rollback GPU state transactions`)

Status: implementation and asset-free validation complete on the isolated
branch. This document records the player-reported release failure, source
conclusions, and the renderer-backed release gate; it is not evidence that a
corrected player package has passed that gate.

## Verdict

A rendered pixel mismatch should not decide a match. An emulated GPU mismatch
can: the guest CPU writes a command FIFO which the emulated GPU parses, GPU
commands can write guest RAM/EFB-visible results, and games can synchronously
read bounding-box, performance-query, FIFO, and EFB state. The host renderer is
a disposable presentation cache; the emulated command processor, FIFO bytes and
pointers, GPU registers, pending guest-visible writes, and their ordering with
CPU memory are part of the deterministic machine.

The reported `GFX FIFO: Unknown Opcode` is therefore not a cosmetic desync.
Dolphin emits it when its GPU opcode decoder no longer sees a valid command
stream and warns that the process will likely crash or hang
(`ModernGekko/vendor/dolphin/Source/Core/VideoCommon/CommandProcessor.cpp`).

The audited rollback implementation did serialize video/FIFO state, but that
was insufficient in dual-core mode. `State::DoState` serializes video on the GPU
thread first and then serializes CoreTiming, hardware/guest memory, and PowerPC
state on the CPU thread. A blocking video request makes the video section
coherent by itself; it did not keep the GPU stopped while the CPU completed the
rest of the snapshot or load. A checkpoint could consequently combine GPU/FIFO
state from one instant with guest FIFO memory and CPU state from another.

The frontend also contradicted the runtime safety default. Offline play had
already moved to single-core after a live StaticRecomp FIFO failure, while
`RunNetplayLobby` unconditionally forced netplay back to dual-core based on
short deterministic soaks. Those soaks did not repeatedly restore full-machine
state and did not cover the player failure.

Source evidence for that conclusion is in the implementation commit:

- `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/DolphinRollbackStateStore.cpp:18-42,55-101`
  defines and applies the whole-transaction GPU quiescence guard;
- `ModernGekko/vendor/dolphin/Source/Core/VideoCommon/AsyncRequests.cpp:33-44`
  and `ModernGekko/vendor/dolphin/Source/Core/VideoCommon/Fifo.cpp:446-450`
  ensure a blocking video-state request can still execute while emulated GPU
  work is paused;
- `ModernGekko/tools/netplay_session.cpp:1081-1112` selects the conservative
  rollback policy;
- `ModernGekko/src/runtime/dolphin_runtime.cpp:296-331` consumes the caller
  policy instead of silently replacing it during runtime creation.

## Primary-source findings

The [GGPO developer guide](https://github.com/pond3r/ggpo/blob/master/doc/DeveloperGuide.md)
requires a fully deterministic, fully serializable simulation which can advance
without rendering. It says render/audio effects may be excluded only when they
do not affect game state, and recommends a sync test which performs a one-frame
rollback on every frame. For an emulator, that boundary is the emulated machine,
not the host graphics API.

Upstream [Dolphin savestate code](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/State.cpp)
serializes the video backend before the rest of hardware specifically so video
caches can write modified data back to RAM. The corresponding RingOut vendor
code serializes BP/CP/XF/TMEM, FIFO bytes and cursors, command processor, pixel
engine, shader managers, EFB/framebuffer state, texture cache, and presenter
state in `VideoCommon/VideoState.cpp`. This confirms that GPU-visible state is
not merely a screenshot.

Slippi is useful but not a drop-in architecture for a generic emulator fork.
Its game-specific
[`SlippiSavestate`](https://github.com/project-slippi/Ishiiruka/blob/slippi/Source/Core/Core/Slippi/SlippiSavestate.cpp)
copies selected Melee memory regions, explicitly excludes sound and XFB/VI
regions, and leaves most Dolphin subsystem state commented out. Slippi can do
that because its rollback is coordinated with game-specific ASM and known game
memory semantics. RingOut currently has no equivalent SoulCalibur-specific
simulation/render split, so it must retain a coherent full-machine boundary.

RetroArch independently uses the same general rollback shape. Its official
[`network/netplay/README`](https://github.com/libretro/RetroArch/blob/master/network/netplay/README)
describes a ring of pre-frame serialized states, predicted remote input, and a
restore/replay from the last confirmed frame. Its production
[`netplay_frontend.c`](https://github.com/libretro/RetroArch/blob/master/network/netplay/netplay_frontend.c)
serializes before a frame, loads the selected historical state, marks replay,
and re-runs the core with resolved input. That frontend delegates the contents
of a state to each emulator core, so it supports RingOut's timeline design but
does not make a partial Dolphin GPU state safe. Audited RetroArch revision:
`5651dde2c7f239e92645d53a3e79e8c4e024180c`.

## Branch correction

The branch makes two independent safety changes:

1. Rollback player sessions default to single-core. Fixed-delay behavior is
   unchanged. `RINGOUT_ROLLBACK_DUALCORE=1` exists only as an experimental test
   seam, while `RINGOUT_NETPLAY_SINGLECORE=1` remains an overriding diagnostic.
2. `DolphinRollbackStateStore` pauses and synchronizes emulated GPU execution
   across the *entire* `SaveToBuffer` or `LoadFromBuffer` transaction. Async
   video-state requests still execute on the host GPU thread while emulation is
   paused; `AsyncRequests` now explicitly wakes that thread in deterministic
   mode. The first checkpoint logs
   `gpu_transaction_barrier=single-core-quiesced` or
   `gpu_transaction_barrier=dual-core-quiesced`.

The single-core default limits exposure immediately. The transaction barrier is
still required: it defines the correct snapshot contract and permits the
dual-core path to be tested without torn state rather than silently relied on.

## Required test harness

The fast headless route remains valuable for protocol, prediction, correction,
horizon, and confirmed-state checks, but it cannot catch a host-renderer FIFO
failure. `.github/scripts/rollback-live-real-game.sh` now supports
`--windowed`, fails on GPU/FIFO/crash markers, verifies the requested threading
policy and GPU transaction-barrier marker on both peers, and preserves the
existing correction plus confirmed logical-state oracle.

The minimum local release matrix is:

```bash
# Asset-free contracts
.github/scripts/test-rollback-live-real-game-harness.sh
ctest --test-dir /tmp/ringout-rollback-gpu-system-sdl-build \
  --output-on-failure \
  -R '^moderngekko\.(rollback_|live_rollback|netplay_protocol|frontend_config)'

# Real game, ordinary player threading, real renderer, repeated correction
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package "$RINGOUT_PRIVATE_PACKAGE" \
  --fault-script .github/input-scripts/rollback-fault-correction.txt \
  --production --windowed --play-seconds 180

# Guarded dual-core experiment; informative until it passes every platform
RINGOUT_REAL_GAME_ACK=I_OWN_THE_GAME \
  .github/scripts/rollback-live-real-game.sh \
  --package "$RINGOUT_PRIVATE_PACKAGE" \
  --fault-script .github/input-scripts/rollback-fault-correction.txt \
  --production --windowed --dual-core --play-seconds 180
```

The renderer-backed route must fail on process exit, hang, confirmed logical
state divergence, missing correction, `GFX FIFO`, unknown opcode, linked-FIFO
mismatch, desynced read pointers, negative/out-of-bounds FIFO, sanitizer error,
or missing barrier/threading provenance. A public package is not validated by a
headless handshake or a single short correction.

CI without a private game image should run the asset-free contracts and shell
evidence gate. Private release qualification must run Vulkan/OpenGL (Linux),
D3D (Windows), and at least one cross-machine match under delay/jitter/loss. The
repository must not distribute or upload the disc image.

## Current evidence and blocker

At the time of this entry, the complete native system-SDL build succeeds and
all 45 registered tests pass in parallel. The network test must run outside a
network-restricted sandbox because it binds loopback UDP; running it inside that
sandbox fails at `enet_host_create`, which is an execution-environment denial,
not a product result. Exact validation commands:

```bash
cmake --build /tmp/ringout-rollback-gpu-system-sdl-build -j 12
ctest --test-dir /tmp/ringout-rollback-gpu-system-sdl-build \
  --output-on-failure -j 8
bash -n .github/scripts/rollback-live-real-game.sh \
  .github/scripts/netplay-match.sh \
  .github/scripts/test-rollback-live-real-game-harness.sh
.github/scripts/test-rollback-live-real-game-harness.sh
git diff --check
```

The final CTest result was `100% tests passed, 0 tests failed out of 45` in
`23.43 sec`; the asset-free evidence-gate test reported
`rollback live real-game evidence gate: PASS`.

A clean Windows cross-build also completed the full launcher/runtime dependency
graph after the state-store changes and produced x86-64 PE executables for
`RingOut.exe`, `moderngekko-run.exe`, and `dolrecomp.exe`:

```bash
cmake -S ModernGekko -B /tmp/ringout-rollback-gpu-win-build -GNinja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-x86_64.cmake \
  -DCMAKE_C_COMPILER=/usr/bin/x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/x86_64-w64-mingw32-g++ \
  -DCMAKE_RC_COMPILER=/usr/bin/x86_64-w64-mingw32-windres \
  -DCMAKE_BUILD_TYPE=Release -DUSE_SYSTEM_LIBS=OFF \
  -DENABLE_QT=OFF -DENABLE_TESTS=OFF -DENABLE_ANALYTICS=OFF \
  -DENABLE_AUTOUPDATE=OFF -DBUILD_TESTING=OFF \
  -DMODERNGEKKO_ENABLE_DOLPHIN_TESTS=OFF
cmake --build /tmp/ringout-rollback-gpu-win-build \
  --target moderngekko-launcher -j 12
file /tmp/ringout-rollback-gpu-win-build/{RingOut.exe,moderngekko-run.exe} \
  /tmp/ringout-rollback-gpu-win-build/dolrecomp-build/dolrecomp.exe
```

That closes the fresh Windows compile gate only. These PE files were not run
under Windows and are not evidence of a Windows renderer-backed netplay match.

No owned game image or intact extracted private package is present under the
repository, `/home/ellg`, `/mnt`, or retained `/tmp` package directories. The
retained `/tmp/ringout-live-package.USuoYFcK` has a runtime and recomp module,
but its `game` path is a broken symlink to the deleted
`/tmp/ringout-rollback-private.mGbUYiaf/package/game`. It therefore cannot boot,
and the real renderer-backed two-peer gate cannot yet be claimed. The older
2026-08-25 private correction run remains useful input/protocol evidence but
predates this GPU transaction barrier and does not close the player report.
