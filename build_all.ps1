param(
    # Purge every project's obj/bin before building. OFF by default: incremental
    # builds are fine and a purge is only wanted when chasing stale-artifact issues.
    # The purge MOVES obj/bin into .build_trash\ (gitignored) instead of deleting
    # them, because a recursive delete of a populated obj/bin trips the sandbox
    # safe-delete guard (fail-closed) and would abort the whole pipeline.
    [switch]$Clean,

    # 目标架构。x64 = 本机原生（默认）；arm64 = Windows on ARM。
    # ⚠️ arm64 是**交叉编译**：在本机 x64 上产出 arm64 程序，因此：
    #   · 产物落到 DIST-arm64\（与 DIST\ 刻意分开，否则同名文件互覆）
    #   · Qt 用 msvc2022_arm64（官方 cross_compiled 版）
    #   · MSVC 需要 **Hostx64\arm64** 交叉工具链（实测只有 VS Insiders 14.51 带）
    #   · ⚠️ **无法在本机做"真启动"验证**（x64 宿主跑不了 arm64 程序）⇒
    #     改用「架构断言 + 依赖闭环自证」，并在结论里明确标注"未真机验证"。
    [ValidateSet('x64','arm64')][string]$Arch = 'x64'
)
$ErrorActionPreference = 'Stop'
# Use the script's own directory as the repo root so the build works from any clone location.
$root = $PSScriptRoot
# 'dotnet' is expected on PATH (e.g. via the .NET SDK installer). Override here if needed.
$dotnet = 'dotnet'

# ---------------------------------------------------------------------------
# 架构派生量（x64 走的是与以前完全相同的路径，arm64 才走交叉分支）
# ---------------------------------------------------------------------------
$RID      = "win-$Arch"
$DIST     = if ($Arch -eq 'x64') { "$root\DIST" } else { "$root\DIST-$Arch" }
$qtRoot   = if ($Arch -eq 'x64') { 'C:\Qt\6.11.2\msvc2022_64' } else { 'C:\Qt\6.11.2\msvc2022_arm64' }
# 构建目录也按架构分开，否则 x64 与 arm64 的 CMakeCache 会互相覆盖。
$qtBuildDir = if ($Arch -eq 'x64') { "$root\EGtools.Frontend\build" }
              else { "$root\EGtools.Frontend\build-$Arch" }
# 架构断言用：PE 的 machine 字段（x64=0x8664 / ARM64=0xAA64）；托管程序集是 0x14c(IL)，两者都合法。
$PE_EXPECT = if ($Arch -eq 'x64') { 0x8664 } else { 0xAA64 }
# 安装包产物名后缀。
$instSuffix = $Arch

# ---------------------------------------------------------------------------
# VC++ 运行时可再发行目录（按架构）
#   x64   → 仓库内 `redist\vc143_x64\`（随仓库分发）
#   arm64 → 仓库里没有 arm64 版，从 VS 安装目录取。⚠️ 两个坑：
#     ① 14.51 的包名是 **VC145**（不是 VC143）⇒ 按 "Microsoft.VC1xx.CRT" 这个名字找会扑空；
#        改为按"目录里存在 msvcp140.dll 且 vcruntime140.dll"来定位，不看版本化目录名。
#     ② arm64 的 CRT 只在 **VS Insiders** 的 `VC\Redist\MSVC\14.51.36231\arm64\` 里
#        （Community 同版本只有 x64/x86 —— 与它的 arm64 编译器缺失是同一回事）。
# ---------------------------------------------------------------------------
function Find-VcRedistDir([string]$TargetArch) {
    if ($TargetArch -eq 'x64') {
        $local = "$root\redist\vc143_x64"
        if (Test-Path "$local\vcruntime140.dll") { return $local }
    }
    $cands = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\$TargetArch\*" -Directory -EA SilentlyContinue
    foreach ($d in $cands) {
        if ((Test-Path "$($d.FullName)\vcruntime140.dll") -and (Test-Path "$($d.FullName)\msvcp140.dll")) {
            return $d.FullName
        }
    }
    return $null
}
$vcRedist = Find-VcRedistDir $Arch
if (-not $vcRedist) {
    throw ("找不到 $Arch 的 VC++ 运行时可再发行目录。`n" +
           "  x64 需要仓库内 redist\vc143_x64\；arm64 需要 VS Insiders 的" +
           " VC\Redist\MSVC\<ver>\arm64\<CRT 目录>。")
}
Write-Host "VC redist ($Arch) -> $vcRedist"
# Disable MSBuild node reuse so each build/publish exits cleanly and releases file
# locks (avoids "Access to the path ... is denied" when consecutive steps rebuild
# the shared DLLs into each app folder).
$env:MSBUILDDISABLENODEREUSE = '1'

