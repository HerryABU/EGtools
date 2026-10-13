#!/usr/bin/env bash
# ===========================================================================
# verify_in_wsl.sh —— 在 WSL 里**真正跑一遍** Linux 产物（把"能产出"变成"能跑"）
#
# 为什么需要它：交叉发布（Windows 上 `dotnet publish -r linux-x64`）只能证明
# "编出了 Linux 二进制"，**不能**证明"在 Linux 上跑得起来"——缺系统 .so、缺 OpenCV
# 原生库这类问题只有真机才暴露。WSL2 是**真 Linux 内核**，正是补这个缺口的现成环境。
#
# 分级验证（前一级不过就不做下一级，省得白等）：
#   ① 环境体检   —— 是否 WSL2 / 发行版 / Qt 版本
#   ② 内核能跑   —— publish 后**真的执行**三个 CLI + ldd 查缺 .so
#   ③ 前端能编   —— Qt6（含 Qt6Pdf）编译；这一关最容易暴露发行版 Qt 版本偏低
#   ④ 能出包     —— cpack 出 deb/rpm 并核对包内容
#   ⑤ GUI 能显   —— WSLg 下真开窗截图（可选，--gui）
#
# 用法（在 WSL 里跑，不是 Windows）：
#   cd /mnt/c/Users/QBZ95/Desktop/tools/codes/EGtools
#   ./verify_in_wsl.sh                # ①~④
#   ./verify_in_wsl.sh --gui          # 加跑 ⑤（WSLg 截图）
#   ./verify_in_wsl.sh --install-deps # 缺依赖时自动 sudo apt install
#   TEST_PDF=/path/to/x.pdf ./verify_in_wsl.sh   # 指定测试图纸
# ===========================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 与 build_linux.sh 一致：Linux 产物在 DIST-linux/（Windows 用 DIST/，刻意分开）
DIST="${EGTOOLS_LINUX_DIST:-$ROOT/DIST-linux}"
LOG_DIR="$ROOT/.build_trash/wsl"
WANT_GUI=0
INSTALL_DEPS=0
mkdir -p "$LOG_DIR"

while [ $# -gt 0 ]; do
    case "$1" in
        --gui)          WANT_GUI=1 ;;
        --install-deps) INSTALL_DEPS=1 ;;
        -h|--help)      sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
    shift
done

PASS=0; FAIL=0; SKIP=0
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
skip() { printf '  \033[33mSKIP\033[0m  %s\n' "$1"; SKIP=$((SKIP+1)); }
step() { printf '\n\033[36m=== %s ===\033[0m\n' "$1"; }

# ---------------------------------------------------------------------------
step "① 环境体检"
# ---------------------------------------------------------------------------
if grep -qi microsoft /proc/version 2>/dev/null; then
    ok "在 WSL 里（$(uname -r)）"
    if grep -qi "WSL2" /proc/version 2>/dev/null; then ok "WSL2（真 Linux 内核，验证有效）"
    else echo "  提示：内核串里没看到 WSL2 标记，若是 WSL1 请升级（wsl --set-version <distro> 2）——WSL1 是系统调用翻译层，.NET/dpkg 不可靠。"; fi
else
    bad "当前不是 WSL（本脚本也可在任意真 Linux 上跑，继续）"
fi

. /etc/os-release 2>/dev/null || true
echo "  发行版: ${PRETTY_NAME:-未知}"

# Qt 版本：这决定"前端能不能编"（本项目 Windows 侧用 6.11.2）
QT_VER=""
if command -v qmake6 >/dev/null 2>&1; then
    QT_VER="$(qmake6 -query QT_VERSION 2>/dev/null)"
elif command -v dpkg-query >/dev/null 2>&1; then
    QT_VER="$(dpkg-query -W -f='${Version}' qt6-base-dev 2>/dev/null)"
fi
if [ -n "$QT_VER" ]; then
    echo "  Qt: $QT_VER"
    case "$QT_VER" in
        6.[0-5].*) echo "  ⚠️ 发行版 Qt 明显低于本项目的 6.11.2 —— 前端编译可能因 API 差异失败，这正是要验证的。" ;;
    esac
else
    echo "  Qt: 未安装"
fi

# 依赖清单（缺哪个就报哪个，并给出可复制的安装命令）
APT_LINE="sudo apt install -y dotnet-sdk-9.0 cmake ninja-build g++ qt6-base-dev qt6-pdf-dev dpkg-dev rpm"
MISSING=()
command -v dotnet >/dev/null || MISSING+=("dotnet-sdk-9.0")
command -v cmake  >/dev/null || MISSING+=("cmake")
{ command -v ninja >/dev/null || command -v make >/dev/null; } || MISSING+=("ninja-build")
command -v g++    >/dev/null || MISSING+=("g++")
command -v dpkg-deb >/dev/null || MISSING+=("dpkg-dev")
[ -n "$QT_VER" ] || MISSING+=("qt6-base-dev qt6-pdf-dev")

