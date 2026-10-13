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
#   ② 内核能跑   —— **真的执行**三个 CLI + ldd 查缺 .so（内核可用 Windows 侧交叉发布好的）
#   ③ 前端能编   —— Qt6（含 Qt6Pdf）编译；这一关最容易暴露发行版 Qt 版本偏低
#   ④ 能出包     —— cpack 出 deb/rpm 并核对包内容
#   ⑤ 装得上也跑得起来 —— `dpkg -i` 真装一遍，再从 /opt 里跑（**这一关最值钱**：
#                        实测靠它抓到"内核 apphost 没有 exec 位 → 126"、
#                        "自动发现把同名目录当程序"、"hostfxr 只在自己目录旁找" 三个真 bug）
#   ⑥ GUI 能显   —— WSLg 下真开窗截图 + **断言进程干净退出**（可选，--gui）
#
# 用法（在 WSL 里跑，不是 Windows）：
#   cd /mnt/c/Users/QBZ95/Desktop/tools/codes/EGtools
#   ./verify_in_wsl.sh                # ①~⑤
#   ./verify_in_wsl.sh --gui          # 加跑 ⑥（WSLg 截图）
#   ./verify_in_wsl.sh --install-deps # 缺构建依赖时自动 sudo apt install
#   TEST_PDF=/path/to/x.pdf ./verify_in_wsl.sh   # 指定测试图纸
#
# ⚠️ 没有 .NET SDK 也能跑大部分：内核可用 Windows 侧 `build_linux.sh --kernels-only`
#    交叉发布好放 DIST-linux/，本脚本会直接拿它做 ②/⑤ 的"真跑"验证（Qt 前端本来就必须在 Linux 编）。
# ===========================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$ROOT/.build_trash/wsl"
WANT_GUI=0
INSTALL_DEPS=0
# 架构：默认按**本机**（在 arm64 机器上跑就验 arm64）。交叉发布的产物不能在本机验。
ARCH="${EGTOOLS_ARCH:-native}"
mkdir -p "$LOG_DIR"

while [ $# -gt 0 ]; do
    case "$1" in
        --gui)          WANT_GUI=1 ;;
        --install-deps) INSTALL_DEPS=1 ;;
        --arch)         ARCH="$2"; shift ;;
        -h|--help)      sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
    shift
done

HOST_ARCH="$(uname -m)"
case "${ARCH}" in
    native|"") case "$HOST_ARCH" in aarch64|arm64) ARCH="arm64" ;; *) ARCH="x64" ;; esac ;;
    amd64|x86_64) ARCH="x64" ;;
    aarch64)      ARCH="arm64" ;;
esac
ELF_WANT="3e00"; [ "$ARCH" = "arm64" ] && ELF_WANT="b700"
# 与 build_linux.sh 一致：x64 产物在 DIST-linux/，arm64 在 DIST-linux-arm64/。
DIST_DEFAULT="$ROOT/DIST-linux"
[ "$ARCH" != "x64" ] && DIST_DEFAULT="$ROOT/DIST-linux-$ARCH"
DIST="${EGTOOLS_LINUX_DIST:-$DIST_DEFAULT}"

PASS=0; FAIL=0; SKIP=0
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
skip() { printf '  \033[33mSKIP\033[0m  %s\n' "$1"; SKIP=$((SKIP+1)); }
step() { printf '\n\033[36m=== %s ===\033[0m\n' "$1"; }

# root 包装：优先免密 sudo；否则用 EGTOOLS_SUDO_PW（**只从环境读，不落盘、不进脚本**）。
asroot() {
    if sudo -n true 2>/dev/null; then
        sudo -n "$@"
    elif [ -n "${EGTOOLS_SUDO_PW:-}" ]; then
        printf '%s\n' "$EGTOOLS_SUDO_PW" | sudo -S -p '' "$@"
    else
        return 127
    fi
}

# ---------------------------------------------------------------------------
step "① 环境体检"
# ---------------------------------------------------------------------------
echo "  宿主架构: $HOST_ARCH   目标架构: $ARCH   产物目录: $DIST"

# ⚠️ 产物架构与本机不符 = 跑不了（交叉发布的产物必须拿到目标机上验）。
#    这里先判，免得后面给出一堆看不懂的动态链接错误。
DIST_BIN="$DIST/EGpdf2excel/EGpdf2excel"
if [ -f "$DIST_BIN" ]; then
    GOT="$(od -An -tx1 -j18 -N2 "$DIST_BIN" 2>/dev/null | tr -d ' \n')"
    if [ "$GOT" = "$ELF_WANT" ]; then
        ok "产物架构匹配（e_machine=$GOT）"
    else
        bad "产物架构($GOT) ≠ 目标($ELF_WANT) —— 本机跑不了。交叉发布的产物要拷到 $ARCH 机器上验"
    fi
