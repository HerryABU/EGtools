; EGtools v4.3.0 — Inno Setup 6 installer script (Qt 6 front-end)
; Author / Signer: HerryABU
;
; Compile with Inno Setup 6 (ISCC.exe). From this script's folder:
;   ISCC.exe build_installer.iss
;
; Output: EGtools-4.3.0-x64.exe  (in the script's Output folder)
;
; WHAT IS SHIPPED
;   The graphical front-end is EGtools.Frontend (Qt 6.11.2, msvc2022_64). It is
;   made self-contained by windeployqt at build time (Qt runtime DLLs + the
;   qwindows platform plugin + the MSVC runtime sit next to the exe), so the app
;   runs on a clean Windows machine WITHOUT Qt or a VC++ redistributable
;   installed, and WITHOUT the Windows App Runtime (the legacy WinUI front-end
;   is gone). The Qt front-end drives the three .NET CLI kernels through QProcess
;   (EGpdf2excel / EGexcel2df / EGpdf2df); it auto-discovers them as sibling
;   folders next to the exe, so no configuration is needed after install.
;
;   The three CLI kernels are self-contained (.NET 9) and are ALSO installed as
;   standalone command-line tools. With the "addtopath" task selected their
;   directories are appended to the SYSTEM PATH (HKLM) so they are callable from
;   any prompt.

#define MyAppName "EGtools"
#define MyAppVersion "4.3.0"
#define MyAppPublisher "HerryABU"
#define MyAppURL "https://github.com/HerryABU/EGtools"
#define MyQtExeName "EGtoolsFrontend.exe"

; ---------------------------------------------------------------------------
; 架构参数（由 build_all.ps1 通过 ISCC 的 /D 传入）：
;   x64   → /DMyArch=x64   /DDistDir=DIST        /DOutSuffix=x64
;   arm64 → /DMyArch=arm64 /DDistDir=DIST-arm64  /DOutSuffix=arm64
; ⚠️ 三个都必须**成组**传：架构标识决定包能装在哪种机器上，DistDir 决定装什么内容，
;    漏一个就会做出"标着 arm64 却装着 x64 文件"的包（装上去能装完、一跑就崩）。
; ⚠️ 这里给默认值，是为了让 `ISCC build_installer.iss` 单独跑（不带 /D）仍然出 x64 包 ——
;    与以前的行为一致。
; ---------------------------------------------------------------------------
#ifndef MyArch
  #define MyArch "x64"
#endif
#ifndef DistDir
  #define DistDir "DIST"
#endif
#ifndef OutSuffix
  #define OutSuffix "x64"
#endif

[Setup]
; NOTE: AppId must stay stable once published so in-place updates work (same
; identity -> Inno offers "Update"; [Files] use 'ignoreversion' so every payload
; file is replaced).
AppId={{8F2C3A1E-7B4D-4E90-A1C2-19F3B6E8D5A0}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
CloseApplications=yes
AppMutex=EGtoolsAppMutex
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

; ⚠️ 这两个必须跟目标架构一致：写成 x64 的包**装不进 arm64 的安装模式**，
;    反之亦然（Inno 会直接拒绝安装，或按 32/64 位模式装错目录）。
ArchitecturesInstallIn64BitMode={#MyArch}
ArchitecturesAllowed={#MyArch}
PrivilegesRequired=admin
; Let a STANDARD USER install without elevation (a per-user install under
; %LOCALAPPDATA%\Programs) either from the wizard or with /CURRENTUSER. The
; default remains an all-users machine-wide install under Program Files.
PrivilegesRequiredOverridesAllowed=dialog commandline
; Lowest supported OS: Windows 10 1809 (build 17763) — the floor for .NET 9.
MinVersion=10.0.17763

DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=installer
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-{#OutSuffix}
SetupIconFile={#DistDir}\app.ico
UninstallDisplayIcon={app}\{#MyQtExeName},0
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ChangesEnvironment=yes

[Files]
; Qt 6 front-end (self-contained: exe + Qt runtime DLLs + plugins\platforms\qwindows.dll
; + MSVC runtime). This is the main GUI.
Source: "{#DistDir}\EGtoolsFrontend\*"; DestDir: "{app}\EGtoolsFrontend"; Flags: ignoreversion recursesubdirs createallsubdirs
; CLI kernels (self-contained .NET 9). Installed as siblings so the Qt front-end
; finds them automatically, and usable standalone from any prompt.
Source: "{#DistDir}\EGpdf2excel\*"; DestDir: "{app}\EGpdf2excel"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#DistDir}\EGexcel2df\*"; DestDir: "{app}\EGexcel2df"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#DistDir}\EGpdf2df\*"; DestDir: "{app}\EGpdf2df"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs
; Documentation.
Source: "{#DistDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs
; App icon (installer / shortcut icon).
Source: "{#DistDir}\app.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\EGtoolsFrontend\{#MyQtExeName}"; WorkingDir: "{app}\EGtoolsFrontend"; IconFilename: "{app}\app.ico"
Name: "{group}\EGpdf2excel (CLI)"; Filename: "{app}\EGpdf2excel\EGpdf2excel.exe"; WorkingDir: "{app}\EGpdf2excel"; IconFilename: "{app}\app.ico"
Name: "{group}\EGexcel2df (CLI)"; Filename: "{app}\EGexcel2df\EGexcel2df.exe"; WorkingDir: "{app}\EGexcel2df"; IconFilename: "{app}\app.ico"
Name: "{group}\EGpdf2df (CLI)"; Filename: "{app}\EGpdf2df\EGpdf2df.exe"; WorkingDir: "{app}\EGpdf2df"; IconFilename: "{app}\app.ico"
Name: "{group}\使用文档 (中文)"; Filename: "{app}\docs\README_zh.md"
Name: "{group}\Documentation (English)"; Filename: "{app}\docs\README_en.md"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\EGtoolsFrontend\{#MyQtExeName}"; WorkingDir: "{app}\EGtoolsFrontend"; IconFilename: "{app}\app.ico"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
; Explicit, visible opt-in to add the three CLI tools to the system PATH.
; Hidden for per-user (non-elevated) installs: HKLM\...\Environment\Path cannot be
; written without administrator rights, so offering the option there would only
; produce a silently-skipped step.
Name: "addtopath"; Description: "将命令行工具 (EGpdf2excel / EGexcel2df / EGpdf2df) 添加到 PATH"; GroupDescription: "附加选项"; Flags: checkedonce; Check: IsAdminInstallMode

[Run]
; Launch the Qt front-end on finish (optional).
Filename: "{app}\EGtoolsFrontend\{#MyQtExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; WorkingDir: "{app}\EGtoolsFrontend"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\EGtoolsFrontend"
Type: filesandordirs; Name: "{app}\EGpdf2excel"
Type: filesandordirs; Name: "{app}\EGexcel2df"
Type: filesandordirs; Name: "{app}\EGpdf2df"
Type: filesandordirs; Name: "{app}\docs"

[Code]
// Append the three CLI directories to the SYSTEM PATH (HKLM) at install time and
// remove them at uninstall — ONLY when the user chose the "addtopath" task.
//
// The installer runs elevated (PrivilegesRequired=admin) and is a per-machine
// install under C:\Program Files. Writing to HKCU\Environment\Path while
// elevated would put the entries into the ADMINISTRATOR's user PATH so the real
// user never sees them. The system PATH (HKLM) is visible to every user, and
// ChangesEnvironment=yes broadcasts WM_SETTINGCHANGE so open consoles pick it up.
//
// The install dir is recorded in a private registry key so uninstall can
// reconstruct the CLI paths without relying on the {app} constant.

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

// Remove 'dir' from a semicolon-delimited path string, collapsing duplicates /
// stray separators so the value stays well-formed.
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

function GetInstallDir(): string;
var
  s: string;
begin
  if RegQueryStringValue(APP_ROOT, APP_KEY, APPDIR_VAL, s) then
    Result := s
  else
    Result := ExpandConstant('{app}');
end;

// Add/remove the three CLI dirs from the system PATH. Defined BEFORE the
// CurStep* handlers because Pascal Script requires declaration-before-use.
procedure ModifyPath(const add: boolean; const base: string);
begin
  if add then
  begin
    WriteSystemPath(AppendDir(ReadSystemPath(), base + '\EGpdf2excel'));
    WriteSystemPath(AppendDir(ReadSystemPath(), base + '\EGexcel2df'));
    WriteSystemPath(AppendDir(ReadSystemPath(), base + '\EGpdf2df'));
  end
  else
  begin
    WriteSystemPath(RemoveDir(ReadSystemPath(), base + '\EGpdf2excel'));
    WriteSystemPath(RemoveDir(ReadSystemPath(), base + '\EGexcel2df'));
    WriteSystemPath(RemoveDir(ReadSystemPath(), base + '\EGpdf2df'));
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  dir: string;
begin
  if CurStep = ssPostInstall then
  begin
    // A per-user (non-elevated) install cannot write HKLM. Only record the install
    // dir / touch the system PATH in an all-users install, and never let a failed
    // registry write abort the installation itself.
    if IsAdminInstallMode then
    begin
      dir := ExpandConstant('{app}');
      try
        RegWriteStringValue(APP_ROOT, APP_KEY, APPDIR_VAL, dir);
      except
      end;
      if WizardIsTaskSelected('addtopath') then
        ModifyPath(True, dir);
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  dir: string;
begin
  if CurUninstallStep = usUninstall then
  begin
    if IsAdminInstallMode then
    begin
      try
        dir := GetInstallDir();
        ModifyPath(False, dir);
        RegDeleteKeyIncludingSubkeys(APP_ROOT, APP_KEY);
      except
      end;
    end;
  end;
end;