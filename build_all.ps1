$ErrorActionPreference = 'Stop'
# Use the script's own directory as the repo root so the build works from any clone location.
$root = $PSScriptRoot
# 'dotnet' is expected on PATH (e.g. via the .NET SDK installer). Override here if needed.
$dotnet = 'dotnet'

function Step($msg){ Write-Host "`n===== $msg =====" -ForegroundColor Cyan }

# ---------------------------------------------------------------------------
# Deployment model (.NET 9, self-contained WinAppSDK, 2026-08-10):
#   * GUI (EGtools.exe, WinUI 3): .NET 9 RUNTIME + Windows App Runtime are BOTH
#     self-contained (FolderProfile: SelfContained=true, WindowsAppSDKSelfContained=true,
#     WindowsAppSdkUndockedRegFreeWinRTInitialize=true). The app carries its own
#     exact WinAppSDK runtime (reg-free, local) and does NOT depend on any
#     externally-registered Windows App Runtime. This avoids the 0xC000027B native
#     assertion crash in Microsoft.UI.Xaml.dll that hits published unpackaged
#     WinUI 3 apps which resolve an external runtime at startup.
#   * CLIs: fully self-contained, independently runnable.
#
# NOTE on restore: the IMPLICIT restore done by build/publish silently hangs in
# this toolchain, and the `-c`/`-r` switch form triggers MSB1001. We therefore
# restore EXPLICITLY with the `/p:Configuration`/`/p:RuntimeIdentifier` form and
# build/publish with --no-restore.
#
# NOTE on obj: stale obj subfolders from prior .NET 10 / multi-TFM builds cause
# CS0579 (duplicate AssemblyInfo). We purge obj/bin for ALL projects first.
# ---------------------------------------------------------------------------

$projects = @(
    'EGtools.Core\EGtools.Core.csproj',
    'EGpdf2excel\EGpdf2excel.csproj',
    'EGexcel2df\EGexcel2df.csproj',
    'EGtools.Gui\EGtools.Gui.csproj'
)

# ---- 0. Clean obj/bin for every project (purge stale TFM subfolders) ----
Step 'Clean obj/bin for all projects'
foreach ($p in $projects) {
    $dir = Split-Path "$root\$p"
    Remove-Item -Recurse -Force "$dir\obj","$dir\bin" -ErrorAction SilentlyContinue
}

# ---- 1. Explicit restore for every project (/p: form) ----
Step 'Restore all projects (explicit, /p: form)'
foreach ($p in $projects) {
    & $dotnet restore "$root\$p" /p:Configuration=Release /p:RuntimeIdentifier=win-x64
    if ($LASTEXITCODE -ne 0) { throw "Restore failed: $p" }
}

# ---- 2. Build Core (reference engine) ----
Step 'Build EGtools.Core'
& $dotnet build "$root\EGtools.Core\EGtools.Core.csproj" /p:Configuration=Release /p:RuntimeIdentifier=win-x64 --no-restore
if ($LASTEXITCODE -ne 0) { throw "Core build failed" }

# ---- 3. Publish CLIs (fully self-contained) ----
Step 'Publish EGpdf2excel'
& $dotnet publish "$root\EGpdf2excel\EGpdf2excel.csproj" /p:Configuration=Release /p:RuntimeIdentifier=win-x64 /p:SelfContained=true /p:PublishTrimmed=false --no-restore -o "$root\DIST\EGpdf2excel"
if ($LASTEXITCODE -ne 0) { throw "EGpdf2excel publish failed" }

Step 'Publish EGexcel2df'
& $dotnet publish "$root\EGexcel2df\EGexcel2df.csproj" /p:Configuration=Release /p:RuntimeIdentifier=win-x64 /p:SelfContained=true /p:PublishTrimmed=false --no-restore -o "$root\DIST\EGexcel2df"
if ($LASTEXITCODE -ne 0) { throw "EGexcel2df publish failed" }

# ---- 4. Publish GUI (WinUI 3, self-contained .NET + self-contained WinAppSDK) ----
Step 'Publish EGtools.Gui (FolderProfile: SelfContained + WindowsAppSDKSelfContained)'
& $dotnet publish "$root\EGtools.Gui\EGtools.Gui.csproj" /p:Configuration=Release /p:RuntimeIdentifier=win-x64 /p:PublishProfile=FolderProfile --no-restore
if ($LASTEXITCODE -ne 0) { throw "GUI publish failed" }

# ---- 5. Ensure VC++ 2022 runtime DLLs in every app dir ----
Step 'Copy VC++ 2022 (v14) runtime into each app dir'
$vc = "$root\redist\vc143_x64"
foreach ($d in @("$root\DIST\EGtools", "$root\DIST\EGpdf2excel", "$root\DIST\EGexcel2df")) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
    Copy-Item -Path "$vc\*" -Destination $d -Force
}

# ---- 6. Docs + icon ----
if (-not (Test-Path "$root\DIST\app.ico")) { Copy-Item "$root\EGtools.Gui\app.ico" "$root\DIST\app.ico" -Force }
if (-not (Test-Path "$root\DIST\docs"))    { Copy-Item "$root\docs" "$root\DIST\docs" -Recurse -Force }

# ---- 7. Verify exes exist ----
Step 'Verify DIST layout'
foreach ($d in @('EGtools','EGpdf2excel','EGexcel2df')) {
    $exe = "$root\DIST\$d\$d.exe"
    if (-not (Test-Path $exe)) { throw "Missing $exe" }
    Write-Host "OK  $exe  ($(Get-Item $exe).Length) bytes"
}

# ---- 8. Recompile installer from fresh DIST ----
Step 'Compile installer (ISCC)'
$iscc = 'C:\Users\QBZ95\AppData\Local\Programs\Inno Setup 6\ISCC.exe'
& $iscc "$root\build_installer.iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
$out = "$root\installer\EGtools-3.0.0-x64.exe"
if (-not (Test-Path $out)) { throw "Installer not produced" }
Start-Sleep -Seconds 2
$size = (Get-Item $out).Length
Write-Host "Installer size: $size bytes"
if ($size -lt 40MB) { throw "Installer too small ($size) — payload missing!" }

Write-Host "`nALL DONE" -ForegroundColor Green