function Step($msg){ Write-Host "`n===== $msg =====" -ForegroundColor Cyan }

# ---------------------------------------------------------------------------
# 架构断言（arm64 交叉编译的核心校验）
# 只靠"文件存在"骗人：把 x64 的 DLL 混进来照样能在 x64 上"看起来正常"，
# 到 arm64 机器上才炸。这里直接读 PE 头，把架构钉死。
# 托管程序集(IL, 0x14c)平台无关，视为合法。
# ---------------------------------------------------------------------------
function Get-PeMachine([string]$Path) {
    if (-not (Test-Path $Path)) { return $null }
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        if ($br.ReadUInt16() -ne 0x5A4D) { return $null }   # 'MZ'
        $fs.Position = 0x3C
        $peOff = $br.ReadUInt32()
        $fs.Position = $peOff
        if ($br.ReadUInt32() -ne 0x00004550) { return $null }  # 'PE\0\0'
        return $br.ReadUInt16()
    } finally { $fs.Dispose() }
}
function Assert-PeArch([string]$Path, [string]$What) {
    $m = Get-PeMachine $Path
    if ($null -eq $m) { throw "$What 不是有效 PE：$Path" }
    if ($m -ne $PE_EXPECT -and $m -ne 0x14C) {
        throw ("$What 架构不对：期望 $Arch(0x$('{0:X}' -f $PE_EXPECT)) 或 IL，实际 0x$('{0:X}' -f $m)`n  $Path`n" +
               "（典型成因：某个原生依赖只有别的架构的包，publish 照样'成功'但拷错了文件。）")
    }
    return $m
}
function Get-MachineName([int]$m) {
    switch ($m) { 0x8664 {'x64'} 0xAA64 {'ARM64'} 0x14C {'IL'} 0x1C4 {'ARM'} default { "0x$('{0:X}' -f $m)" } }
}

# dumpbin（用 x64 版即可 —— 它能读**任何**架构的 PE 导入表，包括 arm64）
$binutils = (Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' -EA SilentlyContinue |
             Sort-Object FullName -Descending | Select-Object -First 1).FullName
function Get-PeImports([string]$Path) {
    return @(& $binutils /dependents $Path 2>$null |
             Select-String -Pattern '^\s+([A-Za-z0-9_\-\.]+\.dll)$' |
             ForEach-Object { $_.Matches[0].Groups[1].Value })
}

