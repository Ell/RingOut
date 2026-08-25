# Ring Out

A native PC port of a GameCube fighting game (disc ID `GRSEAF`), produced by **static
recompilation** rather than emulation: the disc's PowerPC executable is translated
ahead of time into C, compiled for x86-64, and run as native code inside a
Dolphin-derived runtime that provides the graphics, audio, input and hardware
emulation around it.

**No game data and no game code are distributed here.** You supply a disc image you
already own; setup extracts it and recompiles it on your machine.

---

## Status

Fully playable — boots to menu and through gameplay, with **zero interpreter
fallback**: every executed block runs as native code. The recompiler translates
535,368 instructions with 0 unknown opcodes. Rendering is Vulkan on the GPU, with
the CPU emulation and the runtime on separate cores.

**Steam Deck**: supported, with its own prebuilt package — no toolchain, no
compile step. Runs in both Desktop and Game Mode at 45–49 fps in a match.

**Windows x86-64**: revived as an experimental portable package. The runtime
and recompiler are cross-compiled with MinGW on Linux; first-run compilation
uses the Windows-native LLVM, CMake, Ninja and Python tools bundled in the ZIP.
Earlier prereleases have booted and reached gameplay on real Windows hardware;
the release workflow now also runs the exact portable ZIP through Wine and
builds/loads a synthetic recompilation module with its bundled tools. Physical
Windows setup, menu, gameplay and two-peer netplay QA is still recommended
before treating an experimental prerelease as stable.

**Netplay**: working. Fixed-delay play over a deterministic dual-core setup, with a lobby
showing live ping and per-player game status; two peers stayed byte-identical
over 6,470 frames.

---

## Getting it

Packages are available from the [Releases](../../releases) page:

| | for | needs a toolchain? |
| --- | --- | --- |
| `RingOut-1.2.1-linux-x86_64.zip` | desktop Linux | yes — compiles on your machine |
| `RingOut-1.2.1-steamdeck-x86_64.zip` | Steam Deck / SteamOS | no — prebuilt |
| `RingOut-1.2.1-ell.6-linux-x86_64.AppImage` | Linux x86-64 (experimental) | yes — compiles on your machine |
| `RingOut-1.2.1-ell.6-windows-x86_64.zip` | Windows 10/11 x86-64 (experimental) | no — toolchain bundled |

The Deck package ships no module: build one on a desktop with the package below,
then copy `game/` and `bin/gGRSEAF_recomp.so` across. Add `RingOut` to Steam as a
non-Steam game to launch it from Game Mode.

For desktop, unzip and run:

```sh
./RingOut
```

On first run it asks for your disc image (`.iso` / `.gcm` / `.nkit.iso` / `.rvz`),
then extracts and recompiles it — several minutes, once. Every run after that starts
straight away. You can also pass the image directly:

```sh
./RingOut /path/to/disc.iso     # or: ./setup.sh /path/to/disc.iso
```

On Windows, extract the entire ZIP to a writable folder and double-click
`RingOut.exe`. Select a plain GameCube `.iso` or `.wbfs` image when prompted.
Do not run it from inside the ZIP or install it under `Program Files`: first-run
setup creates `game/`, `work/`, and the private recompiled module beside the
launcher.

For the AppImage, make the download executable and run it:

```sh
chmod +x RingOut-1.2.1-ell.6-linux-x86_64.AppImage
./RingOut-1.2.1-ell.6-linux-x86_64.AppImage
```

Its read-only image keeps generated files, saves and settings under
`${XDG_DATA_HOME:-$HOME/.local/share}/ringout`. Set `RINGOUT_DATA_DIR` to choose
another dedicated writable directory. On a system without FUSE, set
`APPIMAGE_EXTRACT_AND_RUN=1`, or use `--appimage-extract` and run the extracted
`AppRun`. Keep the adjacent checksummed
`RingOut-1.2.1-ell.6-appimage-runtime-sources.tar.zst` release asset available
with the AppImage; it contains the exact static-prefix source and libfuse
relink materials named by the image's `SOURCE.txt`.

### Requirements

- A GameCube disc image you already own
- `cmake`, `ninja`, `python3`, and `clang` (or `gcc`)
  - Arch: `sudo pacman -S cmake ninja clang python`
  - Debian/Ubuntu: `sudo apt install cmake ninja-build clang python3`
