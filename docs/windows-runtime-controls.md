# Windows runtime, controls, and first-run guide

Audit commit: `ff0ad952980f5083afd21c3d3758208a7a093d72`

Release tag at that commit: `v1.2.1-ell.6`

Audit date: 2026-08-25

This document records the durable Windows-specific conclusions reached while
bringing the portable release back: what is in the ZIP, what first run builds,
what common log messages mean, how the Windows controls and netplay transition
work, and which claims still need a physical-Windows test. Paths and line
numbers are relative to the repository root and pinned to the commit above.

The netplay implementation discussed here is **fixed-delay lockstep, not
rollback**. See [the netplay audit](netplay-audit.md) for the protocol verdict
and [the lobby audit](netplay-lobby.md) for the broader lobby lifecycle.

That sentence is the pinned `ff0ad952`/`ell.6` release verdict. The later
`codex/rollback-netplay` worktree integrates the C++ launcher and an
Experimental rollback selector into Windows package scripts, but no complete
ZIP from that worktree has been tag-published or physically tested. Its latest
Linux/source production-path correction evidence passed after the memory-card,
output-teardown, and corrected-frontier fault fixes. That does not validate the
Windows artifact path. Do not apply the branch instructions below to the
published `ell.6` ZIP.

## Executive answers

| Question | Answer at the audited commit |
| --- | --- |
| Does a player install a compiler, CMake, Ninja, Python, or LLD? | No. The Windows ZIP carries Windows-native copies of all five. |
| What must a player provide? | One legally obtained, plain GameCube `.iso` or `.wbfs`, a Vulkan-capable Windows driver, a writable extraction folder, and free space. |
| Does the release ship game data or the recompiled game DLL? | No. First run extracts the player's disc and creates `bin/g<ID>_recomp.dll` locally. |
| Is LLD required for correctness? | No. It is preferred and enables the persistent ThinLTO cache. CMake deliberately falls back to the compiler driver's default linker if its LLD probe fails. |
| Is LLD actually included and exercised? | Yes. Packaging requires an LLVM linker, and the Wine smoke test requires `ld.lld.exe`, selects it, builds a synthetic module, verifies exports, and loads the DLL. |
| Does Windows need `qwave.dll` from the package? | No. The MinGW build stubs the optional QWave/DSCP marking path and does not import QWave. Netplay still uses ENet/UDP normally. |
| Does normal FMV playback require FFmpeg? | No. External FFmpeg takeover is developer-only and off unless `STATICRECOMP_FMV_TAKEOVER` is set. |
| Is `audio backend: Cubeb` an error? | No. Cubeb is the expected Windows default in this build. |
| What does Escape do? | In the game render window it opens/closes the settings overlay. `Shift+Escape`, `Alt+F4`, and the window close button use the safe quit path. In the separate netplay lobby, Escape intentionally cancels the lobby and exits. |
| Can players unlock every character? | Yes. The shipped USA `GRSEAF` code list includes `All Characters Unlocked` and `Absolutely Everything Unlocked`, both disabled by default. |
| Is the exact release fully proven on Windows? | The toolchain and generated DLL path are Wine-smoked end to end. Earlier prereleases reached gameplay on real Windows, but the exact current menu, controller, FMV, audio, and two-machine netplay paths still need physical-Windows regression QA. |

## The portable-package contract

The current artifact is named
`RingOut-1.2.1-ell.6-windows-x86_64.zip`. It has one top-level directory and is
meant to be extracted in full to a writable location. Do not run from inside
the ZIP and do not place it under `Program Files`: first run writes `game/`,
`work/`, `userdata/`, and a module beside the launcher
(`dist/windows/README.txt:13-27`).

### What the ZIP contains

The package stage is an allowlist, not a copy of a developer's working tree.
The significant entries are:

| Path in extracted package | Purpose | Player downloads separately? |
| --- | --- | --- |
| `RingOut.exe` | Native launcher and first-run disc picker | No |
| `RingOut.cmd`, `RingOut.ps1` | Console/troubleshooting launchers | No |
| `setup.ps1` | Disc extraction, recompilation, and DLL build driver | No |
| `bin/moderngekko-run.exe` | Windows runtime | No |
| `bin/Sys/` | Required Dolphin DSP/fonts/config resources | No |
| `tools/dolrecomp.exe` | GameCube PowerPC static recompiler | No |
| `toolchain/bin/clang.exe` | Compiles the generated C for this PC | No |
| `toolchain/bin/ld.lld.exe` | Preferred ThinLTO-capable linker | No |
| `toolchain/bin/cmake.exe` | Configures the per-game DLL build | No |
| `toolchain/bin/ninja.exe` | Drives the per-game DLL build | No |
| `toolchain/python/python.exe` | Generates module metadata tables | No |
| `module-src/` | Source, ABI glue, and CMake recipe for the private DLL | No |
| `userdata/GameSettings/GRSEAF.ini` | USA cheat list, disabled by default | No |
| `PE-IMPORTS.txt`, `MANIFEST.sha256`, `SOURCE.txt` | Import, integrity, and exact-source records | No |

