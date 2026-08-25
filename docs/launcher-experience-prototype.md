# RingOut launcher experience prototype

- Date: 2026-08-25
- Branch: `codex/launcher-experience`
- Base commit: `ff0ad952980f5083afd21c3d3758208a7a093d72`
- Implementation commit: `514cd424d11f8a3b9ac05fef07696e7d54bf7f48`
- Scope: source-built SDL3/Dear ImGui launcher prototype and native-module setup handoff

## Outcome

This branch replaces the launcher's single scrolling form with dedicated Play,
Game files, Netplay, Mods, and Settings screens. The presentation uses neutral
dark surfaces, one restrained blue accent, plain language, and conventional
desktop form layouts. Droid Sans is used for body text and Roboto Medium for
headings and the product name; both fonts are copied beside the launcher by the
build.

The `Game files` action is wired for source-tree experimentation. It:

1. asks for a GameCube ISO, RVZ, or WBFS image;
2. validates and atomically extracts it into the per-user game directory;
3. starts the sibling `moderngekko-port` helper with the release-owned module
   sources, dependency snapshot, and the GRSEAF idle-loop address;
4. streams explicit inspect, translate, configure, compile, and publish phases;
5. records the full helper output in `Logs/setup.log`; and
6. accepts only a module whose adjacent manifest matches the active disc ID and
   DOL hash, then passes that exact module explicitly to `moderngekko-run`.

The full first-run path was exercised on 2026-08-25 with the user's private,
ignored USA RVZ. It extracted `GRSEAF`, built and published a manifest-bound
native module, launched that exact module, reached the in-game first-run save
prompt, and accepted the generated keyboard profile's Return-to-Start binding.
No disc image or extracted content is tracked by Git.

The user-facing copy calls this “preparing” and “translating,” not
decompilation. The implementation is static recompilation.

## Source evidence

- Launcher setup state, child-process output capture, and progress mapping:
  `ModernGekko/tools/moderngekko_launcher.cpp:327-465` and `:988-1088`
- Existing module build/cache implementation and new explicit setup phase
  events: `ModernGekko/tools/moderngekko_port.cpp:340-622`
- Target name, RingOut identity, helper dependency, and required disc ID:
  `ModernGekko/CMakeLists.txt:461-550`
- Module discovery and explicit `--module` behavior:
  `ModernGekko/tools/moderngekko_run.cpp`
- GameCube controller-profile and keyboard fallback behavior:
  `ModernGekko/tools/frontend_config.cpp`

## Product decisions

- Play is a single-game launch surface, not a library or install selector. The
  launcher is fixed to Soulcalibur II `GRSEAF`; it keeps one configured set of
  local game files and replaces that set when setup is run with another image.
  The previous bordered game card and “selected game” language were removed
  because they incorrectly suggested a multi-install model.
- Game-file preparation has a separate screen because it is a long,
  inspectable workflow rather than a modal file picker.
- Labels are placed above text fields and selectors so that values and labels
  do not compete for horizontal space. Descriptions and file paths use explicit
  wrap bounds, and setup progress is a vertical status list that remains
  readable in the 900x600 minimum window.
- User-facing copy is factual. The launcher does not use taglines, arcade-style
  terminology, implementation jargon, or speculative mod features.
- Netplay defaults to fixed-delay and labels rollback Experimental. The
  rollback choice is driven by the runtime's authoritative production-readiness
  predicate and passes an explicit mode to the runner once available. The
  current rollback worktree's predicate exposes the choice. The memory-card-
  state rewind, teardown-latched output suppression, and corrected-frontier
  barrier fault changes identified in review are implemented, and the final
  Linux/source production rerun passed. Complete packaged and physical-machine
  validation is still required before calling the launcher path shipped.
  Manual delay values are SI samples, not frames.
- The Mods surface is intentionally informational and exposes no install
  action. RingOut does not yet have a versioned runtime mod contract,
  compatibility rules, deterministic load order, failure containment, or a
  mod-aware netplay fingerprint.
- The launcher now uses the GameCube pad configuration path and keyboard
  fallback. It no longer describes or generates a Wii Remote profile for this
  GameCube title.
- Closing the launcher is refused during preparation. There is no Cancel button
  yet because terminating a shell or helper without a process-group/job and
  staged publication could leave child compilers running or partial state on
  disk.
- `Replace game files` is an explicit full rebuild. It bypasses both the
  extracted-game fast path and the compiled-module cache, regenerates the
  translated code, and recreates the intermediate module build. The last
  published module and manifest remain in place until the replacement build is
  successfully published.

Dusklight informed the native single-shell model, asynchronous disc work, and
the rule that mod UI must follow a real runtime contract:

- <https://github.com/TwilitRealm/dusklight>
- <https://github.com/TwilitRealm/dusklight/blob/main/src/dusk/ui/prelaunch.cpp>
- <https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md>

The visual treatment and setup status list are RingOut-specific rather than a
copy of Dusklight.

## Release integration update

The rollback branch integrates this launcher into both release entry points.

`moderngekko-port` no longer compiles an absolute source-tree path. A local
build copies the release module template and GRSEAF settings beside the helper;
packaged builds resolve them from the Windows package root or AppImage payload.
On Windows the helper also resolves packaged Clang, CMake, Ninja, and Python.
Both workflows build `moderngekko-launcher`; the ZIP stages it as the top-level
`RingOut.exe`, and AppImage `AppRun` executes it with the private data directory
explicitly selected. Existing setup scripts/assets remain packaged as recovery
material.

