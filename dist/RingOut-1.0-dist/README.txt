Ring Out - Ver 1.0
=======================

This package contains NO game data and NO game code. You supply a
GameCube disc image that you already have; the setup step extracts it
and recompiles its executable on your machine.

QUICK START
  1. Run  ./RingOut
  2. It asks for your disc image (.iso / .gcm / .nkit.iso / .rvz)
  3. Setup extracts and recompiles it -- several minutes, one time only
  4. It launches, and every run after this starts straight away

  You can also do the setup step directly:
      ./setup.sh /path/to/your/disc.iso

REQUIREMENTS
  - A GameCube disc image you already have
  - cmake, ninja, python3, and clang (or gcc)
      Arch:    sudo pacman -S cmake ninja clang python
      Debian:  sudo apt install cmake ninja-build clang python3
  - A working Vulkan driver
  - About 1.5 GB free space for the extracted disc and build output

CONTROLS
  Escape          settings menu
  Arrow keys      navigate; Left/Right change a value or switch tab
  Space           confirm / activate
  Alt+W           toggle widescreen (16:9)
  Alt+Enter       fullscreen
  F1-F8           load state      Shift+F1-F8   save state
  Shift+Escape    quit

SETTINGS MENU TABS
  VIDEO     widescreen, internal resolution, aspect ratio, v-sync,
            anti-aliasing, anisotropic + texture filtering,
            texture packs, show FPS, free camera, fullscreen
  AUDIO     volume, mute, latency, fill audio gaps
  SYSTEM    emulation speed, save/load state, quit
  CONTROLS  rebind any pad button (Space to rebind, Left to clear)
  CHEATS    master switch, plus any codes you add yourself to
            userdata/GameSettings/<DISCID>.ini

FREE CAMERA (enable in the VIDEO tab)
  Shift + W/A/S/D   move          Shift + Q/E   down / up
  Shift + arrows    look          Shift + Z/C   roll
  Shift + 1/2       speed         Shift + R     reset view

WHAT IS IN THIS FOLDER
  RingOut    launcher; runs setup on first use
  setup.sh      extracts your disc and builds your module
  bin/          the runtime
  lib/          bundled support libraries
  tools/        dolrecomp, the static recompiler
  module-src/   build recipe and sources for the module

  Created on your machine by setup:
  game/         your extracted disc
  work/         recompiler output and build files
  bin/g<ID>_recomp.so   your recompiled module
  userdata/     settings, save states, screenshots

CREDITS, DISCLAIMER AND LICENSING
  See CREDITS.txt. Short version: this is an unofficial fan project by
  Jack Poison, made with AI assistance, built almost entirely on the
  work of the Dolphin Emulator Project, ExpansionPak's ModernGekko and
  DolRecomp, and Dear ImGui. The game is Bandai Namco's and is not
  included here.

  The runtime is GPL-2.0-or-later, so the matching source ships in
  source/ -- keep it with this package if you pass it on.

  Anything setup builds from your disc is derived from the game itself.
  Keep it to yourself.