else
    echo "  （$DIST_BIN 还不存在，② 步会先生成）"
fi

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
# ⚠️ dotnet SDK **不算失败项**：内核可以在 Windows 侧交叉发布好（build_linux.sh --kernels-only），
#    本脚本会直接拿现成产物做真跑验证；Qt 前端本来就必须在 Linux 上编。
APT_LINE="sudo apt install -y cmake ninja-build g++ qt6-base-dev qt6-pdf-dev dpkg-dev rpm"
MISSING=()
command -v cmake  >/dev/null || MISSING+=("cmake")
{ command -v ninja >/dev/null || command -v make >/dev/null; } || MISSING+=("ninja-build")
command -v g++    >/dev/null || MISSING+=("g++")
command -v dpkg-deb >/dev/null || MISSING+=("dpkg-dev")
command -v readelf >/dev/null || MISSING+=("binutils")
[ -n "$QT_VER" ] || MISSING+=("qt6-base-dev qt6-pdf-dev")

if command -v dotnet >/dev/null; then
    echo "  dotnet: $(dotnet --version)（可本机发布内核）"
else
    echo "  dotnet: 未安装 —— 不碍事：内核可用 Windows 侧 build_linux.sh --kernels-only 交叉发布，"
    echo "          本脚本会直接拿 DIST-linux/ 里现成的产物做「真跑」验证。"
    echo "          （若想在本机发布内核：sudo apt install dotnet-sdk-9.0）"
fi

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
# 内核从哪来都行：本机 publish（有 dotnet SDK）或 Windows 侧交叉发布好的 DIST-linux/。
if command -v dotnet >/dev/null; then
    if bash "$ROOT/build_linux.sh" --kernels-only > "$LOG_DIR/kernels.log" 2>&1; then
        ok "三个内核 publish 成功且产物断言通过（见 .build_trash/wsl/kernels.log）"
    else
        bad "内核 publish/断言失败：tail -30 .build_trash/wsl/kernels.log"
        tail -20 "$LOG_DIR/kernels.log" | sed 's/^/        /'
    fi
elif [ -x "$DIST/EGpdf2excel/EGpdf2excel" ]; then
    ok "无 dotnet SDK，复用已有内核产物（$DIST）—— 依然会做下面的真跑验证"
else
    skip "既无 dotnet SDK 也没有现成内核产物；请在 Windows 侧先跑 build_linux.sh --kernels-only"
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

# ---- 真的执行 CLI（材料表提取：MuPDF 通道 + 写盘）----
if [ -x "$DIST/EGpdf2excel/EGpdf2excel" ]; then
    if [ -n "$TEST_PDF" ] && [ -f "$TEST_PDF" ]; then
        EXDIR="$LOG_DIR/extract"; rm -rf "$EXDIR"; mkdir -p "$EXDIR"
        if "$DIST/EGpdf2excel/EGpdf2excel" "$TEST_PDF" -o "$EXDIR" -f csv \
                > "$LOG_DIR/extract.log" 2>&1; then
            CSV="$(ls -1 "$EXDIR"/*.csv 2>/dev/null | head -1)"
            if [ -n "$CSV" ] && [ -s "$CSV" ]; then
                ok "EGpdf2excel 真跑通并产出 CSV（$(wc -l < "$CSV") 行）：$(basename "$CSV")"
            else
                # 退出 0 但没产物 = 假成功，必须报出来（别只看退出码）
                bad "EGpdf2excel 退出 0 但没产出 CSV：tail -20 .build_trash/wsl/extract.log"
            fi
        else
            bad "EGpdf2excel 提取失败：tail -20 .build_trash/wsl/extract.log"
            tail -20 "$LOG_DIR/extract.log" | sed 's/^/        /'
        fi
    else
        skip "EGpdf2excel 冒烟：没找到测试 PDF（可用 TEST_PDF=... 指定）"
    fi
else
    skip "EGpdf2excel 不存在（上一步没成功？）"
fi

