# RingOut Launcher UI Research

Date: 2026-08-25
Audited baseline: `ff0ad952980f5083afd21c3d3758208a7a093d72`

## Summary

RingOut can support a proper cross-platform graphical launcher without starting
from scratch. The repository already contains an SDL3/Dear ImGui launcher that
can select and extract a disc, configure controllers and graphics, launch solo
play, host or join netplay, capture the game log, and translate common netplay
exit codes into useful errors.

The release packages do not currently use that launcher. Windows launches a
small native wrapper which delegates first-run setup to PowerShell, while Linux
and the AppImage delegate it to shell scripts. Those scripts own the expensive
static-recompilation pipeline. At the audited baseline, the graphical launcher
only showed disc extraction progress and could not prepare the recompiled game
module used by the release.

The recommended approach is to retain a native C++ launcher, extract setup into
a cross-platform helper with a structured progress protocol, and make both the
GUI and CLI/script entry points use that helper. This also creates a stable
place for repair, diagnostics, updates, and eventually mod installation.

## Implementation update

Commit `514cd424d11f8a3b9ac05fef07696e7d54bf7f48` implements a source-tree
prototype of the first part of that recommendation. The SDL3/Dear ImGui target
is branded as RingOut and now drives extraction, translation, configuration,
compilation, atomic module publication, replacement, and launch through the
sibling `moderngekko-port` helper. The exact UI and RVZ validation evidence is
recorded in [Launcher experience prototype](launcher-experience-prototype.md).

This does not close the release packaging gap described below. The Windows ZIP
and Linux AppImage still use their existing wrapper and script entry points, and
the helper still depends on source-tree-owned module inputs. Unless explicitly
marked otherwise, the component inventory below describes the audited
`ff0ad952` baseline; the proposed release stages remain future work.

## Baseline RingOut Components

### Existing graphical launcher

`ModernGekko/tools/moderngekko_launcher.cpp` already provides:

- SDL3 window, renderer, file dialog, and controller discovery
- Dear ImGui interface with keyboard navigation and DPI scaling
- Game disc discovery, selection, validation, and extraction
- Per-file extraction status and a progress bar
- Controller-profile creation and selection
- Internal-resolution and FPS-title settings
- Solo, host-netplay, and join-netplay launch modes
- Child-process creation, log redirection, exit monitoring, and useful netplay
  failure messages

At the audited baseline, the target is declared as `moderngekko-launcher` in
`ModernGekko/CMakeLists.txt` and outputs an executable named `ModernGekko`.

### Release setup pipeline

The release setup scripts perform the work the existing GUI does not:

1. Validate the supplied GameCube ISO or WBFS image and host toolchain.
2. Extract the disc with DolRecomp.
3. Translate the PowerPC `main.dol` into generated C sources.
4. Configure the generated module with CMake.
5. Compile and link it with Ninja into `g<disc-id>_recomp.dll` or `.so`.
6. Install the module, shaders, artwork, and platform integration files.

This process is static recompilation, rather than conventional decompilation.
The launcher can use the friendly top-level label "Preparing your game" while
showing the technical phase beneath it.

Relevant release entry points:

- `dist/windows/RingOut.ps1`
- `dist/windows/setup.ps1`
- `dist/RingOut-1.0-dist/RingOut`
- `dist/RingOut-1.0-dist/setup.sh`
- `dist/appimage/AppRun`

### Packaging gap

Windows packages currently ship `RingOut.exe`, a small C wrapper, rather than
the C++ `moderngekko-launcher` target. The AppImage similarly installs a shell
payload as its primary entry point. Packaging the current C++ launcher alone
would not solve first-run setup because its extraction path never runs
DolRecomp code generation or the module build.

## Dusklight Reference

Dusklight is a useful product and architecture reference, but it is not an
Electron/Tauri launcher wrapped around a separate game. Its prelaunch interface
is part of the native C++ application and uses SDL with a styled RmlUi-based UI.
It includes asynchronous disc validation, cancellable progress, configuration,
updates, and game startup in one shell.

Sources:

- Repository: <https://github.com/TwilitRealm/dusklight>
- Prelaunch UI: <https://github.com/TwilitRealm/dusklight/blob/main/src/dusk/ui/prelaunch.cpp>
- Modding design: <https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md>
- Mod manager UI: <https://github.com/TwilitRealm/dusklight/blob/main/src/dusk/ui/mods_window.cpp>
- Mod loader types: <https://github.com/TwilitRealm/dusklight/blob/main/src/dusk/mod_loader.hpp>

The most valuable Dusklight ideas for RingOut are:

- Treat setup as an explicit asynchronous state machine.
- Keep the detailed task visible without forcing users to understand a console.
- Offer cancellation at safe points and report whether partial state remains.
- Put disc, configuration, updates, logs, and launch actions in one native shell.
- Back a mod manager with a real runtime mod contract instead of only copying
  arbitrary files.
- Give mods stable IDs, metadata, compatibility rules, lifecycle states, error
  messages, dependencies, and separate persistent data directories.