The staging calls are at
`.github/scripts/package-windows-cross.sh:206-234` and the bundled-toolchain
assembly and required-file checks are at `:288-362`. The package script also
walks every project executable's PE imports, copies every non-system runtime
DLL beside the executable that needs it, and rejects unresolved imports
(`:364-446`). Cubeb or another vendored runtime dependency is therefore not a
separate player download.

The artifact deliberately rejects disc images, extracted DOLs, movies, saves,
generated recompilation modules, profiles, and private working directories
(`.github/scripts/package-windows-cross.sh:457-500`). `MANIFEST.sha256` covers
every shipped file, and the package script verifies it after a fresh unzip
(`:502-550`).

### What the player supplies

A player supplies:

- 64-bit Windows 10 or 11 with PowerShell 5.1 or newer.
- A Vulkan-capable GPU driver.
- Roughly 1.5 GB of writable space after extracting the release.
- A plain GameCube `.iso` or `.wbfs` image the player is entitled to use.

Compressed RVZ/GCZ/NKit data and the PS2/Xbox versions are not valid inputs.
`setup.ps1` checks the extension, GameCube header magic, and implausibly large
DVD-sized inputs before the expensive build (`dist/windows/setup.ps1:97-155`).

The player does **not** separately install Visual Studio, the Microsoft SDK,
MinGW, LLVM, CMake, Ninja, Python, DolRecomp, Cubeb, QWave, or FFmpeg for normal
play. Antivirus is the main exception to the self-contained contract: compiler
and linker binaries are sometimes quarantined, so a partial extraction can
look like a missing-tool problem (`dist/windows/setup.ps1:64-94`).

## What first run actually does

There are three distinct stages. Calling all of them “recompilation” hides
which tool failed.

1. `tools/dolrecomp.exe extract` extracts the supplied disc into `game/`.
2. `tools/dolrecomp.exe --gamecube ... -j<N>` translates `sys/main.dol` into
   generated C and metadata under `work/out/`.
3. Bundled CMake, Ninja, Python, Clang, and a linker compile and link that
   generated project into `work/build/g<ID>_recomp.dll`, then copy it to `bin/`.

The exact commands are in `dist/windows/setup.ps1:157-245`. Both DolRecomp and
the module build use `[Environment]::ProcessorCount`, so first run already uses
all logical processors (`:172-180`, `:240-241`). Later launches detect both the
extracted `game/` and any `bin/g*_recomp.dll` and skip setup
(`dist/windows/RingOut.ps1:16-20`, `:60-81`; the native launcher has the same
contract at `dist/windows/launcher/RingOut.c:44-57`, `:156-185`).

The generated DLL is CPU-specific because setup passes `-march=native`. Do not
copy a completed folder to a different class of CPU and assume the module is
portable (`dist/windows/setup.ps1:225-237`; `README.md:228-229`). Sharing the
original release ZIP is the portable path; each player should build locally.

### LLD versus “the default linker”

The module CMake file probes LLD rather than assuming it works. With Clang and
ThinLTO enabled it tries a trivial link with `-fuse-ld=lld`
(`dist/RingOut-1.0-dist/module-src/CMakeLists.txt:156-172`). There are two
possible status lines:

```text
-- module: linking with lld
-- module: ThinLTO cache at C:/.../RingOut/thinlto
```

This is the expected `v1.2.1-ell.6` path. LLD is then selected explicitly and
the persistent cache is enabled (`CMakeLists.txt:174-201`). Windows setup places
that bounded cache under `%LOCALAPPDATA%\RingOut\thinlto` and passes it to CMake
(`dist/windows/setup.ps1:217-237`). It survives the disposable `work/` tree, so
a later rebuild can reuse ThinLTO code generation.

The other outcome is:

```text
-- module: lld not usable, linking with the default linker
```

