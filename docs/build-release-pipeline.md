# Build and release pipeline handoff

- Audit date: 2026-08-25
- Audited commit: `ff0ad952980f5083afd21c3d3758208a7a093d72` (`v1.2.1-ell.6`)
- Scope: Windows x86-64 cross-build and first-run module build, CI parallelism and caches, release-asset publication, and Linux x86-64 AppImage packaging.

This document separates what the audited commit does from suggestions that have not been implemented. Commands and line anchors are for the audited commit; re-check them after changing a workflow or packaging script.

## Executive answers

> Rollback-branch entry-point update (2026-08-25): the C++ launcher now owns the
> player-facing fixed-delay/Experimental rollback selector and is built by both
> release workflows. The Windows ZIP stages it as `RingOut.exe`; AppImage's
> `AppRun` executes `usr/bin/RingOut`. `moderngekko-port` resolves shipped
> module sources, game settings, and the bundled Windows toolchain relative to
> its executable instead of embedding a checkout path. Package and Wine/AppRun
> smoke gates verify the C++ launcher, fonts, runner, dependency closure, and
> the helper's exact module-source/game-settings resolution. This is
> implementation evidence in scripts/workflows, not a completed package run for
> this branch. Windows still configures `BUILD_TESTING=OFF` while Linux builds
> and runs the CTest target set (`.github/workflows/windows-cross.yml:118-142`;
> `.github/workflows/linux-appimage.yml:117-164`). No complete Windows ZIP or
> AppImage from the rollback worktree has been retained from a manual job,
> published by a tag, or tested on physical/cross-machine peers. Manual workflow
> dispatch is validation-only and intentionally retains no package
> (`.github/workflows/windows-cross.yml:214-238` and
> `.github/workflows/linux-appimage.yml:311-358`). Treat full-package rollback
> coverage as CI-only and still pending, not as a shipped artifact claim.

| Question | Current answer at the audited commit |
| --- | --- |
| How is Windows built? | The runtime and `dolrecomp.exe` are cross-compiled directly on `ubuntu-24.04` with distro MinGW-w64 GCC, GNU binutils, CMake, and Ninja. The Windows workflow does not use Docker. See `.github/workflows/windows-cross.yml:78-96,118-142` and `cmake/toolchains/mingw-x86_64.cmake:1-45`. |
| Is LLD required? | Not for the Linux-to-Windows runtime cross-build. It is, however, an expected and release-gated part of the bundled Windows first-run module toolchain: the package must contain an LLVM linker, and the Wine smoke test must configure and link a generated DLL with bundled LLD. The module CMake retains a default-linker fallback for non-package environments. See `.github/scripts/package-windows-cross.sh:350-362`, `.github/scripts/smoke-windows-package.sh:75-93,127-179`, and `dist/RingOut-1.0-dist/module-src/CMakeLists.txt:156-204`. |
| Must a Windows user install a compiler or linker? | No. The ZIP includes Windows-native Clang/LLVM-MinGW (including `ld.lld.exe`), CMake, Ninja, and Python, plus the static recompiler and module sources. `setup.ps1` uses them in place. See `.github/workflows/windows-cross.yml:39-43,152-203`, `.github/scripts/package-windows-cross.sh:288-362`, and `dist/windows/setup.ps1:9-10,30-68,217-245`. |
| Do we need QWave? | Not for this MinGW build. Dolphin's QWave implementation and `qwave.lib` link are excluded when `MINGW`/`__MINGW32__` is set; the MinGW implementation is a no-op because Ubuntu 24.04's MinGW headers lack required declarations. Netplay remains functional without DSCP/QoS marking. See `ModernGekko/vendor/dolphin/Source/Core/Common/QoSSession.cpp:6-61` and `ModernGekko/vendor/dolphin/Source/Core/Common/CMakeLists.txt:311-323`. |
| Are builds using all threads? | Yes for the CMake build and test phases: Windows uses `--parallel "$(nproc)"`; AppImage runtime, tests, and static DolRecomp use `--parallel "$(nproc)"` or `ctest -j"$(nproc)"`; Windows first-run setup uses `[Environment]::ProcessorCount` for both DolRecomp and CMake. See `.github/workflows/windows-cross.yml:141-142`, `.github/workflows/linux-appimage.yml:142-197`, and `dist/windows/setup.ps1:172-180,240-241`. |
| Can a workflow request more threads? | The commands already consume every logical CPU exposed to the job. `runs-on: ubuntu-24.04` chooses the standard runner class; a larger GitHub-hosted runner or self-hosted label would be needed to expose more CPUs. Increasing `--parallel` beyond `nproc` cannot create runner capacity. See both workflows at `.github/workflows/windows-cross.yml:27-30` and `.github/workflows/linux-appimage.yml:26-29`. |
| What is cached? | Windows and AppImage use separate cross-run ccache directories; pinned tool downloads are cached; the AppImage Docker image uses the GitHub Actions BuildKit cache. The Windows player's bounded ThinLTO link cache persists in `%LOCALAPPDATA%`. Build trees, successful manual-run packages, and a player-side compiler object cache are not cached. |
| Why can a green build have no ZIP/AppImage download? | Manual dispatch is deliberately validation-only. Its package exists only in the ephemeral runner workspace and is not uploaded. Only a matching tag push uploads package assets to a draft prerelease; failure-only CMake logs use Actions artifacts. See `.github/workflows/windows-cross.yml:15-19,214-263` and `.github/workflows/linux-appimage.yml:15-18,311-406`. |
| How are releases coordinated? | A `v*-ell.*` tag starts both platform workflows. Windows is the sole draft creator; Linux waits for and joins that same source-bound draft. Identical assets are idempotent; same-name/different-digest assets, moved tags, duplicate drafts, a non-draft release, or mismatched provenance fail closed. The release remains a draft prerelease until a human publishes it. |
| Is Linux AppImage release support present? | Yes. The tagged workflow builds in a digest-pinned Debian 12 image, packages and validates an AppImage, checksum, runtime corresponding-source/relink archive, and its checksum, then joins the Windows-created draft. Manual dispatch validates but retains nothing. See `.github/workflows/linux-appimage.yml`. |

