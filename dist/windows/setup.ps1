# Ring Out first-time setup for Windows. The package version is in SOURCE.txt.
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

# The module's CMake does find_package(Python3 REQUIRED) for gen_module_tables.py.
# The bundled interpreter is the embeddable build, which is deliberately not on
# PATH and not in the registry, so CMake will never discover it on its own --
# it has to be pointed at explicitly.
$Python = Join-Path $Tools 'python\python.exe'
if (-not (Test-Path -LiteralPath $Python)) {
    $onPath = Get-Command 'python.exe' -ErrorAction SilentlyContinue
    $Python = if ($onPath) { $onPath.Source } else { $null }
}

$missing = @()
if (-not $Clang)  { $missing += 'clang' }
if (-not $Cmake)  { $missing += 'cmake' }
if (-not $Ninja)  { $missing += 'ninja' }
if (-not $Python) { $missing += 'python' }
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
# Check the image BEFORE the long extract, and say something useful. dolrecomp
# accepts only .iso and .wbfs, and wants a plain uncompressed image: it reads
# the GameCube magic C2 33 9F 3D at offset 0x1C. A compressed image (NKit, RVZ
# renamed to .iso) passes the extension test and then fails with a message that
# does not explain what to do about it.
$ext = [System.IO.Path]::GetExtension($Iso).ToLowerInvariant()
if ($ext -notin '.iso', '.wbfs') {
    Die @"
'$ext' images cannot be read directly -- only .iso and .wbfs are supported.

Convert it to a plain ISO first. In Dolphin: right-click the game ->
Properties -> Convert File, choose format "ISO" with no compression.
"@
}
if ($ext -eq '.iso') {
    $fs = [System.IO.File]::OpenRead($Iso)
    try {
        $hdr = New-Object byte[] 32
        $null = $fs.Read($hdr, 0, 32)
    } finally { $fs.Dispose() }
    # Big-endian 0xC2339F3D at 0x1C.
    if (-not ($hdr[0x1C] -eq 0xC2 -and $hdr[0x1D] -eq 0x33 -and
              $hdr[0x1E] -eq 0x9F -and $hdr[0x1F] -eq 0x3D)) {
        $id = -join ($hdr[0..5] | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
        $sizeMB = [int]((Get-Item -LiteralPath $Iso).Length / 1MB)
        # A GameCube disc holds at most ~1.36 GB. Anything materially larger is
        # a DVD -- the PS2 or Xbox release of the same game, which people pick
        # by mistake because the filename looks right. Telling them to
        # "decompress it in Dolphin" would be useless advice for that case.
        if ($sizeMB -gt 1500) {
            Die @"
That image is $sizeMB MB, which is too large to be a GameCube disc (max ~1400 MB).

It is almost certainly the PlayStation 2 or Xbox release of the game. Those
are the same game but a completely different console, and this port recompiles
GameCube PowerPC code specifically -- it cannot use them.

  disc id read: '$id'

You need the GameCube version, around 1.0-1.4 GB, whose first six bytes are a
disc id like GRSEAF.
"@
        }
        Die @"
That .iso is not a plain GameCube disc image.

The GameCube signature is missing from its header, which usually means the file
is compressed -- an NKit image, or an RVZ/GCZ renamed to .iso. It has to be
decompressed before it can be recompiled.

  disc id read: '$id'
  size:         $sizeMB MB

In Dolphin: right-click the game -> Properties -> Convert File, choose
format "ISO" and compression "None".
"@
    }
}

Write-Host "==> 1/3  Extracting disc"
$Game = Join-Path $Here 'game'
if (Test-Path -LiteralPath $Game) { Remove-Item -Recurse -Force -LiteralPath $Game }
# 2>&1 so dolrecomp's stderr lands in this window; it explains the failure and
# was previously invisible to anyone reporting a problem.
& (Join-Path $Here 'tools\dolrecomp.exe') extract $Iso $Game 2>&1 | Write-Host
if ($LASTEXITCODE -ne 0) { Die "Disc extraction failed (dolrecomp exit $LASTEXITCODE). See the messages above." }

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
# ZIP stores DOS timestamps with no timezone, so files written by CI at 21:49
# UTC extract as 21:49 LOCAL. Anyone west of UTC therefore ends up with build
# inputs dated in the future, and ninja can never make build.ninja newer than
# CMakeLists.txt:
#
#   ninja: error: manifest 'build.ninja' still dirty after 100 tries,
#                 perhaps system time is not set
#
# The clock is fine; the files are ahead of it. Pull anything future-dated back
# to now before configuring.
$now = Get-Date
$future = @(Get-ChildItem (Join-Path $Here 'module-src') -Recurse -File -ErrorAction SilentlyContinue |
           Where-Object { $_.LastWriteTime -gt $now })
if ($future.Count -gt 0) {
    Write-Host "    normalising $($future.Count) future-dated file(s) from the archive"
    foreach ($f in $future) { try { $f.LastWriteTime = $now } catch { } }
}

Write-Host "==> 3/3  Building the module"
& $Cmake -S (Join-Path $Here 'module-src') -B (Join-Path $Work 'build') -GNinja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_C_COMPILER=$Clang" `
    "-DPython3_EXECUTABLE=$Python" `
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

# Bundled post-processing filters (scanlines, CRT). Dolphin only searches
# <userdir>\Shaders, so they are installed there. Existing files are left
# alone so an edited filter is never overwritten.
$shaderSrc = Join-Path $Here 'shaders'
if (Test-Path -LiteralPath $shaderSrc) {
    $shaderDst = Join-Path $Here 'userdata\Shaders'
    New-Item -ItemType Directory -Force -Path $shaderDst | Out-Null
    Get-ChildItem $shaderSrc -Filter *.glsl | ForEach-Object {
        $target = Join-Path $shaderDst $_.Name
        if (-not (Test-Path -LiteralPath $target)) { Copy-Item $_.FullName $target }
    }
}

Write-Host ""
Write-Host "Setup complete. Run RingOut.exe to play."