That line is a status message, not a fatal error. It means the explicit
`-fuse-ld=lld` probe did not pass, so CMake omits the LLD-specific options and
lets the Clang driver select its normal linker
(`CMakeLists.txt:202-204`). If the build then produces
`bin/g<ID>_recomp.dll`, linking occurred. If the runtime later prints
`[staticrecomp] module loaded`, Windows also loaded and accepted that DLL.

LLD is preferred because the persistent ThinLTO cache requires its
`--thinlto-cache-dir` option, not because it changes emulated correctness. The
measurements recorded beside the implementation found no clean-build time win
from merely switching linkers; the reusable cache is the material win
(`CMakeLists.txt:107-150`). A successful default-linker build is usable but
does not get that LLD cache path.

Historical context matters when reading old logs. Before commit `248b7c6d`, the
probe was guarded by `UNIX`, so Windows could never select the explicit LLD
branch even when the binary was present. That commit expanded the probe to
`UNIX OR WIN32`. The current package additionally refuses to assemble without
an LLVM linker (`package-windows-cross.sh:350-356`).

### Was the Windows compilation path tested?

Yes, with an important boundary. On each tag build, CI extracts the exact ZIP
under Wine and does all of the following:

- Runs packaged Python, CMake, Ninja, Clang, and LLD.
- Loads `moderngekko-run.exe --help`, exercising its ordinary PE imports.
- Generates a project-authored, two-instruction synthetic GameCube DOL.
- Runs the packaged Windows `dolrecomp.exe` on it with all runner CPUs.
- Configures the shipped `module-src/` with the packaged tools.
- Requires the configure log to say `module: linking with lld` and name the
  requested ThinLTO cache.
- Builds a Windows `gTST001_recomp.dll` and requires the verbose link to show
  LLD.
- Requires `staticrecomp_get_module` and `ppc_set_gather_pipe` exports.
- Loads the DLL through packaged Windows Python and validates its ABI, game ID,
  and entry point.

That sequence is implemented in `.github/scripts/smoke-windows-package.sh:62-179`
and `.github/scripts/windows-package-smoke.py:12-57`; the workflow calls it at
`.github/workflows/windows-cross.yml:190-212`.

This is meaningful end-to-end compiler/linker/loader coverage. It is not native
hardware coverage: Wine does not prove the current Vulkan window, Windows
audio device, physical controller, FMV, firewall prompt, or a two-PC netplay
match. The workflow disables the ordinary test suite for the cross-build
(`windows-cross.yml:118-139`). `README.md:24-31` records that earlier
prereleases reached gameplay on real Windows while still requiring physical QA
for the current experimental release.

### Re-running CMake messages

One Ninja line such as this is not itself a failure:

```text
[0/6] Re-running CMake...
```

Ninja reruns CMake when a build-system input appears newer than
`build.ninja`. Old Windows packages stored timezone-less ZIP timestamps that
could extract in the future on a western time zone, making Ninja repeat this
indefinitely and eventually report that the manifest remained dirty.

Current setup normalizes future-dated files in both `module-src/` and the
bundled CMake `share/` tree before configuration
(`dist/windows/setup.ps1:185-215`). Current packaging also dates ZIP entries a
full day before the source epoch and tests extraction in UTC-12
(`package-windows-cross.sh:508-541`). Repeated CMake regeneration on an older
`ell.1`/`ell.2` folder is a reason to use a freshly extracted current ZIP, not a
reason to install a different linker. Do not merge a new release over an old
generated `work/` tree.

## Runtime log decoder

### `controller configuration: using existing controller profile`

This is informational. A fresh user directory writes
`userdata/Config/GCPadNew.ini`: it maps the first detected SDL gamepad, or a
keyboard fallback when no pad is present. Existing GC pad profiles are left
untouched (`ModernGekko/tools/frontend_config.cpp:575-622`). The generated
gamepad map uses Dolphin's SDL names for the face buttons, triggers, D-pad,
sticks, and calibration (`frontend_config.cpp:399-470`).

Device names are not perfectly stable between a physical pad, Steam Input, and
a virtual gamepad. At runtime, if the profile's exact device no longer exists
but an SDL gamepad does, Ring Out moves the live pad binding to the first
present SDL gamepad and logs the change
(`ModernGekko/src/runtime/dolphin_runtime.cpp:172-218`). This repair does not
make a missing or blocked controller magically available to SDL.

If the message appears but the game gets no input:

1. Open the Controls tab and check the selected backend and device.
2. Confirm `userdata\Config\GCPadNew.ini` names the connected `SDL/...` device.
3. Check the console for `[input] pad ... is not connected` or `rebinding to`.
4. Back up and rename `GCPadNew.ini`, then relaunch to generate a fresh default.
5. For netplay diagnostics, set `RINGOUT_NETPLAY_PADLOG=1`; send- and
   receive-side pad logs identify whether input was read locally or delivered
   to the in-game port (`NetPlayClientInput.cpp:117-135`, `:219-240`).