## 1. Windows has two different compilation stages

It is important not to conflate the release cross-build with the player's first-run recompilation.

### 1.1 Release CI cross-compiles the frontend and recompiler

The workflow installs Ubuntu's POSIX-threaded MinGW-w64 GCC/G++, GNU PE binutils, CMake, Ninja, ccache, and Wine (`.github/workflows/windows-cross.yml:78-96`). It configures `ModernGekko` with:

- `CMAKE_SYSTEM_NAME=Windows` through `cmake/toolchains/mingw-x86_64.cmake`;
- explicit `/usr/bin/x86_64-w64-mingw32-gcc-posix`, `g++-posix`, and `windres` tools;
- Release mode, bundled dependencies, Qt/tests/analytics/updater disabled; and
- ccache as the C and C++ compiler launcher.

The exact configure command is at `.github/workflows/windows-cross.yml:118-139`.
The workflow requests `moderngekko-launcher` with all runner CPUs at lines
141-142; its dependency graph builds the launcher, `moderngekko-port`,
`moderngekko-run`, and `dolrecomp`. `ModernGekko/CMakeLists.txt:134-162` shows
that the live sibling `DolRecomp` tree is the one added to this build.

The cross-toolchain file also handles two Linux-host-specific hazards:

- case aliases for Windows SDK header spellings (`cmake/toolchains/mingw-x86_64.cmake:12-18`); and
- target-only CMake/pkg-config lookup, preventing Linux headers or libraries from leaking into a PE build (`cmake/toolchains/mingw-x86_64.cmake:20-45`).

This stage uses GNU MinGW's default PE linker from `binutils-mingw-w64-x86-64`. It does not install or select LLD. LLD therefore is not required to cross-compile the shipped frontend or `dolrecomp.exe`.

### 1.2 The Windows user recompiles a private module locally

No game-derived DLL is shipped. On first run, `dist/windows/setup.ps1`:

1. validates and extracts the user's plain GameCube ISO/WBFS with packaged `tools/dolrecomp.exe` (`dist/windows/setup.ps1:97-170`);
2. invokes DolRecomp with `-j[Environment]::ProcessorCount` to generate native C chunks (`dist/windows/setup.ps1:172-183`); and
3. configures and builds `g<disc-id>_recomp.dll` with packaged CMake, Ninja, Clang, Python, and the module sources (`dist/windows/setup.ps1:185-245`).

The package carries those tools because first-run compilation happens on the player's Windows machine. The Linux CI cross-compiler itself is not copied. The Windows-native archives are separately downloaded, pinned, checksummed, unpacked, and filtered during packaging:

- LLVM-MinGW `20260616` UCRT x86-64;
- CMake `4.4.2` for Windows x86-64;
- Ninja `1.13.2`; and
- Python embeddable `3.11.9`.

The filenames, URLs, and SHA-256 values are at `.github/workflows/windows-cross.yml:39-43,152-188`. The packager copies only the x86-64 target from LLVM-MinGW, drops unrelated ARM/i686 sysroots and LLVM-MinGW's duplicate Python, and then adds the separately pinned tools (`.github/scripts/package-windows-cross.sh:288-348`). It refuses a toolchain missing Clang, CMake, Ninja, Python, an LLVM linker, Python's standard-library ZIP, Windows headers, or CMake modules (`.github/scripts/package-windows-cross.sh:350-362`).

The user therefore downloads and extracts one ZIP. They do **not** separately download Clang, LLD, a Windows SDK, CMake, Ninja, or Python. Other boundaries remain intentional:

- system Windows DLLs and a Vulkan-capable graphics driver come from Windows/the driver vendor;
- ordinary non-system runtime DLL imports are copied beside the PE by the package import-closure pass (`.github/scripts/package-windows-cross.sh:364-446`);
- FFmpeg is not bundled and is needed only for the opt-in developer `STATICRECOMP_FMV_TAKEOVER` path (`dist/windows/README.txt:41-48`); and
- no disc image, extracted game, save, generated module, or gameplay profile ships (`.github/scripts/package-windows-cross.sh:478-493`).

## 2. LLD: optional in source, required by the portable-package gate

The generated module defaults to LTO. Clang receives `-flto=thin`; GCC 10+ receives `-flto=auto` (`dist/RingOut-1.0-dist/module-src/CMakeLists.txt:77-105,358-361`). `MODULE_LLD` defaults on, but CMake probes `-fuse-ld=lld` before enabling it. If the probe fails, configuration reports:

```text
module: lld not usable, linking with the default linker
```

and continues (`dist/RingOut-1.0-dist/module-src/CMakeLists.txt:156-204`). That fallback is useful for unpackaged Linux/local environments where Clang and LLD are separate packages. It does not mean an official Windows ZIP is supposed to omit LLD.

For the Windows portable package, LLD is effectively a release requirement:

- the packager fails unless `toolchain/bin/ld.lld.exe` or `lld-link.exe` exists (`.github/scripts/package-windows-cross.sh:350-356`);
- the smoke test specifically requires `ld.lld.exe`, starts the packaged setup
  helper under Wine, and rejects configuration unless the helper's CMake output
  prints `module: linking with lld` (`.github/scripts/smoke-windows-package.sh:75-93,119-156`);
- the verbose link must contain `-fuse-ld=lld` or `ld.lld`, and the link must populate a ThinLTO cache (`.github/scripts/smoke-windows-package.sh:152-159`); and
- the generated DLL is checked for both required exports and loaded through Windows `ctypes`, which also catches unresolved imports (`.github/scripts/smoke-windows-package.sh:161-179`; `.github/scripts/windows-package-smoke.py:37-57`).

This is an actual compile/link/load test, not merely checking that files exist.
It creates a project-authored extracted GRSEAF game around a 264-byte synthetic
GameCube DOL, then runs `tools/moderngekko-port.exe build` through every player
setup phase. The helper must find and directly launch sibling DolRecomp plus the
packaged Python/CMake/Ninja/Clang/LLD stack, publish a PE DLL, and load/check its
ABI under Wine (`.github/scripts/smoke-windows-package.sh:95-176`). Package,
game, and output paths deliberately contain spaces. The workflow runs that test
after assembling every candidate ZIP (`.github/workflows/windows-cross.yml:190-212`).

The test does not cover a physical Windows kernel, antivirus quarantine, driver
behavior, or a proprietary full game DOL. Those remain platform QA gaps; see
section 9.

### Windows direct child-process boundary (`ff3a49fd`, 2026-08-25)

The public `.ell.10` ZIP contains `tools/dolrecomp.exe` and its complete import
closure, but its integrated first-run path is broken. `moderngekko-port` found
the correct sibling path, rendered it with a leading quote, and passed the
whole command string to `std::system()`. Windows `cmd.exe` removed/misassociated
that quote and treated the executable plus arguments as one missing command.
The exact public ZIP reproduced this under Wine with package paths both with
and without spaces. The failure text began `Can't recognize
'...tools\\dolrecomp.exe\" -j16 ...'`; the file itself was present.

The helper now models every compiler probe, DolRecomp translation, CMake
configure, and CMake build as an argument vector. Windows uses `CreateProcessA`
directly with CRT-compatible argument quoting and a combined stdout/stderr
pipe; it no longer invokes `cmd.exe`. Linux retains its explicit shell path so
the AppImage host-environment prefix remains auditable
(`ModernGekko/tools/moderngekko_port.cpp:102-119,302-433,811-920`).

The earlier Windows smoke was a false-positive boundary: it self-tested
`moderngekko-port` resource discovery, then invoked DolRecomp and CMake
independently from Bash/Wine. It therefore never exercised the faulty
`std::system()` path. The replacement smoke's packaged-helper build closes that
gap and passed locally from the `.ell.11` validation ZIP with all five setup
phases, a path containing spaces, bundled Clang 22.1.8, LLD ThinLTO, module
publication, PE exports, and a loaded GRSEAF ABI descriptor.

### Why LLD is retained even though it is not a clean-build speedup

The module's measured notes say LLD and GNU `ld.bfd` took essentially the same time for a clean 132-chunk build. The large incremental win came from LLD's persistent ThinLTO cache, not from swapping linkers: a one-chunk rebuild fell from about 61 seconds without the cache to about 1.31 seconds with it (`dist/RingOut-1.0-dist/module-src/CMakeLists.txt:107-150`).

`setup.ps1` places that bounded cache under `%LOCALAPPDATA%\RingOut\thinlto` and passes it as `MODULE_LINK_CACHE` (`dist/windows/setup.ps1:217-240`). The module CMake caps it at 2 GiB and prunes entries older than 168 hours (`dist/RingOut-1.0-dist/module-src/CMakeLists.txt:174-201`).

## 3. QWave is intentionally not a MinGW dependency

Dolphin's native Microsoft-toolchain implementation calls the Windows QWave API for optional QoS/DSCP marking. In this tree:

- the QWave headers and implementation are compiled only for `_WIN32 && !__MINGW32__` (`ModernGekko/vendor/dolphin/Source/Core/Common/QoSSession.cpp:6-51`);
- the MinGW branch is a no-op with an explicit explanation that Ubuntu 24.04's MinGW-w64 11 lacks declarations (`ModernGekko/vendor/dolphin/Source/Core/Common/QoSSession.cpp:52-61`); and
- `qwave.lib` is linked only inside `if(NOT MINGW)` (`ModernGekko/vendor/dolphin/Source/Core/Common/CMakeLists.txt:311-323`).

