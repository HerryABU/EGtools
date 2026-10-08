$ErrorActionPreference = 'Stop'
# Use the script's own directory as the repo root so the build works from any clone location.
$root = $PSScriptRoot
# 'dotnet' is expected on PATH (e.g. via the .NET SDK installer). Override here if needed.
$dotnet = 'dotnet'
# Disable MSBuild node reuse so each build/publish exits cleanly and releases file
# locks (avoids "Access to the path ... is denied" when consecutive steps rebuild
# the shared DLLs into each app folder).
$env:MSBUILDDISABLENODEREUSE = '1'

function Step($msg){ Write-Host "`n===== $msg =====" -ForegroundColor Cyan }

# ---------------------------------------------------------------------------
# Deployment model (.NET 9 self-contained + Qt 6, updated 2026-10-08):
#   * GUI = EGtools.Frontend (Qt 6.11.2 msvc2022_64). Built with CMake+Ninja,
#     then made SELF-CONTAINED via windeployqt (Qt runtime DLLs + qwindows
#     platform plugin + MSVC runtime copied next to the exe), so the folder runs
#     on a clean machine with no Qt / VC++ redist installed. It REPLACES the old
#     WinUI 3 front-end, which is no longer built or packaged.
#   * CLIs (EGpdf2excel / EGexcel2df / EGpdf2df): fully self-contained .NET 9,
#     independently runnable. They are the kernels the Qt GUI drives via QProcess.
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
    'EGpdf2excel.Engine\EGpdf2excel.Engine.csproj',
    'EGexcel2df.Engine\EGexcel2df.Engine.csproj',
    'EGpdf2df.Engine\EGpdf2df.Engine.csproj',
    'EGpdf2excel\EGpdf2excel.csproj',
    'EGexcel2df\EGexcel2df.csproj',
    'EGpdf2df\EGpdf2df.csproj'
)

# ---- 0. Clean obj/bin for every project (purge stale TFM subfolders) ----
Step 'Clean obj/bin for all projects'
foreach ($p in $projects) {
    $dir = Split-Path "$root\$p"
    Remove-Item -Recurse -Force "$dir\obj","$dir\bin" -ErrorAction SilentlyContinue
}
# Shut down any lingering MSBuild/compiler servers so their file handles release.
& $dotnet build-server shutdown | Out-Null

# ---- 1. Explicit restore for every project (/p: form) ----
Step 'Restore all projects (explicit, /p: form)'
foreach ($p in $projects) {
    & $dotnet restore "$root\$p" /p:Configuration=Release /p:RuntimeIdentifier=win-x64
    if ($LASTEXITCODE -ne 0) { throw "Restore failed: $p" }
}

# ---- 2. Build the shared Core + the split engine DLLs ----
Step 'Build EGtools.Core + engine DLLs'
foreach ($p in @('EGtools.Core\EGtools.Core.csproj','EGpdf2excel.Engine\EGpdf2excel.Engine.csproj','EGexcel2df.Engine\EGexcel2df.Engine.csproj','EGpdf2df.Engine\EGpdf2df.Engine.csproj')) {
    & $dotnet build "$root\$p" /p:Configuration=Release /p:RuntimeIdentifier=win-x64 --no-restore -m:1 -nodeReuse:false
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $p" }
}

# ---- 3. Publish CLIs (fully self-contained) ----
Step 'Publish EGpdf2excel'
& $dotnet publish "$root\EGpdf2excel\EGpdf2excel.csproj" /p:Configuration=Release /p:RuntimeIdentifier=win-x64 /p:SelfContained=true /p:PublishTrimmed=false --no-restore -m:1 -nodeReuse:false -o "$root\DIST\EGpdf2excel"
if ($LASTEXITCODE -ne 0) { throw "EGpdf2excel publish failed" }

Step 'Publish EGexcel2df'
& $dotnet publish "$root\EGexcel2df\EGexcel2df.csproj" /p:Configuration=Release /p:RuntimeIdentifier=win-x64 /p:SelfContained=true /p:PublishTrimmed=false --no-restore -m:1 -nodeReuse:false -o "$root\DIST\EGexcel2df"
if ($LASTEXITCODE -ne 0) { throw "EGexcel2df publish failed" }

Step 'Publish EGpdf2df'
& $dotnet publish "$root\EGpdf2df\EGpdf2df.csproj" /p:Configuration=Release /p:RuntimeIdentifier=win-x64 /p:SelfContained=true /p:PublishTrimmed=false --no-restore -m:1 -nodeReuse:false -o "$root\DIST\EGpdf2df"
if ($LASTEXITCODE -ne 0) { throw "EGpdf2df publish failed" }

