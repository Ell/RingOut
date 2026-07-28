# Ring Out - Ver 1.0 : first-time setup (Windows)
#
# Builds your personal copy from a GameCube disc image you already have. Nothing
# game-derived ships with this package -- this script extracts the disc and
# recompiles its executable here, on your machine.
#
# Usage:  .\setup.ps1 C:\path\to\your\disc.iso
#
# Unlike the Linux build, the toolchain is bundled: toolchain\ carries clang,
# lld, cmake and ninja, so there is nothing for you to install.

[CmdletBinding()]
param([Parameter(Position = 0)][string]$Iso)

$ErrorActionPreference = 'Stop'
$Here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Deps  = Join-Path $Here 'module-src\deps'
$Tools = Join-Path $Here 'toolchain'

function Die($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }

if (-not $Iso -or -not (Test-Path -LiteralPath $Iso -PathType Leaf)) {
    Write-Host "Usage: .\setup.ps1 C:\path\to\your\disc.iso"
    Write-Host ""
    Write-Host "Supply a GameCube disc image you already have."
    exit 1
}
$Iso = (Resolve-Path -LiteralPath $Iso).Path

# --- toolchain ------------------------------------------------------------
# Prefer the bundled toolchain; fall back to anything already on PATH so a
# developer with their own LLVM is not forced to use ours.
function Find-Tool($name) {
    $bundled = Join-Path $Tools "bin\$name.exe"
    if (Test-Path -LiteralPath $bundled) { return $bundled }
    $onPath = Get-Command "$name.exe" -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    return $null
}

$Clang = Find-Tool 'clang'
$Cmake = Find-Tool 'cmake'
$Ninja = Find-Tool 'ninja'

$missing = @()
if (-not $Clang) { $missing += 'clang' }
if (-not $Cmake) { $missing += 'cmake' }
if (-not $Ninja) { $missing += 'ninja' }
if ($missing.Count -gt 0) {
    Die ("Bundled toolchain is incomplete -- missing: {0}`nExpected it under {1}`nRe-download the release; the toolchain\ folder must be extracted with everything else." -f ($missing -join ', '), $Tools)
}

# A compiler being PRESENT is not the same as a compiler that WORKS. The Linux
# build learned this the hard way on SteamOS (clang present, libc headers
# absent, failure only after several minutes of extracting and recompiling).
# Compile something trivial FIRST -- an unpacked-wrong or AV-quarantined
# toolchain fails here in a second instead of ten minutes in.
$probe = Join-Path ([System.IO.Path]::GetTempPath()) ("ringout-probe-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $probe | Out-Null
try {
    $probeSrc = Join-Path $probe 'probe.c'
    @'
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) { return (int)strlen(""); }
'@ | Set-Content -LiteralPath $probeSrc -Encoding ASCII

    & $Clang $probeSrc -o (Join-Path $probe 'probe.exe') 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Die @"
The bundled C compiler cannot compile a trivial program.

Most likely causes:
  1. Antivirus quarantined part of toolchain\ -- clang and lld are frequently
     false-positived. Check your AV quarantine and restore/exclude this folder.
  2. The zip was extracted with a tool that dropped or truncated files. Extract
     again with Windows Explorer or 7-Zip.
"@
    }
} finally {
    Remove-Item -Recurse -Force -LiteralPath $probe -ErrorAction SilentlyContinue
}
Write-Host "Using C compiler: $Clang"

# --- 1/3 extract ----------------------------------------------------------
Write-Host "==> 1/3  Extracting disc"
$Game = Join-Path $Here 'game'
if (Test-Path -LiteralPath $Game) { Remove-Item -Recurse -Force -LiteralPath $Game }
& (Join-Path $Here 'tools\dolrecomp.exe') extract $Iso $Game
if ($LASTEXITCODE -ne 0) { Die "Disc extraction failed." }

$bootBin = Join-Path $Game 'sys\boot.bin'
if (-not (Test-Path -LiteralPath $bootBin)) { Die "Could not read a disc ID -- is that a GameCube disc image?" }
$idBytes = [System.IO.File]::ReadAllBytes($bootBin)[0..5]
$DiscId  = -join ($idBytes | ForEach-Object { [char]$_ })
if ($DiscId -notmatch '^[A-Za-z0-9]{6}$') { Die "Could not read a disc ID -- is that a GameCube disc image?" }
Write-Host "    disc id: $DiscId"

# --- 2/3 recompile --------------------------------------------------------
Write-Host "==> 2/3  Recompiling the game executable (several minutes)"
$Work = Join-Path $Here 'work'
if (Test-Path -LiteralPath $Work) { Remove-Item -Recurse -Force -LiteralPath $Work }
New-Item -ItemType Directory -Force -Path $Work | Out-Null

$jobs = [Environment]::ProcessorCount
& (Join-Path $Here 'tools\dolrecomp.exe') --gamecube (Join-Path $Game 'sys\main.dol') "-j$jobs" (Join-Path $Work 'out')
if ($LASTEXITCODE -ne 0) { Die "Recompilation failed." }

# gen_module_tables.py reads main.dol from alongside the generated sources.
Copy-Item (Join-Path $Game 'sys\main.dol') (Join-Path $Work 'out\generated\main.dol')

# --- 3/3 build the module -------------------------------------------------
Write-Host "==> 3/3  Building the module"
& $Cmake -S (Join-Path $Here 'module-src') -B (Join-Path $Work 'build') -GNinja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_C_COMPILER=$Clang" `
    -DCMAKE_C_FLAGS="-march=native" `
    "-DGAME_ID=$DiscId" `
    "-DGENERATED_DIR=$(Join-Path $Work 'out\generated')" `
    "-DDOLRECOMP_SRC=$(Join-Path $Deps 'dolrecomp-src')" `
    "-DGXRUNTIME_INC=$(Join-Path $Deps 'gxruntime-include')" `
    "-DCHASSIS_ABI_DIR=$(Join-Path $Deps 'chassis-abi')" `
    "-DMODULE_TEMPLATE=$(Join-Path $Deps 'module-template')"
if ($LASTEXITCODE -ne 0) { Die "Module configure failed." }

& $Cmake --build (Join-Path $Work 'build')
if ($LASTEXITCODE -ne 0) { Die "Module build failed." }

$dll = Join-Path $Work "build\g${DiscId}_recomp.dll"
if (-not (Test-Path -LiteralPath $dll)) { Die "Module built but g${DiscId}_recomp.dll was not produced." }
Copy-Item $dll (Join-Path $Here 'bin') -Force

Write-Host ""
Write-Host "Setup complete. Run RingOut.cmd to play."