The Windows cross-build deliberately uses SDL input; Microsoft-SDK-only native
Windows controller backends are outside this package (`README.md:221-227`).

### `audio backend: Cubeb`

This is the expected default, not a request to install Cubeb. ModernGekko
forces Cubeb on for Windows builds (`ModernGekko/CMakeLists.txt:66-85`). The
runtime enumerates compiled backends, chooses the host default, and prefers
Cubeb when it is valid (`ModernGekko/src/runtime/dolphin_runtime.cpp:388-409`;
`ModernGekko/vendor/dolphin/Source/Core/AudioCommon/AudioCommon.cpp:99-113`).
The line reports the backend actually selected
(`ModernGekko/tools/moderngekko_run.cpp:365-371`).

The standalone Dolphin WASAPI backend is omitted from this MinGW build, but
that does not mean “no Windows audio”: Cubeb supplies the supported frontend
and uses the platform audio stack internally. If Cubeb is printed but no sound
is audible, investigate the Windows output device/mixer and the in-game Audio
tab rather than downloading a Cubeb DLL.

### QWave is intentionally absent

QWave is an optional Windows QoS path, not the network transport. Dolphin's
native non-MinGW implementation associates the ENet socket with a QWave flow
and requests a DSCP value (`ModernGekko/vendor/dolphin/Source/Core/Common/QoSSession.cpp:6-50`).
Ubuntu 24.04's MinGW-w64 headers did not expose everything that implementation
needed, so commit `83297cb8` made the MinGW constructor a no-op and left
ordinary ENet traffic unmarked (`QoSSession.cpp:52-61`). The build only links
`qwave.lib` when it is **not** MinGW
(`ModernGekko/vendor/dolphin/Source/Core/Common/CMakeLists.txt:311-323`).

Consequences:

- Hosting, joining, reliable ENet delivery, pad exchange, and LAN discovery
  remain available.
- Windows does not request expedited DSCP treatment for these packets.
- On a congested network that honors DSCP this may forgo a latency/jitter
  optimization. It does not explain a connection that cannot bind or route.
- The player must not download or copy `qwave.dll`; the MinGW runtime does not
  import it. The Wine package smoke explicitly checks the no-QWave runtime path
  (`.github/scripts/smoke-windows-package.sh:95-105`).

At audited release `ell.6`, fixed delay, direct connection, no relay, and no
hostile-peer hardening matter much more than the absent traffic marker. The
later rollback branch changes the scheduling mode but not those connectivity or
trust limitations.

### FFmpeg, FMVs, and paths with spaces

Normal playback stays in the recompiled game's own Sofdec/MPEG path. The
external native player is armed only when the process starts with
`STATICRECOMP_FMV_TAKEOVER` present
(`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore_Run.cpp:624-648`).
That is why `dist/windows/README.txt:41-48` says FFmpeg is neither bundled nor
required.

Older builds accidentally armed the external player whenever a movie started.
On a normal Windows machine this produced:

```text
'ffmpeg' is not recognized as an internal or external command
```

and could break the movie path. Commit `ab8f3b80` put the entire external-player
path behind the explicit takeover flag. A current ordinary run should not
spawn `ffmpeg.exe` and should not print an `[fmv-hle] player: ...` line.

When a developer deliberately enables takeover, the current Windows command
uses `_popen(..., "rb")` for raw binary frames and double-quotes the complete
input URL because `cmd.exe` does not treat single quotes as quoting
(`StaticRecompCore_Run.cpp:34-50`, `:205-224`). The archive lookup uses the
package's `userdata/../game/files/movie.afs` layout
(`:61-121`). Spaces in an extracted path such as `V:\GAMES\emu\Ring Out\...`
are therefore covered. Literal command-shell metacharacters inside a developer
override have not been security-hardened; takeover is an opt-in experiment, not
the shipping FMV path.

If a current normal run invokes FFmpeg, inspect and clear inherited developer
flags before doing anything else:

```powershell
Get-ChildItem Env:STATICRECOMP_FMV*
Remove-Item Env:STATICRECOMP_FMV_TAKEOVER -ErrorAction SilentlyContinue
```

External takeover is also refused during netplay and determinism runs because
host decoder timing would write different pixels and control flow into guest
state (`StaticRecompCore_Run.cpp:376-426`).