Dusklight's mod implementation is considerably more advanced than a launcher
feature. Its `.dusk` ZIP bundles can contain resources, disc overlays, texture
replacements, and native libraries. Native mods use lifecycle exports and
individually versioned C service APIs, and the runtime understands dependency
ordering, reloads, failures, and dependent-mod suspension.

## Recommended Architecture

```text
RingOut Launcher
|-- Home / Play
|-- Setup and repair UI
|-- Netplay
|-- Controllers and settings
|-- Logs and diagnostics
`-- Mods (after a runtime mod contract exists)
    |
    |-- ringout-setup helper
    |   |-- Disc validation and extraction
    |   |-- DolRecomp translation
    |   |-- CMake and Ninja compilation
    |   `-- Module validation and installation
    |
    `-- moderngekko-run
        |-- Game runtime
        `-- Future resource and mod APIs
```

### Setup helper

Create a small cross-platform `ringout-setup` executable. It should be the sole
owner of the setup state machine and should invoke the existing DolRecomp,
CMake, Ninja, artwork, and installation operations. The GUI must not duplicate
the setup logic.

Benefits:

- The GUI remains responsive and can survive helper failures.
- Windows and Linux use the same phase and error model.
- CLI and automated smoke tests can exercise the exact player setup path.
- Setup output can be logged and attached to diagnostics without scraping a
  terminal window.
- A future launcher rewrite does not require another setup rewrite.
- Cancellation, retry, checkpoints, and repair can be implemented once.

### Structured progress protocol

The helper should write versioned JSON Lines events to stdout and human-readable
diagnostics to a persistent log. Example events:

```json
{"protocol":1,"type":"phase","id":"translate","label":"Translating game executable","index":3,"total":5}
{"protocol":1,"type":"progress","phase":"translate","current":728,"total":1840,"unit":"functions"}
{"protocol":1,"type":"log","level":"info","message":"Generating recomp_0728.c"}
{"protocol":1,"type":"artifact","kind":"module","path":"gGRSEAF_recomp.dll"}
{"protocol":1,"type":"complete","ok":true}
```

Suggested setup phases:

1. Validating disc and toolchain
2. Extracting game files
3. Translating the PowerPC executable
4. Compiling and linking the native module
5. Validating and installing the result

Each phase should have its own progress measurement. Overall progress should be
weighted using measured phase durations rather than treating all phases as
equal. Indeterminate operations, such as the initial CMake configure, should be
shown as indeterminate instead of presenting invented percentages.

DolRecomp should ideally emit structured progress directly. Ninja progress can
be made machine-readable using `NINJA_STATUS` or wrapped at the helper boundary.
Parsing existing free-form console text should only be a compatibility fallback.

### Process behavior

The launcher should:

- Run setup and the game as monitored child processes.
- Read progress asynchronously without blocking the render/event loop.
- Persist a complete log even when the compact UI hides verbose output.
- Offer "Cancel setup" only when the helper can terminate children and leave a
  known recoverable state.
- Ask before closing during setup, with choices to keep running in the
  background or cancel safely.
- Publish completed builds atomically from staging directories.
- Detect incomplete setup on the next launch and offer Resume, Retry, or Clean.
- Keep a stable diagnostic bundle containing versions, paths, phase results,
  exit codes, and logs, but no copyrighted disc contents.

## User Experience

### Home

- Locally extracted disc artwork and game identity
- Installation state: Not configured, Preparing, Ready, Needs repair, or Failed
- Primary Play button
- Host Netplay and Join Netplay actions
- Last-played and current runtime/recomp-module versions
- Repair or Finish setup action when not ready

### Setup

- Disc picker with format and game-region validation
- Phase timeline with current phase highlighted
- Current-phase and overall progress
- Elapsed time, task count, and active worker count where meaningful
- A short changing status line
- Expandable live log with Copy and Open log actions
- Clear recovery instructions derived from structured errors
- Cancel, Retry, and Resume actions

### Netplay

- Nickname, host/IP, UDP port, and automatic/manual buffer
- Controller slots and selected local controllers
- Compatibility summary for runner, game revision, module, and enabled mods
- Host and Join actions with firewall/port feedback
- Session log link after failure

### Settings

- Controller devices, mapping profile, and refresh action
- Display mode, internal resolution, FPS display, and graphics backend
- Audio backend and device
- User-data location and portable mode
- Open data, logs, screenshots, and mods directories
- Rebuild module and clear disposable build cache actions

### Support

- Copy system and version summary
- Create privacy-reviewed diagnostic archive
- View launcher/setup/game logs
- Verify installation
- Re-run toolchain probe
- Open the release/source page

## UI Technology

### Recommended initial choice

Continue with native C++ and SDL3. Use the existing Dear ImGui launcher to ship
the functional setup state machine first, while separating UI state from setup
and launch services. ImGui is already built and exercised across the same
platforms as the runner, so it is the lowest-risk way to make setup observable.

For a more polished, controller-first Dusklight-like presentation, RmlUi is a
reasonable later view layer. It permits HTML/CSS-like layout and animation
without adding a browser engine. The backend and screen state should not depend
on ImGui so this transition remains possible.