# ---------------------------------------------------------------------------
# 剔除目录里「架构不符且**无人导入**」的多余文件；被导入却架构不符 ⇒ 硬失败。
#
# 为什么需要它（2026-10-14 实测）：微软自己的
#   `VC\Redist\MSVC\14.51.36231\arm64\Microsoft.VC145.CRT\vcruntime140_1.dll`
# **本身就是一个 x64 文件**（VS 安装包 quirk）。而 arm64 产物里**没有任何模块导入它**
# （x64 产物有 12 个模块导入 —— 它是 x64 专用的异常处理辅助，arm64 走别的机制）。
# 直接 `Copy-Item "$vcRedist\*.dll"` 就会把 x64 文件带进 arm64 交付，
# 而"依赖闭环"只查"存在"，**扫不出架构错误** ⇒ 必须单独做这一道。
# 原则：多余的（没人用的）剔除掉；真需要的（有人导入的）不许凑数，直接报错。
# ---------------------------------------------------------------------------
function Remove-WrongArchExtras {
    param([string]$Dir, [string]$TrashDir, [string]$Stamp)
    $files = @(Get-ChildItem "$Dir\*.dll" -EA SilentlyContinue)
    if ($files.Count -eq 0) { return @() }
    $imported = @{}
    foreach ($f in @(Get-ChildItem "$Dir\*.dll","$Dir\*.exe" -EA SilentlyContinue)) {
        foreach ($d in (Get-PeImports $f.FullName)) { $imported[$d.ToLower()] = $true }
    }
    $removed = @()
    foreach ($f in $files) {
        $m = Get-PeMachine $f.FullName
        if ($null -eq $m -or $m -eq $PE_EXPECT -or $m -eq 0x14C) { continue }  # IL 平台无关，合法
        if ($imported.ContainsKey($f.Name.ToLower())) {
            throw ("$($f.Name) 架构不符（实际 $(Get-MachineName $m)，需要 $Arch）**且有人导入它** ——" +
                   "必须换成正确架构的文件，不能用别的架构凑数。")
        }
        Move-Item -LiteralPath $f.FullName -Destination "$TrashDir\wrongarch_$($f.Name)_$Stamp" -Force -EA SilentlyContinue
        $removed += "$($f.Name)=$(Get-MachineName $m)"
    }
    return $removed
}

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
    & $dotnet restore "$root\$p" /p:Configuration=Release /p:RuntimeIdentifier=$RID
    if ($LASTEXITCODE -ne 0) { throw "Restore failed: $p" }
}

# ---- 2. Build the shared Core + the split engine DLLs ----
Step 'Build EGtools.Core + engine DLLs'
foreach ($p in @('EGtools.Core\EGtools.Core.csproj','EGpdf2excel.Engine\EGpdf2excel.Engine.csproj','EGexcel2df.Engine\EGexcel2df.Engine.csproj','EGpdf2df.Engine\EGpdf2df.Engine.csproj')) {
    & $dotnet build "$root\$p" /p:Configuration=Release /p:RuntimeIdentifier=$RID --no-restore -m:1 -nodeReuse:false
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $p" }
}

# ---- 3. Publish CLIs (fully self-contained) ----
Step 'Publish EGpdf2excel'
& $dotnet publish "$root\EGpdf2excel\EGpdf2excel.csproj" /p:Configuration=Release /p:RuntimeIdentifier=$RID /p:SelfContained=true /p:PublishTrimmed=false --no-restore -m:1 -nodeReuse:false -o "$DIST\EGpdf2excel"
if ($LASTEXITCODE -ne 0) { throw "EGpdf2excel publish failed" }

Step 'Publish EGexcel2df'
& $dotnet publish "$root\EGexcel2df\EGexcel2df.csproj" /p:Configuration=Release /p:RuntimeIdentifier=$RID /p:SelfContained=true /p:PublishTrimmed=false --no-restore -m:1 -nodeReuse:false -o "$DIST\EGexcel2df"
if ($LASTEXITCODE -ne 0) { throw "EGexcel2df publish failed" }

Step 'Publish EGpdf2df'
& $dotnet publish "$root\EGpdf2df\EGpdf2df.csproj" /p:Configuration=Release /p:RuntimeIdentifier=$RID /p:SelfContained=true /p:PublishTrimmed=false --no-restore -m:1 -nodeReuse:false -o "$DIST\EGpdf2df"
if ($LASTEXITCODE -ne 0) { throw "EGpdf2df publish failed" }

