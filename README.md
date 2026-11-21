# EGtools

**工程图纸材料表提取与对比工具集** · A toolkit for extracting and comparing engineering drawing material tables

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)
[![CI](https://github.com/HerryABU/EGtools/actions/workflows/ci.yml/badge.svg)](https://github.com/HerryABU/EGtools/actions/workflows/ci.yml)
![Platform: Windows x64 / arm64](https://img.shields.io/badge/Platform-Windows%20x64%20%7C%20arm64-0078D4)
![Platform: Linux x64 / arm64](https://img.shields.io/badge/Platform-Linux%20x64%20%7C%20arm64-FCC624)

EGtools 面向 **CAD 等轴测（isometric）PDF 图纸**与**管道材料表（Excel）**，把"人工翻图、抄表、逐个比对版本"变成一条可复现的流水线。它采用 **三层架构**：**Qt 图形前端只做壳**（交互与编排），**全部 PDF / Excel 实际处理**都下沉到**独立的 .NET CLI 内核**，两者通过 **JSON / 文件契约**完全解耦 —— 因此内核既能被界面驱动，也能在脚本、CI、批处理里单独使用。

```text
        ┌─────────────────────────────┐        ┌──────────────────────────────┐
        │  EGtools.Frontend  (Qt 6)   │        │   三个 .NET CLI 内核（独立可用） │
        │  ─────────────────────────  │        │  ──────────────────────────  │
        │  画布 / 框选 / 差异清单      │  契约  │  EGpdf2excel  PDF → 材料表      │
        │  面板 / 主题 / 方案          │ ─────► │  EGexcel2df   Excel 版本对比    │
        │  PDFium 矢量预览             │ JSON   │  EGpdf2df     PDF 图纸差异      │
        └─────────────────────────────┘        └──────────────────────────────┘
                  内核调用融合层：① 内嵌调库（hostfxr）② 命令行（QProcess）
```

---

## 目录

- [功能特性](#功能特性)
- [平台支持](#平台支持)
- [组件一览](#组件一览)
- [安装](#安装)
- [图形前端使用](#图形前端使用)
- [CLI 内核参考](#cli-内核参考)
- [构建](#构建)
- [自检与验证](#自检与验证)
- [环境变量](#环境变量)
- [配置外部化](#配置外部化)
- [架构与关键约定](#架构与关键约定)
- [故障排查](#故障排查)
- [仓库结构](#仓库结构)
- [版本号](#版本号)
- [许可证](#许可证)

---

## 功能特性

### 1. 材料表提取（EGpdf2excel）

- **直接读取可编辑矢量文本，不做 OCR**：CAD 直出 PDF 里的文字是可复制的矢量字形，本工具解析其**字形几何**而不是把页面当图片识别。矢量可用时结果精确到字符；**仅在区域内确实没有矢量文字**时才用内核自带的 Tesseract（MuPDF 内置）兜底，并在结果里用 `ocrUsed` 标记。
- **表格分列完全由字形几何涌现**：行按 y 聚类、列按「墨盒水平间隙」切分，**不硬编码任何列坐标** ⇒ 换图纸版式、换项目不需要改代码，只有需要时用配置微调。
- **空格重建**：MuPDF 结构化文本**不携带空格**，工具用字形包围盒间距比例判断词边界（词内≈0、词间≈0.28×字号），并可给一份参考工作簿用其 `DESCRIPTION` 词表校准 → 无空格 PDF 的描述能还原成正常可读文本。
- **管线号识别是纯字符状态机**，不用正则、不按坐标猜、不剥离后缀：点分 ≥3 段 + ≥2 段纯数字 + 恰好 1 段「字母+数字」P 段，再叠加全页频率统计，且**原样返回**（`401.102.P513_TP` 不会被截断成 `401.102`）。
- **区域框选提取**：只提取你框出的区域（全域表 / 细表 / 材料编号区三种语义），支持"图中管线号 ↔ 材料表"**一一对应核对**。
- **批量与并发**：一次给多个文件 / 目录 / 通配符，多线程并发且计数准确。
- 输出 **CSV / XLSX / 两者**；组件列可合并进描述（8 列）或独立成列（9 列）。

### 2. 图纸差异（EGpdf2df）

- **图形与文字两路独立**，避免互相误报：
  - **图形差异走纯矢量检测**（默认）——几何标定 + 共线覆盖匹配（沿线采样，平行 ≤12° 且距离 ≤容差），不用像素比对；仍保留 `--graphic-method pixel` 作为对照；
  - **文字差异 = 内容流逐字形重绘**——自研字节级分词器，**从不解析字体**，把命中差异框的字形从原内容流里抽出来自包含重绘并 `RehostFonts` 复用**原 PDF 自带字形**（所以**不需要任何系统字体**，中文/土耳其语等字形照常标注）。
- **按真图号配对**，不按页码：每页标题栏都印同一个项目号，按页码配会全错；工具按**管段号**（如 `408.101.P02`）配对，只在一侧的判为新增/删除，无图号页用**线画指纹**兜底。
- **三态标注**：删除 / 新增 / **变更**（未匹配段在变更半径内有同向对侧未匹配段 ⇒ 判为"变更"，用第三色标出）。
- 输出**带红色标注的 PDF**（保留原图矢量，另叠加矢量标注层）+ JSON 摘要；颜色、线宽、透明度、标注样式、判定阈值全部可配。

### 3. Excel 版本对比（EGexcel2df）

- 以 `PIPE NO` 为主键分组；组内以 `TABLE + DN(mm) + 项目/DESCRIPTION + NO` 为项目唯一键。
- 输出：整条 PIPE 新增/删除、项目新增/删除、`QTY` 变化（±N）；`QTY` 相同不输出。
- 结果写成多工作表 Excel。

### 4. 图形前端（EGtools.Frontend）

- **PDFium 矢量预览**：默认 **CPU 光栅**、异步分块渲染 + 缓存 + 渐进式细化，缩放/滚动不糊；GPU 可选，且在 GL 版本不足（远程桌面 / 虚拟机的 GDI Generic 1.1）时**自动回退光栅**（避免"一块黑画布"）。
- **框选方案可复用**：把一组框选存成具名方案，下次一键复用到画布。
- **双布局**：侧栏式 / 竖向堆叠式；三个面板（导航栏 / 缩略图栏 / 结果面板）都能折叠并记忆尺寸。
- **深/浅色主题**、批量提取进度与实时日志、差异清单**点一行即跳转并高亮**。

---

## 平台支持

| 平台 | 形态 | 状态 |
| --- | --- | --- |
| **Windows 10 1809+ / 11（x64）** | `installer\EGtools-<版本>-x64.exe`（Inno Setup），或免安装复制 `DIST\` | ✅ 本机构建 + 全量自检回归 |
| **Windows on ARM（arm64）** | `installer\EGtools-<版本>-arm64.exe`；在 x64 上**交叉编译**产出 | ⚠️ 已产出并通过架构/依赖自证；**未在 arm64 真机上运行过** |
| **Linux x64** | `deb` / `rpm`（装到 `/opt/egtools`，软链 `/usr/bin/egtools`） | ✅ 在 WSL2（真 Linux 内核）**真装真跑**通过（28 项验证） |
| **Linux arm64** | 内核可交叉发布；**deb/rpm 需在 arm64 环境里打** | ⚠️ 内核已在 qemu + arm64 chroot 里**真跑**通过；arm64 安装包未产出 |

> Windows 的 x64 ↔ arm64 模拟是**单向的**（只有 arm64 能跑 x64），所以 x64 机器上无法"真启动"arm64 产物——那部分用架构断言 + 依赖闭环替代，构建脚本会在结论里明确标注"未真机验证"。

---

## 组件一览

| 组件 | 类型 | 作用 |
| --- | --- | --- |
| **EGpdf2excel** | CLI 内核 | 从 CAD 等轴测 PDF **直接读取可编辑矢量文本**（无 OCR），提取材料表 → CSV / XLSX；支持区域框选提取与页面栅格化渲染。 |
| **EGexcel2df** | CLI 内核 | 对比两份图纸 Excel（旧 vs 新），以 `PIPE NO` 为主键输出变化清单。 |
| **EGpdf2df** | CLI 内核 | 对比两份图纸 PDF（baseline vs target），输出**带红色差异标注**的新 PDF + JSON 摘要。 |
| **EGtools.Core** | 共享库 | 日志转发（`ForwardingWriter`）+ 内嵌内核服务端（`EmbeddedKernel`）+ 文本几何。 |
| **EGtools.Frontend** | Qt 6 应用 | **唯一 GUI**。打开 PDF → 框选/批处理 → 调内核提取与比对 → 预览差异。 |

**引擎一致性**：PDF 解析统一用 [MuPDFCore](https://www.nuget.org/packages/MuPDFCore)（MuPDF 的 .NET 绑定，与 Python / Node / C++ 参考实现同源，中文与多语言文本均正确解析）；图像与差异计算用 [OpenCvSharp4](https://www.nuget.org/packages/OpenCvSharp4)；PDF 写出用 iText7。

---

## 安装

### Windows（安装包，推荐）

运行 `installer\EGtools-<版本>-x64.exe`：

- 默认装到 `Program Files\EGtools`（勾选 / 带 `/CURRENTUSER` 时可装成当前用户的单用户安装）；
- 自动创建开始菜单项与桌面快捷方式，并把随附文档装到 `<安装目录>\docs`；
- 卸载走「应用和功能」或安装目录里的 `unins000.exe`。

> 安装包按架构分开：**arm64 包在 x64 机器上会被直接拒绝安装**（架构标识不符），反之亦然，不会装错。

### Windows（免安装）

把 `DIST\` 整个复制到目标机器即可，双击 `DIST\EGtoolsFrontend\EGtoolsFrontend.exe`。三个内核是 **.NET 9 自包含**发布，Qt 运行库与 VC++ 运行库都已随目录分发 ⇒ **目标机无需安装 .NET、Qt 或 VC++ 运行库**。

### Linux（deb / rpm）

```bash
sudo dpkg -i egtools-<版本>-Linux.deb     # Debian / Ubuntu
sudo rpm  -i egtools-<版本>-Linux.rpm     # Fedora / RHEL
```

- 装到 `/opt/egtools`，并创建 `/usr/bin/egtools` 软链、桌面项与图标。
- 依赖已按**本发行版实测**反推（Qt6 运行库 + OpenCV 所需系统库 + `libicu` / `libssl`），在**干净的 Ubuntu Server** 上装完即可运行。
- 卸载：`sudo dpkg -r egtools` 或 `sudo rpm -e egtools`。

> ⚠️ **只有 `EGpdf2excel` 支持 `--version`**；`EGpdf2df` 与 `EGexcel2df` 会回 `Unknown option` 并退出码 2 —— 脚本里探活请用 `--help`。

---

## 图形前端使用

启动后左侧是导航树（可用 `Ctrl+B` 折叠）：

| 导航项 | 说明 |
| --- | --- |
| **首页** | 入口卡片：打开 PDF 框选提取 / 整页材料表批量抽取。同时显示**当前内核状态**（内嵌可用？hostfxr 取自哪里？）与渲染路径（GPU / CPU 光栅）。 |
| **材料表提取** | 批量提取工作台：加入多个 PDF、设格式与列布局、看进度与日志。 |
| **PDF 工具 → 渲染与框选提取** | 主工作区：画布 + 三模式框选 + 导出区域 JSON + 调内核提取，字框回叠到画布。 |
| **PDF 工具 → 材料表 OCR 增强** | 同一工作区、以 OCR 兜底模式运行（区域无矢量文字时启用）。 |
| **图纸比对** | 选基线与目标 PDF → 调 `EGpdf2df` → 差异清单 + 内嵌预览红标注图；点清单任意一行跳到并高亮该差异。 |
| **Excel 比对** | 选两份 Excel → 调 `EGexcel2df` 生成变化清单。 |
| **框选方案** | 管理具名框选方案（存于 `~/Documents/EGtools/schemes.json`），一键复用/重写。 |

### 快捷键

| 快捷键 | 作用 |
| --- | --- |
| `Ctrl+B` | 折叠 / 展开导航栏（循环 180 / 220 / 280 px） |
| `Ctrl+Shift+B` | 折叠 / 展开缩略图栏（140 / 180 / 240 px） |
| `Ctrl+J` | 折叠 / 展开结果面板（160 / 240 / 360 px） |
| `Ctrl+1` / `Ctrl+2` | 切换画布视图（单页 / 连续） |
| `F1` | 打开使用说明书 |

> 全应用**分割条一律不可拖拽**（面板宽度用预设 / 快捷键 / 折叠切换），避免误拖把版面拖乱。

### 框选三种语义

| 语义 | `kind` | 用途 |
| --- | --- | --- |
| 全域表 | `global_table` | 整张材料表 |
| 细表 | `detail_table` | 子表 / 明细块 |
| 材料编号区 | `material_no_region` | 图纸中管道/管线编号出现的区域 |

---

## CLI 内核参考

三个内核都可**完全独立运行**；stdout 输出 JSON，stderr 输出日志。被前端调用时走同一条契约（内嵌通道通过内存传递，命令行通道落临时文件）。

### EGpdf2excel — PDF 材料表抽取 / 区域提取 / 页面渲染

```bash
EGpdf2excel [inputs...] [options]

# 整页表格抽取 → CSV（默认）
EGpdf2excel drawing.pdf
EGpdf2excel "C:/docs/*.pdf" -o out/ -f xlsx
EGpdf2excel -v *.pdf -f both -r ref.xlsx -o out/

# 页面栅格化渲染（输出 BMP + JSON 摘要，供前端显示）
EGpdf2excel --render --page 0 --scale 2 --render-out page0.bmp drawing.pdf

# 区域框选提取（请求 JSON 可来自文件或 stdin）
EGpdf2excel --mode region --json request.json
echo '{...}' | EGpdf2excel --mode region --stdin
```

| 选项 | 说明 |
| --- | --- |
| `inputs` | PDF 文件、目录或通配符；省略时默认当前目录 `*.pdf`。 |
| `-o, --output DIR\|FILE` | 输出目录；单输入时可为单个输出文件（扩展名由 `--format` 决定）。默认与各输入同目录。 |
| `-f, --format csv\|xlsx\|both` | 输出格式（默认 `csv`）。 |
| `-r, --ref FILE.xlsx` | 参考工作簿；用其 `DESCRIPTION` 词表重建无空格 PDF 的描述分词空格。 |
| `--config FILE.json` | 抽取配置（词表 / 列坐标 / 页面区间）；缺省读 exe 同级 `config.json`，再退回内置默认。 |
| `-G, --group-header embed\|omit` | 表内分组小标题（FITTINGS / VALVES / FLANGES…）处理：`embed`（默认，前缀到每条描述）/ `omit`。 |
| `-C, --component merged\|separate` | 组件列布局：`merged`（默认，并入描述，8 列）/ `separate`（独立列，9 列）。 |
| `--tag NAME` | 输出文件名标签（默认 `V3`）→ `<pdf>_<tag>.csv/.xlsx`。 |
| `--render / --page N / --scale S / --render-out F.bmp` | 页面栅格化渲染。 |
| `--mode region / --region / --json / --stdin` | 区域框选提取模式。 |
| `-v, --verbose` | 每页进度与警告输出到 stderr。 |
| `--version` | 显示版本。 |
| `-h, --help` | 显示帮助。 |

**区域提取 JSON 契约**（`RegionRequest`，字段名大小写不敏感）：

```json
{
  "pdf": "C:/docs/drawing.pdf",
  "pages": [0],
  "regions": [
    { "page": 0, "kind": "global_table",       "bbox": {"x":80,"y":60,"width":300,"height":220} },
    { "page": 0, "kind": "material_no_region", "bbox": {"x":40,"y":40,"width":400,"height":60} }
  ],
  "options": { "ocrFallback": true, "mergeOverlapping": true }
}
```

内核返回 `RegionResponse`：每个区域含 `kind`、`ocrUsed`、`pipeNumbers`（编号区）、`table`（表格区的行×列网格）、`words`（区域内全部字框，供叠加 / 调试）。

### EGpdf2df — 两份 PDF 图纸差异

```bash
EGpdf2df baseline.pdf target.pdf -o diff.pdf
EGpdf2df --json request.json        # 或 --stdin
```

| 选项 | 说明 |
| --- | --- |
| `-o, --output F` | 标注 PDF 输出路径。 |
| `-p, --pages L` | 限定比对页；默认 `filter` 语义 = 先按图号全量配对、再筛出指定页。 |
| `--page-mode filter\|pin` | `-p` 的语义：`filter`（默认）/ `pin`（把范围页视为"只比这几页"）。`--pin-pages` 等价于后者。 |
| `--graphic-method vector\|pixel` | 图形差异方法：`vector`（默认，纯矢量检测）/ `pixel`（渲染 + OpenCV 比对）。 |
| `--mark-style semi\|solid\|outline` | 图形标注样式（默认 `semi`：半透明填充 + 彩色描边）。 |
| `--mark-stroke-color #RRGGBB` | 图形标注颜色（默认自动取对比色）。 |
| `--stroke-width PT` / `--stroke-opacity A` | 矢量标注线宽（默认 1.0 pt）/ 不透明度（默认 0.55）。 |
| `--vector-tol PT` | 矢量线段判定容差（默认 3.0 pt）。 |
| `--near-ink-tol PT` | 墨迹重叠容差（默认 0.5 pt）。 |
| `--report-mode cluster\|run` | 图形差异报告方式（默认 `cluster`）。 |
| `--text-added-color` / `--text-removed-color #RRGGBB` | 新增（默认 `#FF0000` 红）/ 删除（默认 `#0000FF` 蓝）文字标注色。 |
| `--text-insert-direction down\|up\|right\|left` | 新增文字的插入方向（默认 `down`）。 |
| `--config FILE` | 差异配置（pdf2df config JSON）。 |
| `--json FILE` / `--stdin` | 从文件 / 标准输入读取 JSON 契约。 |
| `-h, --help` | 显示帮助。 |

**验收口径（本项目自用的铁律）**：同一份 PDF 自己跟自己比，必须 **0 图形差异 / 0 文字差异**；否则说明比对链路引入了伪差异。

### EGexcel2df — 两份 Excel 图纸对比

```bash
EGexcel2df old.xlsx new.xlsx -o changes.xlsx
```

| 选项 | 说明 |
| --- | --- |
| `-o, --output <文件>` | 输出 Excel 路径（默认 `图纸变化清单_<时间戳>.xlsx`）。 |
| `--sheet1 <名称\|序号>` | 旧图纸读取的工作表（默认第 1 个）。 |
| `--sheet2 <名称\|序号>` | 新图纸读取的工作表（默认第 1 个）。 |
| `--json <文件>` / `--locate` | 定位模式：在指定工作表的指定列里查单元格字符串。 |
| `-v, --version` | 显示版本。 |
| `-h, --help` | 显示帮助。 |

---

## 构建

### 先决条件

| 项目 | Windows | Linux |
| --- | --- | --- |
| .NET SDK | **9 或 10** | **9+**（无 SDK 时可复用 Windows 侧交叉发布的内核） |
| Qt | **6.11.2 `msvc2022_64`**（arm64 用 `msvc2022_arm64`） | Qt6 **含 `Qt6Pdf`**（Ubuntu 24.04 为 6.4.2） |
| 编译器 | VS 2022 / 2026（「使用 C++ 的桌面开发」）；arm64 交叉需带 `Hostx64\arm64` 工具链的 MSVC + arm64 VC 运行库 | `g++` |
| 构建系统 | Ninja + CMake ≥ 3.21（VS 自带 CMake 亦可） | cmake ≥ 3.21 + ninja 或 make |
| 打包 | Inno Setup 6 | `dpkg-deb`（deb）/ `rpmbuild`（rpm） |
| 其他 | 按 [`redist/README.md`](redist/README.md) 补齐 `redist/vc143_x64/`（VC++ 运行库，不入库） | — |

### Windows x64（一键）

```powershell
# 首次构建前先按 redist/README.md 放入 VC++ 运行库
.\build_all.ps1                 # 内核 + Qt 前端 + 安装包
.\build_all.ps1 -Clean          # 顺带把 obj/bin 移入 .build_trash 强制全量
```

脚本流程：清理/还原全部项目 → 构建 Core 与三个 Engine DLL → **自包含发布**三个 CLI 到 `DIST\` → 构建 Qt 前端到 `EGtools.Frontend\build` 并部署到 `DIST\EGtoolsFrontend` → 拷 Qt 运行库 / 插件 / VC++ 运行库 → **两级自证** → Inno Setup 出包 → 校验体积。

`build_all.ps1` 会做**逐项自证**，任何一项不过就直接 `throw` 中止（避免打出用旧二进制或缺文件的包）：

1. **依赖闭环**：凡存在于我们 Qt / redist 来源目录里的依赖，都必须出现在发布目录（抓"忘拷某个 Qt 模块"，典型症状 `0xC0000135` 缺 DLL）；
2. **架构一致性**：PE 头逐个读 `e_machine`，全部须等于目标架构（托管程序集的 `IL` 视为合法）；**架构不符且无人导入** ⇒ 剔除，**被导入** ⇒ 硬失败；
3. **真启动发布产物**：真的把 `DIST\EGtoolsFrontend\EGtoolsFrontend.exe` 跑起来（故意不给 PATH 加 Qt 目录，模拟干净机器）并断言 `--version` 含 `Release`；
4. 校验 `CMAKE_BUILD_TYPE` 确为 Release（Ninja 是单配置生成器，必须显式）。

### Windows arm64（交叉编译）

```powershell
.\build_all.ps1 -Arch arm64     # → DIST-arm64\ 与 installer\EGtools-<版本>-arm64.exe
```

- Qt 用 `msvc2022_arm64`（官方 **cross_compiled** 版，**含 Qt6Pdf**）；CMake 走 Qt 的 `qt.toolchain.cmake`；
- 内核加 `-r win-arm64` 并**额外引用** `OpenCvSharp4.runtime.win-arm64`（`OpenCvSharp4.runtime.win` 只有 `win-x64`，不加就"发布成功、一碰 OpenCV 就崩"）；
- arm64 跳过"真启动"自证（本机跑不了），改由架构断言 + 依赖闭环替代，并在结论里明确标注**未真机验证**。

### Linux

```bash
./build_linux.sh                  # 内核 + 前端 + deb + rpm
./build_linux.sh --no-package     # 只构建
./build_linux.sh --kernels-only   # 只出内核（不碰 Qt）
./build_linux.sh --deb            # 只出 deb（同理 --rpm）
./build_linux.sh --no-kernels     # 复用现成内核，只编前端/打包（构建机无需 .NET SDK）
QT_PREFIX=/opt/Qt/6.8.0/gcc_64 ./build_linux.sh   # 指定 Qt

# 交叉发布 arm64 内核（在 x64 上即可）
./build_linux.sh --arch arm64 --kernels-only      # → DIST-linux-arm64/
```

Ubuntu 24.04 依赖：

```bash
sudo apt install dotnet-sdk-9.0 cmake ninja-build g++ qt6-base-dev qt6-pdf-dev dpkg-dev rpm
```

> ⚠️ **Qt 前端不能交叉编译**（需要 aarch64 的 Qt6 + Qt6Pdf）⇒ arm64 的前端与 deb/rpm 必须在 arm64 环境（真机，或 qemu + arm64 chroot）里构建。在 x64 上跑 `--arch arm64` 而不带 `--kernels-only` 会**明确报错退出**，不会静默产出错架构。
>
> ⚠️ `DIST-linux/`（Linux x64）、`DIST-linux-arm64/`、`DIST/`（Windows x64）、`DIST-arm64/` 四个输出目录**刻意分开**——否则同名产物互相覆盖。

### 单独构建

```powershell
# 单个 CLI 内核（自包含）
dotnet publish EGpdf2excel/EGpdf2excel.csproj -c Release -r win-x64 --self-contained true
dotnet publish EGpdf2df/EGpdf2df.csproj       -c Release -r win-x64 --self-contained true

# Qt 前端（cmd / Developer PowerShell）
cd EGtools.Frontend
.\build_qt.bat          # vcvars64 + Ninja + CMake → build\EGtoolsFrontend.exe
```

---

## 自检与验证

本项目把"能不能用"尽量做成**可断言、可复现**的，而不是靠肉眼。前端内置一组无头自检：

| 自检开关 | 验证什么 |
| --- | --- |
| `--test-math` | 坐标映射由渲染倍率与缩放推导，不是硬编码。 |
| `--test-render` | 渲染一页并报告尺寸/格式/平均亮度（"黑页面"诊断）。 |
| `--test-paint` | 渲染后真正画进画布壳，并抓取像素（抓"纯渲染测得出、但实际画布全黑"）。 |
| `--test-ui-paint` | 打开**真实主窗口**加载 PDF，测量画布控件真正显示的像素（在真实嵌套里抓黑画布）。 |
| `--test-vector` | 矢量预览：**按设备分辨率重绘**、比旧位图路径明显更锐、且不黑（首帧即见纸面）。 |
| `--test-continuous` | 连续多页视图：版面 / 命中测试 / 懒渲染（含滚动到末页）。 |
| `--test-batch` | 并发批量提取：线程安全、计数准确、有实际产物。 |
| `--test-theme` | 深浅两套主题**真的**改变渲染亮度（含首页等原先写死颜色的页面）。 |
| `--test-panels` | 面板：宽度预设 / 折叠展开 / 导航面自动交接；并断言**全部 QSplitter 不可拖拽**。 |
| `--test-diffui` | 图纸比对页：清单填充 / 6 种筛选 / 跳转定位。 |
| `--dump-layout` | 审计窗口布局（最小尺寸、各面板宽度）并退出。 |
| `--selftest` | 端到端：渲染 + 映射 + 提取（走内核融合层）。 |
| `--screenshot` | 真开窗、加载 PDF 并保存窗口/屏幕 PNG（视觉凭证）。 |

### 配套脚本

| 脚本 | 作用 |
| --- | --- |
| `.build_trash/run_regress.ps1` | Windows 全量自检回归（含 **`kernelclient_test`** 与`退出码=0`断言），产出 `PASS/FAIL/SKIP` 汇总。 |
| `verify_in_wsl.sh` | 在 WSL 里**真跑一遍** Linux 产物：环境体检 → 内核真跑 + `ldd` → 前端真编 → 出 deb/rpm 并核对 → **`dpkg -i` 真装真跑** → WSLg 真开窗且断言干净退出。 |
| `EGtools.Frontend/build/kernelclient_test` | 同一份 JSON 请求分别走**内嵌**与**命令行**两条路，比对结果是否一致——"两条路融合"的唯一自动化证据。 |

> ⚠️ 回归必须**断言退出码**，不能只记录：实测过"图存好了、正文全 `[PASS]`，进程却在析构期段错误"（Linux `139` / Windows `0xC0000005`）——只记不判就永远发现不了。

---

## 环境变量

### 图形前端

| 变量 | 作用 |
| --- | --- |
| `EGTOOLS_CLI_DIR` | 指定内核目录（等价于 `--cli-dir`）。 |
| `EGTOOLS_KERNEL_BACKEND` | 内核调用后端：`auto` / `embedded` / `cli`。 |
| `EGTOOLS_THEME` | 强制主题：`light` / `dark`。 |
| `EGTOOLS_LAYOUT` | 强制布局：`sidebar` / `stacked`。 |
| `EGTOOLS_SIDEBAR` / `EGTOOLS_SIDEBAR_WIDTH` | 导航栏折叠状态 / 宽度（180 / 220 / 280）。 |
| `EGTOOLS_RESULTS` | 结果面板折叠状态。 |
| `EGTOOLS_RENDER_BACKEND` | 画布渲染后端：`auto` / `cpu` / `gpu`。 |
| `EGTOOLS_NO_GPU` | 置位即强制 CPU 光栅（"画布黑屏"的一键兜底）。 |
| `EGTOOLS_GL_INJECT_BLACK` | **故障注入**：模拟黑画布，用于验证回退路径真的生效。 |
| `EGTOOLS_GL_LEGACY_PAINT` / `EGTOOLS_GPU_SELFCHECK_OFF` | GL 兼容 / 自检开关（排障用）。 |
| `EGTOOLS_SELFTEST_PDF` | 自检用的 PDF 路径。 |

### 构建 / 验证脚本

| 变量 | 作用 |
| --- | --- |
| `EGTOOLS_ARCH` | `x64` / `arm64`（`build_linux.sh --arch` 的等价形式）。 |
| `EGTOOLS_VERSION` | 覆盖打包版本号。 |
| `EGTOOLS_LINUX_DIST` / `EGTOOLS_LINUX_BUILD` | Linux 产物目录 / 构建目录。 |
| `EGTOOLS_CPACK_STAGE` | CPack 暂存目录（默认放 Linux 原生文件系统，见下）。 |
| `EGTOOLS_MSVC_ENV_READY` | 置位则跳过 MSVC 环境探测（已在 Developer PowerShell 里时用）。 |
| `EGTOOLS_SUDO_PW` | `verify_in_wsl.sh` 里 `dpkg -i` 需要的 sudo 密码（**只从环境读，不落盘**）。 |

---

## 配置外部化

抽取与差异的**全部参数都不写死在代码里**，随产物分发，**改完不需要重新编译**（缺失字段自动合并内置默认值）：

| 文件 | 内容 |
| --- | --- |
| `EGpdf2excel.Engine/config.json` | 抽取词表、列坐标、页面区间、分组小标题规则等。 |
| `EGpdf2df.Engine/config.json` | 差异检测的 DPI、阈值、配准、标注样式等。 |

优先级：`--config` 指定 → 可执行文件同级 `config.json` → 内置默认。

---

## 架构与关键约定

### 内核调用融合层

Qt 前端**不直接调 `QProcess`**，而是走一层融合层（`KernelClient`）：

- **内嵌通道（hostfxr）**：把**已发布的同一份**内核 DLL 载入前端进程内调用。优点是无进程启动开销、日志实时回传（内核 stderr 即时转发到界面）。
- **命令行通道（QProcess）**：起 `EGpdf2xxx.exe --json ...` 子进程。
- **`auto`（默认）**：优先内嵌；**仅当内嵌自身失败**才回退命令行，并在界面显示实际走了哪条路。

内嵌通道需要宿主自己的 `*.runtimeconfig.json` 与一份**能服务 framework-dependent 组件**的 hostfxr；工具按候选链（exe 旁 → 机器 .NET 安装，版本降序 → 随包分发目录）**逐个尝试初始化**，取第一个成功者（不同大版本的 hostfxr 能力不同，实测 .NET 10 会拒绝、6.x 可以）。目标机完全没装 .NET 时，内嵌按设计不可用，自动回退命令行。

### 坐标系（重要）

前端、内核、契约**统一**采用与渲染图像一致的约定：

- **左上原点，y 向下增长，单位 = PDF 点（pt）**（与 MuPDF 结构化文本坐标一致）；
- 像素 → 点的唯一映射：**`pt = px / renderScale`**，`renderScale` 由内核 `--render` 下发的 JSON 提供，**不硬编码任何图纸坐标**；
- 区域 bbox 字段为 `x / y / width / height`。

---

## 故障排查

| 现象 | 原因 / 处理 |
| --- | --- |
| 双击后闪一下黑窗就没了 | 控制台子系统伪装成 GUI 的典型症状 —— 已用 `WIN32_EXECUTABLE TRUE` 规避；若仍出现，从命令行启动看 stderr。 |
| 启动即退出、退出码 `0xC0000135` (`-1073741515`) | 缺 DLL。用 `dumpbin /dependents` 查导入表；构建脚本的"依赖闭环自证"就是抓这个的。 |
| 画布全黑 | 远程桌面 / 虚拟机只给 GDI Generic GL 1.1，而 `QOpenGLWidget` 需要 GL 2.0。置 `EGTOOLS_NO_GPU=1`，或用菜单 **视图 → 渲染后端：CPU 光栅**。默认后端本就是 CPU 光栅，正常不该出现。 |
| 界面显示"内嵌不可用，已回退命令行" | 目标机没装 .NET 运行时（设计内回退，功能不受影响）；若已装仍如此，用 `kernelclient_test` 查各 hostfxr 候选的失败原因。 |
| Linux 上装了包但 `egtools` 不能执行 | exec 位丢失（打包时未保留权限）。用 `ls -l /opt/egtools/EGpdf2excel/EGpdf2excel` 确认是否 `0755`。 |
| Python / 脚本调 `--version` 返回非 0 | 只有 `EGpdf2excel` 支持 `--version`；`EGpdf2df` / `EGexcel2df` 用 `--help` 探活。 |
| 图纸差异报出大量"新增/删除"但肉眼看着一样 | 多半是**页面配对错位**：按真图号（管段号）配对，不要按页码；用 `--vecdiag` / `--vecscan` / `--sheetnos` 排查。 |
| 自比对不为 0 差异 | 说明比对链路有伪差异（常见于连续 `m` 未按子路径组织而造出"幻影线段"）；自比对必须 0 差异是本项目的验收铁律。 |

---

## 仓库结构

```text
EGtools/
├── EGtools.Core/                # 共享库：日志转发 + 内嵌内核服务端 + 文本几何
├── EGpdf2excel.Engine/          # PDF 抽取引擎（DLL，MuPDFCore）
│   ├── PdfExtractor.cs          #   主抽取流程（整页表格 → CSV/XLSX）
│   ├── RegionExtractor.cs       #   区域框选提取（动态、零硬编码坐标）
│   ├── RegionContracts.cs       #   区域框选 JSON 契约（请求/响应）
│   ├── PdfRenderer.cs           #   页面栅格化（BMP）
│   ├── ExtractConfig.cs         #   配置加载
│   └── config.json              #   抽取配置（词表/列坐标/页面区间）
├── EGexcel2df.Engine/           # Excel 图纸对比引擎（DLL）
├── EGpdf2df.Engine/             # PDF 图纸差异引擎（DLL，MuPDFCore + OpenCvSharp4 + iText7）
│   ├── DiffEngine.cs            #   配对 + 差异调度
│   ├── TextRedlineDiff.cs       #   文字差异：内容流逐字形重绘（复用原 PDF 字形）
│   ├── PdfVectorDiff.cs         #   矢量差异写出（保留原图矢量 + 矢量标注层）
│   └── config.json              #   差异配置（DPI/阈值/配准/标注）
├── EGpdf2excel/                 # CLI：材料表抽取 / 区域提取 / 页面渲染
├── EGexcel2df/                  # CLI：两份 Excel 对比
├── EGpdf2df/                    # CLI：两份 PDF 差异
├── EGtools.Frontend/            # Qt 6 图形前端（唯一 GUI）
│   ├── src/pdfrender.*          #   PDFium 矢量渲染服务（唯一预览来源）
│   ├── src/pdfview.*            #   页面显示 + 框选 + 坐标映射
│   ├── src/diffpage.*           #   图纸比对页（清单/筛选/跳转）
│   ├── src/materialextractpage.*#   批量提取工作台
│   ├── src/panels.*             #   面板/分割条统一约束（全应用不可拖）
│   ├── src/theme.*              #   设计系统与主题（QPalette + QSS 双轨）
│   ├── src/kernelclient.*       #   内核调用融合层（内嵌 hostfxr + 命令行）
│   ├── packaging/               #   Linux 打包件（.desktop / deb / rpm 控制脚本）
│   ├── tools/                   #   无头探针（kernelclient_test / pdfium_probe）
│   └── build_qt.bat             #   Qt + MSVC + Ninja 单机构建
├── docs/                        # 详细文档（中文 / English / CSDN 文章）—— 由 Windows 安装包分发到 <安装目录>\docs
├── redist/                      # 第三方运行库（gitignore，见 redist/README.md）
├── build_all.ps1                # Windows 一键构建 + 打包（-Arch x64|arm64）
├── build_linux.sh               # Linux 一键构建 + deb/rpm（--arch x64|arm64）
├── verify_in_wsl.sh             # 在 WSL 里真跑 Linux 产物（含真装真跑）
├── build_installer.iss          # Inno Setup 安装包定义
└── LICENSE                      # AGPL-3.0
```

构建产物（`bin/`、`obj/`、`build*/`、`DIST*/`、`installer/`、`.build_trash/`）与临时日志均被 `.gitignore` 排除。

---

## 版本号

| 位置 | 当前值 |
| --- | --- |
| 安装包 / CPack / Linux 包 | **4.3.0** |
| 三个 CLI 内核自报（`EGpdf2excel --version`） | 4.2.1 |
| Qt 前端自报（`EGtoolsFrontend.exe --version`） | 1.0.0 (Release) |

> ⚠️ 三处历史上未统一：安装包与发布产物用 **4.3.0**，而 .NET 内核程序集里仍是 4.2.1、Qt 前端是 1.0.0。升级版本号时需要同步：`build_installer.iss` 的 `MyAppVersion`、`build_all.ps1` 末尾的产物名、`EGtools.Frontend/CMakeLists.txt` 的 `CPACK_PACKAGE_VERSION`，以及各 `.csproj` 的 `Version` / `FileVersion`。

---

## 详细文档

- 中文使用说明（随安装包分发）：[docs/README_zh.md](docs/README_zh.md)
- English（随安装包分发）：[docs/README_en.md](docs/README_en.md)
- 技术文章（CSDN 风格）：[docs/CSDN_等轴测PDF材料表提取与对比.md](docs/CSDN_等轴测PDF材料表提取与对比.md)

---

## 许可证

本项目以 **GNU Affero General Public License v3.0 (AGPL-3.0)** 开源。详见 [LICENSE](LICENSE)。

版权 © 2026 HerryABU。

> AGPL-3.0 是强 copyleft：如果你把本工具作为网络服务提供给别人使用，也需要向使用者提供对应源码。
