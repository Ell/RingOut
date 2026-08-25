@echo off
rem Ring Out (Windows) launcher shim. The package version is recorded in
rem SOURCE.txt so this version-neutral script does not drift between releases.
rem Windows blocks .ps1 files that came from a download, so the policy is
rem overridden for THIS process only -- nothing system-wide is changed.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RingOut.ps1" %*