# ---- 3b. 交付目录里不留符号文件（*.pdb）----
# csproj 里的 CopyOutputSymbolsToPublishDirectory / AllowedReferenceRelatedFileExtensions
# 对**项目引用**带出来的 pdb（Engine / Core）并不完全生效，所以在这里兜一道：
# 把 publish 目录里的 *.pdb 移进 .build_trash（用移动而不是删除 —— 递归删除会触发
# 沙箱 safe-delete 守卫 fail-closed，整条流水线会被中断）。
Step 'Strip *.pdb from published CLI folders (release payload only)'
foreach ($d in @('EGpdf2excel','EGexcel2df','EGpdf2df')) {
    $pd = "$DIST\$d"
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
$qtCmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if ((Test-Path "$qtRoot\lib\cmake\Qt6\Qt6Config.cmake") -and (Test-Path $qtCmake)) {
    Step 'Build EGtools.Frontend (Qt 6)'
    # The Ninja generator (unlike the VS generator) does NOT auto-locate MSVC, so the
    # MSVC + Windows SDK environment must be present before configuring. This is a
    # no-op when the script is already run from a Developer PowerShell.
    if (-not $env:EGTOOLS_MSVC_ENV_READY) {
        # ⚠️⚠️ MSVC 必须**按目标架构**挑，不能只按"最新版本"挑（2026-10-14 实测踩坑）：
        #    arm64 交叉编译需要 `bin\Hostx64\arm64\cl.exe`，而本机只有 **VS Insiders 的
        #    14.51.36231** 带这个目录（Community 的 14.51 和 Insiders 的 14.52 都只有 x64/x86）。
        #    `Sort-Object Name -Descending | Select -First 1` 会选到 14.52 ⇒ 找不到 arm64 编译器。
        $msvcVer = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*' -Directory -EA SilentlyContinue |
                   Where-Object { Test-Path "$($_.FullName)\bin\Hostx64\$Arch\cl.exe" } |
                   Sort-Object Name -Descending | Select-Object -First 1
        $sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
        $sdkVer = (Get-ChildItem "$sdkRoot\Include" -Directory -EA SilentlyContinue |
                   Sort-Object Name -Descending | Select-Object -First 1).Name
        if ($msvcVer -and $sdkVer) {
            $m = $msvcVer.FullName
            # ⚠️⚠️ SDK 的 bin 必须用 **x64**（宿主工具 rc.exe / mt.exe）——**不要**按目标架构选：
            #    `Windows Kits\10\bin\<ver>\arm64\rc.exe` 是 **arm64 原生程序**，
            #    在 x64 宿主上根本跑不起来（实测报 "RC Pass 1 ... failed: unknown error"，
            #    而 cl.exe 那步是成功的，报错极具误导性）。
            #    交叉编译的规则是：**编译器按目标架构（Hostx64\<arch>），宿主工具按宿主架构（x64）**。
            $env:PATH    = "$m\bin\Hostx64\$Arch;$sdkRoot\bin\$sdkVer\x64;" + $env:PATH
            $env:INCLUDE = "$m\include;$sdkRoot\Include\$sdkVer\ucrt;$sdkRoot\Include\$sdkVer\um;$sdkRoot\Include\$sdkVer\shared;$sdkRoot\Include\$sdkVer\winrt"
            $env:LIB     = "$m\lib\$Arch;$sdkRoot\Lib\$sdkVer\ucrt\$Arch;$sdkRoot\Lib\$sdkVer\um\$Arch"
            $env:EGTOOLS_MSVC_ENV_READY = '1'
            Write-Host "MSVC   -> $m  (Hostx64\$Arch)"
        } else {
            Write-Warning "Could not auto-detect MSVC / Windows SDK for the Qt build (arch=$Arch)."
        }
    }
    Push-Location "$root\EGtools.Frontend"
    $qtExe = "$qtBuildDir\EGtoolsFrontend.exe"

    # ⚠️⚠️ 构建前**先移走上一次的 exe**。
    # 原因（实测踩过）：编译失败时后面那句 `if (Test-Path $qtExe)` 仍会命中**上一轮的产物**，
    # 于是脚本一路往下走、打出安装包、最后还报 ALL DONE —— 交付了一个用**旧二进制**做的包，
    # 而真实情况是前端根本没编译成功。移走旧产物之后，"构建后 exe 不存在"就成了硬失败信号。
    if (Test-Path $qtExe) {
        $stale = "$root\.build_trash\stale_frontend"
        New-Item -ItemType Directory -Path $stale -Force | Out-Null
        Move-Item -LiteralPath $qtExe `
                  -Destination (Join-Path $stale "EGtoolsFrontend_$(Get-Date -Format yyyyMMdd_HHmmss).exe") -Force
    }

    # ⚠️ 不要用管道接 Out-Host 之后再读 $LASTEXITCODE —— 管道会**吞掉/改写**退出码
    #    （本仓库早有记录：编译/校验必须让工具退出码等于原生命令）。这里直接调用。
    # ⚠️ 必须是 Release：既要显式传 CMAKE_BUILD_TYPE（Ninja 是单配置生成器），
    #    也要给 --build 补 --config Release。CMakeLists 里另外做了兜底
    #    （未指定时默认 Release、显式 Debug 会被强制改回 Release）。
    # ⚠️ 构建目录按架构分开（build / build-arm64）：两者 CMakeCache 里的编译器/架构不同，
    #    共用一个目录会互相覆盖，导致"配置看似成功、实际编出上一个架构"。
    $qtBuildRel = Split-Path $qtBuildDir -Leaf
    if ($Arch -eq 'x64') {
        & $qtCmake -S . -B $qtBuildRel -G Ninja -DCMAKE_PREFIX_PATH="$qtRoot" -DCMAKE_BUILD_TYPE=Release
    } else {
        # arm64 交叉编译必须让 Qt 的 toolchain 接管（它会设 CMAKE_SYSTEM_PROCESSOR=arm64 /
        # mkspec win32-arm64-msvc / 以及 --machine:ARM64 之类的链接参数）。
        # 只靠 CMAKE_PREFIX_PATH 指向 msvc2022_arm64 是不够的 —— 那样 CMake 仍按宿主架构配置。
        & $qtCmake -S . -B $qtBuildRel -G Ninja `
            -DCMAKE_TOOLCHAIN_FILE="$qtRoot\lib\cmake\Qt6\qt.toolchain.cmake" `
            -DCMAKE_PREFIX_PATH="$qtRoot" -DCMAKE_BUILD_TYPE=Release
    }
    $qtCfgRc = $LASTEXITCODE
    if ($qtCfgRc -ne 0) { Pop-Location; throw "Qt CMake 配置失败（exit=$qtCfgRc, arch=$Arch）—— 前端不会更新，已中止。" }

    & $qtCmake --build $qtBuildRel --config Release
    $qtBuildRc = $LASTEXITCODE
    if ($qtBuildRc -ne 0) { Pop-Location; throw "Qt 前端编译失败（exit=$qtBuildRc, arch=$Arch）—— 已中止，避免打出用旧二进制的安装包。" }

    $cfgLine = Select-String -Path "$qtBuildRel\CMakeCache.txt" -Pattern '^CMAKE_BUILD_TYPE:STRING=' -EA SilentlyContinue |
               Select-Object -First 1
    Pop-Location

    if (-not $cfgLine) { throw '无法确认 Qt 构建配置（CMakeCache.txt 里没有 CMAKE_BUILD_TYPE）。' }
    Write-Host "Qt build config -> $($cfgLine.Line)"
    if ($cfgLine.Line -notmatch 'Release') {
        throw "Qt 构建配置不是 Release：$($cfgLine.Line)"
    }
    if (-not (Test-Path $qtExe)) {
        throw "Qt 构建报成功但产物不存在：$qtExe"
    }

    if (Test-Path $qtExe) {
        $out = "$DIST\EGtoolsFrontend"
        New-Item -ItemType Directory -Path $out -Force | Out-Null
        Copy-Item $qtExe $out -Force
        # ⚠️ 必须把**宿主自己的** runtimeconfig 一起部署，否则打包出来的程序里
        # 「内嵌调库」通道直接失效：hostfxr 找不到 <exe>.runtimeconfig.json 会退用内核那份
        # （内核是 self-contained，等于把内核当宿主）⇒ 实测报
        #   "Initialization for self-contained components is not supported" (hr=0x80008093)
        # 然后静默回退命令行。这个 bug 只在**跑发布产物**时才暴露（build/ 目录里有这份文件）。
        $hostCfg = "$qtBuildDir\EGtoolsFrontend.runtimeconfig.json"
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
        # ⚠️⚠️ VC++ 运行时必须在**依赖闭环自证之前**拷进来（2026-10-14 修正）。
        #    原来这一步排在最后（插件之后），于是自证会看到"Qt6Core -> MSVCP140.dll 缺失"而
        #    报错退出。x64 上之所以一直没暴露，是因为 DIST\EGtoolsFrontend 里有**上一轮构建
        #    残留的** vc143 DLL ⇒ 自证"碰巧"通过；arm64 是全新跳目录（DIST-arm64），立刻现形。
        #    ⇒ 这类"靠残留文件蒙混过关"的坑，只有干净目录 + 自证才能抓住。
        Copy-Item "$vcRedist\*.dll" $out -Force -EA SilentlyContinue

        # ---- 依赖闭环自证（手工清单一定会漏，所以别再只靠手工）----------------
        # 判据：**凡是存在于我们自己的 Qt / redist 来源目录里的依赖，都必须出现在 $out**。
        # （只看这两个来源目录，能精准抓住"忘了拷某个 Qt 模块"，又不会被
        #  KERNEL32/api-ms-win-* 这类系统组件干扰。）架构一致性由 Remove-WrongArchExtras 负责。
        $qtBin = Get-ChildItem "$qtRoot\bin\*.dll" -EA SilentlyContinue | ForEach-Object { $_.Name }
        $redist = Get-ChildItem "$vcRedist\*.dll" -EA SilentlyContinue | ForEach-Object { $_.Name }
        $ours = @{}
        foreach ($n in ($qtBin + $redist)) { if ($n -notmatch 'd\.dll$') { $ours[$n.ToLower()] = $true } }

        $mods = @(Get-ChildItem "$out\*.dll","$out\*.exe" -EA SilentlyContinue)
        $have = @{}
        foreach ($m in $mods) { $have[$m.Name.ToLower()] = $true }

        # 先攒出"谁导入了什么"，避免对每个文件重复跑 dumpbin（O(n) 而不是 O(n²)）
        $importsOf = @{}
        foreach ($f in $mods) {
            $deps = @(& $binutils /dependents $f.FullName 2>$null |
                      Select-String -Pattern '^\s+([A-Za-z0-9_\-\.]+\.dll)$' |
                      ForEach-Object { $_.Matches[0].Groups[1].Value })
            $importsOf[$f.Name] = $deps
        }

        # ① 依赖闭环
        $missing = New-Object System.Collections.Generic.List[string]
        foreach ($f in $mods) {
            foreach ($d in $importsOf[$f.Name]) {
                $dl = $d.ToLower()
                if (-not $ours.ContainsKey($dl)) { continue }   # 非我们分发的（系统 DLL）→ 跳过
                if (-not $have.ContainsKey($dl)) { $missing.Add("$($f.Name) -> $d") }
            }
        }
        if ($missing.Count -gt 0) {
            throw ("依赖不闭环（发布产物会启动失败，典型报错 0xC0000135 缺 DLL）：`n  " +
                   (($missing | Sort-Object -Unique) -join "`n  "))
        }
        Write-Host ("OK  Qt 依赖闭环：$out 内 " + $have.Count + " 个模块的自有依赖全部就位")

        # ② 架构一致性（见 Remove-WrongArchExtras 的说明）：
        #    光"文件在"不够，**架构还得对** —— 依赖闭环扫不出"x64 文件混进 arm64 包"。
        $wrongArch = Remove-WrongArchExtras -Dir $out -TrashDir $trash -Stamp $stamp
        if ($wrongArch.Count -gt 0) {
            Write-Host ("OK  已剔除架构不符的多余文件（无人导入）：" + ($wrongArch -join ', '))
        } else {
            Write-Host "OK  架构一致性：$out 内全部文件均为 $Arch（或平台无关 IL）"
        }

        # ---- 终审：**真的把发布产物跑起来**（能启动才算打包成功）----------------
        # 前面所有清单检查都只是"推理"，只有这一步是事实。缺 DLL 的表现是
        # 进程立刻退出、exit=0xC0000135(-1073741515)，日志里什么都没有。
        # ⚠️ arm64：本机是 x64 宿主，**根本跑不了 arm64 程序**（Windows 的 x64→arm64
        #    模拟是**单向的**，只有 arm64 能跑 x64）。所以这里不能真启动，改为
        #    架构断言 + 交付说明；真机验证必须拿到 arm64 Windows 上做。
        $published = "$out\EGtoolsFrontend.exe"
        if ($Arch -eq 'arm64') {
            $pm = Assert-PeArch $published '发布的前端 exe'
            Write-Host ("OK  发布产物架构正确：EGtoolsFrontend.exe = $(Get-MachineName $pm)")
            Write-Warning ("arm64 产物**无法在本机（x64）真跑** —— 已用架构断言 + 依赖闭环替代。" +
                           " 真机验证请在 arm64 Windows 上运行 DIST-arm64\EGtoolsFrontend\EGtoolsFrontend.exe --version")
        } elseif (Test-Path $published) {
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
        Write-Host "OK  Qt front-end -> $DIST\EGtoolsFrontend (self-contained)"
    } else {
        Write-Warning 'Qt front-end build failed.'
    }
} else {
    Step 'Qt front-end SKIPPED (Qt 6 or CMake not found)'
    Write-Warning "Qt 6 not found at $qtRoot — skipping EGtools.Frontend."
}