The package gates now assert the launcher/helper/font/runtime closure and run a
non-graphical launcher resolution self-test. Do not call this shipped until the
remaining validation is complete:

- use a staging directory for the entire module build; final module, manifest,
  and active-pointer publication are staged, but intermediate compilation is
  not yet a recoverable transaction;
- add structured JSON Lines progress, recovery checkpoints, and safe
  process-tree cancellation;
- add launcher/setup fixtures for missing helpers, failed phases, incomplete
  output, and successful postcondition verification; and
- run physical Windows and Linux first-run tests with a supported disc.

For the rollback worktree specifically, both package workflows build and stage
the launcher and their smokes exercise its top-level self-test, but no complete
rollback-branch ZIP/AppImage has been retained or tag-published. Windows
cross-build tests remain disabled and neither workflow boots a real two-peer
game. The integrated package path is therefore CI-only implementation until the
exact workflows complete and physical/cross-machine rollback QA follows
(`.github/workflows/windows-cross.yml:118-142,190-238`;
`.github/workflows/linux-appimage.yml:117-164,264-309`).

## Reproduction and validation

Configure a native test tree with the host SDL3. The bundled SDL in this source
snapshot conflicts with this host's newer X11 headers, so this exact machine
needs `USE_SYSTEM_SDL3=ON`.

```bash
cmake -S ModernGekko -B /tmp/ringout-launcher-cmake-probe -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_SYSTEM_SDL3=ON \
  -DENABLE_QT=OFF \
  -DENABLE_TESTS=OFF \
  -DENABLE_ANALYTICS=OFF \
  -DENABLE_AUTOUPDATE=OFF \
  -DBUILD_TESTING=ON \
  -DMODERNGEKKO_ENABLE_DOLPHIN_TESTS=OFF

cmake --build /tmp/ringout-launcher-cmake-probe \
  --target moderngekko-launcher moderngekko-port \
           moderngekko_frontend_config_test moderngekko_game_inspect_test \
  --parallel "$(nproc)"

ctest --test-dir /tmp/ringout-launcher-cmake-probe --output-on-failure \
  -R '^(moderngekko\.frontend_config|moderngekko\.game_inspect)$'
```

Launch the prototype without touching normal user data:

```bash
XDG_DATA_HOME=/tmp/ringout-launcher-data \
  /tmp/ringout-launcher-cmake-probe/RingOut
```

A real game-setup test requires a legally obtained USA GameCube image with
disc ID `GRSEAF`. The private test RVZ is copied to the worktree root, where the
existing unanchored `*.rvz` rule excludes it from Git. Extracted and recompiled
test data stays under `/tmp`.

The real-test setup was:

```bash
sha256sum \
  '<path-to-legally-obtained-USA-rvz>' \
  'Soulcalibur II (USA).rvz'
git check-ignore -v 'Soulcalibur II (USA).rvz'

mkdir -p /tmp/ringout-rvz-e2e-20260825/home/Documents \
         /tmp/ringout-rvz-e2e-20260825/data
ln -s "$PWD/Soulcalibur II (USA).rvz" \
  '/tmp/ringout-rvz-e2e-20260825/home/Documents/Soulcalibur II (USA).rvz'

HOME=/tmp/ringout-rvz-e2e-20260825/home \
XDG_DATA_HOME=/tmp/ringout-rvz-e2e-20260825/data \
  /tmp/ringout-launcher-cmake-probe/RingOut --x11
```

Observed on 2026-08-25:

- the four requested native targets built successfully;
- `moderngekko.frontend_config` and `moderngekko.game_inspect` both passed;
- a missing-game helper smoke emitted
  `[ringout-setup] phase=inspect`, reported the unreadable game root, and exited
  1; and
- an X11 capture at 1080x720 showed the singular Game files screen with the
  bundled fonts, aligned text, replacement wording, and unclipped vertical
  status list. Its temporary capture hash was
  `e2a65ebfb8e7ddc5e23846c8dab736f8f65e3d6a26611160645d147509214107`;
- an exact UI-driven `Replace game files` regression test refreshed the
  extracted `main.dol` at `2026-08-25 12:39:27 -0500`, emitted all inspect,
  translate, configure, compile, and publish phases, regenerated 132 translated
  chunks, compiled all 136 targets, and returned the launcher to Play with
  Ready status. The replacement module was published at
  `2026-08-25 12:43:27 -0500`; its SHA-256 remained
  `dd0acbf89221b4ed4cdb7b1123af145397ff321937102ea484075728b51fade9`.
  The final Ready capture SHA-256 was
  `68c13ec97270864221d55b2271fc1e36b242777488687804cf9c0379f5cb6590`;
- a subsequent ordinary host-side build produced an immediate `cache hit` and
  publish phase, confirming that forced rebuilding is limited to the explicit
  replacement action;
- the source and copied RVZ hashes both matched
  `4b53a6013cc762c31d7a18283dc419969e89e6286864633a66213cb1e7a0fe3b`;
- the extracted `main.dol` hash and manifest value both matched
  `0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5`;
- the published 22,713,240-byte module hashed to
  `dd0acbf89221b4ed4cdb7b1123af145397ff321937102ea484075728b51fade9`;
- the runner loaded that module at entry `0x80003154`, opened
  `Ring Out Ver 1.0 | 39.4 FPS`, and reached Soulcalibur II's first-run save
  prompt; and
- a focused Return keypress advanced to the autosave-disabled confirmation,
  verifying the generated GameCube keyboard profile. The resulting capture
  hash was
  `2ebc2abda364264668412047bc0397e81f080076ffd79b9fe7afbfd2cc66f146`.