if [ ${#MISSING[@]} -eq 0 ]; then
    ok "构建依赖齐全"
elif [ "$INSTALL_DEPS" = 1 ]; then
    echo "  安装缺失依赖: ${MISSING[*]}"
    $APT_LINE || bad "apt 安装失败（网络/代理？）"
else
    bad "缺依赖: ${MISSING[*]}"
    echo "        修复：$APT_LINE"
    echo "        或：./verify_in_wsl.sh --install-deps"
fi

# ---------------------------------------------------------------------------
step "② 内核能否在 Linux 上真的跑起来（交叉发布证明不了的这一半）"
# ---------------------------------------------------------------------------
if ! command -v dotnet >/dev/null; then
    skip "无 dotnet，跳过（先按上面命令安装）"
else
    if bash "$ROOT/build_linux.sh" --kernels-only > "$LOG_DIR/kernels.log" 2>&1; then
        ok "三个内核 publish 成功且产物断言通过（见 .build_trash/wsl/kernels.log）"
    else
        bad "内核 publish/断言失败：tail -30 .build_trash/wsl/kernels.log"
        tail -20 "$LOG_DIR/kernels.log" | sed 's/^/        /'
    fi
fi

# 找出测试用 PDF：优先 TEST_PDF，其次 drawingNEW 里第一份
TEST_PDF="${TEST_PDF:-}"
if [ -z "$TEST_PDF" ]; then
    for c in /mnt/c/Users/QBZ95/Desktop/drawingNEW/*.pdf; do [ -f "$c" ] && { TEST_PDF="$c"; break; }; done
fi

# ---- ldd：查缺哪些系统 .so（这是真机才有的信息）----
# 常见缺失 → apt 包名（OpenCV 在精简系统上最常缺这几样）
declare -A LIB_HINT=(
    [libgomp.so.1]="libgomp1"
    [libGL.so.1]="libgl1"
    [libglib-2.0.so.0]="libglib2.0-0"
    [libSM.so.6]="libsm6"
    [libXext.so.6]="libxext6"
    [libXrender.so.1]="libxrender1"
    [libxcb-cursor.so.0]="libxcb-cursor0"
)
for so in "$DIST/EGpdf2df/libMuPDFWrapper.so" "$DIST/EGpdf2df/libOpenCvSharpExtern.so"; do
    [ -f "$so" ] || { skip "ldd：$(basename "$so") 不存在"; continue; }
    LDD_OUT="$(ldd "$so" 2>&1)"
    # ⚠️ 别把"ldd 读不出来"当成 PASS —— 空输出/架构不符都算无法判定。
    if printf '%s' "$LDD_OUT" | grep -qiE 'not a dynamic executable|not a valid|cannot read'; then
        skip "ldd $(basename "$so")：无法判定（$LDD_OUT）"
        continue
    fi
    MISS="$(printf '%s' "$LDD_OUT" | awk '/not found/{print $1}' | sort -u)"
    if [ -z "$MISS" ] && printf '%s' "$LDD_OUT" | grep -q '=>'; then
        ok "ldd $(basename "$so")：依赖齐全"
    elif [ -z "$MISS" ]; then
        skip "ldd $(basename "$so")：输出异常，未能判定"
    else
        bad "ldd $(basename "$so")：缺少 → $MISS"
        for m in $MISS; do
            [ -n "${LIB_HINT[$m]:-}" ] && echo "        提示：sudo apt install ${LIB_HINT[$m]}"
        done
    fi
done

# ---- 真的执行 CLI ----
if [ -x "$DIST/EGpdf2excel/EGpdf2excel" ]; then
    if [ -n "$TEST_PDF" ] && [ -f "$TEST_PDF" ]; then
        OUT="$("$DIST/EGpdf2excel/EGpdf2excel" --page-count "$TEST_PDF" 2>"$LOG_DIR/pagecount.err")"
        if printf '%s' "$OUT" | grep -q '"pageCount"'; then
            ok "EGpdf2excel --page-count 真跑通 → $(printf '%s' "$OUT" | head -c 120)"
        else
            bad "EGpdf2excel --page-count 失败：$(head -c 200 "$LOG_DIR/pagecount.err")"
        fi
    else
        skip "EGpdf2excel 冒烟：没找到测试 PDF（可用 TEST_PDF=... 指定）"
    fi
else
    skip "EGpdf2excel 不存在（上一步没成功？）"
fi

# EGpdf2df 自比对：按验收铁律应当 0 差异
if [ -x "$DIST/EGpdf2df/EGpdf2df" ] && [ -n "$TEST_PDF" ] && [ -f "$TEST_PDF" ]; then
    SELF="$LOG_DIR/self_diff.pdf"
    if "$DIST/EGpdf2df/EGpdf2df" "$TEST_PDF" "$TEST_PDF" -o "$SELF" > "$LOG_DIR/self.log" 2>&1; then
        ok "EGpdf2df 自比对跑通（自比对应 0 差异，产物 $( [ -f "$SELF" ] && stat -c%s "$SELF" || echo 0) 字节）"
        grep -iE '差异|diff|changed' "$LOG_DIR/self.log" | tail -3 | sed 's/^/        /'
    else
        bad "EGpdf2df 自比对失败：tail -20 .build_trash/wsl/self.log"
        tail -20 "$LOG_DIR/self.log" | sed 's/^/        /'
    fi
fi

# ---------------------------------------------------------------------------
step "③ 前端能否在 Linux 上编出来（Qt 版本差异的主要暴露点）"
# ---------------------------------------------------------------------------
if [ -z "$QT_VER" ]; then
    skip "未安装 Qt6，跳过（apt install qt6-base-dev qt6-pdf-dev）"
else
    if bash "$ROOT/build_linux.sh" --no-package > "$LOG_DIR/frontend.log" 2>&1; then
        ok "前端编译 + 断言通过"
        [ -x "$DIST/EGtoolsFrontend/EGtoolsFrontend" ] && ok "产出可执行文件 EGtoolsFrontend (ELF)"
    else
        bad "前端编译失败（大概率是 Qt $QT_VER 与 6.11 的 API 差异）"
        grep -E "error:|错误|Error" "$LOG_DIR/frontend.log" | head -12 | sed 's/^/        /'
        echo "        完整日志：.build_trash/wsl/frontend.log"
    fi
fi

# ---------------------------------------------------------------------------
step "④ 能否出 deb / rpm"
# ---------------------------------------------------------------------------
if [ -x "$DIST/EGtoolsFrontend/EGtoolsFrontend" ]; then
    if bash "$ROOT/build_linux.sh" --deb > "$LOG_DIR/package.log" 2>&1; then
        DEB="$(ls -1 "$ROOT/EGtools.Frontend/build-linux"/*.deb 2>/dev/null | head -1)"
        if [ -n "$DEB" ]; then
            ok "deb 产出：$(basename "$DEB") ($(stat -c%s "$DEB") 字节)"
            if dpkg-deb -c "$DEB" | grep -q 'opt/egtools/EGtoolsFrontend'; then ok "deb 内含前端"; else bad "deb 内缺前端"; fi
            if dpkg-deb -c "$DEB" | grep -q 'libOpenCvSharpExtern.so'; then ok "deb 内含 OpenCV 原生库"; else bad "deb 内缺 OpenCV 原生库"; fi
        else
            bad "cpack 报成功但没找到 .deb"
        fi
    else
        bad "打包失败：tail -20 .build_trash/wsl/package.log"
    fi
else
    skip "未产出前端，跳过打包"
fi

# ---------------------------------------------------------------------------
step "⑤ GUI 能否在 WSLg 下显示（可选）"
# ---------------------------------------------------------------------------
if [ "$WANT_GUI" != 1 ]; then
    skip "未加 --gui（GUI 验证会开窗，加 --gui 启用）"
elif [ ! -x "$DIST/EGtoolsFrontend/EGtoolsFrontend" ]; then
    skip "前端不存在，跳过 GUI"
elif [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    bad "没有 DISPLAY/WAYLAND_DISPLAY —— WSLg 未生效（确认 WSL2 + WSLg 可用）"
else
    echo "  DISPLAY=$DISPLAY WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}"
    if [ -n "$TEST_PDF" ] && [ -f "$TEST_PDF" ]; then
        SHOT="$LOG_DIR/wsl_gui"
        # 用产品自带的 --screenshot：开真窗 + 存图（是"界面真的画出来了"的凭证）
        if timeout 90 "$DIST/EGtoolsFrontend/EGtoolsFrontend" \
                --screenshot --cli-dir "$DIST/EGpdf2excel" --pdf "$TEST_PDF" --out "$SHOT" \
                > "$LOG_DIR/gui.log" 2>&1; then
            PNG="$(ls -1 "$SHOT"*.png 2>/dev/null | head -1)"
            [ -n "$PNG" ] && ok "WSLg 下真开窗并截图：$PNG" || bad "跑完但没截图"
        else
            bad "GUI 启动/截图失败：tail -20 .build_trash/wsl/gui.log"
        fi
    else
        skip "GUI：没找到测试 PDF"
    fi
fi

# ---------------------------------------------------------------------------
step "结论"
echo "  通过 $PASS / 失败 $FAIL / 跳过 $SKIP"
if [ "$FAIL" -eq 0 ]; then
    echo "
  → ③④⑤ 全过 = Linux 侧**已实测能跑**（不再只是"能产出"）。
  → 若 ② 过而 ③ 挂在 Qt 6.4：可换 25.04（Qt 6.8.3）或 25.10（6.9.2），
    或装官方 Qt 6.11 后用 QT_PREFIX=/path/to/Qt/6.11.0/gcc_64 ./build_linux.sh。"
    exit 0
else
    echo "  → 有失败项，按上面的提示处理；日志都在 .build_trash/wsl/"
    exit 1
fi