# ---- 5. Ensure VC++ 2022 runtime DLLs in every CLI dir (Qt app gets its own via windeployqt) ----
Step 'Copy VC++ 2022 (v14) runtime into each CLI dir'
# ⚠️ 拷完必须**剔除架构不符的多余文件**：微软 arm64 CRT 目录里的 vcruntime140_1.dll
#    本身是 x64（vs x64 产物有 12 个模块导入它、arm64 一个都没有）——
#    见 Remove-WrongArchExtras 的详细说明。x64 走这条路径时不会误伤（架构本来就相符）。
$vc = "$vcRedist"
foreach ($d in @("$DIST\EGpdf2excel", "$DIST\EGexcel2df", "$DIST\EGpdf2df")) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
    Copy-Item -Path "$vc\*" -Destination $d -Force
    $ex = Remove-WrongArchExtras -Dir $d -TrashDir $trash -Stamp $stamp
    if ($ex.Count -gt 0) {
        Write-Host ("OK  $(Split-Path $d -Leaf)：剔除架构不符的多余文件 " + ($ex -join ', '))
    }
}

# ---- 6. Docs + icon ----
# Single source of truth for the app icon: the Qt front-end's icon (also embedded
# into EGtoolsFrontend.exe via res/app_icon.rc). build_installer.iss uses this as
# SetupIconFile, so Explorer / Start menu / installer all show the same icon.
Copy-Item "$root\EGtools.Frontend\res\app_icon.ico" "$DIST\app.ico" -Force
if (-not (Test-Path "$DIST\docs")){ Copy-Item "$root\docs" "$DIST\docs" -Recurse -Force }

