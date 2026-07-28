# Ring Out - Ver 1.0 (Windows)
#
# First run asks for a GameCube disc image you already have, then extracts it
# and recompiles its executable on this machine. Nothing game-derived ships with
# this package; everything personal is produced here and stays in this folder.
#
# Launch via RingOut.cmd -- it sets the execution policy for this process, which
# Windows otherwise blocks for downloaded scripts.

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

$UserDir = Join-Path $Here 'userdata'
New-Item -ItemType Directory -Force -Path $UserDir | Out-Null

function Have-Module {
    $bin = Join-Path $Here 'bin'
    (Test-Path $bin) -and @(Get-ChildItem -Path $bin -Filter 'g*_recomp.dll' -ErrorAction SilentlyContinue).Count -gt 0
}

# Report to a dialog when there is no console to read (double-clicked), and
# always to stderr.
function Report($msg) {
    [Console]::Error.WriteLine($msg)
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [System.Windows.Forms.MessageBox]::Show($msg, 'Ring Out',
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
    } catch { }
}

function Pick-Iso {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title  = 'Ring Out - select your game disc image'
        $dlg.Filter = 'Disc images (*.iso;*.gcm;*.rvz;*.wbfs;*.gcz)|*.iso;*.gcm;*.rvz;*.wbfs;*.gcz|All files (*.*)|*.*'
        $dlg.InitialDirectory = [Environment]::GetFolderPath('UserProfile')
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.FileName }
        return $null
    } catch {
        # No GUI available (Server Core, SSH session) -- fall back to a prompt.
        return (Read-Host 'Path to your disc image')
    }
}

# Accept the disc image as a plain argument so `RingOut.cmd game.iso` works from
# a terminal, and do NOT forward it to the emulator.
$Passthrough = New-Object System.Collections.Generic.List[string]
$IsoArg = $null
foreach ($a in $args) {
    if (-not $IsoArg -and $a -match '\.(iso|gcm|rvz|wbfs|gcz)$' -and (Test-Path -LiteralPath $a -PathType Leaf)) {
        $IsoArg = (Resolve-Path -LiteralPath $a).Path
    } else {
        $Passthrough.Add($a)
    }
}

if (-not (Have-Module) -or -not (Test-Path (Join-Path $Here 'game'))) {
    Write-Host "First run: this package contains no game data."
    Write-Host "You need a GameCube disc image you already have."
    Write-Host ""

    $iso = if ($IsoArg) { $IsoArg } else { Pick-Iso }
    if (-not $iso -or -not (Test-Path -LiteralPath $iso -PathType Leaf)) {
        Report "No disc image selected. Setup cancelled."
        exit 1
    }

    # Recompiling takes several minutes, so run setup in a visible console when
    # launched from Explorer -- otherwise it looks like nothing is happening.
    $setup = Join-Path $Here 'setup.ps1'
    $p = Start-Process -FilePath 'powershell.exe' `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit', '-File', $setup, $iso) `
        -Wait -PassThru
    if (-not (Have-Module)) {
        Report "Setup did not finish. See the messages in the setup window."
        exit 1
    }
}

$gameArgs = @()
if (-not ($Passthrough -contains '--game') -and (Test-Path (Join-Path $Here 'game'))) {
    $gameArgs = @('--game', (Join-Path $Here 'game'))
}

$exe = Join-Path $Here 'bin\moderngekko-run.exe'
if (-not (Test-Path -LiteralPath $exe)) { Report "bin\moderngekko-run.exe is missing."; exit 1 }

& $exe --user-dir $UserDir @gameArgs @Passthrough
exit $LASTEXITCODE
