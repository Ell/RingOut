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

---

## Getting it

Grab `RingOut-1.0-linux-x86_64.zip` from the
[Releases](../../releases) page, unzip, and run:

```sh
./RingOut
```

On first run it asks for your disc image (`.iso` / `.gcm` / `.nkit.iso` / `.rvz`),
then extracts and recompiles it — several minutes, once. Every run after that starts
straight away. You can also pass the image directly:

```sh
./RingOut /path/to/disc.iso     # or: ./setup.sh /path/to/disc.iso
```

### Requirements

- A GameCube disc image you already own
- `cmake`, `ninja`, `python3`, and `clang` (or `gcc`)
  - Arch: `sudo pacman -S cmake ninja clang python`
  - Debian/Ubuntu: `sudo apt install cmake ninja-build clang python3`
- A working Vulkan driver
- ~1.5 GB free for the extracted disc and build output

The release binary is built on **glibc 2.44**. For older hosts the launcher falls
back to a bundled glibc in `libc-fallback/` — see [Known issues](#known-issues)
before relying on that.

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
- **FMV playback** via FFmpeg, replacing the software Sofdec decoder
- **23 verified cheat codes** shipped in `GameSettings/GRSEAF.ini`

### Controls

| Key | Action |
| --- | --- |
| `Escape` | settings menu |
| Arrow keys | navigate; Left/Right change a value or switch tab |
| `Space` | confirm / activate |
| `Alt+W` | toggle widescreen |
| `Alt+Enter` | fullscreen |
| `F1`–`F8` / `Shift+F1`–`F8` | load / save state |
| `Shift+Escape` | quit |

Free camera (enable in the Video tab): `Shift+WASD` move, `Shift+Q/E` down/up,
`Shift+arrows` look, `Shift+Z/C` roll, `Shift+1/2` speed, `Shift+R` reset.

---

## Performance work

The recompiled module went through several rounds of optimisation, measured with a
headless benchmark counting native dispatches, each change lockstep-verified against
the interpreter for floating-point divergence (0 in every case):

| Change | Gain |
| --- | --- |
| `-march=native` | +8.9% |
| Profile-guided optimisation (gameplay + attract-mode profiles) | +11.6% under load |
| `ldexp` → inline `psq_pow2i` in the paired-single quantise path | +8.2% |
| BOLT layout optimisation | +3.1% |
| **Cumulative** | **≈ +33%** |

The `ldexp` one is the interesting case: paired-single dequantisation called libm
`ldexp` at 3,086 inlined sites for what is only a multiply by a power of two — 5.37
billion hot PLT calls, replaced with a bit-cast that is bit-identical over the
6-bit signed GQR scale range.

A register-allocation rewrite was prototyped and **rejected**: the dispatcher
returns to the loop head on every backward branch, so registers round-trip through
memory each iteration and there is no cross-iteration residency for a register
allocator to exploit. The measured benefit on realistic short blocks was 0.97–1.00×.

---

## Known issues

- **Shutdown abort.** Quitting can end with `terminate called without an active
  exception` (exit 134). Cosmetic — it happens after the session ends — but open.
- **Netplay is stubbed out.** The code paths exist but are not wired up.
- **The bundled-glibc fallback is untested on SteamOS / Steam Deck.** It is verified
  to work mechanically, but only on the machine the libraries came from. The
  predicted failure point is NSS: `libnss_files`/`libnss_dns` are dlopen'd from the
  *host* and built against the host glibc. **A crash mentioning `libnss` means
  bundling is exhausted and a container build is required.** On SteamOS, prefer
  `distrobox create -n ringout -i archlinux:latest` over `steamos-readonly disable`,
  which every system update reverts.
- The `-march=native` build is machine-specific by design; setup compiles on your
  own machine, so this only matters if you copy a built folder to another CPU.

---

## Building from source

```sh
cmake -S ModernGekko -B ModernGekko/build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C ModernGekko/build
```

Then build the recompiled module for your disc with `dist/RingOut-1.0-dist/setup.sh`,
which drives `dolrecomp` and compiles the generated C.

### Repository layout

| Path | What it is |
| --- | --- |
| `ModernGekko/` | the runtime fork — chassis, settings overlay, StaticRecomp core |
| `ModernGekko/vendor/dolphin/` | the vendored Dolphin tree the runtime is built from |
| `DolRecomp/` | the static recompiler fork (PowerPC → C) |
| `dist/RingOut-1.0-dist/` | the redistributable scaffolding: launcher, `setup.sh`, module build recipe |
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
- **FFmpeg** — used as a separate program to decode the game's Sofdec video.
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

**GPL-2.0-or-later.** See [`LICENSE`](LICENSE).

The runtime derives from Dolphin (GPL-2.0-or-later) and ModernGekko, whose sources
are tagged `GPL-2.0-or-later`; DolRecomp is GPL-3.0-or-later and is used as a
separate build-time tool. Because the GPL obliges anyone receiving a binary to be
able to get the matching source, the full Dolphin and recompiler trees are vendored
in this repository rather than referenced.

`ModernGekko/LICENSE` and `ModernGekko/vendor/dolphin/COPYING` are upstream's own
terms and are left untouched.

---

## Disclaimer

An unofficial fan project. Not affiliated with, endorsed by, or connected to any of
the projects or companies named above. No game data or game code is included, and
none will be provided — do not ask. Anything setup builds from your disc is derived
from the game itself; keep it to yourself.