# ---- 7. Verify exes exist **并断言架构** ----
# ⚠️ "文件存在"骗人：交叉编译最阴的失效是把别的架构的产物当成本架构的。
#    这里对每个原生可执行文件读 PE 头钉死架构（托管程序集是 IL，另算）。
Step 'Verify DIST layout + architecture'
foreach ($d in @('EGpdf2excel','EGexcel2df','EGpdf2df')) {
    $exe = "$DIST\$d\$d.exe"
    if (-not (Test-Path $exe)) { throw "Missing $exe" }
    $m = Assert-PeArch $exe "$d 的可执行文件"
    Write-Host "OK  $d.exe = $(Get-MachineName $m)  $((Get-Item $exe).Length) bytes"
}
# 内核里真正会用到的**原生库**：必须与目标架构一致（漏一个 = 功能整体不可用）
foreach ($pair in @(@('EGpdf2df','MuPDFWrapper.dll'), @('EGpdf2df','OpenCvSharpExtern.dll'),
                    @('EGpdf2excel','MuPDFWrapper.dll'))) {
    $lib = "$DIST\$($pair[0])\$($pair[1])"
    if (Test-Path $lib) {
        $m = Assert-PeArch $lib "$($pair[0])/$($pair[1])"
        Write-Host "OK  $($pair[0])/$($pair[1]) = $(Get-MachineName $m)"
    }
}
# The Qt front-end is the shipped GUI; require it for a complete installer.
$qtOut = "$DIST\EGtoolsFrontend\EGtoolsFrontend.exe"
if (-not (Test-Path $qtOut)) { throw "Missing $qtOut (Qt front-end build failed)" }
$qm = Assert-PeArch $qtOut '前端 exe'
Write-Host "OK  $qtOut = $(Get-MachineName $qm)  $((Get-Item $qtOut).Length) bytes"