Netplay still constructs `QoSSession`, but an unsuccessful/empty QoS session merely leaves packets unmarked; it is not the ENet transport itself (`ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayClient.cpp:1071-1090` and `ModernGekko/vendor/dolphin/Source/Core/Core/NetPlay/NetPlayServer.cpp:495-507`). The Windows package smoke starts the runtime far enough to resolve its normal WinSock/ENet imports and documents that the MinGW build imports no QWave (`.github/scripts/smoke-windows-package.sh:95-105`).

Current conclusion: do not ship or ask users to download QWave. The official MinGW runtime does not link it. If a future MSVC build enables it, `qwave.dll` is treated as a Windows system DLL, not an application payload (`.github/scripts/package-windows-cross.sh:370-375`). Re-enabling QoS for MinGW would be a separate optional enhancement requiring declarations/import-library support plus Windows network tests; it is not a blocker for netplay or releases.

## 4. Parallelism and runner capacity

### Current behavior

All important compile/test paths already derive their parallelism from available logical CPUs:

| Path | Current parallelism |
| --- | --- |
| Windows CI runtime + DolRecomp | `cmake --build ... --parallel "$(nproc)"` (`.github/workflows/windows-cross.yml:141-142`) |
| Windows packaged DolRecomp smoke | `-j$(nproc)` (`.github/scripts/smoke-windows-package.sh:117-120`) |
| Windows packaged module smoke | CMake `--parallel "$(nproc)"` (`.github/scripts/smoke-windows-package.sh:152-155`) |
| Windows player's first run | `[Environment]::ProcessorCount` passed to DolRecomp and CMake (`dist/windows/setup.ps1:172-180,240-241`) |
| AppImage runtime/tests | CMake `--parallel "$(nproc)"`, CTest `-j"$(nproc)"` (`.github/workflows/linux-appimage.yml:142-164`) |
| AppImage static DolRecomp | build/test with `nproc` (`.github/workflows/linux-appimage.yml:184-197`) |
| AppImage synthetic module smoke | CMake `--parallel "$(nproc)"` (`.github/scripts/smoke-appimage-module.sh:66-84`) |

The AppImage `docker run` calls do not set `--cpus`, so the container sees the runner's available CPUs. In the workflow shell, `$(nproc)` is expanded for the command and supplies that count to the tool in the container. The Windows workflow is not containerized at all.

The Docker image construction itself is handled by BuildKit through `docker/build-push-action`; there is no meaningful CMake-style thread knob in the workflow. BuildKit can use the runner concurrently where the Dockerfile graph permits, while the current Containerfile is mostly one dependency-install layer (`.github/containers/appimage-debian12.Containerfile:1-21`). Its most important speed control is the existing layer cache, not an extra `-j`.

### Proposed, not implemented

If wall time remains unacceptable after checking cache hit rates:

1. **Choose a larger runner or self-hosted label.** Both jobs currently specify only `ubuntu-24.04`, so the runner service controls CPU/RAM (`.github/workflows/windows-cross.yml:27-30`; `.github/workflows/linux-appimage.yml:26-29`). This is the only direct way to expose more actual cores. It may require GitHub plan/repository configuration and costs more.
2. **Measure before oversubscribing.** Keep `--parallel $(nproc)` as the baseline. A value larger than `nproc` can make memory-heavy Dolphin C++ builds or ThinLTO slower and less reliable.
3. **Do not use a matrix to split one CMake target graph without a designed merge boundary.** Windows and AppImage already run as independent workflows. Duplicating the same dependency graph across runners increases work and complicates provenance.
4. **Keep BuildKit cache scopes versioned when the Containerfile/dependency contract changes.** The present AppImage scope is `ringout-appimage-debian12-v1` (`.github/workflows/linux-appimage.yml:80-91`). Bumping it deliberately is safer than silently mixing incompatible layers.

## 5. Cache and artifact reuse

### Current CI caches

| Cache | Current implementation | What it does not do |
| --- | --- | --- |
| Windows compiler outputs | 1.0 GiB compressed ccache, content-based compiler check, exact-SHA key with a platform/toolchain prefix fallback (`.github/workflows/windows-cross.yml:34-39,98-116,118-150`) | Does not retain a CMake/Ninja build tree or final ZIP. |
| AppImage compiler outputs | 1.5 GiB compressed ccache mounted into the Debian container; exact-SHA key plus prefix fallback (`.github/workflows/linux-appimage.yml:33-40,93-115,117-215`) | Does not share objects with Windows; compiler/ABI/environment differ. |
| Windows tool downloads | One stable cache for the four pinned native archives (`.github/workflows/windows-cross.yml:152-188`) | The archives are unpacked into each package; this cache is not a user download. |
| AppImage construction downloads | Stable cache for appimagetool, type-2 runtime, and corresponding-source archives (`.github/workflows/linux-appimage.yml:217-262`) | Does not cache a finished AppImage. |
| AppImage Docker layers | GitHub Actions BuildKit cache with `mode=max` and a named scope (`.github/workflows/linux-appimage.yml:80-91`) | Does not add CPU capacity. |
| Player ThinLTO codegen | Bounded persistent cache selected by module CMake; Windows points it at `%LOCALAPPDATA%\RingOut\thinlto` (`dist/windows/setup.ps1:217-240`) | Does not cache DolRecomp generation or ordinary Clang object compilation. |