### VEH exception-handler warning

This alert:

```text
Condition: !s_veh_handle
File: ../ModernGekko/vendor/dolphin/Source/Core/Core/MemTools.cpp
Function: InstallExceptionHandler
```

does not mean a DLL or game file is missing. Windows' vectored exception
handler (VEH) is already non-null when a CPU thread tries to install it again.
The assertion and install are exactly
`ModernGekko/vendor/dolphin/Source/Core/Core/MemTools.cpp:103-107`; a normal CPU
thread removes it as it exits (`ModernGekko/vendor/dolphin/Source/Core/Core/Core.cpp:306-363`).

On the old menu-to-netplay path this was consistent with starting the second
runtime before the first CPU thread had completely unwound. Commit `6f1ef65e`
reworked that lifecycle: Start Netplay first resumes and safely quits the
offline runtime, `Runtime::Run` performs Core stop/shutdown, and only then does
the runner consume the request and construct the lobby/runtime replacement
(`RecompMenu.cpp:2287-2322`; `ModernGekko/src/runtime/dolphin_runtime.cpp:532-579`;
`ModernGekko/tools/moderngekko_run.cpp:397-478`). Windows also accepts the
already-registered window class for the replacement runtime
(`PlatformWin32.cpp:69-99`).

There is no physical-Windows regression test proving that the current
transition can never reproduce the VEH assertion. Treat it as an unresolved
lifecycle defect if it appears on `ell.6`; do not normalize repeated “Ignore
and continue” prompts. Capture the entire console log, whether it followed
Start Netplay, and whether two render windows or CPU threads appeared. Restart
from a clean process before continuing a match.

### GFX FIFO / Dual Core warning

Commit `ab8f3b80` changed **offline** Windows play to single-core CPU/GPU
scheduling by default because a live static-recomp run produced the same GFX
FIFO desync whose Dolphin alert recommends disabling Dual Core. The current
runtime prints one of these explicit modes:

```text
cpu/gpu threading: single-core (safe default)
cpu/gpu threading: dual-core (explicit opt-in)
```

The selection is at `ModernGekko/src/runtime/dolphin_runtime.cpp:296-323`.
`RINGOUT_DUAL_CORE=1` is an experiment; leave it unset for normal offline play.
If an old `ell.2` build reports `Unknown Opcode`/GFX FIFO desync, updating to a
current clean extraction is the first fix.

Netplay is intentionally different. It uses dual-core plus Dolphin's
deterministic GPU-thread mode by default. Historical two-peer runs stayed
byte-identical, but one reported scheduling assertion was fixed by source
inspection rather than locally reproduced. `RINGOUT_NETPLAY_SINGLECORE=1`
restores the conservative netplay path and is the first diagnostic if a current
peer asserts, desyncs, or reports a FIFO error
(`ModernGekko/tools/netplay_session.cpp:778-812`). In PowerShell:

```powershell
$env:RINGOUT_NETPLAY_SINGLECORE = '1'
.\RingOut.cmd
```

Do not set `RINGOUT_DUAL_CORE=1` as a response to an offline FIFO warning; it
does the opposite of the alert's recommendation.

### `module lacks ppc_set_gather_pipe export`

The current package smoke requires `ppc_set_gather_pipe` in every newly built
test module (`.github/scripts/smoke-windows-package.sh:161-168`). Seeing the
missing-export message usually means an older generated module was carried
into a newer runtime. A clean first-run build from one current extracted ZIP is
the supported pairing. Do not copy `gGRSEAF_recomp.dll` from an earlier package
over the current one.

## Windows controls and menu behavior

The Windows platform now handles keys directly rather than treating Escape as
an unconditional shutdown. The important render-window bindings are:

| Input | Behavior |
| --- | --- |
| `Escape` | Open the overlay; while open, cancel a pending bind/address edit or close it |
| `Shift+Escape` | Safe immediate quit |
| `Alt+F4` / close button | Safe quit through the same resume-then-stop path |
| Arrow keys | Navigate; Left/Right switch tab or change the selected value |
| `Space` / `Enter` | Activate/confirm; activations do not auto-repeat |
| Hold `Tab` | Fast-forward; release/focus loss clears it |
| `F10` | Pause/resume offline only |
| `F1`-`F8` | Load state offline |
| `Shift+F1`-`F8` | Save state offline |
| `Alt+Enter` | Fullscreen |
| `Alt+W` | Widescreen |

