# RingOut engineering documents

This directory contains source-led audits, implementation research, release handoffs, and technical roadmaps. The repository-root `README.md` remains the user-facing project overview; documents here preserve deeper evidence and claim boundaries for maintainers.

## Current documents

| Document | Purpose |
| --- | --- |
| [Netplay audit](netplay-audit.md) | Executive verdict, scorecard, evidence summary, priority order, and index for all netplay research |
| [Netplay lobby](netplay-lobby.md) | Entry points, restart lifecycle, LAN discovery, connection topology, roster, mapping, start synchronization, and lobby UX |
| [Netplay connectivity](netplay-connectivity.md) | Current direct-IP behavior, Dolphin/Slippi traversal research, proposed room codes, ICE/STUN/TURN relay architecture, privacy UX, and NAT-matrix tests |
| [Netplay protocol and security](netplay-protocol-security.md) | Input transport, trust boundaries, parser/save-transfer risks, desync and compatibility protection, and hardening plan |
| [Netplay rollback roadmap](netplay-rollback-roadmap.md) | Fixed-delay behavior, historical rollback measurements, test coverage, performance constraints, and staged rollback design |
| [Rollback implementation handoff](rollback-netplay-implementation.md) | Branch-local live rollback architecture, safety policy, final Linux/source evidence, and remaining platform/connectivity gaps |
| [Live rollback test harness](rollback-live-test-harness.md) | Production, correction, horizon, confirmed-state mismatch, and fixed-delay commands; retained evidence and claim boundaries |
| [Rollback GPU-state research](rollback-emulation-gpu-state.md) | Why emulated GPU/FIFO state matters, player FIFO failure root cause, whole-snapshot quiescence fix, and renderer-backed release gate |
| [SC2 game-specific rollback](sc2-slippi-rollback.md) | Slippi/GGPO comparison, exact game-hook discovery, selective checkpoint groundwork, and certification gates |
| [Build and release pipeline](build-release-pipeline.md) | Windows cross-compilation, build parallelism, caching, ZIP publication, draft releases, and Linux AppImage packaging |
| [Windows runtime and controls](windows-runtime-controls.md) | Package contents, first-run module compilation, media/runtime dependencies, controls, netplay startup, and troubleshooting |
| [Release history and testing](release-history-and-testing.md) | `ell` tag lineage, change provenance, validation evidence, artifact status, and unproven platform cases |
| [Launcher UI research](launcher-ui-research.md) | Existing launcher capabilities, release setup gap, recommended cross-platform architecture, UX, and delivery stages |
| [Launcher experience prototype](launcher-experience-prototype.md) | Implemented source-tree launcher, integrated setup flow, UI decisions, end-to-end RVZ evidence, and release boundary |

## Maintenance rules

- Pin audits and measurements to a commit and date.
- Cite exact source paths and line numbers where practical.
- Include reproduction commands and prerequisites.
- Distinguish current shipped behavior from historical evidence, proposed work, and unverified assumptions.
- Update conclusions when later code or measurements invalidate them; do not silently leave contradictory claims.
- Keep new research Markdown in this directory rather than at the repository root.
- Leave user-facing and subsystem-owned `README.md`, `CONTRIBUTING.md`, and provenance files beside the code they govern.
