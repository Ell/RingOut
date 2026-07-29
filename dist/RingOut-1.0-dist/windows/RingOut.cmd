@echo off
rem Ring Out - Ver 1.0 (Windows) launcher shim.
rem Windows blocks .ps1 files that came from a download, so the policy is
rem overridden for THIS process only -- nothing system-wide is changed.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RingOut.ps1" %*