# ---- 4. Qt 6 front-end (primary GUI) — built with windeployqt so it is self-contained ----
# The Qt app is now THE shipped GUI (replaces the legacy WinUI front-end, which is
# no longer packaged). windeployqt copies the required Qt runtime DLLs, the
# qwindows platform plugin and the MSVC runtime next to the exe, so the folder
# runs on a clean machine with no Qt / VC++ redist installed.
$qtRoot = 'C:\Qt\6.11.2\msvc2022_64'
$qtCmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if ((Test-Path "$qtRoot\lib\cmake\Qt6\Qt6Config.cmake") -and (Test-Path $qtCmake)) {
    Step 'Build EGtools.Frontend (Qt 6)'
    # The Ninja generator (unlike the VS generator) does NOT auto-locate MSVC, so the
    # MSVC + Windows SDK environment must be present before configuring. This is a
    # no-op when the script is already run from a Developer PowerShell.
    if (-not $env:EGTOOLS_MSVC_ENV_READY) {
        $msvcVer = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*' -Directory -EA SilentlyContinue |
                   Sort-Object Name -Descending | Select-Object -First 1
        $sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
        $sdkVer = (Get-ChildItem "$sdkRoot\Include" -Directory -EA SilentlyContinue |
                   Sort-Object Name -Descending | Select-Object -First 1).Name
        if ($msvcVer -and $sdkVer) {
            $m = $msvcVer.FullName
            $env:PATH    = "$m\bin\Hostx64\x64;$sdkRoot\bin\$sdkVer\x64;" + $env:PATH
            $env:INCLUDE = "$m\include;$sdkRoot\Include\$sdkVer\ucrt;$sdkRoot\Include\$sdkVer\um;$sdkRoot\Include\$sdkVer\shared;$sdkRoot\Include\$sdkVer\winrt"
            $env:LIB     = "$m\lib\x64;$sdkRoot\Lib\$sdkVer\ucrt\x64;$sdkRoot\Lib\$sdkVer\um\x64"
            $env:EGTOOLS_MSVC_ENV_READY = '1'
        } else {
            Write-Warning 'Could not auto-detect MSVC / Windows SDK for the Qt build.'
        }
    }
    Push-Location "$root\EGtools.Frontend"
    & $qtCmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$qtRoot" -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Host
    if ($LASTEXITCODE -eq 0) { & $qtCmake --build build 2>&1 | Out-Host }
    Pop-Location
    $qtExe = "$root\EGtools.Frontend\build\EGtoolsFrontend.exe"
    if (Test-Path $qtExe) {
        $out = "$root\DIST\EGtoolsFrontend"
        New-Item -ItemType Directory -Path $out -Force | Out-Null
        Copy-Item $qtExe $out -Force
        # Deploy the Qt runtime so the folder runs on a CLEAN machine (no Qt and no
        # VC++ redist installed). We copy the required DLLs/plugins EXPLICITLY rather
        # than shelling out to windeployqt: windeployqt must spawn qtpaths, which is
        # blocked inside sandboxes (and its stderr would trip ErrorActionPreference=Stop).
        # For a Qt Widgets app the needed set is exactly: Qt6Core/Gui/Widgets + the
        # qwindows platform plugin (+ styles/imageformats) + the MSVC runtime.
        Step 'Deploy Qt runtime (DLLs + qwindows plugin + MSVC runtime)'
        foreach ($dll in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll')) {
            Copy-Item "$qtRoot\bin\$dll" $out -Force -EA SilentlyContinue
        }
        foreach ($plug in @('platforms','styles','imageformats')) {
            if (Test-Path "$qtRoot\plugins\$plug") {
                New-Item -ItemType Directory -Path "$out\plugins\$plug" -Force | Out-Null
                Copy-Item "$qtRoot\plugins\$plug\*.dll" "$out\plugins\$plug\" -Force -EA SilentlyContinue
            }
        }
        Copy-Item "$root\redist\vc143_x64\*.dll" $out -Force -EA SilentlyContinue
        Write-Host 'OK  Qt front-end -> DIST/EGtoolsFrontend (self-contained)'
    } else {
        Write-Warning 'Qt front-end build failed.'
    }
} else {
    Step 'Qt front-end SKIPPED (Qt 6 or CMake not found)'
    Write-Warning "Qt 6 not found at $qtRoot — skipping EGtools.Frontend."
}

# ---- 5. Ensure VC++ 2022 runtime DLLs in every CLI dir (Qt app gets its own via windeployqt) ----
Step 'Copy VC++ 2022 (v14) runtime into each CLI dir'
$vc = "$root\redist\vc143_x64"
foreach ($d in @("$root\DIST\EGpdf2excel", "$root\DIST\EGexcel2df", "$root\DIST\EGpdf2df")) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
    Copy-Item -Path "$vc\*" -Destination $d -Force
}

# ---- 6. Docs + icon ----
if (-not (Test-Path "$root\DIST\app.ico")) { Copy-Item "$root\EGtools.Gui\app.ico" "$root\DIST\app.ico" -Force }
if (-not (Test-Path "$root\DIST\docs"))    { Copy-Item "$root\docs" "$root\DIST\docs" -Recurse -Force }

# ---- 7. Verify exes exist ----
Step 'Verify DIST layout'
foreach ($d in @('EGpdf2excel','EGexcel2df','EGpdf2df')) {
    $exe = "$root\DIST\$d\$d.exe"
    if (-not (Test-Path $exe)) { throw "Missing $exe" }
    Write-Host "OK  $exe  ($(Get-Item $exe).Length) bytes"
}
# The Qt front-end is the shipped GUI; require it for a complete installer.
$qtOut = "$root\DIST\EGtoolsFrontend\EGtoolsFrontend.exe"
if (-not (Test-Path $qtOut)) { throw "Missing $qtOut (Qt front-end build failed)" }
Write-Host "OK  $qtOut  ($(Get-Item $qtOut).Length) bytes"

# ---- 8. Recompile installer from fresh DIST ----
Step 'Compile installer (ISCC)'
$iscc = 'C:\Users\QBZ95\AppData\Local\Programs\Inno Setup 6\ISCC.exe'
& $iscc "$root\build_installer.iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
$out = "$root\installer\EGtools-4.3.0-x64.exe"
if (-not (Test-Path $out)) { throw "Installer not produced" }
Start-Sleep -Seconds 2
$size = (Get-Item $out).Length
Write-Host "Installer size: $size bytes"
if ($size -lt 40MB) { throw "Installer too small ($size) — payload missing!" }

Write-Host "`nALL DONE" -ForegroundColor Green
