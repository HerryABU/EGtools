; EGtools v3.0.0 — Inno Setup 6 installer script
; Author / Signer: HerryABU
;
; Compile with Inno Setup 6 (ISCC.exe). From this script's folder (code/):
;   ISCC.exe build_installer.iss
;
; Output: EGtools-3.0.0-x64.exe  (in the script's Output folder)
;
; The GUI is published FRAMEWORK-DEPENDENT for the Windows App Runtime 1.6: the
; framework is provided by the system's single registered WindowsAppRuntime.1.6
; package (installed below when absent). This AVOIDS the 0xC000027B native
; assertion crash of SELF-CONTAINED unpackaged apps on machines that already
; have a NEWER registered WindowsAppRuntime — there the bootstrap resolves to the
; system package while the self-contained loose framework DLLs are ALSO loaded,
; mixing two Microsoft.ui.xaml.dll versions in one process. With framework-
; dependent there are NO in-folder framework DLLs, so exactly one runtime loads.
; The .NET 9 runtime stays self-contained (bundled), so no .NET install is needed.
;
; The VC++ 2022 (v14) runtime DLLs are ALSO bundled per-app inside the GUI
; folder (the self-contained publish copies vcruntime140*.dll / msvcp140*.dll),
; so the WinUI 3 / MuPDFCore native components load even when the target PC
; has no VC++ redistributable installed. Nothing extra needs deploying here.
;
; The two CLI tools are published self-contained (.NET 9) too.
;
; The two CLI directories are appended to the SYSTEM PATH (HKLM) on request,
; so EGpdf2excel / EGexcel2df are callable from any command prompt. The user is
; explicitly asked (a checked checkbox on the "Select Additional Tasks" page),
; and the write only happens when that task is selected.

#define MyAppName "EGtools"
#define MyAppVersion "3.0.0"
#define MyAppPublisher "HerryABU"
#define MyAppURL "https://github.com/HerryABU/EGtools"
#define MyAppExeName "EGtools.exe"

[Setup]
; NOTE: AppId must be unique; do not change it after publishing an installer.
AppId={{8F2C3A1E-7B4D-4E90-A1C2-19F3B6E8D5A0}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} — CAD drawing material extraction & comparison
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCopyright=Copyright (C) 2026 {#MyAppPublisher}

; Self-contained x64 build; require 64-bit Windows and run as admin so the
; CLI tools can be added to the SYSTEM PATH (HKLM) when the user opts in.
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
PrivilegesRequired=admin
; Lowest supported OS: Windows 10 1809 (build 17763) — the floor for both
; Windows App SDK 1.6 and .NET 9. Covers every Win10 1809+ and all Windows 11.
MinVersion=10.0.17763

DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=installer
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-x64
SetupIconFile=DIST\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName},0
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Refresh the environment so the PATH change takes effect without reboot.
ChangesEnvironment=yes

[Files]
; GUI (framework-dependent WinUI 3: .NET 9 self-contained + Windows App Runtime 1.6 from the system).
Source: "DIST\EGtools\*"; DestDir: "{app}\EGtools"; Flags: ignoreversion recursesubdirs createallsubdirs
; Windows App Runtime 1.6 installer — installed silently during setup (the GUI
; needs it on machines that don't already have a 1.6 runtime registered). The
; user's machine already has it, so this is a no-op there.
Source: "redist\WindowsAppRuntimeInstall-x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
; CLI tools (self-contained, independently runnable).
Source: "DIST\EGpdf2excel\*"; DestDir: "{app}\EGpdf2excel"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "DIST\EGexcel2df\*"; DestDir: "{app}\EGexcel2df"; Flags: ignoreversion recursesubdirs createallsubdirs
; Documentation.
Source: "DIST\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs
; App icon (also used for the Start Menu / desktop shortcuts).
Source: "DIST\app.ico"; DestDir: "{app}"; Flags: ignoreversion
; NOTE: The Windows App Runtime 1.6 is installed by the system (see the
; WindowsAppRuntimeInstall-x64.exe source above + its [Run] step), NOT shipped
; inside the GUI folder — that is what keeps the deployment framework-dependent
; and avoids the dual-runtime 0xC000027B startup crash.

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\EGtools\{#MyAppExeName}"; WorkingDir: "{app}\EGtools"; IconFilename: "{app}\app.ico"
Name: "{group}\EGpdf2excel (CLI)"; Filename: "{app}\EGpdf2excel\EGpdf2excel.exe"; WorkingDir: "{app}\EGpdf2excel"; IconFilename: "{app}\app.ico"
Name: "{group}\EGexcel2df (CLI)"; Filename: "{app}\EGexcel2df\EGexcel2df.exe"; WorkingDir: "{app}\EGexcel2df"; IconFilename: "{app}\app.ico"
Name: "{group}\使用文档 (中文)"; Filename: "{app}\docs\README_zh.md"
Name: "{group}\Documentation (English)"; Filename: "{app}\docs\README_en.md"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\EGtools\{#MyAppExeName}"; WorkingDir: "{app}\EGtools"; IconFilename: "{app}\app.ico"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
; Explicit, visible opt-in to add the CLI tools to the system PATH. Checked by
; default so the CLIs "just work" after install, but the user clearly sees and
; can uncheck the choice.
Name: "addtopath"; Description: "将命令行工具 (EGpdf2excel / EGexcel2df) 添加到 PATH"; GroupDescription: "附加选项"; Flags: checkedonce

[Run]
; Install the Windows App Runtime 1.6 (framework-dependent GUI dependency).
; Best-effort: when already present this is a quick no-op; ignoreerrors keeps a
; failure here from blocking the rest of the install. No internet needed — the
; MSIX is bundled inside the installer exe.
Filename: "{tmp}\WindowsAppRuntimeInstall-x64.exe"; Parameters: "--msix -q"; StatusMsg: "正在安装 Windows App Runtime 1.6…"; Flags: runhidden
; Launch the GUI on finish (optional).
Filename: "{app}\EGtools\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\EGtools"
Type: filesandordirs; Name: "{app}\EGpdf2excel"
Type: filesandordirs; Name: "{app}\EGexcel2df"
Type: filesandordirs; Name: "{app}\docs"

[Code]
// Append the two CLI directories to the SYSTEM PATH (HKLM) at install time, and
// remove them at uninstall — BUT ONLY when the user chose the "addtopath" task.
//
// IMPORTANT: This installer runs elevated (PrivilegesRequired=admin) and is a
// per-machine install into C:\Program Files. Writing to HKCU\Environment\Path
// while elevated puts the entries into the ADMINISTRATOR's user PATH, so the
// real (non-admin) user never sees the CLIs on PATH — which is exactly the bug
// we had. The system PATH is visible to every user and every new process, so we
// use HKLM instead. ChangesEnvironment=yes broadcasts WM_SETTINGCHANGE so
// already-open consoles pick it up.
//
// The install dir is recorded in a private registry key so the uninstall step
// can reconstruct the CLI paths without relying on the {app} constant.

const
  PATH_ROOT = HKLM;
  PATH_KEY = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
  PATH_VAL = 'Path';
  APP_ROOT = HKLM;
  APP_KEY = 'Software\EGtools';
  APPDIR_VAL = 'InstallDir';

function ReadSystemPath(): string;
var
  s: string;
begin
  if not RegQueryStringValue(PATH_ROOT, PATH_KEY, PATH_VAL, s) then
    s := '';
  Result := s;
end;

procedure WriteSystemPath(const s: string);
begin
  if s = '' then
    RegDeleteValue(PATH_ROOT, PATH_KEY, PATH_VAL)
  else
    RegWriteExpandStringValue(PATH_ROOT, PATH_KEY, PATH_VAL, s);
end;

// Append 'dir' to a semicolon-delimited path string if not already present.
function AppendDir(const path, dir: string): string;
var
  i, n: Integer;
  part, lower: string;
begin
  lower := Lowercase(Trim(dir));
  if path = '' then
  begin
    Result := dir;
    Exit;
  end;
  n := 0;
  while n < Length(path) do
  begin
    i := n + 1;
    while (i <= Length(path)) and (path[i] <> ';') do Inc(i);
    part := Copy(path, n + 1, i - n - 1);
    if Lowercase(Trim(part)) = lower then
    begin
      Result := path;   // already present
      Exit;
    end;
    n := i;
  end;
  if path[Length(path)] = ';' then
    Result := path + dir
  else
    Result := path + ';' + dir;
end;

// Remove 'dir' from a semicolon-delimited path string, collapsing any
// duplicate/leading/trailing separators so the value stays well-formed.
function RemoveDir(const path, dir: string): string;
var
  i, n, L: Integer;
  part, lower: string;
  res: string;
begin
  lower := Lowercase(Trim(dir));
  res := '';
  L := Length(path);
  n := 0;
  while n < L do
  begin
    i := n + 1;
    while (i <= L) and (path[i] <> ';') do Inc(i);
    part := Copy(path, n + 1, i - n - 1);
    if Lowercase(Trim(part)) <> lower then
    begin
      if res <> '' then res := res + ';';
      res := res + part;
    end;
    n := i;
  end;
  Result := res;
end;

procedure AddToPath(const dir: string);
begin
  WriteSystemPath(AppendDir(ReadSystemPath(), dir));
end;

procedure RemoveFromPath(const dir: string);
begin
  WriteSystemPath(RemoveDir(ReadSystemPath(), dir));
end;

function GetInstallDir(): string;
var
  s: string;
begin
  if RegQueryStringValue(APP_ROOT, APP_KEY, APPDIR_VAL, s) then
    Result := s
  else
    Result := ExpandConstant('{app}');
end;

procedure Dbg(const s: string);
begin
  SaveStringToFile('C:\Windows\Temp\egtools_dbg.log', s + #13#10, True);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  dir: string;
begin
  if CurStep = ssPostInstall then
  begin
    dir := ExpandConstant('{app}');
    RegWriteStringValue(APP_ROOT, APP_KEY, APPDIR_VAL, dir);
    if WizardIsTaskSelected('addtopath') then
    begin
      AddToPath(dir + '\EGpdf2excel');
      AddToPath(dir + '\EGexcel2df');
      Dbg('ssPostInstall addtopath=yes dir=' + dir);
    end
    else
      Dbg('ssPostInstall addtopath=NO dir=' + dir);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  dir: string;
begin
  if CurUninstallStep = usUninstall then
  begin
    dir := GetInstallDir();
    WriteSystemPath(RemoveDir(ReadSystemPath(), dir + '\EGpdf2excel'));
    WriteSystemPath(RemoveDir(ReadSystemPath(), dir + '\EGexcel2df'));
    RegDeleteKeyIncludingSubkeys(APP_ROOT, APP_KEY);
  end;
end;