Tauri or Electron would add another language/toolchain or webview runtime,
platform-specific process integration, and more packaging surface. They do not
remove any of the native setup work. They are not the preferred choice for the
current RingOut codebase.

## Data Layout

Use a consistent per-user data root:

- Windows: `%LOCALAPPDATA%\RingOut`
- Linux: `${XDG_DATA_HOME:-$HOME/.local/share}/ringout`

Suggested layout:

```text
RingOut data root/
|-- games/GRSEAF/                 extracted private game data
|-- builds/GRSEAF/<build-id>/     generated module and build metadata
|-- userdata/                     saves and runtime configuration
|-- mods/                         installed mod packages
|-- mod-data/                     per-mod persistent state
|-- cache/                        disposable compiler and extracted-mod caches
|-- logs/                         launcher, setup, game, and per-mod logs
`-- state.json                    installation and migration state
```

The AppImage mount is read-only, so all generated state must remain in this data
root. Windows should migrate existing portable ZIP installations rather than
silently abandoning their `game`, `userdata`, and module directories. An
explicit `portable.flag` can preserve the current beside-the-executable layout
for users who want a self-contained install.

## Mod Manager Roadmap

A launcher can install and display mods, but it cannot make arbitrary mods work
without support from `moderngekko-run` and the recompiled module. Define the mod
contract before building a store-like interface.

### First supported content types

Begin with types that can be validated and applied without native code:

- Texture replacements
- Post-processing shaders
- Gecko or Action Replay code packs
- Controller and configuration presets
- Resource/file overlays implemented through a runtime virtual filesystem

### Package format

Use a ZIP-based `.ringmod` package with a root manifest. For example:

```json
{
  "schema": 1,
  "id": "org.example.hd-textures",
  "name": "HD Texture Pack",
  "version": "1.0.0",
  "author": "Example Author",
  "ringoutApi": "1",
  "gameIds": ["GRSEAF"],
  "dependencies": [],
  "conflicts": [],
  "content": {
    "textures": "textures/",
    "shaders": "shaders/"
  }
}
```

Installation should use a staging directory, reject absolute and `..` archive
paths, verify size limits and hashes, and atomically publish the completed mod.
Packages should not contain post-install scripts. Original game assets must not
be redistributed.

### Runtime requirements

The eventual runtime needs:

- Stable mod and service ABI versions
- Compatibility with a specific RingOut build and supported game revisions
- Dependency and conflict resolution
- Deterministic load order
- Resource overlay precedence
- Enable/disable state stored outside the package
- Per-mod data, cache, and logs
- Failure reporting that can disable the offending mod cleanly
- Netplay compatibility fingerprints covering simulation-affecting mods

Native DLL/SO mods should be a later milestone. They are arbitrary executable
code and require prominent trust warnings, exact architecture and ABI checks,
dependency handling, and a clear crash/failure model. Static-recompiled game
hooks also need a stable symbol or hook registry; a launcher UI cannot supply
that contract by itself.

## Suggested Delivery Stages

### Stage 1: Setup backend

- Introduce `ringout-setup` and its JSON Lines protocol.
- Move the PowerShell/Bash orchestration behind it.
- Preserve current scripts as thin compatibility frontends.
- Add fixture-driven protocol, failure, cancellation, and resume tests.

### Stage 2: Release launcher

- Rebrand and package the existing C++ launcher as RingOut.
- Add the full setup screen, persistent logs, repair, and diagnostics.
- Make Windows ZIP and AppImage launch it directly.
- Migrate existing installations and retain an explicit portable mode.
- Test first run, interrupted setup, retry, normal play, game crash, host, and
  join on real Windows and Linux environments.

### Stage 3: Product polish

- Add locally extracted artwork and a controller-friendly screen hierarchy.
- Improve accessibility, DPI behavior, keyboard/controller navigation, and
  screen-reader labeling where the selected UI stack permits it.
- Add update awareness without silently replacing generated or user data.
- Evaluate replacing the ImGui view with RmlUi after backend behavior is stable.

### Stage 4: Mod foundation

- Specify `.ringmod`, resource overlay rules, compatibility fingerprints, and
  netplay policy.
- Implement asset-only mod loading and launcher installation first.
- Add dependency/conflict UI and diagnostics.
- Design a versioned native hook/service ABI only after the safe content path is
  proven.

## Acceptance Criteria for the First GUI Release

- A new user can select a valid disc and complete setup without opening a
  terminal or PowerShell window.
- Every long operation visibly reports its current phase and activity.
- Failure messages identify the failed phase, give an actionable remedy, and
  link to the full log.
- Closing or cancelling setup cannot publish a partial game/module as ready.
- A completed installation launches solo play and both netplay modes from the
  same GUI.
- Windows and AppImage use the same setup state model and error vocabulary.
- Existing saves/configuration survive launcher upgrades and data migration.
- Automated smoke tests exercise the exact packaged launcher/helper/runtime
  chain, while at least one release test verifies real first-run setup on each
  supported operating system.
