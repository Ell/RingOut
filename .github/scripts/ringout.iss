; Ring Out - Ver 1.0 : Windows installer (Inno Setup)
;
; Built by .github/scripts/package-windows.ps1 on the CI runner.
;
; PrivilegesRequired=lowest is deliberate and load-bearing. Ring Out recompiles
; the game from the user's own disc INTO ITS OWN FOLDER -- setup.ps1 writes
; game/, work/ and bin/g<ID>_recomp.dll there, and the runtime writes userdata/.
; A machine-wide install under Program Files is read-only for a normal user, so
; first-run setup would fail. "lowest" resolves {autopf} to the per-user
; Programs folder, which is writable.

#define AppName    "Ring Out"
#define AppVersion "1.0"
#define AppPublisher "Jack Poison"

[Setup]
AppId={{7C1D9E2A-4F63-4A11-9E5B-3D8A2C6F1B04}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\Ring Out
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=.
OutputBaseFilename=RingOut-{#AppVersion}-windows-x64-setup
Compression=lzma2/ultra64
SolidCompression=yes
LZMANumBlockThreads=4
WizardStyle=modern
; The bundled toolchain is thousands of small files; without this the progress
; bar sits at 0% for a long time while Inno enumerates them.
SetupLogging=yes
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\RingOut.exe
; Extracted disc + build output need considerably more room than the install.
ExtraDiskSpaceRequired=1600000000

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "RingOut-1.0\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}";           Filename: "{app}\RingOut.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";     Filename: "{app}\RingOut.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\RingOut.exe"; Description: "Set up my game disc now"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
; Everything below is produced on the user's machine after install, so Inno does
; not track it and would otherwise leave gigabytes behind.
Type: filesandordirs; Name: "{app}\game"
Type: filesandordirs; Name: "{app}\work"
Type: filesandordirs; Name: "{app}\userdata"
Type: files;          Name: "{app}\bin\g*_recomp.dll"

[Messages]
WelcomeLabel2=This will install {#AppName} {#AppVersion} on your computer.%n%nNo game data is included. After installing, you supply a GameCube disc image you already own, and it is extracted and recompiled on this machine -- that step takes several minutes and needs about 1.5 GB of free space.