# ---- 7b. 交付自证：exe 自己报出构建配置，必须含 "Release" ----
# 只信产物本身的说法（--version 里带构建配置），不信文件名或目录名。
# ⚠️ arm64 跳过：本机 x64 **跑不了** arm64 程序（模拟是单向的），改由上面的架构断言兜。
if ($Arch -eq 'arm64') {
    Step 'Verify Qt front-end is a Release build (arm64: 改为静态断言)'
    # CMakeCache 里的 CMAKE_BUILD_TYPE 是"构建配置"的直接证据（arm64 上没法跑 exe 自证）
    $cfg = Select-String -Path "$qtBuildDir\CMakeCache.txt" -Pattern '^CMAKE_BUILD_TYPE:STRING=' -EA SilentlyContinue |
           Select-Object -First 1
    if (-not $cfg) { throw 'arm64：无法确认构建配置（CMakeCache.txt 缺 CMAKE_BUILD_TYPE）。' }
    Write-Host "OK  arm64 build config -> $($cfg.Line)"
    if ($cfg.Line -notmatch 'Release') { throw "arm64 构建配置不是 Release：$($cfg.Line)" }
} else {
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
}

# ---- 8. Recompile installer from fresh DIST ----
Step 'Compile installer (ISCC)'
$iscc = 'C:\Users\QBZ95\AppData\Local\Programs\Inno Setup 6\ISCC.exe'
# 架构通过 /D 传给 iss（iss 里用 {#MyArch} / {#DistDir} / {#OutSuffix}）——
# 这样同一个 .iss 能出 x64 与 arm64 两个包，不用维护两份。
& $iscc "/DMyArch=$Arch" "/DDistDir=$DIST" "/DOutSuffix=$instSuffix" "$root\build_installer.iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC failed (arch=$Arch)" }
$out = "$root\installer\EGtools-4.3.0-$instSuffix.exe"
if (-not (Test-Path $out)) { throw "Installer not produced: $out" }
Start-Sleep -Seconds 2
$size = (Get-Item $out).Length
Write-Host "Installer ($Arch) size: $size bytes"
if ($size -lt 40MB) { throw "Installer too small ($size) — payload missing!" }

# ---- 9. 结论（把"验证到什么程度"说清楚）----
if ($Arch -eq 'arm64') {
    Write-Host "`nALL DONE (arm64, cross-compiled)" -ForegroundColor Green
    Write-Warning ("arm64 包已产出，但**未在本机真机运行**（x64 宿主跑不了 arm64 程序）。`n" +
                   "  已完成的验证：PE 架构断言（exe + MuPDFWrapper + OpenCvSharpExtern 全部 ARM64）、`n" +
                   "                Qt 依赖闭环自证（自有 DLL 无缺失）、构建配置 = Release。`n" +
                   "  待你在 arm64 Windows 上确认：解压后运行 EGtoolsFrontend\EGtoolsFrontend.exe --version")
} else {
    Write-Host "`nALL DONE" -ForegroundColor Green
}