- A working Vulkan driver
- ~1.5 GB free for the extracted disc and build output

The experimental Windows package requires 64-bit Windows 10 or 11, PowerShell
5.1 or newer, a Vulkan-capable driver, and about 1.5 GB of writable space after
extraction. Its compiler toolchain is included; antivirus software can
occasionally quarantine `clang.exe` or `lld.exe` and may need an exception for
the extracted package folder.

The legacy desktop ZIP binary is built on **glibc 2.44**. For older hosts its
launcher falls back to a bundled glibc in `libc-fallback/` — see
[Known issues](#known-issues) before relying on that. The AppImage runtime is
built on Debian 12 and validated with a **glibc 2.36** symbol-version ceiling;
it does not replace the host's graphics drivers.

---

## Features

- **Static recompilation** — native x86-64 execution, no interpreter fallback
- **Vulkan renderer** with internal-resolution scaling, AA, anisotropic filtering
- **Widescreen (16:9)** — `Alt+W`
- **HD texture pack support** — drop a pack in `userdata/Load/Textures/GRSEAF/`
- **In-game overlay**: pause menu with staged settings and Reset Game; Video, Audio,
  System, Controls and Cheats tabs
- **Full controller remapping**
- **Save states** — `Shift+F1`–`F8` to save, `F1`–`F8` to load
- **Free camera** — fly the camera anywhere in a match
- **Optional experimental FMV takeover** via external FFmpeg; it is off by
  default because the ordinary emulated Sofdec path measured faster
- **23 verified cheat codes** shipped in `GameSettings/GRSEAF.ini`
- **Netplay** — fixed-delay lockstep, with experimental rollback groundwork and a lobby
  showing live ping and per-player game status
- **Its own icon** — the disc banner and the memory-card icon are extracted from
  your disc and saves on your machine, and a desktop entry is written for you.
  None of that artwork ships; it is the publisher's.

### Controls

| Key | Action |
| --- | --- |
| `Escape` | settings menu |
| Arrow keys | navigate; Left/Right change a value or switch tab |
| `Space` / `Enter` | confirm / activate |
| Controller Back/View (hold) | settings menu |
| Controller D-pad / A / B | navigate / activate / back |
| `Alt+W` | toggle widescreen |
| `Alt+Enter` | fullscreen |
| `F1`–`F8` / `Shift+F1`–`F8` | load / save state |
| `Shift+Escape` | quit |

Free camera (enable in the Video tab): `Shift+WASD` move, `Shift+Q/E` down/up,
`Shift+arrows` look, `Shift+Z/C` roll, `Shift+1/2` speed, `Shift+R` reset.

### Netplay and cheats

Open the settings menu and use the System tab to host or join. Both players need
the same Ring Out release and compatible disc revision/module. The default port
is UDP 2626; direct Internet hosting may require forwarding the selected port.
Starting netplay restarts the runtime into its lobby, which is expected.
During a match the overlay stays live without pausing either peer. Core-state
controls (speed, pause, save/load/reset), controller rebinding, and cheats are
locked; menu navigation is suppressed from the game while the overlay is open.

Codes ship disabled. The host's enabled AR/Gecko codes are synchronized to the
guest before boot. Choose any unlock codes before starting: the Cheats tab is
read-only during a match so one peer cannot change deterministic state alone.
Unlock codes alter live emulated memory and do not guarantee permanent
memory-card progression.

---

## Performance work

Everything below is measured on a **fixed-work gameplay benchmark** — the same
emulated frames, driven by a frame-keyed input script, counted in retired CPU
cycles rather than wall time. That harness exists because the earlier one did
not measure the right thing: it ran boot and an idle menu, which carry ~0.1% of
a real session's paired-single traffic, so a change could look free there and
cost 3% in a match. **Earlier figures in this README were taken that way and have
been removed rather than restated.**

### What shipped

| Change | Effect |
| --- | --- |
| Inline the paired-single helper chain | psq cost per unit of work −43% |
| Compile loop back-edges as native `goto` | dispatches −66%, CPU −11.5% |
| Defer FPRF classification until FPSCR is observable | −2.14% cycles (Deck) |
| `-flto=auto` | module relink 22 min → 6 |

The FPRF one is the shape most of these take: the FP condition register is
written 3.07 billion times per run and read 15,885 times — mandatory by
architecture, dead in practice — so it is now computed only where something can
observe it, with frame hashes proving guest state is unchanged.

### What was measured and rejected

Kept here because the negative results cost as much to establish as the wins,
and each one looks plausible enough to be retried:

| Idea | Result |
| --- | --- |
| BOLT post-link layout | **8.1% slower** |
| `-O2` instead of `-O3` | **9.1% slower**, despite 12.5% less code |
| LLVM object backend | **4% slower**, even removing 43% of dispatches |
| Profile-guided optimisation | does not build — SIGBUS before the first frame |
| Leaders-only entry labels | 81% of execution fell out to the interpreter |
| Block-local register allocation | tied; no cross-iteration residency to exploit |
| CR / XER[CA] elision | ceiling ~0.05% and ~0.2% respectively |

The pattern: BOLT, `-O2` and the LLVM backend all bought instruction locality or
fewer dispatches, and all three lost by retiring more instructions at lower IPC.
PMU attribution explains why — front-end starvation is 2.4% of cycles and the
back end is saturated at IPC 1.92. The workload is not inefficient, just large:
86.4 million host instructions per emulated frame.


---

## Known issues

- **The Deck package is SteamOS-only.** Its runtime is built against glibc 2.36 in
  a Debian 12 container, which clears SteamOS and essentially every current
  distro. A build made natively on SteamOS instead has a 2.38 floor and will not
  run on older SteamOS releases — so the container build stays the shipped one.
- **Windows support is experimental.** Its MinGW cross-build uses Vulkan or
  OpenGL, Cubeb/OpenAL audio, and SDL input. Microsoft-SDK-only Direct3D,
  WASAPI, native Windows controller backends, and the native Bluetooth Wii
  Remote transport are omitted. QWave/DSCP traffic marking is also omitted;
  netplay itself remains available. The release is cross-built and Wine-smoked;
  physical Windows setup, gameplay, menu and two-peer netplay QA remains a
  recommended follow-up for every experimental prerelease.
- The `-march=native` build is machine-specific by design; setup compiles on your
  own machine, so this only matters if you copy a built folder to another CPU.

### Fixed since the last revision of this page

- **Netplay was described here as stubbed out.** It is not: there is a lobby with
  live ping and per-player game status, automatic pad assignment, and a
  host-controlled input buffer.
- **The shutdown abort** (`terminate called without an active exception`) was a
  real bug rather than a cosmetic one, and is fixed.
- **The bundled-glibc fallback on SteamOS** is no longer the plan. The predicted
  NSS failure is moot: the Deck package is built in a container against an old
  glibc instead, and is verified on hardware in both Desktop and Game Mode.

---

## Building from source

```sh
cmake -S ModernGekko -B ModernGekko/build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C ModernGekko/build
```

Then build the recompiled module for your disc with `dist/RingOut-1.0-dist/setup.sh`,
which drives `dolrecomp` and compiles the generated C.

To cross-compile the Windows runtime and recompiler from Linux, install a
64-bit MinGW-w64 POSIX-thread toolchain, CMake and Ninja, then run:

On Ubuntu 24.04, use the explicit POSIX packages
`gcc-mingw-w64-x86-64-posix` and `g++-mingw-w64-x86-64-posix`. Ubuntu also
installs Win32-thread variants under the generic compiler names, so add
`-DCMAKE_C_COMPILER=/usr/bin/x86_64-w64-mingw32-gcc-posix`,
`-DCMAKE_CXX_COMPILER=/usr/bin/x86_64-w64-mingw32-g++-posix`, and
`-DCMAKE_RC_COMPILER=/usr/bin/x86_64-w64-mingw32-windres` to the configure
command there, as the release workflow does. Arch's MinGW package exposes the
POSIX compiler under the unsuffixed names used by the toolchain file.

```sh
cmake -S ModernGekko -B build-windows-cross -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_SYSTEM_LIBS=OFF -DBUILD_TESTING=OFF \
  -DENABLE_QT=OFF -DENABLE_TESTS=OFF \
  -DENABLE_ANALYTICS=OFF -DENABLE_AUTOUPDATE=OFF
cmake --build build-windows-cross --target moderngekko-run dolrecomp --parallel
```

This cross-build deliberately excludes Windows backends that require the
Microsoft C++/WinRT SDK. The tag workflow in
`.github/workflows/windows-cross.yml` downloads checksum-pinned Windows-native
first-run tools, invokes `.github/scripts/package-windows-cross.sh`, validates
the PE import closure and privacy allowlist, and creates a draft prerelease.

Releases are assembled by script, never by zipping a working directory — those
hold the extracted disc, saves and build output. Each stage is built from an
allowlist and then checked: no disc-derived files, no personal data, and for the
desktop package, that the prebuilt runtime can actually load a module built from
the sources shipped beside it.

```sh
.github/scripts/package-deck.sh     # Steam Deck zip
.github/scripts/package-dist.sh     # desktop Linux zip
.github/scripts/package-windows-cross.sh --help
.github/scripts/package-appimage.sh --help
.github/scripts/package-appimage-runtime-sources.sh --help
.github/scripts/regen-source.sh     # refresh the GPL source shipment
```

### Repository layout

| Path | What it is |
| --- | --- |
| `ModernGekko/` | the runtime fork — chassis, settings overlay, StaticRecomp core |
| `ModernGekko/vendor/dolphin/` | the vendored Dolphin tree the runtime is built from |
| `DolRecomp/` | the static recompiler fork (PowerPC → C) |
| `dist/RingOut-1.0-dist/` | the desktop redistributable: launcher, `setup.sh`, module build recipe |
| `dist/RingOut-1.0-deck/` | the Steam Deck package scaffolding |
| `dist/windows/` | version-neutral Windows launcher and first-run setup scaffolding |
| `dist/appimage/` | version-neutral Linux AppImage entry point, metadata and documentation |
| `dist/shared/gc-art.py` | extracts the game's banner and icon on the player's machine |
| `.github/scripts/` | packaging, the privacy scan, the GPL source shipment, benchmarks |
| `work/mg_userdir/GameSettings/GRSEAF.ini` | the verified cheat codes |

Everything is vendored as plain files — clone and build, no submodule init needed.

---

## Credits

Almost none of the heavy lifting here is original. This is a thin layer on other
people's work:

- **[Dolphin Emulator Project](https://dolphin-emu.org)** — the runtime *is* Dolphin.
  Graphics, audio, input, save states, GameCube hardware emulation and the Vulkan
  backend are all theirs. GPL-2.0-or-later.
- **[ExpansionPak — ModernGekko](https://github.com/ExpansionPak/ModernGekko)** — the
  chassis that hosts a statically recompiled game inside Dolphin, and the
  StaticRecomp core that hands execution between recompiled code and the emulator.
- **[ExpansionPak — DolRecomp](https://github.com/ExpansionPak/DolRecomp)** — the
  static recompiler. GPL-3.0-or-later.
- **Dear ImGui** (Omar Cornut and contributors) — the settings overlay UI. MIT.
- **FFmpeg** — optional external program for the developer-only FMV takeover;
  it is not bundled or used by ordinary playback.
- **Bandai Namco Entertainment** — the original game, its code and all its assets.
  Not included, not redistributed, and not ours. All rights remain theirs.
- Action Replay cheat codes are published community data (Codejunkies, via Almar's
  Guides). None were written here.
- The r/decomps post that described the recompilation workflow this project followed.

Assembled by **Jack Poison**, working with AI assistance (Anthropic's Claude). The AI
wrote and debugged a large share of the code — the settings overlay, controller
remapping and cheat pages, the free-camera wiring, the FMV replacement path, several
recompiler codegen fixes and the packaging — under direction, with testing and
decisions made by a human.

---

## Licence

Project code covered by the root licence is **GPL-2.0-or-later**. See
[`LICENSE`](LICENSE).

The runtime derives from Dolphin (GPL-2.0-or-later) and ModernGekko
(GPL-3.0-or-later); DolRecomp is GPL-3.0-or-later. The portable Windows package
ships these compatible works together and is distributed under GPLv3-compatible
terms. Because the GPL obliges anyone receiving a binary to be able to get the
matching source, the full Dolphin and recompiler trees are vendored in this
repository rather than referenced.

`ModernGekko/LICENSE` and `ModernGekko/vendor/dolphin/COPYING` are upstream's own
terms and are left untouched.

---

## Disclaimer

An unofficial fan project. Not affiliated with, endorsed by, or connected to any of
the projects or companies named above. No game data or game code is included, and
none will be provided — do not ask. Anything setup builds from your disc is derived
from the game itself; keep it to yourself.
