#!/usr/bin/env bash
# ===========================================================================
# build_linux.sh —— Linux 一键构建 + 打包（deb / rpm）
#
# 与 Windows 的 build_all.ps1 对应。产出的安装包布局：
#   /opt/egtools/EGtoolsFrontend             Qt 前端（含 runtimeconfig.json）
#   /opt/egtools/EGpdf2excel/…               三个 .NET 内核（**self-contained**）
#   /opt/egtools/EGpdf2df/…
#   /opt/egtools/EGexcel2df/…
#   /opt/egtools/share/applications/…        桌面项（postinst 软链到 /usr/share）
# 内核放在前端**同级的 <name>/<name>**，正是 KernelClient 自动发现会命中的布局。
#
# 用法：
#   ./build_linux.sh                 # 内核 + 前端 + deb + rpm
#   ./build_linux.sh --no-package    # 只构建，不打安装包（快）
#   ./build_linux.sh --kernels-only  # 只出内核（不碰 Qt；先证明 .NET 侧在 Linux 上能跑）
#   ./build_linux.sh --arch arm64 --kernels-only   # **交叉发布 arm64 内核**（在 x64 上即可）
#   QT_PREFIX=/opt/Qt/6.8.0/gcc_64 ./build_linux.sh   # 指定 Qt
#
# 架构（EGTOOLS_ARCH / --arch）：
#   x64（默认）→ RID=linux-x64、产物 DIST-linux/、可在任意 Linux 上编
#   arm64      → RID=linux-arm64、产物 DIST-linux-arm64/
#                ⚠️ **内核**可在 x64 上交叉发布（.NET 支持，已实测）；
#                ⚠️ **Qt 前端**必须在 arm64 环境里编（需要 aarch64 的 Qt6 + Qt6Pdf），
#                   故 arm64 的 deb/rpm 也只能在 arm64 环境（真机 / qemu chroot）里打。
#                   在 x64 上跑 `--arch arm64` 而不带 --kernels-only 会明确报错退出。
#
# 依赖（构建机）：dotnet SDK 9+、cmake 3.21+、ninja 或 make、g++、Qt6 **含 Qt6Pdf**、
#                打包用 dpkg-deb（deb）/ rpmbuild（rpm）。
#   Ubuntu 24.04: sudo apt install dotnet-sdk-9.0 cmake ninja-build g++ \
#                                qt6-base-dev qt6-pdf-dev dpkg-dev rpm
#   （Ubuntu 24.04 的 Qt 是 **6.4.2**；本项目在 Windows 上用 6.11.2 构建，
#     版本差异造成的编译问题只在真机编译时才暴露 —— 见 verify_in_wsl.sh。）
# ===========================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="${EGTOOLS_VERSION:-4.3.0}"
ARCH="${EGTOOLS_ARCH:-x64}"
# ELF 的 e_machine 期望值（用于断言产物架构 —— 交叉编译最怕"静默产出错架构"）。
ARCH_ELF_X64="3e00"
ARCH_ELF_ARM64="b700"
RID=""
FRONTEND="$ROOT/EGtools.Frontend"
# 构建目录可外置：在 WSL 里编前端时，把 build 目录放到 Linux 原生文件系统
# （如 /tmp）比留在 /mnt/c（9p/drvfs）快很多。
BUILD_DIR="${EGTOOLS_LINUX_BUILD:-$FRONTEND/build-linux}"
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4 )"
QT_PREFIX="${QT_PREFIX:-}"
DO_DEB=1
DO_RPM=1
KERNELS_ONLY=0
SKIP_KERNELS=0

while [ $# -gt 0 ]; do
    case "$1" in
        --no-package)   DO_DEB=0; DO_RPM=0 ;;
        --deb)          DO_DEB=1; DO_RPM=0 ;;
        --rpm)          DO_DEB=0; DO_RPM=1 ;;
        # 只出内核（不碰 Qt）：用于"先证明 .NET 侧在 Linux 上真的能跑"，
        # 也方便在没装 Qt6 的机器/容器里先做一轮验证。
        --kernels-only) KERNELS_ONLY=1; DO_DEB=0; DO_RPM=0 ;;
        # 复用**已发布**的内核，只编 Qt 前端 + 打包。给"构建机没有 .NET SDK"的场景用
        # （内核可在 Windows 侧交叉发布好，Qt 前端本来就必须在 Linux 上编）。
        --no-kernels)   SKIP_KERNELS=1 ;;
        --arch)         ARCH="$2"; shift ;;
        --qt)           QT_PREFIX="$2"; shift ;;
        -h|--help)      sed -n '2,34p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
    shift