The Windows workflow explicitly notes that a default-branch manual dispatch can warm the exact SHA for a later tag, while the prefix may reuse compatible outputs from earlier commits (`.github/workflows/windows-cross.yml:98-106`). GitHub cache visibility rules still apply, so do not assume a cache created only in an isolated pull-request context will be visible to a tag job. Always inspect the emitted `ccache --show-stats` step (`.github/workflows/windows-cross.yml:144-150`; `.github/workflows/linux-appimage.yml:206-215`).

The final packages are release artifacts, not caches. Cache eviction must never be able to erase the only published package, checksum, or corresponding-source bundle.

### PCH feasibility

Precompiled headers are **not** currently enabled for the MinGW or Debian GCC/Clang pipelines. Dolphin's vendored CMake adds and links its `use_pch` target only under MSVC (`ModernGekko/vendor/dolphin/Source/CMakeLists.txt:26-30`; `ModernGekko/vendor/dolphin/Source/Core/Common/CMakeLists.txt:411-413`). There is no `target_precompile_headers` policy in the Ring Out top-level targets.

A portable CMake PCH experiment is possible, but it should not be assumed to outperform the existing ccache:

- it can reduce repeated parsing inside one clean build, but a PCH is compiler-, flag-, target-, and path-sensitive and is a poor artifact to restore blindly between runners;
- the per-game module's 132 generated chunks include only small common/system headers, while optimization and ThinLTO code generation dominate, so a PCH is unlikely to address the measured bottleneck;
- the module CMake's own measurements say a warm ThinLTO cache removes the incremental link/codegen cost, while ccache would cover the remaining fresh-build compile half (`dist/RingOut-1.0-dist/module-src/CMakeLists.txt:138-150`); and
- PCH changes across a large vendored Dolphin graph need both clean-build and warm-cache timings plus dependency-correctness tests on MinGW and Debian.

**Proposed experiment, not current behavior:** add PCH only to one high-cost C++ target behind an opt-in CMake option, record Ninja `-d stats`/ccache results on a cold cache and an exact repeat, and keep it only if total wall time improves without reducing cache hits or breaking dependency tracking. For the player's per-game build, trial a bundled compiler cache before PCH; it has better potential to reuse unchanged generated chunks across rebuilds, but would add another executable, license, cache policy, and package size that must pass the same provenance and Wine smoke gates.

Caching entire `build-*` directories between runners is not recommended as a first step. CMake caches embed absolute paths and tool identities, and Ninja graphs can become stale. Compiler caches plus pinned tool/layer downloads are the safer current boundary.

## 6. ZIP creation, retention, and the missing-download explanation

The Windows packager creates exactly:

```text
dist/out/RingOut-<version>-windows-x86_64.zip
dist/out/RingOut-<version>-windows-x86_64.zip.sha256
```

It stages an allowlisted payload, verifies PE/import closure, strips private/game-derived files, writes `MANIFEST.sha256`, normalizes timestamps, creates a deterministic Explorer-compatible ZIP, tests the ZIP, extracts it again, and verifies its manifest before atomically moving the ZIP and sidecar into `dist/out` (`.github/scripts/package-windows-cross.sh:457-552`).

A successful `workflow_dispatch` nevertheless provides no download by design:

- the workflow comment says the roughly toolchain-heavy ZIP avoids Actions artifacts to protect the repository's shared artifact quota (`.github/workflows/windows-cross.yml:15-19`);
- manual runs build, package, Wine-smoke, and summarize, but the summary explicitly says they have no retained download (`.github/workflows/windows-cross.yml:214-235`);
- release upload is guarded by `github.event_name == 'push' && github.ref_type == 'tag'` (`.github/workflows/windows-cross.yml:237-251`); and
- `actions/upload-artifact` runs only on failure and uploads CMake diagnostics, not the package (`.github/workflows/windows-cross.yml:253-263`).

When the manual job ends, its workspace disappears, so the ZIP disappears with it. Green means validation passed, not that an asset was retained.

The same rule applies to AppImage manual dispatches (`.github/workflows/linux-appimage.yml:311-365,394-406`).

**Proposed policy choices, not implemented:** if maintainers need occasional manual-run downloads, add an explicit boolean such as `retain_validation_asset`, default it off, use short retention, and monitor quota; or keep the current rule and use an immutable release tag for anything meant to be downloaded. Do not silently upload every validation package, because the Windows ZIP intentionally includes a large native toolchain.

## 7. Tag, draft, and cross-workflow concurrency

### Current release sequence

1. Push a tag matching `v*-ell.*`. Both platform workflows trigger (`.github/workflows/windows-cross.yml:3-13`; `.github/workflows/linux-appimage.yml:3-13`).
2. Each checks out the exact revision and restores the canonical remote tag ref. This matters for annotated tags because checkout can replace the local tag object with its peeled commit (`.github/scripts/restore-canonical-tag-ref.sh:1-80`).
3. Each builds and validates independently. Their concurrency groups are platform-specific (`windows-cross-${{ github.ref }}` and `linux-appimage-${{ github.ref }}`), with cancellation disabled, so repeated same-platform runs for one ref queue rather than cancel. The two platforms are **not** serialized against one another (`.github/workflows/windows-cross.yml:23-25`; `.github/workflows/linux-appimage.yml:22-24`).
4. Windows calls the publisher as `--create-release` with ZIP and checksum (`.github/workflows/windows-cross.yml:237-251`).
5. Linux calls it as `--join-release` with AppImage/checksum and runtime-source-bundle/checksum (`.github/workflows/linux-appimage.yml:364-392`). If it finishes first, it polls for the Windows draft up to 90 times at 10-second intervals (`.github/scripts/publish-tag-draft-assets.sh:429-448`).
6. A human inspects both platform assets, checksums, source records, and QA, then publishes the draft. Neither workflow automatically promotes it (`.github/workflows/linux-appimage.yml:15-18`).