`PlatformWin32::HandleKeyDown` owns these semantics and filters key repeat
(`ModernGekko/vendor/dolphin/Source/Core/DolphinNoGUI/PlatformWin32.cpp:226-361`).
`WM_CLOSE` is routed through `RequestQuit`, not raw window destruction
(`PlatformWin32.cpp:431-476`). The quit path resumes a paused CPU before it
stops the platform, preventing the old paused-core shutdown deadlock
(`RecompMenu.cpp:2441-2471`).

Escape is deliberately context-sensitive:

- **Offline render window:** the overlay opens and pauses emulation.
- **Active netplay render window:** the overlay opens without pausing either
  peer; unsafe state-changing controls are locked.
- **Netplay lobby window:** Escape, controller East/B, Back/View, the Quit
  button, or the window close button cancels the lobby. The offline runtime was
  already torn down to enter this pre-boot lobby, so cancellation exits to the
  desktop rather than returning to the prior game (`netplay_session.cpp:316-429`).

`RecompMenu::OnEscape` unwinds a pending control bind or netplay-address octet
edit before it closes the overlay (`RecompMenu.cpp:2405-2439`). Offline menu
pause/resume runs through a worker; active netplay stays running so one peer
cannot pause alone (`RecompMenu.cpp:1811-1878`).

### Controller-driven menu

A keyboard is not required:

- Hold Back/View for about 500 ms to open the overlay.
- Use the D-pad to navigate.
- Use controller A to activate.
- Use B or a new Back/View press to unwind/close.

The hold applies only to opening, avoiding accidental pauses; closing is a
single press (`RecompMenu.cpp:2885-3053`). While the overlay owns these buttons,
`Host_UIBlocksControllerState` captures normal input
(`ModernGekko/src/runtime/dolphin_runtime.cpp:81-85`). In active netplay the
local client sends a neutral pad sample instead of leaking menu D-pad/A/B into
the match (`NetPlayClientInput.cpp:189-217`). Controls and cheats are read-only
once the netplay game has started.

## Starting netplay on Windows

Both peers should use the same Ring Out release, compatible disc revision, and
freshly generated module. This lobby is direct-connect fixed-delay netplay; it
has no central matchmaking, relay, password, or public room browser.

### Integrated launcher path on the rollback worktree

When testing a Windows package built from this worktree, use the top-level
`RingOut.exe` launcher:

1. Both players open **Netplay**. The normal beta flow selects rollback and
   uses **Host online room** / **Join online room**. Advanced Direct IP retains
   fixed-delay and manual-address diagnostics.
2. The host copies the eight-character room code from the lobby; the joiner
   enters that code. No gameplay port forwarding is normally required when
   Dolphin's peer-to-peer traversal succeeds.
3. In the lobby, confirm Same game and a controller assignment, then each player
   selects **Ready**. Any mapping, delay/mode, game, roster, or disconnect change
   clears readiness.
4. The host selects **Start game** only after all mapped players show Ready.

**Show in-game network stats** is enabled by default. During a rollback game it
shows the maximum peer RTT and the actual correction depth in frames. The most
recent correction peak remains visible for one second, then returns to zero;
the runtime suppresses the overlay completely in solo and fixed-delay play.
The preference is stored as `performance_overlay` in `userdata\config.ini`.

For a connection problem, enable **Detailed netplay diagnostics** before
reproducing it. The launcher captures the session in
`userdata\Logs\RingOut.log`, moves the preceding run to
`RingOut.previous.log`, and offers **Copy log path**. Review the file before
sharing because it can contain nicknames, room codes, IP addresses, controller
names, and local paths.

The launcher forwards explicit traversal/direct selection, `--netplay-mode`,
room code or address, port, nickname, buffer, and controller values
(`ModernGekko/tools/moderngekko_launcher.cpp:1197-1324,1474-1515`).
The connect extension requires an exact mode and compatibility fingerprint
before player allocation
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:463-503`).
Online Room uses Dolphin's hosted rendezvous and then direct UDP. It remains
unauthenticated and unencrypted, exposes peer IPs, has no relay fallback, and
can fail on strict NATs. Use only trusted friends. Advanced Direct IP remains
available for a LAN/private VPN.

Rollback sessions quarantine save writes: synchronized memory-card data can be
used in-game, but progress is not copied back to the user's save and writable
SD/serial/GBA paths are disallowed
(`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1127-1146,1644-1697`).
If rollback is unavailable or incompatible, exit and reconnect after selecting
Fixed delay on both peers.

### Launcher-only entry point

The current branch removes the obsolete Netplay, Scan, address, port, and Start
rows from the in-game System tab. Players should always create or join a room
from `RingOut.exe`; Advanced Direct IP is available there when needed. The
runner retains its one-shot request reader only so an already-created request
from an older build can be consumed safely. Older published packages may still
show the historical in-game Direct-IP flow.

The lobby supports keyboard and controller navigation, shows name, ping,
assigned controller port, and game comparison, and only the host starts the
game (`netplay_session.cpp:597-750`). Escape in this window means **cancel the
lobby**, not open the emulation overlay.

### Command-line path

The Windows launchers forward non-disc arguments to `moderngekko-run.exe`, so a
console can bypass the in-game request step after first-run setup:

```powershell
# Online Room host; copy the code from the lobby.
.\RingOut.cmd --netplay-host --netplay-traversal --netplay-mode rollback --nickname Host

