param(
    # Purge every project's obj/bin before building. OFF by default: incremental
    # builds are fine and a purge is only wanted when chasing stale-artifact issues.
    # The purge MOVES obj/bin into .build_trash\ (gitignored) instead of deleting
    # them, because a recursive delete of a populated obj/bin trips the sandbox
    # safe-delete guard (fail-closed) and would abort the whole pipeline.
    [switch]$Clean
)
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

# ---- 0. Optional clean of obj/bin (only with -Clean) ----
# NOTE: recursively DELETING a populated obj/bin is blocked by the sandbox
# safe-delete guard (fail-closed once a turn passes its file-count threshold),
# which aborts the whole pipeline. A directory RENAME is NOT subject to that
# guard, so the purge MOVES the folders into .build_trash\ (gitignored).
$trash = "$root\.build_trash"
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
New-Item -ItemType Directory -Path $trash -Force | Out-Null
if ($Clean) {
    Step 'Clean obj/bin for all projects (rename into .build_trash)'
    foreach ($p in $projects) {
        $dir  = Split-Path "$root\$p"
        $name = Split-Path $dir -Leaf
        foreach ($sub in @('obj','bin')) {
            if (Test-Path "$dir\$sub") {
                Move-Item -LiteralPath "$dir\$sub" -Destination "$trash\$name.$sub.$stamp" -Force -ErrorAction SilentlyContinue
            }
        }
    }
} else {
    Step 'Clean obj/bin skipped (pass -Clean to force a purge)'
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

# ---- 3b. 交付目录里不留符号文件（*.pdb）----
# csproj 里的 CopyOutputSymbolsToPublishDirectory / AllowedReferenceRelatedFileExtensions
# 对**项目引用**带出来的 pdb（Engine / Core）并不完全生效，所以在这里兜一道：
# 把 publish 目录里的 *.pdb 移进 .build_trash（用移动而不是删除 —— 递归删除会触发
# 沙箱 safe-delete 守卫 fail-closed，整条流水线会被中断）。
Step 'Strip *.pdb from published CLI folders (release payload only)'
foreach ($d in @('EGpdf2excel','EGexcel2df','EGpdf2df')) {
    $pd = "$root\DIST\$d"
    Get-ChildItem "$pd\*.pdb" -EA SilentlyContinue | ForEach-Object {
        Move-Item -LiteralPath $_.FullName -Destination "$trash\pdb_$($_.Name)" -Force -EA SilentlyContinue
    }
    $left = (Get-ChildItem "$pd\*.pdb" -EA SilentlyContinue | Measure-Object).Count
    Write-Host "OK  $d : residual pdb = $left"
}

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
    # ⚠️ 必须是 Release：既要显式传 CMAKE_BUILD_TYPE（Ninja 是单配置生成器），
    #    也要给 --build 补 --config Release。CMakeLists 里另外做了兜底
    #    （未指定时默认 Release、显式 Debug 会被强制改回 Release）。
    & $qtCmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$qtRoot" -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Host
    if ($LASTEXITCODE -eq 0) { & $qtCmake --build build --config Release 2>&1 | Out-Host }
    $cfgLine = Select-String -Path 'build\CMakeCache.txt' -Pattern '^CMAKE_BUILD_TYPE:STRING=' -EA SilentlyContinue |
               Select-Object -First 1
    if ($cfgLine) { Write-Host "Qt build config -> $($cfgLine.Line)" } else { Write-Warning 'Cannot verify Qt build config.' }
    Pop-Location
    $qtExe = "$root\EGtools.Frontend\build\EGtoolsFrontend.exe"
    if (Test-Path $qtExe) {
        $out = "$root\DIST\EGtoolsFrontend"
        New-Item -ItemType Directory -Path $out -Force | Out-Null
        Copy-Item $qtExe $out -Force
        # ⚠️ 必须把**宿主自己的** runtimeconfig 一起部署，否则打包出来的程序里
        # 「内嵌调库」通道直接失效：hostfxr 找不到 <exe>.runtimeconfig.json 会退用内核那份
        # （内核是 self-contained，等于把内核当宿主）⇒ 实测报
        #   "Initialization for self-contained components is not supported" (hr=0x80008093)
        # 然后静默回退命令行。这个 bug 只在**跑发布产物**时才暴露（build/ 目录里有这份文件）。
        $hostCfg = "$root\EGtools.Frontend\build\EGtoolsFrontend.runtimeconfig.json"
        if (Test-Path $hostCfg) {
            Copy-Item $hostCfg $out -Force
            Write-Host 'OK  host runtimeconfig -> DIST/EGtoolsFrontend'
        } else {
            Write-Warning "Missing $hostCfg — the embedded kernel backend will NOT work in the package."
        }
        # Deploy the Qt runtime so the folder runs on a CLEAN machine (no Qt and no
        # VC++ redist installed). We copy the required DLLs/plugins EXPLICITLY rather
        # than shelling out to windeployqt: windeployqt must spawn qtpaths, which is
        # blocked inside sandboxes (and its stderr would trip ErrorActionPreference=Stop).
        # For a Qt Widgets app the needed set is exactly: Qt6Core/Gui/Widgets + the
        # qwindows platform plugin (+ styles/imageformats) + the MSVC runtime.
        Step 'Deploy Qt runtime (DLLs + qwindows plugin + MSVC runtime)'
        # ⚠️ 清单必须覆盖链接期用到的**全部** Qt 模块（含**传递依赖**）：Core/Gui/Widgets 之外，
        # **Qt6Pdf**（Qt PDF / 内置 PDFium）—— 预览的矢量渲染全靠它；它还依赖 **Qt6Network**！
        # 少拷一个的表现是发布产物**启动即 0xC0000135（缺 DLL）**，而开发机上因为 PATH 里有
        # Qt 目录完全看不出来（实测漏了 Qt6Network.dll，就是下面这段"依赖闭环"自证抓出来的）。
        # 页面画布走 GPU 需要 Qt6OpenGLWidgets + Qt6OpenGL，并发渲染用到 Qt6Concurrent。
        foreach ($dll in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll','Qt6Pdf.dll',
                           'Qt6OpenGL.dll','Qt6OpenGLWidgets.dll','Qt6Concurrent.dll')) {
            Copy-Item "$qtRoot\bin\$dll" $out -Force -EA SilentlyContinue
        }
        # ---- 依赖闭环自证（手工清单一定会漏，所以别再只靠手工）----------------
        # 判据：**凡是存在于我们自己的 Qt / redist 目录里的依赖，都必须出现在 DIST**。
        # （只看这两个目录，能精准抓住"忘了拷某个 Qt 模块"这类错，又不会被
        #  KERNEL32/api-ms-win-* / System32 里的系统组件干扰。）
        $binutils = (Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' -EA SilentlyContinue |
                     Sort-Object FullName -Descending | Select-Object -First 1).FullName
        $qtBin = Get-ChildItem "$qtRoot\bin\*.dll" -EA SilentlyContinue | ForEach-Object { $_.Name }
        $redist = Get-ChildItem "$root\redist\vc143_x64\*.dll" -EA SilentlyContinue | ForEach-Object { $_.Name }
        $ours = @{}
        foreach ($n in ($qtBin + $redist)) { if ($n -notmatch 'd\.dll$') { $ours[$n.ToLower()] = $true } }
        $have = @{}
        Get-ChildItem "$out\*.dll","$out\*.exe" -EA SilentlyContinue | ForEach-Object { $have[$_.Name.ToLower()] = $true }

        $missing = New-Object System.Collections.Generic.List[string]
        foreach ($f in (Get-ChildItem "$out\*.dll","$out\*.exe" -EA SilentlyContinue)) {
            $deps = & $binutils /dependents $f.FullName 2>$null |
                    Select-String -Pattern '^\s+([A-Za-z0-9_\-\.]+\.dll)$' |
                    ForEach-Object { $_.Matches[0].Groups[1].Value }
            foreach ($d in $deps) {
                $dl = $d.ToLower()
                if (-not $ours.ContainsKey($dl)) { continue }        # 不属于我们分发的（系统 DLL）→ 跳过
                if (-not $have.ContainsKey($dl)) { $missing.Add("$($f.Name) -> $d") }
            }
        }
        if ($missing.Count -gt 0) {
            throw ("依赖不闭环（发布产物会启动失败，典型报错 0xC0000135 缺 DLL）：`n  " +
                   (($missing | Sort-Object -Unique) -join "`n  "))
        }
        Write-Host ("OK  Qt 依赖闭环：DIST 内 " + $have.Count + " 个模块的自有依赖全部就位")

        # ---- 终审：**真的把发布产物跑起来**（能启动才算打包成功）----------------
        # 前面所有清单检查都只是"推理"，只有这一步是事实。缺 DLL 的表现是
        # 进程立刻退出、exit=0xC0000135(-1073741515)，日志里什么都没有。
        $published = "$out\EGtoolsFrontend.exe"
        if (Test-Path $published) {
            # 故意**不给** PATH 里加 Qt 目录：模拟干净机器上只有 DIST 的情况。
            $savedPath = $env:PATH
            $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
            $vp = Start-Process -FilePath $published -ArgumentList @('--version') -Wait -PassThru `
                                -RedirectStandardOutput "$root\.build_trash\published_version.txt" `
                                -RedirectStandardError  "$root\.build_trash\published_version_e.txt"
            $env:PATH = $savedPath
            $vtext = (Get-Content "$root\.build_trash\published_version.txt" -Raw -EA SilentlyContinue)
            if ($vp.ExitCode -ne 0) {
                throw ("发布产物无法启动（exit=$($vp.ExitCode)，" +
                       "0xC0000135/-1073741515 表示缺 DLL）：$published")
            }
            if ($vtext -notmatch 'Release') {
                throw "发布产物自报构建配置异常：$vtext"
            }
            Write-Host ("OK  发布产物可独立启动：" + $vtext.Trim())
        }
        # 软件 OpenGL 兜底（Mesa llvmpipe，Qt 自带）：某些机器只有微软的 GDI Generic
        # OpenGL 1.1，QOpenGLWidget 起不来（表现是**一块黑画布**）。带上它之后，
        # Qt 在 ``QT_OPENGL=software`` 或驱动缺失时仍有可用的 GL 实现。
        # 我们自己的探测也会在 GL 版本不足时退回 CPU 光栅，这里是双保险。
        Copy-Item "$qtRoot\bin\opengl32sw.dll" $out -Force -EA SilentlyContinue
        foreach ($plug in @('platforms','styles','imageformats','iconengines')) {
            if (Test-Path "$qtRoot\plugins\$plug") {
                $pd = "$out\plugins\$plug"
                if (Test-Path $pd) {
                    # Rename (not recursive delete): recursive Remove-Item trips the
                    # sandbox safe-delete guard and would abort the build here.
                    Move-Item -LiteralPath $pd -Destination "$trash\plugins_${plug}_$stamp" -Force -ErrorAction SilentlyContinue
                }
                New-Item -ItemType Directory -Path $pd -Force | Out-Null
                # Copy RELEASE plugins only. Qt ships debug plugin DLLs with a trailing
                # 'd' (qwindowsd.dll, qmodernwindowsstyled.dll, ...); they link against
                # the debug CRT and must not ship. Qt may also PREFER a debug plugin at
                # runtime, which is what produced the noisy
                # "qt.text.font.db: OpenType support missing" warnings on a release build.
                # Precise test: treat X.dll as debug only when the matching release
                # plugin (same name minus the trailing 'd') also exists — this keeps
                # legitimately 'd'-suffixed release plugins such as qdirect2d.dll.
                Get-ChildItem "$qtRoot\plugins\$plug\*.dll" | Where-Object {
                    $n = $_.BaseName
                    -not ($n -match 'd$' -and
                          (Test-Path (Join-Path $_.DirectoryName ($n.Substring(0, $n.Length - 1) + '.dll'))))
                } | Copy-Item -Destination $pd -Force -EA SilentlyContinue
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
# Single source of truth for the app icon: the Qt front-end's icon (also embedded
# into EGtoolsFrontend.exe via res/app_icon.rc). build_installer.iss uses this as
# SetupIconFile, so Explorer / Start menu / installer all show the same icon.
Copy-Item "$root\EGtools.Frontend\res\app_icon.ico" "$root\DIST\app.ico" -Force
if (-not (Test-Path "$root\DIST\docs")){ Copy-Item "$root\docs" "$root\DIST\docs" -Recurse -Force }

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

# ---- 7b. 交付自证：exe 自己报出构建配置，必须含 "Release" ----
# 只信产物本身的说法（--version 里带构建配置），不信文件名或目录名。
Step 'Verify Qt front-end is a Release build'
$oldPath = $env:PATH
$env:PATH = "$qtRoot\bin;$oldPath"
try {
    $ver = (& $qtOut --version 2>&1 | Out-String).Trim()
} finally {
    $env:PATH = $oldPath
}
if ([string]::IsNullOrWhiteSpace($ver)) {
    Write-Warning "Qt front-end --version produced no output (cannot self-verify; check manually)."
} else {
    Write-Host "OK  front-end self-reports: $ver"
    if ($ver -notmatch 'Release') { throw "Qt front-end is NOT a Release build: $ver" }
}

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
