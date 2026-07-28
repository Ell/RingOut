# Assembles the Windows release: the built binaries, the redistributable
# scaffolding, and a self-contained toolchain the user builds their module with.
#
# Runs on the CI runner, not a developer machine -- llvm-mingw alone unpacks to
# roughly 2 GB.
#
# Why a whole toolchain ships: the module is recompiled from the user's own
# disc, so it can never be prebuilt for them, and requiring a multi-GB Visual
# Studio install to play a game is not a real option. llvm-mingw is the only
# self-contained choice -- clang alone is not enough on Windows, it still needs
# a C runtime and headers from somewhere.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BinDir,     # built .exe files
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is ~10x slower with a progress bar

$stage = Join-Path $OutDir 'RingOut-1.0'
$dl    = Join-Path $OutDir '_dl'
New-Item -ItemType Directory -Force -Path $stage, $dl | Out-Null

function Get-LatestAsset($repo, $pattern) {
    # Pinning tags rots; asking the API for the current release does not.
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" `
        -Headers @{ 'User-Agent' = 'ringout-ci'; 'Accept' = 'application/vnd.github+json' }
    $asset = $rel.assets | Where-Object { $_.name -like $pattern } | Select-Object -First 1
    if (-not $asset) { throw "no asset matching '$pattern' in latest release of $repo" }
    Write-Host "  $repo -> $($asset.name)"
    return $asset.browser_download_url
}

function Fetch($url, $file) {
    $path = Join-Path $dl $file
    Write-Host "downloading $file"
    Invoke-WebRequest -Uri $url -OutFile $path
    return $path
}

# --- the built runtime ----------------------------------------------------
Write-Host "==> binaries"
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin'), (Join-Path $stage 'tools') | Out-Null
Copy-Item (Join-Path $BinDir 'moderngekko-run.exe') (Join-Path $stage 'bin') -Force
Copy-Item (Join-Path $BinDir 'dolrecomp.exe')       (Join-Path $stage 'tools') -Force

# The runtime imports MSVCP140 / VCRUNTIME140; shipping the redist DLLs beats
# telling players to go and install the C++ redistributable first.
Write-Host "==> VC++ runtime"
# Take the NEWEST redist, not whichever the filesystem lists first. The runner
# carries several side by side, and the first match was the VS2019 (VC142) set
# while the binary is built with the v143 toolset. MSVCP140.dll is ABI-stable,
# but VCRUNTIME140_1.dll and MSVCP140_ATOMIC_WAIT.dll gain exports over time, so
# an older redist fails with "entry point not found" -- on a player's machine,
# never here, because CI has the real runtime installed system-wide.
$crt = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' `
    -Directory -ErrorAction SilentlyContinue |
    Sort-Object {
        $v = $_.Parent.Parent.Name          # the 14.xx.yyyyy version folder
        try { [version]$v } catch { [version]'0.0.0' }
    } -Descending | Select-Object -First 1
if ($crt) {
    Copy-Item (Join-Path $crt.FullName '*.dll') (Join-Path $stage 'bin') -Force
    Write-Host "  from $($crt.FullName)"
    Get-ChildItem (Join-Path $stage 'bin') -Filter '*.dll' | ForEach-Object {
        Write-Host ("    {0}  {1}" -f $_.Name, $_.VersionInfo.FileVersion)
    }
} else {
    Write-Warning "VC++ redist DLLs not found -- players will need the redistributable installed"
}