done

case "$ARCH" in
    x64|amd64|linux-x64)   ARCH="x64";   RID="linux-x64";   ARCH_ELF="$ARCH_ELF_X64" ;;
    arm64|aarch64|linux-arm64) ARCH="arm64"; RID="linux-arm64"; ARCH_ELF="$ARCH_ELF_ARM64" ;;
    *) echo "未知架构: $ARCH（支持 x64 / arm64）" >&2; exit 2 ;;
esac
# 输出目录按**平台 + 架构**隔离（否则同名产物互相覆盖）。
DIST_DEFAULT="$ROOT/DIST-linux"
[ "$ARCH" != "x64" ] && DIST_DEFAULT="$ROOT/DIST-linux-$ARCH"
DIST="${EGTOOLS_LINUX_DIST:-$DIST_DEFAULT}"

log()  { printf '\n=== %s ===\n' "$*"; }
fail() { printf '\n[FAIL] %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 运行库依赖反推（给 CPack 的 Depends / Requires 用）
#
# 为什么不能用自动推导：本包是 **self-contained 捆绑**（三个 .NET 内核自带整套运行时
# 与私有 .so），`dpkg-shlibdeps` / rpm autoreq 会把捆绑内的私有库也当外部依赖去找 ⇒
# 实测直接报 `cannot find library liblttng-ust.so.0`，**包都出不来**。
# 为什么也不能手写包名：Qt 的运行库包名带 t64 后缀（libqt6widgets6t64），随发行版变。
#
# 所以：只取二进制的**直接 DT_NEEDED**（不是 ldd 的传递闭包），在构建机上解成真实包名。
# ---------------------------------------------------------------------------
lib_path_of() {   # soname → 规范化绝对路径（/lib→/usr/lib 的合并式 usr 会让 dpkg -S 失配）
    local n="$1" d
    for d in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /lib64 /usr/lib; do
        if [ -e "$d/$n" ]; then readlink -f "$d/$n"; return; fi
    done
}

direct_needed() {
    readelf -d "$1" 2>/dev/null | awk -F'[][]' '/NEEDED/{print $2}' | sort -u
}

# ⚠️⚠️ 必须把**内核里的原生库**也算进依赖，不能只算前端二进制。
#    `libOpenCvSharpExtern.so` 链接了 GTK3 全套（libgtk-3.so.0 / libcairo.so.2 /
#    libpango-1.0.so.0 / libfreetype.so.6 / libX11.so.6 …，OpenCV 官方构建开了 GUI 模块），
#    dlopen 时动态链接器会解析**全部** DT_NEEDED ⇒ 缺一个就整块不可用。
#    实测：只按前端反推时，这些包**一个都没进 Depends** ⇒ 在干净的 Ubuntu Server 上装完，
#    `EGpdf2df` 一碰 Cv2.* 就抛
#      `The type initializer for 'OpenCvSharp.Internal.NativeMethods' threw an exception.`
#    （开发机上因为装了 qt6-base-dev，GTK3 被顺带带进来 ⇒ 完全看不出来）。
#    所以这里对**每个**传入的二进制都反推，再合并去重。
deb_depends_for() {   # → 逗号分隔的 Debian 包名（本发行版实测）；可传多个二进制
    local out="" f n p pkg
    for f in "$@"; do
        [ -f "$f" ] || continue
        while read -r n; do
            [ -n "$n" ] || continue
            p="$(lib_path_of "$n")"
            [ -n "$p" ] || continue
            pkg="$(dpkg -S "$p" 2>/dev/null | head -1 | cut -d: -f1 | sed 's/:amd64$//')"
            [ -n "$pkg" ] && out="$out${out:+,}$pkg"
        done < <(direct_needed "$f")
    done
    printf '%s' "$out" | tr ',' '\n' | grep -v '^$' | sort -u | paste -sd, -
}

rpm_requires_for() {  # → 逗号分隔的 soname 能力（跨发行版，rpm 就是这么自报的）
    # ⚠️ `(64bit)` 是 RPM 对 **ELFCLASS64** 的标记，**与 CPU 无关** ⇒ aarch64 上也写作
    #    `libQt6Core.so.6()(64bit)`（只有 32 位目标才写 (32bit)）。
    local f
    for f in "$@"; do
        [ -f "$f" ] || continue
        direct_needed "$f"
    done | sort -u | while read -r n; do
        case "$n" in lib*) printf '%s()(64bit)\n' "$n" ;; esac
    done | sort -u | paste -sd, -
}

