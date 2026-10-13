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
#   ./build_linux.sh --deb           # 只出 deb
#   QT_PREFIX=/opt/Qt/6.8.0/gcc_64 ./build_linux.sh   # 指定 Qt
#
# 依赖（构建机）：dotnet SDK 9+、cmake 3.21+、ninja 或 make、g++、Qt6 **含 Qt6Pdf**、
#                打包用 dpkg-deb（deb）/ rpmbuild（rpm）。
# ===========================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="${EGTOOLS_VERSION:-4.3.0}"
RID="linux-x64"
DIST="$ROOT/DIST"
FRONTEND="$ROOT/EGtools.Frontend"
BUILD_DIR="$FRONTEND/build-linux"
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4 )"
QT_PREFIX="${QT_PREFIX:-}"
DO_DEB=1
DO_RPM=1

while [ $# -gt 0 ]; do
    case "$1" in
        --no-package) DO_DEB=0; DO_RPM=0 ;;
        --deb)        DO_DEB=1; DO_RPM=0 ;;
        --rpm)        DO_DEB=0; DO_RPM=1 ;;
        --qt)         QT_PREFIX="$2"; shift ;;
        -h|--help)    sed -n '2,22p' "$0"; exit 0 ;;
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
command -v cmake  >/dev/null || fail "未找到 cmake"
echo "dotnet : $(dotnet --version)"
echo "cmake  : $(cmake --version | head -1)"

# Qt6 定位：显式 QT_PREFIX 优先，其次交给 cmake 自己找（发行版包会装在标准前缀）。
CMAKE_QT_ARG=()
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
# 2. 前端：Qt6 + CMake + Ninja
# ---------------------------------------------------------------------------
log "构建 Qt 前端"
cmake -S "$FRONTEND" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      "${CMAKE_QT_ARG[@]}" || fail "CMake 配置失败"
cmake --build "$BUILD_DIR" --parallel "$JOBS" || fail "前端构建失败"

# 前端产物 + 它的 runtimeconfig.json 一起进 DIST（hostfxr 内嵌调用必需）。
mkdir -p "$DIST/EGtoolsFrontend"
cp -f "$BUILD_DIR/EGtoolsFrontend" "$DIST/EGtoolsFrontend/" || fail "拷贝前端可执行文件失败"
[ -f "$BUILD_DIR/EGtoolsFrontend.runtimeconfig.json" ] || \
    fail "缺少 EGtoolsFrontend.runtimeconfig.json —— hostfxr 初始化会退用内核那份并静默失败"
cp -f "$BUILD_DIR/EGtoolsFrontend.runtimeconfig.json" "$DIST/EGtoolsFrontend/"

# ---------------------------------------------------------------------------
# 3. 断言：产出的确实是能跑的 Linux 组合（不是"构建成功但缺原生库"）
#    ⚠️ 这几条是本项目的历史教训换来的：
#      · 缺 libOpenCvSharpExtern.so → 图纸差异一碰 Cv2.* 就 DllNotFoundException
#      · 缺 libMuPDFWrapper.so      → 所有 PDF 处理直接不可用
#      · 缺 runtimeconfig.json      → 内嵌通道静默退化成命令行
# ---------------------------------------------------------------------------
log "断言产物完整性"
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
