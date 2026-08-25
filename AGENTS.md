# Repository guidance

Project audits, implementation research, release handoffs, and technical roadmaps live in [`docs/`](docs/). Read the relevant documents there before changing the corresponding subsystem, and update them when new evidence changes a recorded conclusion.

Start with:

- [`docs/README.md`](docs/README.md) for the documentation catalog and maintenance rules.
- [`docs/netplay-audit.md`](docs/netplay-audit.md) for the netplay verdict, evidence boundaries, risk summary, and document index.
- [`docs/netplay-lobby.md`](docs/netplay-lobby.md) for lobby discovery, connection, mapping, and start/stop behavior.
- [`docs/netplay-protocol-security.md`](docs/netplay-protocol-security.md) for protocol, trust-boundary, parser, save-transfer, and reliability findings.
- [`docs/netplay-rollback-roadmap.md`](docs/netplay-rollback-roadmap.md) for fixed-delay behavior, rollback experiments, measurements, tests, and the staged implementation plan.
- [`docs/build-release-pipeline.md`](docs/build-release-pipeline.md) for Windows cross-compilation, caching, artifact publication, and AppImage packaging.
- [`docs/windows-runtime-controls.md`](docs/windows-runtime-controls.md) for packaged Windows dependencies, first-run compilation, controls, netplay startup, and troubleshooting.
- [`docs/release-history-and-testing.md`](docs/release-history-and-testing.md) for the `ell` release lineage, validation evidence, and remaining platform test gaps.
- [`docs/launcher-ui-research.md`](docs/launcher-ui-research.md) for launcher architecture and cross-platform setup research.
- [`docs/launcher-experience-prototype.md`](docs/launcher-experience-prototype.md) for the implemented source launcher, setup flow, UI decisions, validation evidence, and release boundary.

Documentation rules:

- Keep `README.md` at the repository root as the user-facing project entry point.
- Keep subsystem-owned `README.md`, `CONTRIBUTING.md`, and provenance files beside the code they describe; the `docs/` rule applies to repository-level research and handoffs.
- Put new research and handoff Markdown in `docs/`, not in the repository root.
- Record the commit, date, exact reproduction commands, and source path/line evidence for audits and measurements.
- Separate shipped behavior, historical measurements, proposed work, and unverified assumptions.
- Do not describe the integrated C++ launcher as packaged or shipped yet: commit `514cd424` is a source-tree prototype, while the Windows ZIP and Linux AppImage still use their existing wrapper and script setup paths.
- Do not describe current netplay as rollback: the audited implementation is fixed-delay lockstep. Rollback exists only as experimental state/replay groundwork until the live protocol gains prediction, snapshots, correction, and resimulation.