# 需要参与依赖反推的全部二进制：前端 + 内核里会加载的原生库。
# （.NET 运行时自身是 self-contained 捆绑；这里重点是 OpenCV/MuPDF 这两块原生库。）
dep_scan_targets() {
    local d
    for d in "$DIST/EGpdf2excel" "$DIST/EGexcel2df" "$DIST/EGpdf2df"; do
        [ -f "$d/libOpenCvSharpExtern.so" ] && echo "$d/libOpenCvSharpExtern.so"
        [ -f "$d/libMuPDFWrapper.so" ] && echo "$d/libMuPDFWrapper.so"
    done
}

# ⚠️⚠️ **第二类依赖盲区**：.NET 运行时用 **dlopen**（而不是 ELF 的 DT_NEEDED）加载的库。
#    典型 = **libicu**（全球化）与 **libssl/libcrypto**（加密）。
#    它们不在任何一条 DT_NEEDED 里 ⇒ 按 ELF 反推**永远抓不到** ⇒ 只会在**干净机**上暴露，
#    而且报错比缺 .so 更致命：**启动即 FailFast**
#      `Process terminated. Couldn't find a valid ICU package installed on the system.`
#    ⚠️ 本项目处理**土耳其语**图纸（著名的 Turkish-I 大小写问题），依赖 ICU 的 locale 规则
#       ⇒ **不能**用 `InvariantGlobalization` 绕过（会改变大小写/排序语义）。
#    实测：arm64 chroot（minbase）就是被这条卡住的；x64 开发机因发行版自带 libicu 而看不出来。
runtime_dlopen_soname_prefixes() {
    printf '%s\n' libicuuc.so libicui18n.so libicudata.so libssl.so libcrypto.so
}
soname_impl_path() {   # soname 前缀 → 带版本号的实际文件全路径
    local p="$1" d n
    for d in /usr/lib/*-linux-gnu /lib/*-linux-gnu /usr/lib64 /lib64 /usr/lib /lib; do
        for n in "$d/$p".*; do
            [ -e "$n" ] && { readlink -f "$n"; return; }
        done
    done
}
deb_runtime_dlopen_deps() {
    local p path pkg out=""
    while read -r p; do
        path="$(soname_impl_path "$p")"
        [ -n "$path" ] || continue
        pkg="$(dpkg -S "$path" 2>/dev/null | head -1 | cut -d: -f1 | sed 's/:[a-z0-9]*$//')"
        [ -n "$pkg" ] && out="$out${out:+,}$pkg"
    done < <(runtime_dlopen_soname_prefixes)
    printf '%s' "$out" | tr ',' '\n' | grep -v '^$' | sort -u | paste -sd, -
}
rpm_runtime_dlopen_requires() {
    local p path n out=""
    while read -r p; do
        path="$(soname_impl_path "$p")"
        [ -n "$path" ] || continue
        # ⚠️ 必须取 **SONAME** 而不是文件名：Ubuntu 上实体是 `libicuuc.so.74.2`，
        #    SONAME 才是 `libicuuc.so.74` —— 而 RPM 的能力名用的是 SONAME。
        #    （DT_NEEDED 里本来就是 SONAME，所以 direct_needed 那条路没这个问题。）
        n="$(readelf -d "$path" 2>/dev/null | awk -F'[][]' '/SONAME/{print $2}')"
        [ -n "$n" ] || n="$(basename "$path")"
        out="$out${out:+,}${n}()(64bit)"
    done < <(runtime_dlopen_soname_prefixes)
    printf '%s' "$out"
}

# ---------------------------------------------------------------------------
# 0. 前置检查（缺什么先报清楚，别等构建到一半才炸）
# ---------------------------------------------------------------------------
log "前置检查"
HOST_ARCH="$(uname -m)"
echo "目标   : $ARCH（RID=$RID, e_machine=$ARCH_ELF）"
echo "构建机 : $HOST_ARCH"

# ⚠️ 架构保护：.NET 能交叉发布，**Qt 前端不能**（需要目标架构的 Qt6 + Qt6Pdf）。
#    在 x64 上试图编 arm64 前端必然失败，与其编到一半炸，不如立刻说清楚。
if [ "$ARCH" = "arm64" ] && [ "$KERNELS_ONLY" != 1 ]; then
    case "$HOST_ARCH" in
        aarch64|arm64) : ;;   # 原生 arm64 构建，正常走
        *)
            fail "在 $HOST_ARCH 上无法构建 arm64 的 Qt 前端（需要 aarch64 的 Qt6 + Qt6Pdf）。
       · 只要 arm64 **内核**：加 --kernels-only（.NET 交叉发布，无需 arm64 机器）
       · 要 arm64 **完整包**：在 arm64 环境里跑本脚本（真机，或 x64 上 qemu +
         arm64 chroot：debootstrap --arch=arm64 + qemu-user-static + binfmt_misc）"
            ;;
    esac
fi

# dotnet 只在真的要发布内核时需要 —— `--no-kernels` 复用现成产物，不该被 SDK 缺失拦住。
if [ "$SKIP_KERNELS" = 1 ]; then
    echo "模式   : 复用已有内核（$DIST），只编前端/打包"
else
    command -v dotnet >/dev/null || fail "未找到 dotnet（需要 .NET SDK 9+）。
       若本机没有 SDK：改用 --no-kernels，并在 Windows 侧先交叉发布：
         build_linux.sh --kernels-only   （或在 Windows 上 dotnet publish -r $RID）"
    echo "dotnet : $(dotnet --version)"
fi

# qt/Qt 只在真的要编前端时才要求 —— `--kernels-only` 不该被 Qt 缺失拦住。
CMAKE_QT_ARG=()
if [ "$KERNELS_ONLY" = 1 ]; then
    echo "模式   : 只出内核（跳过 Qt 相关前置检查）"
else
    command -v cmake  >/dev/null || fail "未找到 cmake"
    echo "cmake  : $(cmake --version | head -1)"

    # Qt6 定位：显式 QT_PREFIX 优先，其次交给 cmake 自己找（发行版包会装在标准前缀）。
    if [ -n "$QT_PREFIX" ]; then
        [ -d "$QT_PREFIX" ] || fail "QT_PREFIX 不存在: $QT_PREFIX"
        CMAKE_QT_ARG=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
        echo "Qt      : $QT_PREFIX"
    else
        echo "Qt      : 交给 CMake 自动查找（若失败请设 QT_PREFIX）"
    fi

    # ⚠️ Qt6Pdf（Qt PDF / PDFium）是**硬依赖**：预览的矢量渲染全靠它。
    if [ -n "$QT_PREFIX" ] && [ ! -e "$QT_PREFIX/lib/cmake/Qt6Pdf" ]; then
        fail "$QT_PREFIX 里没有 Qt6Pdf —— 预览渲染必需。请安装/编译带 PDF 模块的 Qt6
       （Debian/Ubuntu: apt install qt6-pdf-dev）"
    fi
fi

# ---------------------------------------------------------------------------
# 1. 内核：三个 CLI 自包含发布到 DIST/<name>/
#    为什么 self-contained：目标机不需要预装 .NET 运行时（与 Windows 安装包一致）。
# ---------------------------------------------------------------------------
PROJECTS=(EGpdf2excel EGexcel2df EGpdf2df)
if [ "$SKIP_KERNELS" = 1 ]; then
    log "复用已有内核（--no-kernels）"
    for p in "${PROJECTS[@]}"; do
        [ -d "$DIST/$p" ] || fail "$DIST/$p 不存在 —— --no-kernels 需要一个已发布的内核目录。
       先在 Windows 侧跑 build_linux.sh --kernels-only，或去掉 --no-kernels。"
    done
    echo "内核目录: $DIST"
else
    log "发布 .NET 内核（$RID, self-contained）"
    for p in "${PROJECTS[@]}"; do
        echo "--- dotnet publish $p ---"
        dotnet publish "$ROOT/$p/$p.csproj" \
            -c Release -r "$RID" --self-contained true \
            -p:PublishTrimmed=false \
            -o "$DIST/$p" || fail "$p 发布失败"
    done

    # 交付不带符号文件（与 Windows 侧一致）。
    find "$DIST" -maxdepth 2 -name '*.pdb' -delete 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 2. 内核产物断言（**必须在内核 publish 之后立刻做**，这样 --kernels-only
#    这个"先证明 .NET 侧能跑"的档位也带着断言，而不是空跑一趟）
#    ⚠️ 这几条是历史教训换来的：
#      · 缺 libOpenCvSharpExtern.so → 图纸差异一碰 Cv2.* 就 DllNotFoundException
#      · 缺 libMuPDFWrapper.so      → 所有 PDF 处理直接不可用
#      · 缺 runtimeconfig.json      → 内嵌通道静默退化成命令行
# ---------------------------------------------------------------------------
log "断言内核产物"
# ELF 魔数 + **e_machine 必须匹配目标架构**。
# ⚠️ 只判魔数是不够的：交叉编译最阴的失效模式是"发布成功、架构也对不上"
#    （例如某依赖只有 x64 原生库，它照样会被拷进产物）。实测 arm64 产物里
#    一旦漏了某个 arm64 原生包，就会出现 x64 的 .so 混进来 —— 只有在**目标机**
#    上才会炸，而且报错难看懂。这里用 e_machine 直接断言。
#    e_machine 在 ELF 头偏移 18、2 字节小端 ⇒ arm64=b700、x86-64=3e00。
# ⚠️ 用 `od -j18 -N2`（偏移 + 长度）而不是切字符串：切字符位置极易差一位（踩过）。
elf_machine_of() {
    [ -f "$1" ] || { echo "缺失"; return; }
    od -An -tx1 -j18 -N2 "$1" 2>/dev/null | tr -d ' \n'
}
is_elf() {
    [ -f "$1" ] || return 1
    [ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ] || return 1
    [ "$(elf_machine_of "$1")" = "$ARCH_ELF" ]
}

for p in "${PROJECTS[@]}"; do
    is_elf "$DIST/$p/$p" || fail "$p 不是目标架构（$ARCH）的可执行 ELF
       实际 e_machine=$(elf_machine_of "$DIST/$p/$p")（期望 $ARCH_ELF）／$(file -b "$DIST/$p/$p" 2>/dev/null || echo 缺失)"
    [ -e "$DIST/$p/libMuPDFWrapper.so" ] || fail "$p 缺少 libMuPDFWrapper.so（MuPDF 原生库）"
    is_elf "$DIST/$p/libMuPDFWrapper.so" || fail "$p 的 libMuPDFWrapper.so 架构不对（e_machine=$(elf_machine_of "$DIST/$p/libMuPDFWrapper.so")）"
    [ -f "$DIST/$p/$p.runtimeconfig.json" ] || fail "$p 缺少 runtimeconfig.json"
    echo "  OK  $p（$ARCH ELF + libMuPDFWrapper.so + runtimeconfig.json）"
done
# ⚠️ OpenCV 原生库只在 EGpdf2df 里需要（图纸对齐/图形差异）。
[ -e "$DIST/EGpdf2df/libOpenCvSharpExtern.so" ] || \
    fail "EGpdf2df 缺少 libOpenCvSharpExtern.so ——
       检查 EGpdf2df.Engine.csproj 里的 OpenCvSharp4.*runtime*.linux-$ARCH 引用是否还在"
is_elf "$DIST/EGpdf2df/libOpenCvSharpExtern.so" || \
    fail "libOpenCvSharpExtern.so 架构不对（e_machine=$(elf_machine_of "$DIST/EGpdf2df/libOpenCvSharpExtern.so")，期望 $ARCH_ELF）
       典型成因：csproj 里少了 linux-$ARCH 的 OpenCvSharp 原生包，于是**别的架构**的 .so 被带进来。"
echo "  OK  EGpdf2df/libOpenCvSharpExtern.so（$ARCH OpenCV 原生库）"
# ⚠️ 只有 file 存在不够：**依赖的 soname 必须能在本机解析**。
#    `OpenCvSharp4.runtime.linux-x64`(4.10) 是按 Ubuntu 20.04 构建的，DT_NEEDED 钉在
#    libtiff.so.5 / libtesseract.so.4 / libavcodec.so.58 这些旧 soname 上 —— 在 24.04 上
#    dlopen 直接失败，而 publish 与"文件存在"检查**都看不出来**。故在这里加 ldd 断言。
#    ⚠️ 交叉构建（x64 上出 arm64）时 ldd 读不了别的架构的 .so ⇒ 改为只读 DT_NEEDED 列出来
#       让人核对，并把"真机验证"明确交给目标机。
CROSS=0
{ [ "$ARCH" = "x64" ] && [ "$HOST_ARCH" != "x86_64" ]; } && CROSS=1
{ [ "$ARCH" = "arm64" ] && [ "$HOST_ARCH" = "x86_64" ]; } && CROSS=1
if [ "$CROSS" = 1 ]; then
    echo "  --  交叉构建（$HOST_ARCH → $ARCH）：本机无法 ldd 别的架构，列出 DT_NEEDED 供核对："
    direct_needed "$DIST/EGpdf2df/libOpenCvSharpExtern.so" | head -8 | sed 's/^/        /'
    echo "        真机验证：把 $DIST 拷到 $ARCH 机器（或 qemu chroot）跑 verify_in_wsl.sh"
elif command -v ldd >/dev/null; then
    OCV_MISSING="$(ldd "$DIST/EGpdf2df/libOpenCvSharpExtern.so" 2>/dev/null | awk '/not found/{print $1}' | sort -u)"
    if [ -n "$OCV_MISSING" ]; then
        fail "libOpenCvSharpExtern.so 的依赖在本机无法解析：
$(printf '         %s\n' $OCV_MISSING)
       这会让 Cv2.* 一调用就抛 'The type initializer for OpenCvSharp.Internal.NativeMethods'。
       改用 OpenCvSharp4.official.runtime.linux-x64（见 EGpdf2df.Engine.csproj 注释）。"
    fi
    echo "  OK  libOpenCvSharpExtern.so 的 soname 依赖全部可解析（ldd 无 not found）"
else
    echo "  --  无 ldd，跳过 OpenCV 依赖解析检查"
fi

# ---------------------------------------------------------------------------
# 3. 前端：Qt6 + CMake + Ninja
# ---------------------------------------------------------------------------
if [ "$KERNELS_ONLY" = 1 ]; then
    log "跳过前端与打包（--kernels-only）—— 内核产物在 $DIST"
    echo "上面已断言：三个内核均为 **$ARCH** ELF + 带 libMuPDFWrapper.so / runtimeconfig.json（+ OpenCV）"
    if [ "$ARCH" = "$HOST_ARCH" ] || { [ "$ARCH" = "x64" ] && [ "$HOST_ARCH" = "x86_64" ]; }; then
        echo "下一步（要验证**真能跑**）：./verify_in_wsl.sh"
        echo "  或直接执行：$DIST/EGpdf2excel/EGpdf2excel --help"
    else
        echo "⚠️ 本机是 $HOST_ARCH，上面的产物是交叉发布的 $ARCH —— **本机跑不了**，要真跑需："
        echo "  ① 拷到 $ARCH 机器：scp -r $DIST user@host:~/ ; 再在那边跑 verify_in_wsl.sh"
        echo "  ② 或 qemu-user-static + $ARCH chroot（见 .build_trash/wsl/ 里的做法）"
    fi
    exit 0
fi

log "构建 Qt 前端"
cmake -S "$FRONTEND" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DEGTOOLS_LINUX_DIST="$DIST" \
      "${CMAKE_QT_ARG[@]}" || fail "CMake 配置失败"
cmake --build "$BUILD_DIR" --parallel "$JOBS" || fail "前端构建失败"

# 前端产物 + 它的 runtimeconfig.json 一起进 DIST（hostfxr 内嵌调用必需）。
mkdir -p "$DIST/EGtoolsFrontend"
cp -f "$BUILD_DIR/EGtoolsFrontend" "$DIST/EGtoolsFrontend/" || fail "拷贝前端可执行文件失败"
[ -f "$BUILD_DIR/EGtoolsFrontend.runtimeconfig.json" ] || \
    fail "缺少 EGtoolsFrontend.runtimeconfig.json —— hostfxr 初始化会退用内核那份并静默失败"
cp -f "$BUILD_DIR/EGtoolsFrontend.runtimeconfig.json" "$DIST/EGtoolsFrontend/"

# ---------------------------------------------------------------------------
# 4. 前端产物断言（内核那些已在第 2 步断言过，这里只查前端）
# ---------------------------------------------------------------------------
log "断言前端产物"
is_elf "$DIST/EGtoolsFrontend/EGtoolsFrontend" || fail "前端不是目标架构（$ARCH）的 ELF
       实际 e_machine=$(elf_machine_of "$DIST/EGtoolsFrontend/EGtoolsFrontend")（期望 $ARCH_ELF）"
[ -f "$DIST/EGtoolsFrontend/EGtoolsFrontend.runtimeconfig.json" ] || fail "前端缺 runtimeconfig.json"
echo "  OK  EGtoolsFrontend（$ARCH ELF + runtimeconfig.json）"

if [ "$DO_DEB" = 0 ] && [ "$DO_RPM" = 0 ]; then
    log "跳过打包（--no-package）—— 产物在 $DIST"
    exit 0
fi

# ---------------------------------------------------------------------------
# 4. 打 deb / rpm（CPack；已装到 /opt/egtools，见 CMakeLists 的说明）
# ---------------------------------------------------------------------------
log "生成安装包"
GEN=()
[ "$DO_DEB" = 1 ] && GEN+=("DEB")
[ "$DO_RPM" = 1 ] && GEN+=("RPM")
[ "$DO_DEB" = 1 ] && ! command -v dpkg-deb >/dev/null && fail "要打 deb 但未找到 dpkg-deb"
[ "$DO_RPM" = 1 ] && ! command -v rpmbuild >/dev/null && fail "要打 rpm 但未找到 rpmbuild"

# 按本机实测反推运行库依赖（见文件上方 lib_path_of/direct_needed 的说明）。
FE_BIN="$DIST/EGtoolsFrontend/EGtoolsFrontend"
# ⚠️ 必须把**内核里的原生库**也纳入：OpenCV 链接了 GTK3 全套，只按前端反推会漏掉
#    16 个包 ⇒ 干净机上 dlopen 失败（详见上面 deb_depends_for 的注释）。
mapfile -t DEP_TARGETS < <(dep_scan_targets)
DEP_TARGETS=("$FE_BIN" "${DEP_TARGETS[@]}")
echo "  依赖反推对象（${#DEP_TARGETS[@]} 个）:"
printf '    %s\n' "${DEP_TARGETS[@]}"
DEP_ARGS=()
if command -v readelf >/dev/null; then
    if [ "$DO_DEB" = 1 ]; then
        DEB_DEP="$(deb_depends_for "${DEP_TARGETS[@]}")"
        DLOPEN_DEP="$(deb_runtime_dlopen_deps)"        # .NET dlopen 的（libicu/libssl）—— 见上方说明
        [ -n "$DLOPEN_DEP" ] && DEB_DEP="${DEB_DEP:+$DEB_DEP,}$DLOPEN_DEP"
        DEB_DEP="$(printf '%s' "$DEB_DEP" | tr ',' '\n' | grep -v '^$' | sort -u | paste -sd, -)"
        [ -n "$DEB_DEP" ] || fail "无法反推 deb 依赖（readelf/dpkg -S 没给出结果）—— 不要用空的 Depends 出包"
        echo "  deb Depends : $DEB_DEP"
        DEP_ARGS+=("-D" "CPACK_DEBIAN_PACKAGE_DEPENDS=$DEB_DEP")
    fi
    if [ "$DO_RPM" = 1 ]; then
        RPM_REQ="$(rpm_requires_for "${DEP_TARGETS[@]}")"
        RPM_DLOPEN="$(rpm_runtime_dlopen_requires)"
        [ -n "$RPM_DLOPEN" ] && RPM_REQ="${RPM_REQ:+$RPM_REQ,}$RPM_DLOPEN"
        RPM_REQ="$(printf '%s' "$RPM_REQ" | tr ',' '\n' | grep -v '^$' | sort -u | paste -sd, -)"
        [ -n "$RPM_REQ" ] || fail "无法反推 rpm 依赖"
        echo "  rpm Requires: $RPM_REQ"
        DEP_ARGS+=("-D" "CPACK_RPM_PACKAGE_REQUIRES=$RPM_REQ")
    fi
else
    fail "未找到 readelf（binutils）—— 无法反推依赖；请安装 binutils"
fi

# ⚠️⚠️ CPack 的**暂存目录必须在能保存 Unix 权限的文件系统上**：
#    构建目录若在 /mnt/c（Windows 盘 / drvfs），drvfs 不保存 Unix 权限 ⇒
#    `install(CODE)` 里的 `file(CHMOD)` 会被**静默忽略**（不报错！）⇒
#    包内内核 apphost 变成 0777（world-writable），装完虽然"能跑"（777 含 x 位），
#    但权限不对且是安全隐患。实测踩过：构建目录在 /tmp 时包里是 0755，在 /mnt/c 时是 0777。
#    所以暂存目录固定用 Linux 原生路径，产物再拷回 BUILD_DIR（保持既有引用不变）。
CPACK_STAGE="${EGTOOLS_CPACK_STAGE:-/tmp/egtools-cpack}"
rm -rf "$CPACK_STAGE"; mkdir -p "$CPACK_STAGE"

( cd "$BUILD_DIR" && cpack -G "$(IFS=';'; echo "${GEN[*]}")" \
      -D CPACK_PACKAGE_VERSION="$VERSION" \
      -D CPACK_PACKAGE_DIRECTORY="$CPACK_STAGE" \
      "${DEP_ARGS[@]}" ) \
    || fail "CPack 打包失败"

# 产物拷回构建目录（用户预期与下面的校验都在 BUILD_DIR）。
cp -f "$CPACK_STAGE"/*.deb "$CPACK_STAGE"/*.rpm "$BUILD_DIR"/ 2>/dev/null || true

# ---------------------------------------------------------------------------
# 5. 校验安装包内容（"清单式检查"必须配"事实检查"）
# ---------------------------------------------------------------------------
log "校验安装包"
shopt -s nullglob
PKGS=("$BUILD_DIR"/*.deb "$BUILD_DIR"/*.rpm)
[ ${#PKGS[@]} -gt 0 ] || fail "没有产出任何安装包"
# ⚠️ 清单先落临时文件再匹配，不要 `dpkg-deb -c | grep -q`：
#    grep -q 一命中就退出 ⇒ 给上游 tar 发 SIGPIPE ⇒ dpkg-deb 报 "tar subprocess was killed
#    by signal (Broken pipe)" 且清单被截断 ⇒ **假失败**（实测踩过）。
LIST="$(mktemp)"
for f in "${PKGS[@]}"; do
    case "$f" in
        *.deb)
            dpkg-deb -c "$f" > "$LIST" 2>/dev/null
            grep -q 'opt/egtools/EGtoolsFrontend$' "$LIST" \
                || fail "$f 里没有前端可执行文件"
            grep -q 'opt/egtools/EGtoolsFrontend.runtimeconfig.json' "$LIST" \
                || fail "$f 里缺前端 runtimeconfig.json（hostfxr 内嵌调用必需）"
            grep -q 'opt/egtools/EGpdf2excel/EGpdf2excel$' "$LIST" \
                || fail "$f 里缺内核 apphost（EGpdf2excel/EGpdf2excel）"
            # ⚠️ 权限也是交付内容：内核 apphost 必须是 0755。实测踩过 ——
            #    install(DIRECTORY) 默认不保留源权限、drvfs 又不保存权限 ⇒ 包里成了 0777。
            grep -qE '^-rwxr-xr-x .*opt/egtools/EGpdf2excel/EGpdf2excel$' "$LIST" \
                || fail "$f 里内核 apphost 权限不是 0755 —— 交付缺陷（装后可能 126，或成为 world-writable）：
       $(grep 'opt/egtools/EGpdf2excel/EGpdf2excel$' "$LIST" | tr -s ' ')"
            grep -q 'EGpdf2df/libOpenCvSharpExtern.so' "$LIST" \
                || fail "$f 里缺 OpenCV 原生库"
            grep -q 'EGpdf2df/libMuPDFWrapper.so' "$LIST" \
                || fail "$f 里缺 MuPDF 原生库"
            ;;
        *.rpm)
            rpm -qlp "$f" 2>/dev/null > "$LIST"
            grep -q 'opt/egtools/EGtoolsFrontend$' "$LIST" \
                || fail "$f 里没有前端可执行文件"
            grep -q 'EGpdf2df/libOpenCvSharpExtern.so' "$LIST" \
                || fail "$f 里缺 OpenCV 原生库"
            ;;
    esac
    printf '  OK  %s  (%s MB)\n' "$(basename "$f")" \
        "$(awk -v b="$(stat -c%s "$f")" 'BEGIN{printf "%.1f", b/1048576}')"
done
rm -f "$LIST"

log "完成"
echo "安装包："
ls -1 "$BUILD_DIR"/*.deb "$BUILD_DIR"/*.rpm 2>/dev/null || true
echo
echo "安装：sudo dpkg -i <pkg>.deb        （或 sudo rpm -ivh <pkg>.rpm）"
echo "运行：egtools                       （或 /opt/egtools/EGtoolsFrontend）"
