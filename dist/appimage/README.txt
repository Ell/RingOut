Ring Out for Linux (x86-64 AppImage)
====================================

This AppImage contains NO game data and NO game code. You supply a GameCube
disc image that you made and are entitled to use. First run extracts that image
and builds a private native module on your computer.

QUICK START
-----------

  1. Make the download executable:
       chmod +x RingOut-*-linux-x86_64.AppImage
  2. Run it and select a plain .iso or .wbfs disc image.
  3. Leave the setup terminal open while extraction and compilation finish.

An AppImage is read-only. Ring Out therefore stores generated files, saves,
settings, logs, and the private module in:

  ${XDG_DATA_HOME:-$HOME/.local/share}/ringout

Set RINGOUT_DATA_DIR before launch to choose another writable location. Always
start the AppImage (or the RingOut wrapper created in that data directory), not
RingOut.payload. Replacing the AppImage updates the packaged runtime while
preserving the data directory.

BUILD TOOLS REQUIRED ON FIRST RUN
---------------------------------

The AppImage bundles the Ring Out runtime, Dolphin Sys resources, and a static
DolRecomp executable. It does not bundle a Linux development environment. First
run still needs CMake, Ninja, Python 3, and a working C compiler plus libc
development headers:

  Arch:          sudo pacman -S --needed base-devel cmake ninja clang python
  Debian/Ubuntu: sudo apt install build-essential cmake ninja-build clang python3

Later launches do not recompile unless you remove the generated module. A
Vulkan-capable graphics driver is required; host graphics drivers are never
bundled or replaced by the AppImage. First-run recompilation/build uses all
logical processors; clang/lld builds retain a bounded ThinLTO cache below
${XDG_CACHE_HOME:-$HOME/.cache}/ringout/thinlto for later rebuilds.

MENU, NETPLAY, AND CHEATS
-------------------------

Escape opens the settings menu. Arrow keys navigate, Space/Enter activates,
and Shift+Escape quits. The controller's menu/back button can also open the
overlay, with its D-pad and face buttons used for navigation.

Netplay is started from the System tab. The host chooses Host and a port; the
other player chooses Join and enters the host address and the same port. Both
players need the same Ring Out build and compatible disc/module. Direct
Internet hosting may require forwarding the selected UDP port.

During a match the overlay remains live without pausing only one peer. Speed,
pause, save/load/reset, controller rebinding, and cheats are locked while the
session continues. D-pad/A/B menu navigation is suppressed from the game.

Cheat codes ship disabled. The host's enabled AR/Gecko codes are synchronized
to the guest before boot. Choose unlock codes before starting netplay: the
Cheats tab is read-only during a match so one peer cannot change deterministic
state by itself. Unlock codes alter live emulated memory and are not a guarantee
of permanent memory-card progression.

PACKAGE AND SOURCE
------------------

The AppImage includes README.txt, CREDITS.txt, SOURCE.txt, licenses, the module
build sources, and a manifest. SOURCE.txt records the exact Ring Out source
commit, embedded type-2 runtime provenance, and rebuild instructions. Every
release also includes an adjacent checksummed
RingOut-*-appimage-runtime-sources.tar.zst with the exact static-prefix source
and libfuse relink materials; keep it available with the AppImage. On a system
without FUSE, launch with
APPIMAGE_EXTRACT_AND_RUN=1, or use --appimage-extract and run AppRun from the
extracted AppDir.

This is an unofficial fan project. It is not affiliated with or endorsed by
Nintendo, Bandai Namco, Dolphin, AppImage, or the other projects named in the
credits. Anything generated from your disc is private; do not redistribute it.