### Safety properties

The shared publisher:

- accepts tag-triggered jobs only and binds HEAD, `GITHUB_SHA`, the local tag object, its peeled commit, and the remote tag ref (`.github/scripts/publish-tag-draft-assets.sh:43-94,228-285`);
- requires every payload/checksum pair to match exactly before any mutation (`.github/scripts/publish-tag-draft-assets.sh:96-146`);
- finds draft releases through authenticated pagination and requires exactly one exact-tag match (`.github/scripts/publish-tag-draft-assets.sh:287-386`);
- accepts only a draft prerelease with the expected commit and unique source marker (`.github/scripts/publish-tag-draft-assets.sh:388-415`);
- gives Windows the sole creation role, sends the non-idempotent creation POST only once, and reconciles ambiguous transport/422/5xx outcomes by listing (`.github/scripts/publish-tag-draft-assets.sh:441-492`);
- never deletes or replaces an asset; an equal digest is idempotent and a different digest is a hard failure (`.github/scripts/publish-tag-draft-assets.sh:555-630`); and
- revalidates release identity, every asset, and local/remote tag identity after uploads (`.github/scripts/publish-tag-draft-assets.sh:632-648`).

This means force-moving a release tag, attempting to reuse a version for different bytes, or manually publishing/editing the draft before both jobs finish should fail closed. Create a new version tag rather than replacing a released artifact.

### Remaining concurrency limitation

The Linux join wait is finite: 15 minutes after Linux reaches publication. If Windows has not exposed its draft by then, Linux fails even if its AppImage is valid. Rerunning Linux after Windows creates the draft is safe because equal assets are idempotent. A future coordinator workflow or longer bounded wait could improve ergonomics, but the current single-creator design avoids duplicate drafts.

## 8. Linux AppImage pipeline

The AppImage workflow is release-capable at this commit.

### Build environment and outputs

The job builds a local Docker image from a digest-pinned Debian 12 base and caches its BuildKit layers (`.github/workflows/linux-appimage.yml:80-91`; `.github/containers/appimage-debian12.Containerfile:1-21`). Inside it, the workflow:

- builds the Linux ModernGekko runtime, module-info tool, and tests, then runs the tests (`.github/workflows/linux-appimage.yml:117-165`);
- separately builds and tests a statically linked DolRecomp, failing if it has an ELF interpreter (`.github/workflows/linux-appimage.yml:166-205`);
- downloads and verifies pinned appimagetool/type-2 runtime and source archives (`.github/workflows/linux-appimage.yml:217-262`); and
- packages in the same Debian container (`.github/workflows/linux-appimage.yml:264-288`).

The release outputs are:

```text
RingOut-<version>-linux-x86_64.AppImage
RingOut-<version>-linux-x86_64.AppImage.sha256
RingOut-<version>-appimage-runtime-sources.tar.zst
RingOut-<version>-appimage-runtime-sources.tar.zst.sha256
```

The corresponding-source/relink archive covers the statically incorporated AppImage runtime materials and is required beside the AppImage (`.github/scripts/package-appimage.sh:208-223,493-536`; `.github/workflows/linux-appimage.yml:311-392`).

### Package design and validation

The AppImage bundles the runtime, a static DolRecomp, Dolphin `Sys`, module sources, selected runtime DSOs, provenance, manifests, and licenses (`.github/scripts/package-appimage.sh:231-427`). It intentionally does **not** bundle a Linux compiler toolchain: first run requires host CMake, Ninja, Python 3, a C compiler, and libc development headers (`dist/appimage/README.txt:26-41`). This differs from the self-contained Windows ZIP.

The packager enforces:

- the exact Debian base identity, x86-64 ELF type, static DolRecomp, resolved runtime DSOs, and a glibc floor no newer than Debian 12's 2.36 (`.github/scripts/package-appimage.sh:83-170`);
- clean/tag-matching source for publishable packages, while manual validation is explicitly marked non-publishable (`.github/scripts/package-appimage.sh:173-185,483-511`);
- an allowlisted payload with no disc/save/generated module/profile (`.github/scripts/package-appimage.sh:556-622`);
- a normalized type-2 AppImage with a pinned runtime and manifest-verifying re-extraction (`.github/scripts/package-appimage.sh:639-720`);
- explicit extracted-payload checks plus a setup-helper self-test that resolve
  `usr/share/ringout/module-src` and `GRSEAF.ini` through the same search path
  first-run compilation uses;
