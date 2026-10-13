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
#   ./build_linux.sh --deb           # 只出 deb
#   QT_PREFIX=/opt/Qt/6.8.0/gcc_64 ./build_linux.sh   # 指定 Qt
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
RID="linux-x64"
# ⚠️ 输出目录**按平台隔离**：Windows 的 build_all.ps1 用 DIST/，Linux 用 DIST-linux/。
#    两者共用同一个 DIST/ 会互相覆盖（Windows 的 .exe 与 Linux 的无后缀 ELF 同名），
#    且做完 Linux 验证后 Windows 自检/打包会拿到错的二进制。
DIST="${EGTOOLS_LINUX_DIST:-$ROOT/DIST-linux}"
FRONTEND="$ROOT/EGtools.Frontend"
BUILD_DIR="$FRONTEND/build-linux"
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4 )"
QT_PREFIX="${QT_PREFIX:-}"
DO_DEB=1
DO_RPM=1
KERNELS_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --no-package)   DO_DEB=0; DO_RPM=0 ;;
        --deb)          DO_DEB=1; DO_RPM=0 ;;
        --rpm)          DO_DEB=0; DO_RPM=1 ;;
        # 只出内核（不碰 Qt）：用于"先证明 .NET 侧在 Linux 上真的能跑"，
        # 也方便在没装 Qt6 的机器/容器里先做一轮验证。
        --kernels-only) KERNELS_ONLY=1; DO_DEB=0; DO_RPM=0 ;;
        --qt)           QT_PREFIX="$2"; shift ;;
        -h|--help)      sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
    shift
done

log()  { printf '\n=== %s ===\n' "$*"; }
fail() { printf '\n[FAIL] %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 0. 前置检查（缺什么先报清楚，别等构建到一半才炸）
# ---------------------------------------------------------------------------
log "前置检查"
command -v dotnet >/dev/null || fail "未找到 dotnet（需要 .NET SDK 9+）"
echo "dotnet : $(dotnet --version)"

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
log "发布 .NET 内核（$RID, self-contained）"
PROJECTS=(EGpdf2excel EGexcel2df EGpdf2df)
for p in "${PROJECTS[@]}"; do
    echo "--- dotnet publish $p ---"
    dotnet publish "$ROOT/$p/$p.csproj" \
        -c Release -r "$RID" --self-contained true \
        -p:PublishTrimmed=false \
        -o "$DIST/$p" || fail "$p 发布失败"
done

# 交付不带符号文件（与 Windows 侧一致）。
find "$DIST" -maxdepth 2 -name '*.pdb' -delete 2>/dev/null || true

# ---------------------------------------------------------------------------
# 2. 内核产物断言（**必须在内核 publish 之后立刻做**，这样 --kernels-only
#    这个"先证明 .NET 侧能跑"的档位也带着断言，而不是空跑一趟）
#    ⚠️ 这几条是历史教训换来的：
#      · 缺 libOpenCvSharpExtern.so → 图纸差异一碰 Cv2.* 就 DllNotFoundException
#      · 缺 libMuPDFWrapper.so      → 所有 PDF 处理直接不可用
#      · 缺 runtimeconfig.json      → 内嵌通道静默退化成命令行
# ---------------------------------------------------------------------------
log "断言内核产物"
is_elf() { [ -f "$1" ] && [ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; }

for p in "${PROJECTS[@]}"; do
    is_elf "$DIST/$p/$p" || fail "$p 不是可执行 ELF（实际：$(file -b "$DIST/$p/$p" 2>/dev/null || echo 缺失)）"
    [ -e "$DIST/$p/libMuPDFWrapper.so" ] || fail "$p 缺少 libMuPDFWrapper.so（MuPDF 原生库）"
    [ -f "$DIST/$p/$p.runtimeconfig.json" ] || fail "$p 缺少 runtimeconfig.json"
    echo "  OK  $p（ELF + libMuPDFWrapper.so + runtimeconfig.json）"
done
# ⚠️ OpenCV 原生库只在 EGpdf2df 里需要（图纸对齐/图形差异）。
[ -e "$DIST/EGpdf2df/libOpenCvSharpExtern.so" ] || \
    fail "EGpdf2df 缺少 libOpenCvSharpExtern.so ——
       检查 EGpdf2df.Engine.csproj 里的 OpenCvSharp4.runtime.linux-x64 引用是否还在"
echo "  OK  EGpdf2df/libOpenCvSharpExtern.so（OpenCV 原生库）"

# ---------------------------------------------------------------------------
# 3. 前端：Qt6 + CMake + Ninja
# ---------------------------------------------------------------------------
if [ "$KERNELS_ONLY" = 1 ]; then
    log "跳过前端与打包（--kernels-only）—— 内核产物在 $DIST"
    echo "上面已断言：三个内核均为 ELF + 带 libMuPDFWrapper.so / runtimeconfig.json（+ OpenCV）"
    echo "下一步（要验证**真能跑**）：./verify_in_wsl.sh"
    echo "  或直接执行：$DIST/EGpdf2excel/EGpdf2excel --page-count <pdf>"
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
is_elf "$DIST/EGtoolsFrontend/EGtoolsFrontend" || fail "前端不是 ELF"
[ -f "$DIST/EGtoolsFrontend/EGtoolsFrontend.runtimeconfig.json" ] || fail "前端缺 runtimeconfig.json"
echo "  OK  EGtoolsFrontend（ELF + runtimeconfig.json）"

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

( cd "$BUILD_DIR" && cpack -G "$(IFS=';'; echo "${GEN[*]}")" -D CPACK_PACKAGE_VERSION="$VERSION" ) \
    || fail "CPack 打包失败"

# ---------------------------------------------------------------------------
# 5. 校验安装包内容（"清单式检查"必须配"事实检查"）
# ---------------------------------------------------------------------------
log "校验安装包"
shopt -s nullglob
PKGS=("$BUILD_DIR"/*.deb "$BUILD_DIR"/*.rpm)
[ ${#PKGS[@]} -gt 0 ] || fail "没有产出任何安装包"
for f in "${PKGS[@]}"; do
    case "$f" in
        *.deb)
            dpkg-deb -c "$f" | grep -q '/opt/egtools/EGtoolsFrontend$' || fail "$f 里没有前端"
            dpkg-deb -c "$f" | grep -q 'EGpdf2df/libOpenCvSharpExtern.so' || fail "$f 里缺 OpenCV 原生库"
            ;;
        *.rpm)
            rpm -qlp "$f" 2>/dev/null | grep -q '/opt/egtools/EGtoolsFrontend$' || fail "$f 里没有前端"
            ;;
    esac
    printf '  OK  %s  (%.1f MB)\n' "$(basename "$f")" "$(echo "scale=1; $(stat -c%s "$f")/1048576" | bc)"
done

log "完成"
echo "安装包："
ls -1 "$BUILD_DIR"/*.deb "$BUILD_DIR"/*.rpm 2>/dev/null || true
echo
echo "安装：sudo dpkg -i <pkg>.deb        （或 sudo rpm -ivh <pkg>.rpm）"
echo "运行：egtools                       （或 /opt/egtools/EGtoolsFrontend）"
