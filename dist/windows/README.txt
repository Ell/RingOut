Ring Out for Windows (x86-64)
================================

This is an experimental Windows build of the Ring Out static-recompilation
frontend. The exact package version, source revision, and source-download URL
are recorded in SOURCE.txt.

NO NINTENDO OR GAME DATA IS INCLUDED. You must provide a GameCube disc image
that you made and are entitled to use. Setup accepts a plain .iso or .wbfs
image. It extracts and recompiles that image locally; keep the resulting game/,
work/, and bin/g<ID>_recomp.dll files private.

QUICK START
-----------

  1. Extract the entire ZIP to a writable folder. Do not run it from inside the
     ZIP, and do not put it under Program Files: setup writes beside RingOut.exe.
  2. Double-click RingOut.exe.
  3. Select your plain GameCube .iso or .wbfs image when prompted.
  4. Leave the setup console open while extraction and compilation run.

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
revision. Open the settings menu, stay on the SYSTEM tab, then:

  Host: set Netplay Mode to Host, choose the port (default 2626), and activate
        Start Netplay. Allow RingOut through Windows Firewall on private
        networks if prompted. The lobby waits for the other player.

  Join: set Netplay Mode to Join. On the same LAN, activate Scan for Hosts and
        select the result. For a direct address, select Netplay Address, press
        Space, edit its four octets with the arrows, press Space again, set the
        matching port, then activate Start Netplay.

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
  toolchain/                               Windows-native first-run tools
  module-src/                              source used to build your module
  shaders/                                 optional post-processing filters
  userdata/GameSettings/GRSEAF.ini         code list, all disabled by default
  LICENSES/, SOURCE.txt                    licences and exact source provenance

The manifest in MANIFEST.sha256 covers every shipped file other than the
manifest itself. CREDITS.txt and THIRD-PARTY-NOTICES.txt identify the upstream
projects. This is an unofficial fan project with no affiliation or endorsement
from Nintendo, Bandai Namco, Dolphin, or the other projects named there.