- a synthetic DOL to native `.so` build and ABI load (`.github/scripts/package-appimage.sh:721-722`; `.github/scripts/smoke-appimage-module.sh:33-140`); and
- a clean Ubuntu 24.04, no-network, no-FUSE self-test of the exact artifact (`.github/workflows/linux-appimage.yml:290-309`).

`dist/appimage/AppRun` installs immutable versioned setup assets into a private user data directory, preserves game/settings/private modules across AppImage updates, and supports an extraction-only self-test (`dist/appimage/AppRun:9-52,67-133,154-185`). Users without FUSE can set `APPIMAGE_EXTRACT_AND_RUN=1` (`dist/appimage/README.txt:65-76`).

### AppImage host-tool environment boundary (`b10380d6`, 2026-08-25)

The published `.ell.9` image could start on a newer Linux distribution but
leaked its Debian 12 `LD_LIBRARY_PATH` into the host's CMake, Ninja, compiler,
and Python during first-run module compilation. On the tested Arch host this
made CMake load the image's older `libstdc++.so.6` and fail for missing
`GLIBCXX_3.4.32` and `CXXABI_1.3.15`. AppImage-owned executables still require
that bundled DSO closure, so globally clearing the variable is not correct.

`AppRun` now records whether the caller originally had `LD_LIBRARY_PATH` and
its exact value before installing the AppImage closure. `moderngekko-port`
restores that original state only around compiler discovery, CMake configure,
Ninja/compiler/Python execution, and bounds Linux compiler-version probes to
five seconds. A failed or wedged `clang` probe falls back to GCC
(`dist/appimage/AppRun:12-26`; `ModernGekko/tools/moderngekko_port.cpp:79-109,`
`287-310,490-542,761-790`).

The exact-package module smoke now launches the packaged setup helper with a
deliberately contaminated AppImage library path. Host-tool wrappers reject any
value other than the recorded caller path, and a deliberately failing `clang`
must lead to a successful GCC build. The test continues through synthetic DOL
translation, native module publication, `dlopen`, and ABI inspection
(`.github/scripts/smoke-appimage-module.sh:53-185`).

Local replacement-candidate reproduction from source commit `b10380d6` plus
the subsequent version/documentation worktree:

```bash
docker run --rm --user "$(id -u):$(id -g)" \
  --volume "$PWD:/src" --workdir /src \
  --env CCACHE_DIR=/src/.cache/ccache-appimage \
  ringout-appimage-build:debian12 \
  cmake --build build-appimage \
    --target moderngekko-launcher moderngekko-module-info moderngekko-tests -j8

docker run --rm --user "$(id -u):$(id -g)" \
  --volume "$PWD:/src" --workdir /src \
  ringout-appimage-build:debian12 \
  ctest --test-dir build-appimage --output-on-failure -j8
```

CTest passed 45/45. The validation-only `.ell.10` AppImage then passed its
2,905-file package policy, contaminated-environment GCC-fallback module smoke,
ABI load, and self-test. Extracted on the Arch host and launched with the exact
AppImage `LD_LIBRARY_PATH`, it translated the real private GRSEAF DOL, selected
`/usr/bin/gcc` after the host's broken Swiftly `clang` timed out, configured
with GCC 16.1.1 and Python 3.14.6, compiled all 132 chunks, linked, and published
the module. The resulting private module SHA-256 was
`e01d1fc7f14d41cf170fb5b036e5c754cb3062b8e5421f147258b627e2931d48`.

## 9. Reproduction and verification

### Confirm source identity before a release

```bash
git status --short
git rev-parse HEAD
git rev-parse 'v1.2.1-ell.6^{commit}'
git rev-parse refs/tags/v1.2.1-ell.6
git ls-remote --refs origin refs/tags/v1.2.1-ell.6
```

For a publishable AppImage, the worktree must be clean and the tag's peeled commit must equal `SOURCE_COMMIT`. For either platform, the publisher repeats local and remote source checks immediately around mutations.

### Run validation-only CI

These runs build and test but deliberately produce no retained download:

```bash
gh workflow run windows-cross.yml -f version=v1.2.1-ell.6
gh workflow run linux-appimage.yml -f version=v1.2.1-ell.6
```

Inspect the run summary and ccache statistics. A retained release requires a new immutable `v*-ell.*` tag, not a manual dispatch.

### Reproduce the Windows cross-build locally

After installing the dependencies listed at `.github/workflows/windows-cross.yml:78-96`:

```bash
cmake -S ModernGekko -B build-windows-cross -GNinja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/mingw-x86_64.cmake" \
  -DCMAKE_C_COMPILER=/usr/bin/x86_64-w64-mingw32-gcc-posix \
  -DCMAKE_CXX_COMPILER=/usr/bin/x86_64-w64-mingw32-g++-posix \
  -DCMAKE_RC_COMPILER=/usr/bin/x86_64-w64-mingw32-windres \
  -DCMAKE_BUILD_TYPE=Release -DUSE_SYSTEM_LIBS=OFF \
  -DENABLE_QT=OFF -DENABLE_TESTS=OFF -DENABLE_ANALYTICS=OFF \
  -DENABLE_AUTOUPDATE=OFF -DBUILD_TESTING=OFF \
  -DMODERNGEKKO_ENABLE_DOLPHIN_TESTS=OFF
cmake --build build-windows-cross \
  --target moderngekko-launcher moderngekko-port moderngekko-run dolrecomp \
  --parallel "$(nproc)"
```

