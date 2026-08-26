# Ell fork release history and test boundary

Audit date: 2026-08-25 (America/Chicago)

Repository: `https://github.com/Ell/RingOut`

Upstream baseline: `d25d4812` (`jackpoison-prog/RingOut` `main` when the fork work began)

Audited checkout: `ff0ad952980f5083afd21c3d3758208a7a093d72`

This is the durable release ledger for the Ell fork. It records what changed,
what GitHub Actions actually completed, what became publicly downloadable, and
what the available evidence does **not** prove. It is not a replacement for the
build instructions in `README.md` or the two release workflows.

## Executive verdict

- `v1.2.1-ell.1`, `.2`, `.3`, `.6`, and `.9` are public prereleases.
- `v1.2.1-ell.4`, `.5`, `.7`, and `.8` are real, immutable annotated tags, but
  are not public releases. `.4` and `.5` failed at draft publication, `.7`
  failed its minimal Linux build, and `.8` failed AppImage assembly after its
  Linux build/tests passed.
- The `.4` and `.5` Windows and Linux jobs completed their builds, package
  validators, Wine/AppImage smokes, and summaries before publication failed.
  A green manual dispatch was never expected to retain a package: both
  workflows deliberately make manual runs validation-only and skip Actions
  artifact upload.
- `.6` is the first release for which both platform tag workflows completed,
  joined one source-bound draft, and left the complete Windows plus AppImage
  asset set for human inspection and publication.
- The exact `.6` Windows ZIP was cross-built on Ubuntu and exercised under
  Wine, including its bundled Clang/`lld` first-run module build. This is strong
  package/toolchain evidence, but it is not physical-Windows GPU, audio,
  controller, menu, real-disc, or two-machine netplay evidence.
- The exact `.6` AppImage passed its CTest suites, synthetic module pipeline,
  extraction checks, and a network-disabled clean-Ubuntu self-test. That does
  not prove end-to-end play or cross-platform netplay.

## Immutable tag ledger

All six tags are annotated. The raw tag objects and peeled commits below were
verified both locally and with `git ls-remote --tags origin` on the audit date.

| Tag | Annotated tag object | Peeled source commit | Commit subject |
| --- | --- | --- | --- |
| `v1.2.1-ell.1` | `c8e924c46eec6afb5ff2e0d9ae2e887ef37f5e07` | `54d4999c21736a1a3752cbcd9428d30e80f53738` | Cache MinGW compiler outputs in CI |
| `v1.2.1-ell.2` | `bdc539e377981b2fe335e8919fa809253eceb4df` | `248b7c6d4bc2be119f0e1e5f7ad67f6d9f67e89e` | Fix Windows first-run CMake regeneration |
| `v1.2.1-ell.3` | `d547c8a9724c8db06f5ed0826d1fde58ea4b2052` | `ab8f3b803967e385deee8a2fcb53d99f33e4d9a6` | Fix Windows runtime FMV and GPU defaults |
| `v1.2.1-ell.4` | `e555cceb896cf6332054e83e41d3c2d3827a4d77` | `6f1ef65e12013e1023fc4bab8bd31460f1f6d399` | Fix controls and add AppImage releases |
| `v1.2.1-ell.5` | `31bbb821c8905185e72fff87761e71ff51663c13` | `885aaecf8660bec118f5d4cea4a0a2e2d2caaa21` | Preserve canonical release tags in CI |
| `v1.2.1-ell.6` | `9f2335e3c15a91759ecf6b5362a984f0348ee18c` | `ff0ad952980f5083afd21c3d3758208a7a093d72` | Harden concurrent draft release publishing |

The `.1` through `.3` public release objects report `target_commitish: main`
because the original `gh release create` path did not source-bind that release
field. Their tag refs are nevertheless immutable, their tag-triggered jobs used
the peeled tag commit, and each retained ZIP's internal `SOURCE.txt` names that
same exact commit. `.6` improves this: its public release object names the full
commit SHA and its body contains the checked tag-object/commit source marker.

## Pre-release cross-build bring-up

The first five fork commits form one bring-up sequence rather than five usable
releases:

| Commit | Change | Public CI evidence |
| --- | --- | --- |
| `666eddc8046ccea36b11f2badd6d58140de77e73` | Restored the retired Windows path: MinGW portability changes in DolRecomp/ModernGekko, toolchain file and case-compatible headers, portable launcher/setup, allowlisted packaging, and the tag workflow. | Dispatch [32801670576](https://github.com/Ell/RingOut/actions/runs/32801670576) failed during CMake configuration. |
| `f651d0e22789f67c15bc06aa9f5f898c25282bb4` | Selected Ubuntu's explicit POSIX MinGW C/C++ compilers and resource compiler by absolute path. | Dispatch [32801820115](https://github.com/Ell/RingOut/actions/runs/32801820115) passed configure and failed during the runtime/recompiler build. |
| `83297cb8e000f745774530990590d8d55b90c07a` | Removed the MinGW dependency on QWave and made QoS marking a no-QWave stub while retaining the Microsoft-SDK path for non-MinGW builds. | Dispatch [32802305187](https://github.com/Ell/RingOut/actions/runs/32802305187) still failed during the build. |
| `448be1224d072165eab461ca9fbad5b51823f1a7` | Added compatibility for Ubuntu 24.04's MinGW-w64 11 Windows headers in threading, networking, Win32 platform, and user-directory code. | No public workflow run at this exact SHA was present in the reviewed run list; it was included in the next successful run. |
| `54218d838e23f687cb12e3b20eca2206f5fa4cfa` | Replaced the two-job build cap with `--parallel "$(nproc)"`. | Dispatch [32803788536](https://github.com/Ell/RingOut/actions/runs/32803788536) completed successfully. |

`54d4999c` then added a bounded 1 GiB `ccache`, per-commit keys with a prefix
fallback, and compiler launchers. That commit became `.1`. Current equivalents
are visible in `.github/workflows/windows-cross.yml:98-150`; the pinned native
tool archives and their SHA-256 gates are at lines 152-188.

## Release-by-release record

### v1.2.1-ell.1 — first Windows prerelease

Source: `54d4999c21736a1a3752cbcd9428d30e80f53738`

Publication: public prerelease, Windows ZIP plus `.sha256` sidecar.

Runs:

- Manual validation [32804728139](https://github.com/Ell/RingOut/actions/runs/32804728139): success.
- Manual validation [32805396439](https://github.com/Ell/RingOut/actions/runs/32805396439): success.
- Tag build/publish [32806510099](https://github.com/Ell/RingOut/actions/runs/32806510099): success.

The tag workflow proved the Ubuntu 24.04 MinGW runtime/recompiler cross-build,
pinned first-run tool download, package construction, PE import closure,
manifest/ZIP integrity, source/licence payload, privacy checks, and no-game-data
policy. Windows tests were disabled in the cross-build. The release notes also
record a separately run native 36/36 suite (including the netplay protocol test)
and Wine loader/help smokes; those were not steps in the `.1` Windows tag job.

Known failure discovered after publication: timezone-less DOS timestamps made
ZIP entries appear future-dated on Windows west of UTC, allowing Ninja to enter
a 100-iteration CMake regeneration loop. `.1` is superseded.

### v1.2.1-ell.2 — first-run timestamp and linker diagnostics hotfix

Source: `248b7c6d4bc2be119f0e1e5f7ad67f6d9f67e89e`

Publication: public prerelease, Windows ZIP plus `.sha256` sidecar.

Runs:

- Manual validation [32808292032](https://github.com/Ell/RingOut/actions/runs/32808292032): success.
- Tag build/publish [32808767968](https://github.com/Ell/RingOut/actions/runs/32808767968): success.

This release fixed deterministic UTC packaging, moved ZIP entry times one day
behind the source epoch, tested extraction in UTC-12, normalized future-dated
CMake inputs defensively in `setup.ps1`, and ran the `lld` usability probe in
the Windows environment. The earlier `lld not usable` line was a false probe;
bundled Clang was already invoking bundled `ld.lld`.

The release notes record reproduction of `.1`'s loop and validation of the exact
fixed package with bundled CMake/Ninja/Clang under Wine, including a linked and
executed Windows binary. The tag workflow itself still did not contain the
later dedicated whole-ZIP Wine module smoke.

Known failures discovered from physical-Windows startup: normal playback could
arm the unbundled developer FFmpeg path; the runtime could install the Windows
exception handler twice; default dual-core scheduling could trigger a GFX FIFO
desynchronization; and the generated module lacked intended gather-pipe/journal
exports. `.2` is superseded.

### v1.2.1-ell.3 — Windows runtime hotfix

Source: `ab8f3b803967e385deee8a2fcb53d99f33e4d9a6`

Publication: public prerelease, Windows ZIP plus `.sha256` sidecar.

Runs:

- Manual validation [32810713611](https://github.com/Ell/RingOut/actions/runs/32810713611): success.
- Tag build/publish [32810975617](https://github.com/Ell/RingOut/actions/runs/32810975617): success.

This release gated the external-FFmpeg takeover behind
`STATICRECOMP_FMV_TAKEOVER`, defaulted offline play to the known-safe single CPU/GPU
thread path, removed duplicate exception-handler ownership, and exported
`ppc_set_gather_pipe` plus `ppc_set_mem_write_journal` from Windows-built
modules. Ordinary playback no longer requires FFmpeg.

The exact bundled Clang 22.1.8/`lld` path was used to build a Windows module and
the export set, loader, manifest, import, privacy, and package checks passed.
The triggering report proves that an earlier package reached physical-Windows
startup. It does **not** prove that the exact `.3` artifact completed fresh
real-disc setup and an end-to-end gameplay session; its release notes explicitly
left that follow-up open.

### v1.2.1-ell.4 — controls/netplay and AppImage implementation, no public release

Source: `6f1ef65e12013e1023fc4bab8bd31460f1f6d399`

Publication: tag exists; no public release/download.

Runs:

- Windows manual validation [32821721822](https://github.com/Ell/RingOut/actions/runs/32821721822): success.
- Linux manual validation [32821723764](https://github.com/Ell/RingOut/actions/runs/32821723764): success.
- Windows tag run [32823275207](https://github.com/Ell/RingOut/actions/runs/32823275207): build/package/Wine smoke passed; failed at publish.
- Linux tag run [32823275215](https://github.com/Ell/RingOut/actions/runs/32823275215): build/tests/package/AppImage smoke passed; failed at publish.

This large commit fixed Win32 keyboard/focus/shutdown/menu behavior, controller
menu navigation, netplay lobby cancellation and Windows socket startup, fixed
netplay pacing, neutral input while a local overlay is open, synchronization of
enabled codes, and locks around unsafe per-peer runtime actions. It also added
the Debian 12 AppImage build/package/source-bundle workflow, exact synthetic
module smokes for both platforms, and a source-bound draft publisher.

Both tag jobs reached the final publisher with every earlier step green. They
failed there in about one second. The next commit added an explicit canonical
tag-ref restoration helper, and the `.4` publisher required a local
`refs/tags/$TAG`; together those source facts identify the failure class. The
public job API exposes the failed step but not its log text, so the precise
error line is an evidence-backed inference rather than a quoted CI log.

### v1.2.1-ell.5 — canonical tag restoration, no public release

Source: `885aaecf8660bec118f5d4cea4a0a2e2d2caaa21`

Publication: tag exists; no public release/download.

Runs:

- Windows manual validation [32824259981](https://github.com/Ell/RingOut/actions/runs/32824259981): success.
- Linux manual validation [32824262347](https://github.com/Ell/RingOut/actions/runs/32824262347): success.
- Windows tag run [32824671755](https://github.com/Ell/RingOut/actions/runs/32824671755): all validation passed; failed at publish.
- Linux tag run [32824671737](https://github.com/Ell/RingOut/actions/runs/32824671737): all validation passed; failed at publish.

The new helper fetched the exact remote tag object, checked its type and peeled
commit against `GITHUB_SHA` and `HEAD`, rechecked `git ls-remote`, and was covered
by a shell test. This resolved `.4`'s missing/local-tag provenance problem.

Publication still failed after roughly 35 seconds. The `.5` publisher attempted
to find its draft through GitHub's tag-specific release endpoint and allowed
both platform jobs to create. The `.6` source correction documents the two
underlying problems: the tag-specific endpoint does not expose drafts, and a
creation POST cannot be made safely retryable when parallel jobs may race.
Because public APIs omit drafts and the local `gh` credential was invalid during
this audit, this document cannot prove whether orphan `.4`/`.5` draft objects
still exist privately. It can prove there are no public release assets.

### v1.2.1-ell.6 — successful dual-platform publication

Source: `ff0ad952980f5083afd21c3d3758208a7a093d72`

Publication: public prerelease with Windows ZIP, AppImage, corresponding
AppImage runtime source/relink bundle, and all three checksum sidecars.

Runs:

- Linux manual validation [32826743813](https://github.com/Ell/RingOut/actions/runs/32826743813): success.
- Windows manual validation [32826747661](https://github.com/Ell/RingOut/actions/runs/32826747661): success.
- Windows tag creator [32827255776](https://github.com/Ell/RingOut/actions/runs/32827255776): success.
- Linux tag joiner [32827255797](https://github.com/Ell/RingOut/actions/runs/32827255797): success.
- Public prerelease: [v1.2.1-ell.6](https://github.com/Ell/RingOut/releases/tag/v1.2.1-ell.6), published 2026-08-25 08:51:30 UTC.

The publisher now assigns Windows as the sole draft creator and Linux as a
wait-and-join participant. It enumerates authenticated releases to see drafts,
never retries an ambiguous creation POST, reconciles transport/5xx/422 outcomes,
pins `target_commitish` to the peeled commit, rejects duplicate release objects,
and uploads only absent or digest-identical assets. Unit tests for the tag
restorer and mock-GitHub publisher run before either release build.

### Post-audit v1.2.1-ell.7 — failed rollback-beta release attempt

Source: `a9ce89ec883fe1812e9d6cab8066d9b95c1b9e34`

Annotated tag object: `acd6b5910448e77c745a9a3b770d7de3ededb58e`

Publication: no release and no downloadable assets. The tag remains immutable as
the record of the failed attempt.

Runs on 2026-08-25:

- Linux tag run [32905381876](https://github.com/Ell/RingOut/actions/runs/32905381876):
  failed while building the focused rollback tests because their standalone
  CMake targets included Dolphin headers without declaring Dolphin's `fmt`
  dependency. The Arch development host had a system `fmt` installation and
  masked the missing target dependency; the minimal Debian 12 release image did
  not.
- Windows tag run [32905381732](https://github.com/Ell/RingOut/actions/runs/32905381732):
  cancelled after the Linux failure, before package publication, so it could not
  create a partial `.ell.7` draft.

The replacement candidate declares `fmt::fmt` on every focused rollback test.
An exact rerun in the digest-pinned Debian 12 release image compiled all eight
executables. That rerun also exposed GNU awk interval-regex use in the shell
evidence verifier; replacing `{8}`/`{16}` with explicit length plus character
checks made the nine-test rollback subset pass in that same image. Reproduction:

```bash
docker run --rm --user "$(id -u):$(id -g)" \
  --volume "$PWD:/src:ro" --volume /tmp/ringout-ell8-debian-build:/build \
  --workdir /src ringout-appimage-build:debian12 \
  ctest --test-dir /build -R 'moderngekko\.(rollback|live_rollback)' \
  --output-on-failure -j4
```

### v1.2.1-ell.8 — failed AppImage packaging attempt

Source: `3881ef28aaff9e380994503c7e9667ff46a0f685`

Annotated tag object: `587b056fce3e98080c1eff02b4ffc8c5acdefbb9`.

Publication: no public release and no public downloads. Windows run
[32908238001](https://github.com/Ell/RingOut/actions/runs/32908238001) completed
its build, package validation, whole-package Wine smoke, and upload to an
unpublished source-bound draft. The immutable tag records a candidate whose
complete Debian 12 build and 45-test suite also passed, but whose Linux package
job failed during AppImage assembly. Linux run
[32908237997](https://github.com/Ell/RingOut/actions/runs/32908237997)
reached the launcher identity gate, where `set -o pipefail` turned the expected
early exit from `grep -q` into a pipeline failure when `strings` received
SIGPIPE. This was a packaging-check race, not an emulator build or test failure.

### v1.2.1-ell.9 — published rollback beta

Source: `94cd55df6ab53b974a41c9d27c124cd0b99e68f2`

Annotated tag object: `c092e9f6ab81338aa038f1ff077d300d647e1c13`.

Public prerelease: [v1.2.1-ell.9](https://github.com/Ell/RingOut/releases/tag/v1.2.1-ell.9),
published 2026-08-25 23:12:44 UTC.

Overlay implementation checkpoint:
`58f215e10d7ef477721600509949a03e792b0b1c` (2026-08-25).

This release adds the player-facing rollback performance OSD on top of the
`.ell.7` build fixes. It displays the maximum peer RTT plus the real inclusive
restore/replay correction depth, retains a recent peak for one second, and
colors 1-3 frame corrections yellow and 4+ red. The launcher preference is on
by default but user-disableable; fixed-delay and solo sessions force it off.

Before tagging, the exact Debian 12 release environment rebuilt the runner,
launcher, module inspector, and full test target. CTest passed 45/45. After the
final formatting-only header/call-site adjustment, the same environment rebuilt
the affected production targets and passed the 11-test rollback/config/harness/
localhost-protocol subset. These are source/build gates, not visual proof of the
rendered OSD or physical two-machine play.

After `.ell.8` exposed the AppImage-only pipe race, both platform packagers were
changed to search the binary directly with `grep -aFq`, eliminating the
`strings` producer and SIGPIPE ambiguity. Before tagging `.ell.9`, the exact
Debian 12 packager completed locally through privacy/provenance checks,
AppImage construction, synthetic DOL translation/native module load, and the
AppImage self-test. It produced a 2,905-file payload at
`.cache/ell9-package-qa/RingOut-1.2.1-ell.9-linux-x86_64.AppImage`; this local QA
artifact is not a published release artifact and its digest is not a substitute
for the tag-workflow result.

Both source-bound tag runs completed successfully:

- Windows [32909109214](https://github.com/Ell/RingOut/actions/runs/32909109214)
  built the portable package, validated its import/privacy/provenance policy,
  and passed the complete packaged first-run toolchain smoke under Wine.
- Linux [32909109237](https://github.com/Ell/RingOut/actions/runs/32909109237)
  passed 45/45 runtime tests, the static DolRecomp suite, AppImage assembly,
  synthetic DOL-to-native-module loading, and the network-disabled clean Ubuntu
  24.04 no-FUSE self-test.

The downloaded public payloads matched their `.sha256` sidecars and GitHub
asset digests. Windows and AppImage `SOURCE.txt` both name the exact source
commit and `.ell.9` tag. The AppImage source marker names the matching
same-release runtime source/relink archive. None of these package checks proves
the OSD's rendered appearance or physical/cross-machine gameplay.

Post-publication Linux finding: `.ell.9` must not be recommended to Linux
players. A real first-run attempt on Arch completed extraction and DolRecomp
translation, then CMake inherited the image's Debian 12 `LD_LIBRARY_PATH` and
failed because its host executables require `GLIBCXX_3.4.32` and
`CXXABI_1.3.15`. This does not invalidate the source-bound artifacts or Windows
package, but it is a player-blocking AppImage integration defect. The release
is preserved for provenance and is marked superseded rather than having
its immutable source-bound assets silently replaced.

### v1.2.1-ell.10 — published Linux replacement

Implementation checkpoint: `b10380d6` (2026-08-25).

Release source: `df76814a0e05655dcbf1efec05620bc02dad0f48`.

Annotated tag object: `67fa2bf50854a215fe85af2fce2848a1d125825c`.

Public prerelease: [v1.2.1-ell.10](https://github.com/Ell/RingOut/releases/tag/v1.2.1-ell.10),
published 2026-08-26 00:04:16 UTC. `.ell.9` remains available with a prominent
superseded warning; its assets were not deleted or replaced.

The candidate preserves the caller's pre-AppImage library environment and
restores it only for host build tools. It also requires compiler probes to exit
successfully, bounds Linux probes to five seconds, and falls back from a broken
`clang` to GCC. The package smoke now exercises the real packaged setup helper
under a deliberately contaminated library path and a deliberately failing
`clang`, rather than invoking DolRecomp and CMake independently.

Local validation on 2026-08-25 passed:

- the exact Debian 12 release build and all 45 CTests;
- AppImage assembly, policy/privacy/provenance checks, a 2,905-file payload,
  synthetic DOL translation, forced GCC fallback, native module load/ABI check,
  and AppImage self-test;
- a real private GRSEAF first-run build on the affected Arch host while the
  packaged helper began with the AppImage DSO closure. It translated 132 code
  chunks, ran CMake with `env -u LD_LIBRARY_PATH`, selected GCC 16.1.1 and
  Python 3.14.6, linked, and published the same known module digest
  `e01d1fc7f14d41cf170fb5b036e5c754cb3062b8e5421f147258b627e2931d48`.

The final validation-only candidate was
`.cache/ell10-host-tool-qa-v3/RingOut-1.2.1-ell.10-linux-x86_64.AppImage`,
SHA-256
`be32ed25094307d23fda6d14360815a43819ce23217a319c8de6102f93699082`.
It is dirty-tree QA evidence, not the release artifact. Publication remains
separate from that local digest.

Both clean, source-bound tag workflows passed:

- Windows [32912875418](https://github.com/Ell/RingOut/actions/runs/32912875418)
  rebuilt and packaged the portable ZIP, then passed its exact-package Wine
  first-run toolchain/module smoke before joining the source-bound draft.
- Linux [32912875400](https://github.com/Ell/RingOut/actions/runs/32912875400)
  passed 45/45 runtime tests, the static DolRecomp suite, the new contaminated-
  environment/failing-clang module smoke, the AppImage self-test, and the clean
  Ubuntu 24.04 no-FUSE smoke before joining the same draft.

All three public payloads were downloaded after both uploads, matched their
adjacent `.sha256` files and GitHub asset digests, and embedded source commit
`df76814a0e05655dcbf1efec05620bc02dad0f48`. The AppImage `SOURCE.txt` also
names the exact same-release runtime source/relink archive and digest.

## What the current release pipeline actually tests

| Layer | `.10` evidence | Important boundary |
| --- | --- | --- |
| Linux runtime build/tests | Debian 12 container builds runtime, module inspector, and tests; `ctest` reports 45/45. | This is host-side unit/integration coverage, not a real game session. |
| Static DolRecomp | Separately configured static executable; 14/14 tests pass and `readelf` rejects an ELF interpreter. | Uses project-authored fixtures/synthetic input, not distributed game data. |
| Windows cross-build | MinGW Release runtime and DolRecomp compile; tests are explicitly off. | Successful PE compilation is not native Windows execution. |
| Windows whole-package smoke | Exact ZIP is extracted into a fresh Wine prefix; its bundled Python creates a synthetic DOL, bundled DolRecomp translates it, bundled CMake/Ninja/Clang/`lld` builds a ThinLTO DLL, and Windows Python loads/checks the ABI and gather-pipe export. | The smoke exits before real game data or a graphics device; it does not cover GPU/audio/controller behavior. |
| AppImage module smoke | Exact extracted AppImage payload runs the packaged setup helper with an intentionally contaminated library path and broken `clang`, restores the host environment, falls back to GCC, translates a synthetic DOL, builds an x86-64 ELF module, `dlopen`s it, and checks ABI/entry metadata. | Does not exercise gameplay; the separate local real-disc build is host-specific evidence. |
| AppImage clean-host smoke | Exact AppImage runs `--ringout-self-test` as uid 65534 in a digest-pinned Ubuntu 24.04 container with no network, no capabilities, no FUSE, and `no-new-privileges`. | Proves extraction/startup self-test, not desktop integration, rendering, sound, or input. |
| Package policy | Both packages validate manifests, exact source/provenance, licence payload, privacy, and absence of disc/save/generated-module data. Windows also validates PE import closure and ZIP timestamps; AppImage validates DSO/source closure and glibc floor. | Policy checks cannot prove runtime correctness. |
| Netplay protocol test | `moderngekko.netplay_protocol` is one test in the 45-test Linux runtime tree. | It is localhost/protocol coverage, not Internet traversal, adverse-network simulation, cross-platform interoperability, or two physical peers. |
| Release publication | Publisher tests simulate tag/source checks, draft creation/joining, races, and immutable digest comparisons. Public `.10` API metadata and sidecars match. | Publication integrity does not add gameplay evidence. |

Source anchors for those claims:

- Windows configure/build, package, Wine smoke, and publication:
  `.github/workflows/windows-cross.yml:118-251`.
- Linux CTest, static DolRecomp, AppImage assembly, clean-host smoke, and
  publication: `.github/workflows/linux-appimage.yml:117-392`.
- Windows synthetic DOL through real packaged `lld` and loader:
  `.github/scripts/smoke-windows-package.sh:97-179` and
  `.github/scripts/windows-package-smoke.py:37-53`.
- AppImage synthetic module loader:
  `.github/scripts/smoke-appimage-module.sh:1-140`.
- Windows provenance/import/privacy/manifest/timestamp gates:
  `.github/scripts/package-windows-cross.sh:95-106`, `250-285`, and `364-545`.
- AppImage provenance/runtime-source/privacy/module gates:
  `.github/scripts/package-appimage.sh:165-222` and `469-738`.
- Current tag restoration and source-bound publishing:
  `.github/scripts/restore-canonical-tag-ref.sh:1-80` and
  `.github/scripts/publish-tag-draft-assets.sh:1-11`, `74-94`, `286-480`, and
  `534-633`.

## Published artifact provenance

Payload SHA-256 values were checked against the public release assets and their
downloaded `.sha256` sidecars on 2026-08-25.

| Release asset | Bytes | Payload SHA-256 |
| --- | ---: | --- |
| `RingOut-1.2.1-ell.1-windows-x86_64.zip` | 210,114,353 | `1bc0b71bc691e42c2667c80bd219544ad68cda59166fb1986ccd69d8d093a1c2` |
| `RingOut-1.2.1-ell.2-windows-x86_64.zip` | 210,114,525 | `0684a3d8b5c5d2b57f5bbd05d54a2c4482fd8697023b32dc51f88460dff312a2` |
| `RingOut-1.2.1-ell.3-windows-x86_64.zip` | 210,113,810 | `74682e4e788405d6e2285c5dab0718ca61f80d583f3a393f500f1f65a5cbc3bc` |
| `RingOut-1.2.1-ell.6-windows-x86_64.zip` | 210,129,296 | `5d1be65d23ec8378cd1fa0512b6d2ae6a71ddcfd058509d7b1ab9419bf4b0425` |
| `RingOut-1.2.1-ell.6-linux-x86_64.AppImage` | 24,168,952 | `21cd643d4781cf047afc364f3de962f61dde060a247ef86b842f5300f66c6aae` |
| `RingOut-1.2.1-ell.6-appimage-runtime-sources.tar.zst` | 4,741,122 | `53ccfab30cf3a5acf1c313099ec71af442f3097f9de37f7efa2509af2b8a062f` |
| `RingOut-1.2.1-ell.9-windows-x86_64.zip` | 221,073,074 | `d1b360fffd1d1d4c077db441399656c769e746557dde5f248e56f15857e88e30` |
| `RingOut-1.2.1-ell.9-linux-x86_64.AppImage` | 34,163,192 | `4039cc29eef1decde221e21a41d08fa920e8a1a0f7380fa40ba456d5c052d5db` |
| `RingOut-1.2.1-ell.9-appimage-runtime-sources.tar.zst` | 4,741,097 | `e0de41d231475521e3c82d1a6692137637f96c09e2cdf7d5a096581256320111` |
| `RingOut-1.2.1-ell.10-windows-x86_64.zip` | 221,109,204 | `fb5274be369aeffedfa718fe37076eb12b336a5dd6115e46b0386c10c9cbab23` |
| `RingOut-1.2.1-ell.10-linux-x86_64.AppImage` | 34,163,192 | `6dc3db976e6c1db044bc7eaa3a77fc3861757f61a17f8c73564d57bb7d55f8df` |
| `RingOut-1.2.1-ell.10-appimage-runtime-sources.tar.zst` | 4,741,081 | `7a8db4b8dcfad9a545a8e317020bf3904993ad761623eee72f999372dd0651bc` |

The checkout retains byte-identical local copies of the published `.1`, `.2`,
and `.3` ZIPs under `dist/out/`; their hashes and internal `SOURCE.txt` commits
match the table and tags. It does not retain the published `.6` payloads, so
the `.6` record is from the public GitHub API plus sidecars rather than a fresh
local full-payload hash.

Do not mistake the locally retained files named `.4` for release artifacts:

- `dist/out/RingOut-1.2.1-ell.4-linux-x86_64.AppImage` hashes to
  `422a968b6f85b74e910abd3d293bad420665be1c9dbb3c33fe11463d72258e60`
  and internally declares validation-only source commit `ab8f3b80` (`.3`).
- `.cache/ell4-final-appimage.01FrXKtP/RingOut-1.2.1-ell.4-linux-x86_64.AppImage`
  hashes to `ce9387abf03f9d607f22d5e986312d04327c8e908827d5d8ce5f4ba2499519ae`;
  its `SOURCE.txt` explicitly says it was made from a local worktree based on
  `ab8f3b80` and must not be redistributed.
- Its retained runtime source bundle hashes to
  `c07bb2e24f033442475aa4a9d4223437c080e07d15f46a0c1ed5f9bc68da4ebf`.
- `.cache/local-package-smoke/RingOut-1.2.1-ell.4-local-smoke-windows-x86_64.zip`
  hashes to `d427a8b6fcf98786c758aaa128a284ecbadc944935bf84c55a1905cefd30de13`
  and also declares base commit `ab8f3b80`.

Those files are useful historical QA evidence only. None is the tag-built
`6f1ef65e` package, none was publicly released, and none should be promoted.

## Claim boundaries and known limitations

The numbered list is the `ff0ad952`/`ell.6` release record. A later
`codex/rollback-netplay` worktree has live rollback and integrated-launcher
implementation evidence, including a production-path two-process correction,
but it has no tagged Windows/AppImage artifact. Its final Linux/source rerun
includes the previously required memory-card/output/fault-path fixes and passed
clean, correction, horizon, desync-negative, and fixed-delay regression routes.
Nothing in the branch changes the release lineage or makes a published package
rollback-ready.
The workflows now describe package assembly/smoke integration, but manual
dispatch retains no full package and no physical/cross-machine rollback run has
been completed (`.github/workflows/windows-cross.yml:214-238`;
`.github/workflows/linux-appimage.yml:311-358`).

1. **Physical Windows:** an earlier prerelease produced the user reports that
   drove `.3`, proving some physical-Windows startup and game execution. The
   exact `.6` ZIP has not completed the documented fresh-folder, real-disc,
   GPU/audio/controller/menu/gameplay matrix on physical Windows.
2. **Cross-platform netplay:** no retained test proves Windows-to-Linux,
   Windows-to-Windows, or Linux-to-Linux play between two physical machines.
   No adverse latency, jitter, loss, disconnect/reconnect, NAT, or port-forward
   matrix is part of release CI.
3. **Rollback:** published `ell.6` netplay is fixed-delay lockstep. The later
   source worktree has branch-local live rollback evidence, but no published
   package or physical/cross-platform proof; see
   [`netplay-audit.md`](netplay-audit.md) and
   [`netplay-rollback-roadmap.md`](netplay-rollback-roadmap.md).
4. **Real game data:** release automation intentionally never receives or
   distributes a disc image, extracted game files, saves, or a generated game
   module. Synthetic-DOL success proves toolchain/ABI plumbing only.
5. **Graphics and audio:** neither the Wine smoke nor the container self-test
   creates the complete physical GPU/audio/input environment used in play.
6. **Windows backend scope:** the MinGW package uses the supported
   Vulkan/OpenGL, Cubeb/OpenAL, and SDL paths. Microsoft-SDK-only paths remain
   outside this release: native Direct3D/WASAPI/controller/Bluetooth Wii Remote
   integrations are not silently supplied. QWave/DSCP marking is deliberately
   omitted under MinGW; users do not need `qwave.dll`.
7. **AppImage first run:** the AppImage includes the runtime, Sys resources,
   and static DolRecomp, but not an entire Linux compiler environment. Users
   still need CMake, Ninja, Python 3, a C compiler/libc headers, and a suitable
   graphics driver for their private first-run module build.
8. **Draft visibility:** unauthenticated GitHub release APIs do not expose
   drafts. The public 404 for `.4`/`.5` proves there is no public release at
   those tag endpoints, not that no inaccessible orphan draft exists.

## Reproduce the audit

Run from the repository root. These commands audit history and publication;
they do not repeat the full build guide.

```sh
# Local tag objects and peeled commits.
git for-each-ref 'refs/tags/v1.2.1-ell.*' \
  --format='%(refname:short) object=%(objectname) type=%(objecttype) peeled=%(*objectname)'

# Confirm that origin still advertises those exact annotated objects/commits.
git ls-remote --tags origin 'refs/tags/v1.2.1-ell.*'

# Review each release delta and the workflow as it existed at that tag.
git log --oneline d25d4812..v1.2.1-ell.6
git diff --stat v1.2.1..v1.2.1-ell.1
git show v1.2.1-ell.6:.github/workflows/windows-cross.yml
git show v1.2.1-ell.6:.github/workflows/linux-appimage.yml

# Audit public runs. Requires curl and jq, but not GitHub authentication.
curl --fail --silent --show-error --location \
  -H 'Accept: application/vnd.github+json' \
  -H 'X-GitHub-Api-Version: 2022-11-28' \
  'https://api.github.com/repos/Ell/RingOut/actions/runs?per_page=100' |
  jq -r '.workflow_runs[] |
    [.id,.name,.event,.head_branch,.head_sha,.conclusion,.html_url] | @tsv'

# Audit the public release/asset inventory and GitHub-computed digests.
curl --fail --silent --show-error --location \
  -H 'Accept: application/vnd.github+json' \
  -H 'X-GitHub-Api-Version: 2022-11-28' \
  'https://api.github.com/repos/Ell/RingOut/releases?per_page=20' |
  jq -r '.[] | ([.tag_name,.target_commitish,.draft,.prerelease,.html_url] | @tsv),
    (.assets[] | ["asset",.name,.size,.digest,.browser_download_url] | @tsv)'

# Verify the locally retained published ZIPs and inspect their source records.
(cd dist/out && sha256sum -c RingOut-1.2.1-ell.1-windows-x86_64.zip.sha256)
(cd dist/out && sha256sum -c RingOut-1.2.1-ell.2-windows-x86_64.zip.sha256)
(cd dist/out && sha256sum -c RingOut-1.2.1-ell.3-windows-x86_64.zip.sha256)
unzip -p dist/out/RingOut-1.2.1-ell.3-windows-x86_64.zip '*/SOURCE.txt'

# A tag build publishes to a draft; a manual dispatch is deliberately
# validation-only and leaves no retained package download.
sed -n '15,25p' .github/workflows/windows-cross.yml
sed -n '15,25p' .github/workflows/linux-appimage.yml
```

For a new release, require both tag jobs to finish, verify all payload sidecars,
inspect the source marker/commit, and complete the missing physical-Windows and
two-machine netplay QA before strengthening any release claim.