# EGpdf2df 自比对：按验收铁律必须 **0 图形差异 / 0 文字差异**
if [ -x "$DIST/EGpdf2df/EGpdf2df" ] && [ -n "$TEST_PDF" ] && [ -f "$TEST_PDF" ]; then
    SELF="$LOG_DIR/self_diff.pdf"
    if "$DIST/EGpdf2df/EGpdf2df" "$TEST_PDF" "$TEST_PDF" -o "$SELF" > "$LOG_DIR/self.log" 2>&1; then
        COUNTS="$(grep -oE 'graphic=[0-9]+[[:space:]]+text=[0-9]+' "$LOG_DIR/self.log" | tail -1)"
        if printf '%s' "$COUNTS" | grep -qE 'graphic=0[[:space:]]+text=0'; then
            ok "EGpdf2df 自比对：0 图形差异 / 0 文字差异（验收铁律满足，产物 $(stat -c%s "$SELF" 2>/dev/null || echo 0) 字节）"
        else
            bad "EGpdf2df 自比对**有差异**（验收铁律要求 0）：${COUNTS:-未报出计数} —— 见 .build_trash/wsl/self.log"
        fi
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
    # --no-kernels：复用 ② 里已经拿到的内核，Qt 前端本来就必须在 Linux 上编。
    if bash "$ROOT/build_linux.sh" --no-kernels --no-package > "$LOG_DIR/frontend.log" 2>&1; then
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
DEB=""
if [ -x "$DIST/EGtoolsFrontend/EGtoolsFrontend" ]; then
    if bash "$ROOT/build_linux.sh" --no-kernels --deb > "$LOG_DIR/package.log" 2>&1; then
        DEB="$(ls -1t "$ROOT/EGtools.Frontend/build-linux"/*.deb 2>/dev/null | head -1)"
        if [ -n "$DEB" ]; then
            ok "deb 产出：$(basename "$DEB") ($(stat -c%s "$DEB") 字节)"
            # ⚠️ 清单先落文件再匹配：`dpkg-deb -c | grep -q` 会因 grep 提前退出给 tar 发 SIGPIPE
            #    ⇒ 报 "tar subprocess was killed by signal (Broken pipe)" 且清单被截断 = 假失败。
            dpkg-deb -c "$DEB" > "$LOG_DIR/deb_list.txt" 2>/dev/null
            grep -q 'opt/egtools/EGtoolsFrontend$' "$LOG_DIR/deb_list.txt" \
                && ok "deb 内含前端可执行文件" || bad "deb 内缺前端可执行文件"
            grep -q 'opt/egtools/EGpdf2excel/EGpdf2excel$' "$LOG_DIR/deb_list.txt" \
                && ok "deb 内含内核 apphost" || bad "deb 内缺内核 apphost"
            grep -q 'libOpenCvSharpExtern.so' "$LOG_DIR/deb_list.txt" \
                && ok "deb 内含 OpenCV 原生库" || bad "deb 内缺 OpenCV 原生库"
            # ⚠️ 权限也是交付内容：内核 apphost 必须是可执行的，
            #    否则装完直跑报 126（install(DIRECTORY) 默认不保留源权限，实测踩过）。
            if grep -qE '^-rwxr-xr-x .*opt/egtools/EGpdf2excel/EGpdf2excel$' "$LOG_DIR/deb_list.txt"; then
                ok "内核 apphost 权限正确（0755）"
            else
                bad "内核 apphost 不是 0755 —— 装完会 'Permission denied'(126)"
                grep 'opt/egtools/EGpdf2excel/EGpdf2excel$' "$LOG_DIR/deb_list.txt" | sed 's/^/        /'
            fi

            # ⚠️⚠️ 依赖必须覆盖**内核里原生库**的系统依赖，不能只按前端反推：
            #    libOpenCvSharpExtern.so 链接了 GTK3 全套（libgtk-3.so.0 / libcairo.so.2 /
            #    libpango / libfreetype / libX11…），dlopen 会解析全部 DT_NEEDED ⇒
            #    漏掉就只在**干净机**上炸（开发机装了 qt6-base-dev 顺带带进来，看不出来）。
            DEB_DEP_LINE="$(dpkg-deb -I "$DEB" 2>/dev/null | sed -n 's/^ Depends: //p' | tr -d ' ')"
            UNCOVERED=""
            for so in "$DIST/EGpdf2df/libOpenCvSharpExtern.so" "$DIST/EGpdf2df/libMuPDFWrapper.so"; do
                [ -f "$so" ] || continue
                while read -r n; do
                    [ -n "$n" ] || continue
                    p=""
                    for d in /usr/lib/*-linux-gnu /lib/*-linux-gnu /usr/lib64 /lib64; do
                        [ -e "$d/$n" ] && { p="$(readlink -f "$d/$n")"; break; }
                    done
                    [ -n "$p" ] || continue
                    pkg="$(dpkg -S "$p" 2>/dev/null | head -1 | cut -d: -f1)"
                    [ -n "$pkg" ] || continue
                    case ",$DEB_DEP_LINE," in *",$pkg,"*) ;; *) UNCOVERED="$UNCOVERED $n->$pkg" ;; esac
                done < <(readelf -d "$so" 2>/dev/null | awk -F'[][]' '/NEEDED/{print $2}')
            done
            if [ -z "$UNCOVERED" ]; then
                ok "deb 的 Depends 覆盖了内核原生库的系统依赖（干净机可跑）"
            else
                bad "deb 的 Depends **漏了**内核原生库需要的包（干净机上会 dlopen 失败）："
                for u in $UNCOVERED; do echo "        $u"; done
                echo "        → 检查 build_linux.sh 是否把内核 .so 也算进了依赖反推（dep_scan_targets）"
            fi
        else
            bad "cpack 报成功但没找到 .deb"
        fi
    else
        bad "打包失败：tail -20 .build_trash/wsl/package.log"
        tail -20 "$LOG_DIR/package.log" | sed 's/^/        /'
    fi
else
    skip "未产出前端，跳过打包"
fi

# ---------------------------------------------------------------------------
step "⑤ 真装一遍再从 /opt 里跑（交叉发布与"看清单"都替代不了这一关）"
# ---------------------------------------------------------------------------
# 为什么必须有这一步：下面三类问题**只有真装真跑才暴露**，实测全都踩到过 ——
#   · 内核 apphost 没有 exec 位            → execvp: Permission denied (126)
#   · 自动发现把**同名目录**当成程序       → spawn 目录后才报错
#   · hostfxr 只在自己目录旁找             → 内嵌通道静默退化成命令行
if [ -z "$DEB" ]; then
    skip "没有 deb，跳过安装验证"
elif ! asroot true 2>/dev/null; then
    skip "需要 root 才能安装。任选其一：
        · 免密 sudo：echo '$USER ALL=(ALL) NOPASSWD:ALL' | sudo tee /etc/sudoers.d/wsl-nopasswd
        · 或设环境变量（不落盘）：EGTOOLS_SUDO_PW='<密码>' ./verify_in_wsl.sh"
else
    if asroot dpkg -i "$DEB" > "$LOG_DIR/install.log" 2>&1; then
        ok "dpkg -i 安装成功（postinst 正常执行）"
    else
        bad "dpkg -i 失败：tail -20 .build_trash/wsl/install.log"
        tail -20 "$LOG_DIR/install.log" | sed 's/^/        /'
    fi

    # 桌面集成由 postinst 建软链（CPack 只支持单一前缀，桌面项只能这样补）
    [ -L /usr/bin/egtools ] || [ -x /usr/bin/egtools ] \
        && ok "/usr/bin/egtools 可用（postinst 软链）" \
        || bad "/usr/bin/egtools 不存在 —— postinst 没起作用"
    [ -e /usr/share/applications/egtools.desktop ] \
        && ok "桌面项 /usr/share/applications/egtools.desktop 就位" \
        || bad "桌面项缺失"
    [ -e /usr/share/icons/hicolor/256x256/apps/egtools.png ] \
        && ok "图标就位" || bad "图标缺失"

    # 内核 apphost 直跑（exec 位 + 依赖 + ELF 三件事一次验完）
    # ⚠️ 用 `--help` 而不是 `--version`：三个 CLI 里只有 EGpdf2excel 支持 --version，
    #    EGpdf2df / EGexcel2df 会回 "Unknown option: --version" 并 exit=2 ——
    #    拿它当 smoke 判据会把"产品正常"误判成失败（实测踩过）。
    for k in EGpdf2excel EGexcel2df EGpdf2df; do
        if [ -x "/opt/egtools/$k/$k" ]; then
            if "/opt/egtools/$k/$k" --help >/dev/null 2>&1; then
                ok "$k 装好后可直接执行（exec 位 + 依赖 OK）"
            else
                bad "$k 存在且可执行，但跑不起来（缺依赖？ldd 看 /opt/egtools/$k/$k）"
            fi
        else
            bad "/opt/egtools/$k/$k 不可执行（exec 位丢了 → 打包时 chmod 没生效）"
        fi
    done

    # 前端从安装位置启动。**故意不给 --cli-dir**：这正是要验"装完免配置就能用"。
    if /opt/egtools/EGtoolsFrontend --version > "$LOG_DIR/installed_ver.txt" 2>&1; then
        ok "装好的前端可启动：$(head -1 "$LOG_DIR/installed_ver.txt")"
    else
        bad "装好的前端启动失败"
    fi

    if [ -n "$TEST_PDF" ] && [ -f "$TEST_PDF" ]; then
        if QT_QPA_PLATFORM=offscreen timeout 300 /opt/egtools/EGtoolsFrontend \
                --test-render --pdf "$TEST_PDF" > "$LOG_DIR/installed_render.log" 2>&1; then
            ok "装好的前端能驱动内核（自动发现 + 内嵌 hostfxr）"
            grep -aE '内核|内嵌|pageWidthPt' "$LOG_DIR/installed_render.log" | head -3 | sed 's/^/        /'
        else
            bad "装好的前端驱动内核失败：tail -20 .build_trash/wsl/installed_render.log"
            tail -20 "$LOG_DIR/installed_render.log" | sed 's/^/        /'
        fi
    fi
fi

# ---------------------------------------------------------------------------
step "⑥ GUI 能否在 WSLg 下显示（可选）"
# ---------------------------------------------------------------------------
# 用**装好的那份**（不是 DIST）跑，顺带断言"进程干净退出"。
# ⚠️ 退出码是独立证据：实机踩过 —— `--screenshot` 把图存好了、正文看着一切正常，
#    但进程在**析构期段错误**（Linux 139 / Windows 0xC0000005）。只看截图会放过它。
GUI_BIN=""
[ -x /opt/egtools/EGtoolsFrontend ] && GUI_BIN="/opt/egtools/EGtoolsFrontend"
[ -z "$GUI_BIN" ] && [ -x "$DIST/EGtoolsFrontend/EGtoolsFrontend" ] && GUI_BIN="$DIST/EGtoolsFrontend/EGtoolsFrontend"

if [ "$WANT_GUI" != 1 ]; then
    skip "未加 --gui（GUI 验证会开窗，加 --gui 启用）"
elif [ -z "$GUI_BIN" ]; then
    skip "前端不存在，跳过 GUI"
elif [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    bad "没有 DISPLAY/WAYLAND_DISPLAY —— WSLg 未生效（确认 WSL2 + WSLg 可用）"
else
    echo "  DISPLAY=$DISPLAY WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}  bin=$GUI_BIN"
    if [ -n "$TEST_PDF" ] && [ -f "$TEST_PDF" ]; then
        SHOT="$LOG_DIR/wsl_gui"; rm -f "$SHOT"*.png
        # 真实开窗要走 xcb（WSLg 的 XWayland）；offscreen 不算"能显示"
        QT_QPA_PLATFORM=xcb timeout 300 "$GUI_BIN" \
            --screenshot --pdf "$TEST_PDF" --out "$SHOT" \
            > "$LOG_DIR/gui.log" 2>&1
        RC=$?
        if [ "$RC" -eq 0 ]; then
            ok "GUI 真开窗并**干净退出**（exit=0，析构期无崩溃）"
        else
            bad "GUI 退出码 $RC ≠ 0（139=段错误 / 0xC0000005=访问越界）—— 截图可能已生成但进程崩了"
        fi
        PNG="$(ls -1 "$SHOT"*window.png 2>/dev/null | head -1)"
        if [ -n "$PNG" ]; then
            ok "窗口截图已生成：$(basename "$PNG") ($(stat -c%s "$PNG") 字节)"
        else
            bad "没生成窗口截图（tail -20 .build_trash/wsl/gui.log）"
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
  → ②~⑤ 全过 = Linux 侧**已实测能跑**（不再只是"能产出"）：内核真跑 + 前端真编 + 包真出 + 真装真跑。
  → 若 ③ 挂在 Qt 版本：Ubuntu 24.04 的 Qt 是 6.4.2（本项目 Windows 侧用 6.11.2）。
     可换 25.04（Qt 6.8.3）/ 25.10（6.9.2），或装官方 Qt 6.11 后用
     QT_PREFIX=/path/to/Qt/6.11.0/gcc_64 ./build_linux.sh。
  → 装了包想撤掉：sudo dpkg -r egtools"
    exit 0
else
    echo "  → 有失败项，按上面的提示处理；日志都在 .build_trash/wsl/"
    exit 1
fi