For a release-equivalent ZIP, use the four pinned archives and exact packaging invocation at `.github/workflows/windows-cross.yml:158-203`; do not substitute unverified latest downloads.

Validate an assembled ZIP with:

```bash
unzip -tq dist/out/RingOut-*-windows-x86_64.zip
(
  cd dist/out
  sha256sum -c RingOut-*-windows-x86_64.zip.sha256
)
xvfb-run -a .github/scripts/smoke-windows-package.sh \
  dist/out/RingOut-*-windows-x86_64.zip /tmp/ringout-windows-smoke
```

The smoke directory must be empty before use. Its successful final line is:

```text
Windows ZIP synthetic DOL -> DolRecomp -> clang/lld module smoke passed
```

### Validate an AppImage

```bash
(
  cd dist/out
  sha256sum -c RingOut-*-linux-x86_64.AppImage.sha256
  sha256sum -c RingOut-*-appimage-runtime-sources.tar.zst.sha256
)
appimage=$(realpath dist/out/RingOut-*-linux-x86_64.AppImage)
data_dir=$(mktemp -d)
chmod +x "$appimage"
APPIMAGE_EXTRACT_AND_RUN=1 RINGOUT_DATA_DIR="$data_dir" \
  "$appimage" --ringout-self-test
```

The exact release workflow additionally runs its clean Ubuntu 24.04 container test at `.github/workflows/linux-appimage.yml:290-309`; use that workflow rather than treating a host-only self-test as full release proof.

### Exercise release-provenance helpers without GitHub mutations

```bash
.github/scripts/tests/test-restore-canonical-tag-ref.sh
.github/scripts/tests/test-publish-tag-draft-assets.sh
```

Both tests passed locally at the audited commit on 2026-08-25. The publisher test uses a mock GitHub API and covers pagination, join waiting, creation races/ambiguous results, upload verification, duplicate drafts, and source mismatches (`.github/scripts/tests/test-publish-tag-draft-assets.sh:139-269`).

## 10. Remaining test gaps and recommended release gate

The repository has strong automated package tests, but the following claims are not established by the audited automation:

1. **Physical Windows 10/11 first run:** Wine proves the Windows executables compile, link, export, and load a synthetic module, but not Explorer extraction, PowerShell UI/quoting, Defender/third-party AV behavior, Windows Firewall prompts, native controller input, or Vulkan drivers.
2. **A complete proprietary game recompilation in CI:** automation deliberately uses a legal synthetic DOL. A maintainer should privately test a supported disc revision without publishing the generated data.
3. **Real netplay across Windows/Linux and NAT/firewalls:** package build tests only verify binaries/options/imports, not an Internet match.
4. **AppImage distro breadth:** the release gate covers the pinned Debian build environment and a clean Ubuntu 24.04 no-FUSE self-test. Test at least one older supported glibc host, SteamOS/Deck, and a FUSE launch when making compatibility claims.
5. **Live GitHub draft race:** the publisher's mock tests and the successful
   `v1.2.1-ell.6` two-workflow tag run prove the current creator/joiner path
   once against GitHub. Future publisher/API changes still need a fresh tag run;
   manual dispatch cannot exercise release mutation. See
   [the release history](release-history-and-testing.md) for the run evidence.
6. **Cache performance:** cache configuration exists, but a release should record actual hit/miss statistics and wall time. Do not describe a theoretical warm key as a confirmed hit.
7. **Larger-runner benefit and PCH:** neither is implemented or benchmarked in this commit. Require controlled cold/warm measurements before changing the defaults.

Recommended human release checklist:

```text
[ ] clean, immutable tag points at the reviewed commit
[ ] Windows workflow passes packaged clang/lld DLL smoke
[ ] Linux workflow passes tests, synthetic module smoke, and clean-Ubuntu self-test
[ ] draft contains Windows ZIP + checksum
[ ] draft contains AppImage + checksum + runtime source/relink bundle + checksum
[ ] each checksum verifies after downloading the draft assets
[ ] SOURCE.txt/manifest identify the same tag and commit
[ ] private physical-Windows first run and one supported-disc boot pass
[ ] no game-derived files are attached
[ ] human publishes the draft only after both platform reviews
```

## 11. Safe improvement order

These are proposals, not shipped behavior:

1. Record several real CI runs' per-step durations and ccache hit rates.
2. If compilation is still dominant, trial a larger runner label on manual validation only.
3. If Docker setup is dominant, inspect BuildKit cache hits and dependency-layer invalidation before changing build commands.
4. If player rebuilds are dominant, benchmark a packaged compiler cache against the existing ThinLTO cache; do not assume PCH helps generated code.
5. If manual downloads are operationally necessary, add an explicit short-retention opt-in rather than changing validation runs globally.
6. If Linux repeatedly outruns Windows by more than 15 minutes at publication, add a release coordinator or extend the bounded join window while preserving a single creator and immutable asset semantics.

The current pipeline is already substantially safer than a generic build-and-upload workflow: it pins tools, validates actual Windows first-run compilation with LLD, packages AppImage runtime source obligations, binds releases to exact tag objects and commits, and refuses to overwrite mismatched assets. Its main outstanding proof is native platform QA, not another compiler flag.
