# Windows support (retired)

Nothing in here is built, run, or maintained. It is kept because it worked and
because deleting it would throw away the only record of how the Windows package
was assembled — not because anyone intends to revive it.

Moved here on 2026-08-17. Windows is no longer a target of this project.

## What is here

| Path | Was |
|---|---|
| `workflows/windows.yml` | `.github/workflows/windows.yml` |
| `scripts/package-windows.ps1` | `.github/scripts/package-windows.ps1` |
| `scripts/ringout.iss` | `.github/scripts/ringout.iss` |
| `dist/RingOut-1.0-dist-windows/` | `dist/RingOut-1.0-dist/windows/` |

## What moving them changed

**CI no longer runs a Windows job.** GitHub only reads workflows under
`.github/workflows/`, so `windows.yml` is inert where it now sits — that is the
point, not a side effect. Reviving it means moving the file back, nothing else.

**The Linux/Deck packages are unaffected.** `package-dist.sh` already listed
`windows` among the directories forbidden from reaching its staging area, so
`dist/RingOut-1.0-dist/windows/` was never in a shipped Linux artifact. That
guard is still in place and is now simply never triggered.

## If it is ever revived

`package-windows.ps1` still reads the *live* `dist/RingOut-1.0-dist/` for
`README.txt`, `CREDITS.txt` and `module-src/`, and only its `windows\launcher\`
path points at content that moved. It was never made self-contained, so expect
to fix paths rather than to drop it back in place.

It also never ran with the test suite that `deck.yml` gained: the ctest step was
deliberately added to the Deck workflow only, because the suite is verified in
the Debian 12 container and nowhere else.
