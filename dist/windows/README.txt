Ring Out for Windows (x86-64)
================================

This is an experimental Windows build of the Ring Out static-recompilation
frontend. The exact package version, source revision, and source-download URL
are recorded in SOURCE.txt.

NO NINTENDO OR GAME DATA IS INCLUDED. You must provide a GameCube disc image
that you made and are entitled to use. Setup accepts a plain .iso or .wbfs
image. It extracts and recompiles that image locally; keep the resulting game/,
work/, and recompiled module files private. The C++ launcher stores those under
%LOCALAPPDATA%\RingOut, not inside this package.

QUICK START
-----------

  1. Extract the entire ZIP. Do not run it from inside the ZIP.
  2. Double-click RingOut.exe.
  3. On Game files, choose your plain GameCube .iso or .wbfs image.
  4. Keep the launcher open while its progress panel extracts and compiles.

The first setup needs roughly 1.5 GB of free space and may take several
minutes. Later launches start directly. The package includes the compiler,
CMake, Ninja, and Python needed for first-run compilation; no system-wide
development tools are required. Recompilation and the module build use all
logical processors. A bounded ThinLTO link cache is retained under
%LOCALAPPDATA%\RingOut\thinlto so later rebuilds can reuse linker work.

REQUIREMENTS
------------

  * 64-bit Windows 10 or 11.
  * A Vulkan-capable graphics driver.
  * PowerShell 5.1 or newer (included with supported Windows versions).
  * A writable NTFS/exFAT folder with about 1.5 GB free after extraction.

Some antivirus products flag compiler/linker programs such as clang.exe and
lld.exe. If setup says the bundled compiler cannot run, check quarantine and
restore the package from its published SHA-256-verified ZIP.

FFMPEG NOTE
-----------

FFmpeg is not bundled and is not required for normal playback. The optional,
developer-only STATICRECOMP_FMV_TAKEOVER mode invokes a program named
`ffmpeg.exe` from PATH. If you deliberately enable that environment variable,
install FFmpeg separately and make ffmpeg.exe available on PATH first. The
ordinary emulated FMV path does not use this external program.

CPU/GPU THREADING
-----------------

Offline play uses single-core CPU/GPU scheduling by default because it is the
known-safe mode for this static-recompilation core. Developers can explicitly
test the faster dual-core path by setting RINGOUT_DUAL_CORE=1 before launch;
leave it unset for normal play.

CONTROLS
--------

  Escape          settings menu
  Hold Back/View  open settings menu from a controller (about half a second)
  Arrow keys      navigate; Left/Right change a value or switch tab
  Space / Enter   confirm / activate
  D-pad / A / B   controller menu navigation / activate / back
  Tab (hold)      fast-forward
  Alt+W           toggle widescreen (16:9)
  Alt+Enter       fullscreen
  F10             pause / resume
  F1-F8           load state      Shift+F1-F8   save state
  Shift+Escape    quit

NETPLAY
-------

Both players need this same Ring Out release and the same GameCube disc
revision. Complete Game files and Controller setup, then open Netplay in the
launcher. The normal beta flow uses Host online room / Join online room and an
eight-character room code through Dolphin's hosted rendezvous, then connects
the players directly over UDP.

Online Room has no relay, authentication, encryption, room password, or IP
hiding, and strict NATs can fail. Use it only with trusted friends. Advanced
Direct IP remains a troubleshooting fallback.

The launcher can enable detailed netplay diagnostics and opens the Logs folder.
Logs can contain endpoint and local-host information, so inspect them before
sharing. The in-game network OSD shows RTT and actual rollback correction depth
and can be disabled in the launcher.

Starting netplay restarts the runtime into its lobby; this is expected. The
host's enabled AR/Gecko codes are synchronized to the guest before boot. Choose
unlock codes before starting—the Cheats tab is read-only during a match so one
peer cannot change deterministic state by itself.

The settings overlay does not pause only one peer during an active session.
Speed, F10, save/load/reset, controller rebinding, and cheats are locked while
the match continues. D-pad/A/B menu navigation is suppressed from the game.

FREE CAMERA (enable in the VIDEO tab)

  Shift + W/A/S/D   move          Shift + Q/E   down / up
  Shift + arrows    look          Shift + Z/C   roll
  Shift + 1/2       speed         Shift + R     reset view

PACKAGE CONTENTS
----------------

  RingOut.exe / RingOut.cmd / RingOut.ps1  launchers
  setup.ps1                                first-run extraction/build
  bin/moderngekko-run.exe                  Windows runtime
  bin/Sys/                                 required Dolphin resources
  tools/dolrecomp.exe                      GameCube static recompiler
  tools/moderngekko-port.exe               setup/build helper
  toolchain/                               Windows-native first-run tools
  module-src/                              source used to build your module
  shaders/                                 optional post-processing filters
  userdata/GameSettings/GRSEAF.ini         code list, all disabled by default
  LICENSES/, SOURCE.txt                    licences and exact source provenance

The manifest in MANIFEST.sha256 covers every shipped file other than the
manifest itself. CREDITS.txt and THIRD-PARTY-NOTICES.txt identify the upstream
projects. This is an unofficial fan project with no affiliation or endorsement
from Nintendo, Bandai Namco, Dolphin, or the other projects named there.