# --- scaffolding ----------------------------------------------------------
Write-Host "==> scaffolding"
$src = Join-Path $RepoRoot 'dist\RingOut-1.0-dist'
foreach ($f in 'setup.ps1', 'RingOut.ps1', 'RingOut.cmd', 'README.txt', 'CREDITS.txt') {
    Copy-Item (Join-Path $src $f) $stage -Force
}
Copy-Item (Join-Path $src 'module-src') $stage -Recurse -Force
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'userdata\GameSettings') | Out-Null
Copy-Item (Join-Path $RepoRoot 'work\mg_userdir\GameSettings\GRSEAF.ini') `
          (Join-Path $stage 'userdata\GameSettings') -Force

# --- toolchain ------------------------------------------------------------
$tc = Join-Path $stage 'toolchain'
New-Item -ItemType Directory -Force -Path $tc | Out-Null

Write-Host "==> llvm-mingw"
$llvmUrl = Get-LatestAsset 'mstorsjo/llvm-mingw' '*ucrt-x86_64.zip'
$llvmZip = Fetch $llvmUrl 'llvm-mingw.zip'
Expand-Archive $llvmZip -DestinationPath $dl -Force
$llvmRoot = Get-ChildItem $dl -Directory -Filter 'llvm-mingw-*' | Select-Object -First 1
if (-not $llvmRoot) { throw "llvm-mingw did not extract as expected" }
Copy-Item (Join-Path $llvmRoot.FullName '*') $tc -Recurse -Force

# Trim what a module build never touches. clang keeps its sysroot
# (x86_64-w64-mingw32/, lib/clang/) -- removing that breaks the compiler.
Write-Host "==> trimming toolchain"
$before = (Get-ChildItem $tc -Recurse -File | Measure-Object Length -Sum).Sum
Get-ChildItem (Join-Path $tc 'bin') -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(lldb|clang-repl|clang-check|clang-tidy|clang-format|clang-refactor|clang-rename|clang-scan-deps|llvm-lto|llvm-reduce|llvm-exegesis|bugpoint|opt|llc|lli|clang-doc|llvm-cfi-verify|clang-linker-wrapper)' } |
    Remove-Item -Force -ErrorAction SilentlyContinue
# The other three target triples are dead weight for an x86_64-only release.
foreach ($t in 'aarch64-w64-mingw32', 'armv7-w64-mingw32', 'i686-w64-mingw32') {
    Remove-Item (Join-Path $tc $t) -Recurse -Force -ErrorAction SilentlyContinue
}
Remove-Item (Join-Path $tc 'share\doc'), (Join-Path $tc 'share\man'), (Join-Path $tc 'include') `
    -Recurse -Force -ErrorAction SilentlyContinue
$after = (Get-ChildItem $tc -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("  {0:N0} MB -> {1:N0} MB" -f ($before / 1MB), ($after / 1MB))

Write-Host "==> ninja"
$ninjaZip = Fetch (Get-LatestAsset 'ninja-build/ninja' 'ninja-win.zip') 'ninja.zip'
Expand-Archive $ninjaZip -DestinationPath (Join-Path $tc 'bin') -Force

Write-Host "==> cmake"
$cmakeZip = Fetch (Get-LatestAsset 'Kitware/CMake' 'cmake-*-windows-x86_64.zip') 'cmake.zip'
Expand-Archive $cmakeZip -DestinationPath $dl -Force
$cmakeRoot = Get-ChildItem $dl -Directory -Filter 'cmake-*-windows-x86_64' | Select-Object -First 1
if (-not $cmakeRoot) { throw "cmake did not extract as expected" }
# Merge into the same prefix: cmake finds its modules at ../share/cmake-X.Y
# relative to the executable, so bin/ and share/ must stay siblings.
Copy-Item (Join-Path $cmakeRoot.FullName 'bin\*')   (Join-Path $tc 'bin')   -Recurse -Force
Copy-Item (Join-Path $cmakeRoot.FullName 'share\*') (Join-Path $tc 'share') -Recurse -Force

Write-Host "==> python"
# gen_module_tables.py needs an interpreter; the embeddable build is ~10 MB.
$pyVer = '3.11.9'
$pyZip = Fetch "https://www.python.org/ftp/python/$pyVer/python-$pyVer-embed-amd64.zip" 'python.zip'
Expand-Archive $pyZip -DestinationPath (Join-Path $tc 'python') -Force

# --- installer ------------------------------------------------------------
# The .exe is the primary download; the .zip stays for people who prefer a
# portable folder they can drop anywhere.
Write-Host "==> installer"
$iscc = Get-ChildItem 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
                      'C:\Program Files\Inno Setup 6\ISCC.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1
if (-not $iscc) { throw "ISCC.exe (Inno Setup 6) not found on this runner" }
Write-Host "  using $($iscc.FullName)"

Copy-Item (Join-Path $PSScriptRoot 'ringout.iss') $OutDir -Force
Push-Location $OutDir
try {
    & $iscc.FullName 'ringout.iss'
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
Remove-Item (Join-Path $OutDir 'ringout.iss') -Force -ErrorAction SilentlyContinue

# --- zip ------------------------------------------------------------------
Write-Host "==> zipping"
$zip = Join-Path $OutDir 'RingOut-1.0-windows-x64.zip'
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal -Force
Remove-Item $dl -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Get-ChildItem $OutDir -File | Where-Object { $_.Extension -in '.exe', '.zip' } |
    ForEach-Object { Write-Host ("{0,-44} {1,6:N0} MB" -f $_.Name, ($_.Length / 1MB)) }
Get-ChildItem $stage | Select-Object Name, Length | Format-Table -AutoSize