# Online Room guest
.\RingOut.cmd --netplay-join 0123abcd --netplay-traversal --netplay-mode rollback --nickname Guest

# Advanced Direct host
.\RingOut.cmd --netplay-host --netplay-port 2626 --nickname Host

# Advanced Direct guest
.\RingOut.cmd --netplay-join 192.168.1.50 --netplay-port 2626 --nickname Guest
```

The supported arguments are declared in
`ModernGekko/tools/moderngekko_run.cpp:31-50`, parsed at `:102-203`, and quoted
when the native Windows launcher forwards them
(`dist/windows/launcher/RingOut.c:188-218`). The interactive lobby still opens;
the commands select the role and address rather than auto-playing a match.

## Character unlock support

The tracked USA game settings include all of these independently selectable
Action Replay codes:

- `Absolutely Everything Unlocked`
- `All Characters Unlocked`
- `All Levels Unlocked`
- Direct P1/P2 “Play As” codes for Lizard Man, Assassin, Berserker, and Inferno

They are in `work/mg_userdir/GameSettings/GRSEAF.ini:17-89`. The package copies
the list but strips every entry from `[ActionReplay_Enabled]` and
`[Gecko_Enabled]`, and packaging fails if any code would ship enabled
(`.github/scripts/package-windows-cross.sh:226-234`, `:457-471`).

To use one offline:

1. Press Escape.
2. Move to the Cheats tab with Left/Right on the top row.
3. Select `All Characters Unlocked` or `Absolutely Everything Unlocked`.
4. Press Space/Enter or controller A.

Selecting an individual code also enables the master cheat switch, persists
the selection in `userdata/GameSettings/GRSEAF.ini`, and applies it to the live
cheat engine (`RecompMenu.cpp:1320-1392`, `:2112-2136`). The codes are for the
USA `GRSEAF` revision; PAL addresses must not be substituted.

For netplay, the host must choose codes **before** starting the session. Ring
Out enables code synchronization and the Cheats tab becomes read-only during
the match (`ModernGekko/tools/netplay_session.cpp:817-823`;
`RecompMenu.cpp:1898-1903`, `:2112-2136`). These codes modify live emulated
memory. They do not promise that every unlock becomes permanent memory-card
progress after the code is disabled (`README.md:159-163`).

## Troubleshooting by symptom

| Symptom | Meaning / action |
| --- | --- |
| `lld not usable, linking with the default linker`, then DLL produced | Successful fallback. The module was linked; only the explicit LLD/cache path was skipped. Check package version and AV if this occurs on `ell.6`, because current CI selects LLD. |
| Many repeated `[0/6] Re-running CMake...` lines | Usually an old/future-timestamp ZIP or a merged working tree. Use a fresh current extraction; do not install another linker first. |
| Bundled compiler probe fails immediately | Check AV quarantine for `toolchain\bin\clang.exe`, linker files, and sysroot files; verify the published ZIP hash and extract again with Explorer or 7-Zip. |
| `'ffmpeg' is not recognized` during an ordinary run | Old pre-`ell.3` behavior or inherited `STATICRECOMP_FMV_TAKEOVER`. Update/unset the developer flag. Normal FMVs need no FFmpeg. |
| `audio backend: Cubeb` | Expected success-path selection, not a missing dependency. |
| `Condition: !s_veh_handle` | Duplicate Windows exception-handler install, usually around runtime replacement. On current release capture the full log and restart; physical-Windows regression remains open. |
| Offline `GFX FIFO: Unknown Opcode` / “turn off Dual Core” | Current default already is single-core. Unset `RINGOUT_DUAL_CORE`, confirm the startup mode line, and use a clean current package. |
| Netplay FIFO/assert/desync | Retry both peers with `RINGOUT_NETPLAY_SINGLECORE=1` and preserve logs. This is a diagnostic fallback, not proof that the network was at fault. |
| `using existing controller profile`, but no input | Inspect `GCPadNew.ini`, the Controls device row, and `[input]` logs; back up/rename the stale profile to regenerate. |
| Escape exits instead of opening settings | In the netplay lobby that is intentional cancellation. In the game window, plain Escape should open settings; Shift+Escape quits. Record which window had focus. |
| `module lacks ppc_set_gather_pipe export` | Stale module/runtime pairing. Build the module in a fresh directory from the same current ZIP. |
| Missing DLL on launch | Compare against `MANIFEST.sha256` and check AV quarantine; package construction already rejects unresolved non-system imports. |

For a support log, launch from PowerShell so the console survives and capture
both standard streams:

```powershell
cmd /c .\RingOut.cmd 2>&1 | Tee-Object -FilePath .\ringout-support.log
```

Record `SOURCE.txt`, the package ZIP SHA-256, Windows version, GPU/driver,
controller name, whether Steam Input is active, and any `RINGOUT_*` or
`STATICRECOMP_*` environment variables. Do not publish the extracted `game/`,
the generated module, saves, or disc paths if those are sensitive.

## Physical-Windows release verification still required

CI establishes package integrity and Windows-tool execution under Wine. A
release candidate should still pass this checklist on Windows 10/11 hardware,
preferably from a path containing spaces:

1. Verify the ZIP hash, extract it whole to a new writable directory, and check
   `MANIFEST.sha256`.
2. First run with a valid plain disc; observe all three setup stages use the
   packaged tools and all logical processors.
3. Require `module: linking with lld`, a ThinLTO-cache message, a nonempty
   `bin/gGRSEAF_recomp.dll`, and no repeated CMake-regeneration loop.
4. Boot through the intro and a match. Confirm ordinary FMVs never invoke
   external FFmpeg, audio is audible through Cubeb, and offline startup reports
   the safe single-core default without a FIFO alert.
5. Test keyboard Escape, Shift+Escape, Alt+F4, close while paused, controller
   hold-to-open, D-pad/A/B navigation, and a fresh as well as existing pad
   profile.
6. Enable and disable `All Characters Unlocked`; confirm the list is initially
   off and the chosen state persists as intended.
7. On two Windows PCs, host/join over LAN, test scan and direct address, verify
   firewall behavior, controller routing, lobby cancel, host start, active-match
   overlay input capture, and clean shutdown.
8. Repeat the match once with the default deterministic dual-core netplay mode
   and once with `RINGOUT_NETPLAY_SINGLECORE=1`. Preserve desync and FIFO logs.
9. Confirm the offline-to-lobby-to-netplay transition never produces the VEH
   assertion or a second-runtime window-registration failure.

Until that checklist passes for the exact tag, describe Windows as an
experimental cross-built release with a strong packaged-toolchain smoke test,
not as fully native-hardware-certified.

## Commit provenance

The main fixes summarized here entered in these commits and are all ancestors
of the audited release:

| Commit | Durable change |
| --- | --- |
| `83297cb8e000f745774530990590d8d55b90c07a` | Stub optional QWave QoS for MinGW so netplay builds without a Microsoft SDK dependency |
| `248b7c6d4bc2be119f0e1e5f7ad67f6d9f67e89e` | Probe LLD on Windows and fix future-timestamp CMake regeneration |
| `ab8f3b803967e385deee8a2fcb53d99f33e4d9a6` | Keep FFmpeg takeover opt-in and make offline single-core the safe default |
| `6f1ef65e12013e1023fc4bab8bd31460f1f6d399` | Repair Windows Escape/menu/control/netplay restart handling and add the packaged LLD module smoke test |
| `ff0ad952980f5083afd21c3d3758208a7a093d72` | Current `v1.2.1-ell.6` audit point and hardened release-asset publication |

Useful source-only verification commands from the repository root:

```bash
git rev-parse HEAD
git tag --points-at HEAD
bash -n .github/scripts/package-windows-cross.sh
bash -n .github/scripts/smoke-windows-package.sh
rg -n "module: linking with lld|lld not usable" \
  dist/RingOut-1.0-dist/module-src/CMakeLists.txt
rg -n "VK_ESCAPE|WM_CLOSE|RequestQuit" \
  ModernGekko/vendor/dolphin/Source/Core/DolphinNoGUI/PlatformWin32.cpp
rg -n "All Characters Unlocked|Absolutely Everything Unlocked" \
  work/mg_userdir/GameSettings/GRSEAF.ini
```
