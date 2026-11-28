# EGtools 使用说明书（中文）

**工程图纸材料表提取与对比工具集** — 由 **HerryABU** 制作。

EGtools 面向 **CAD 等轴测（isometric）PDF 图纸**与**管道材料表（Excel）**，包含四个部件：三个可**完全独立使用**的命令行内核，以及一个把它们串起来的**统一图形界面**：

| 部件 | 作用 |
| --- | --- |
| **EGtools（图形界面）** | Qt 6 桌面程序。打开 PDF → 画布框选 / 批量提取 → 图纸比对 → 预览红标注差异图。**唯一 GUI**。 |
| **EGpdf2excel** | 从 CAD 等轴测 PDF 中**直接读取可编辑矢量文本**（无 OCR），提取 `FABRICATION / ERECTION MATERIALS` 材料表 → CSV / XLSX。 |
| **EGexcel2df** | 对比两份图纸 Excel（旧版 vs 新版），以 `PIPE NO` 为主键，输出变化清单（新增 / 删除 / QTY 变化）。 |
| **EGpdf2df** | 对比两份图纸 PDF（旧版 vs 新版），输出**带红色差异标注**的新 PDF + JSON 摘要。 |

> 引擎一致性：PDF 解析统一使用 MuPDFCore（MuPDF 的 .NET 绑定），与 Python / Node / C++ 参考实现同源，中文与多语言文本均正确解析。
>
> 图形界面**自带完整运行环境**：三个内核是 .NET **自包含**发布，Qt 与 VC++ 运行库随目录分发 ⇒ **目标机无需安装 .NET、Qt 或 VC++ 运行库**。

**目录**

- [1. 安装](#1-安装)
- [2. 快速上手](#2-快速上手)
- [3. 图形界面详解](#3-图形界面详解)
- [4. 命令行（CLI）](#4-命令行cli)
- [5. 配置与参数](#5-配置与参数)
- [6. 在 Linux 上使用](#6-在-linux-上使用)
- [7. 常见问题](#7-常见问题)
- [8. 版本与许可](#8-版本与许可)

---

## 1. 安装

### 1.1 Windows 安装包（推荐）

1. 运行安装程序 `EGtools-<版本>-x64.exe`（64 位 Intel/AMD）或 `EGtools-<版本>-arm64.exe`（Windows on ARM）。
2. 安装程序会：
   - 默认安装到 `Program Files\EGtools`；
   - 创建开始菜单项：GUI、三个 CLI 工具，以及「使用文档 (中文)」与「Documentation (English)」入口；
   - 把本说明书及英文、技术文章三份文档复制到 `<安装目录>\docs`；
   - **默认勾选**「将命令行工具 (EGpdf2excel / EGexcel2df / EGpdf2df) 添加到 PATH」——勾上之后可在任意命令行窗口直接敲 `EGpdf2excel` 等命令，**卸载时会自动移除这些 PATH 项**（该选项只在管理员安装时提供，因为单用户安装无权写入系统 PATH）；
   - **桌面快捷方式为可选**（安装向导「附加图标」里的复选框，默认**不勾选**）；
   - 安装完成后可勾选「立即运行」。
3. 卸载：Windows「设置 → 应用」里选 EGtools 卸载，或运行安装目录下的 `unins000.exe`。卸载会一并删掉四个程序目录（GUI + 三个内核），并按上面所述清理 PATH 项。

> **想装成"只有我自己能用"**（不写 Program Files、不需要管理员）：启动安装程序后把安装位置改到自己的目录，或用命令行参数 `/CURRENTUSER` 静默安装。
>
> ⚠️ **架构别装错**：安装包自带架构标识 —— arm64 包在普通 x64 机器上会**被拒绝安装**（反之亦然），这是有意设计，避免装完打不开。
>
> ⚠️ 安装需要**管理员权限**（装到 Program Files 时）。若你是普通用户，安装程序会询问是否改为单用户安装。

### 1.2 Windows 免安装（绿色）

把发布目录 `DIST\` 整个复制到目标机器，双击 `EGtoolsFrontend\EGtoolsFrontend.exe` 即可。三个内核在 `DIST\EGpdf2excel\`、`DIST\EGexcel2df\`、`DIST\EGpdf2df\`，可直接在命令行使用。

### 1.3 验证安装是否正常

```bash
EGpdf2excel --version          # 应打印版本号
EGpdf2df --help                # 显示帮助
EGexcel2df --help              # 显示帮助
```

> ⚠️ 三个内核里**只有 `EGpdf2excel` 支持 `--version`**；另外两个遇到 `--version` 会报 `Unknown option` 并以退出码 2 结束 —— **脚本探活请用 `--help`**。

---

## 2. 快速上手

**场景一：把一张等轴测 PDF 的材料表导成 Excel**

1. 启动 EGtools，在**首页**点「打开 PDF 框选提取」，选择图纸；
2. 在画布上拖拽框出材料表区域（顶部下拉选「全域表」）；
3. 点「提取（内核）」，结果表格与管线号分栏显示，字框回叠到画布上；
4. 在结果面板点「导出」，得到 CSV / XLSX。

**场景二：一次导一批图纸**

1. 左侧导航进「**材料表提取**」；
2. 加入多个 PDF（或整个目录）；
3. 选输出格式（CSV / XLSX / 两者）与输出目录，点「开始批量提取」；
4. 完成后点「打开」定位到输出文件夹。

**场景三：比对旧版 / 新版图纸**

- **PDF 对 PDF**：导航「**图纸比对**」→ 选基准图（旧）与目标图（新）→ 生成带红色标注的 `diff.pdf`，并在界面内预览；点差异清单任意一行会跳到并高亮该处。
- **Excel 对 Excel**：导航「**Excel 比对**」→ 选两份 Excel → 生成变化清单工作簿。

---

## 3. 图形界面详解

### 3.1 导航

左侧导航树（`Ctrl+B` 可折叠）：

| 导航项 | 说明 |
| --- | --- |
| **首页** | 入口卡片；并显示**内核状态**（内嵌通道是否可用、hostfxr 取自哪里）与**渲染路径**（GPU 或 CPU 光栅）—— 排查"画面不对"时先看这两行。 |
| **材料表提取** | 批量提取工作台（多文件、格式、列布局、进度与日志）。 |
| **PDF 工具 → 渲染与框选提取** | 主工作区：画布 + 框选 + 导出区域 JSON + 提取，字框回叠到画布。 |
| **PDF 工具 → 材料表 OCR 增强** | 同一工作区，以 OCR 兜底模式运行：当框选区域内**没有矢量文字**时才启用识别。 |
| **图纸比对** | PDF 图纸差异比对 + 差异清单 + 预览。 |
| **Excel 比对** | 两份 Excel 的变化清单。 |
| **框选方案** | 管理具名框选方案（保存在 `我的文档\EGtools\schemes.json`），下次一键复用到画布。 |

### 3.2 快捷键

| 快捷键 | 作用 |
| --- | --- |
| `Ctrl+B` | 折叠 / 展开导航栏（在 180 / 220 / 280 px 间循环） |
| `Ctrl+Shift+B` | 折叠 / 展开缩略图栏（140 / 180 / 240 px） |
| `Ctrl+J` | 折叠 / 展开结果面板（160 / 240 / 360 px） |
| `Ctrl+1` / `Ctrl+2` | 画布视图：单页 / 连续 |
| `F1` | 打开本说明书 |

> 所有分割条**故意不可拖拽** —— 面板宽度请用预设值与折叠快捷键调整，避免把版面拖乱。

### 3.3 框选的三种语义

框选时顶部下拉选择区域含义，这决定内核怎么解释这块区域：

| 语义 | 内部名 | 用途 |
| --- | --- | --- |
| **全域表** | `global_table` | 整张材料表 |
| **细表** | `detail_table` | 子表 / 明细块 |
| **材料编号区** | `material_no_region` | 图纸中管道 / 管线编号出现的区域（用于"图中编号 ↔ 材料表"一一对应核对） |

### 3.4 视图与主题

- **视图 → 渲染后端**：三个选项 —— `自动（探测通过用 GPU，否则光栅）` / `GPU 加速` / **`CPU 光栅（最稳，任何机器都不黑屏）`**。默认就是 CPU 光栅（矢量分块渲染，不需要 GPU，也不会黑屏）。切换后立即并持久生效。
- **视图 → 面板尺寸**：分别设置「导航栏宽度」「缩略图宽度」「结果面板高度」，与折叠快捷键配套使用。
- **主题**：浅色 / 深色 / 跟随系统。切换后**整个界面**（含首页）都会跟着变，且设置会被记住。
- **布局**：侧栏式 / 竖向堆叠式（窄屏或 16:9 竖屏时更顺手）。

---

## 4. 命令行（CLI）

三个内核都可独立运行：**stdout 输出 JSON 结果，stderr 输出日志**，因此非常适合脚本化 / 批处理 / 定时任务。

### 4.1 EGpdf2excel — PDF 材料表抽取

```
EGpdf2excel [输入...] [选项]
```

- **输入**：PDF 文件、目录或通配符；省略时默认当前目录 `*.pdf`。

| 选项 | 说明 |
| --- | --- |
| `-o, --output DIR\|FILE` | 输出目录；单输入时可为单个输出文件（扩展名由 `--format` 决定）。默认与各输入同目录。 |
| `-f, --format csv\|xlsx\|both` | 输出格式（默认 `csv`）。 |
| `-r, --ref FILE.xlsx` | 参考工作簿；用其 `DESCRIPTION` 词表重建无空格 PDF 的描述分词空格。 |
| `--config FILE.json` | 抽取配置（词表 / 列坐标 / 页面区间）；缺省读 exe 同级 `config.json`，再退回内置默认。 |
| `-G, --group-header embed\|omit` | 表内分组小标题（FITTINGS / VALVES / FLANGES…）处理：`embed`（默认，前缀到每条描述）/ `omit`。 |
| `-C, --component merged\|separate` | 组件列布局：`merged`（默认，并入描述，共 8 列）/ `separate`（独立成列，共 9 列）。 |
| `--tag NAME` | 输出文件名标签（默认 `V3`）→ `<pdf>_<tag>.csv/.xlsx`。 |
| `--render --page N --scale S --render-out F.bmp` | 页面栅格化渲染（供界面显示 / 排查）。 |
| `--mode region --json FILE` / `--stdin` | 区域框选提取：请求 JSON 可来自文件或标准输入。 |
| `-v, --verbose` | 每页进度与警告输出到 stderr。 |
| `--version` / `-h, --help` | 版本 / 帮助。 |

**示例**

```bash
EGpdf2excel drawing.pdf                              # 单张 → CSV
EGpdf2excel -v *.pdf -f both -r ref.xlsx -o out/     # 批量 → CSV + XLSX
EGpdf2excel "C:/scan/408-101-051*.pdf" --tag V4 -f xlsx
```

### 4.2 EGexcel2df — 两份 Excel 图纸对比

```
EGexcel2df <旧图纸.xlsx> <新图纸.xlsx> [选项]
```

| 选项 | 说明 |
| --- | --- |
| `-o, --output <文件>` | 输出 Excel 路径（默认 `图纸变化清单_<时间戳>.xlsx`）。 |
| `--sheet1 <名称\|序号>` | 旧图纸读取的工作表（默认第 1 个）。 |
| `--sheet2 <名称\|序号>` | 新图纸读取的工作表（默认第 1 个）。 |
| `-v, --version` / `-h, --help` | 版本 / 帮助。 |

**对比逻辑**：以 `PIPE NO` 为主键分组；每组内以 `TABLE + DN(mm) + 项目/DESCRIPTION + NO` 为项目唯一键：

- 某 `PIPE NO` 只存在于一侧 → 整条 PIPE 新增 / 删除；
- 项目键只存在于一侧 → 项目新增 / 删除；
- 两侧都有但 `QTY` 不同 → QTY 变化（±N）；
- `QTY` 完全相同 → 不输出。

```bash
EGexcel2df old.xlsx new.xlsx -o report.xlsx
EGexcel2df old.xlsx new.xlsx --sheet1 "材料表" --sheet2 1
```

### 4.3 EGpdf2df — 两份 PDF 图纸差异

```
EGpdf2df <基线.pdf> <目标.pdf> -o diff.pdf
EGpdf2df --json request.json        # 或 --stdin
```

| 选项 | 说明 |
| --- | --- |
| `-o, --output F` | 标注 PDF 输出路径。 |
| `-p, --pages L` | 限定比对页；默认 `filter` 语义 = 先按图号全量配对，再筛出指定页。 |
| `--page-mode filter\|pin` | `-p` 的语义：`filter`（默认）/ `pin`（只比这几页）。`--pin-pages` 等价于 `pin`。 |
| `--graphic-method vector\|pixel` | 图形差异方法：`vector`（默认，纯矢量检测）/ `pixel`（渲染 + 图像比对）。 |
| `--mark-style semi\|solid\|outline` | 图形标注样式（默认 `semi`：半透明填充 + 彩色描边）。 |
| `--mark-stroke-color #RRGGBB` | 图形标注颜色（默认自动取对比色）。 |
| `--stroke-width PT` / `--stroke-opacity A` | 矢量标注线宽（默认 1.0 pt）/ 不透明度（默认 0.55）。 |
| `--vector-tol PT` | 矢量线段判定容差（默认 3.0 pt）。 |
| `--near-ink-tol PT` | 墨迹重叠容差（默认 0.5 pt）。 |
| `--text-added-color` / `--text-removed-color #RRGGBB` | 新增（默认 `#FF0000` 红）/ 删除（默认 `#0000FF` 蓝）文字标注色。 |
| `--text-insert-direction down\|up\|right\|left` | 新增文字的插入方向（默认 `down`）。 |
| `--report-mode cluster\|run` | 图形差异报告方式（默认 `cluster`）。 |
| `--config FILE` | 差异配置（pdf2df config JSON）。 |
| `-h, --help` | 帮助。 |

**差异分两步、彼此独立**，避免互相误报：

1. **图形 / 图元差异** —— 默认走**纯矢量检测**（几何标定 + 共线覆盖匹配），不使用像素比对；
2. **文字 / 注释差异** —— 按位置匹配两侧文字，识别**增 / 删 / 改 / 移动**，并把命中的字形从原 PDF 内容流里抽取出来重绘（复用**原图纸自带字形**，因此中文等字形照常标注，且**不依赖任何系统字体**）。

**页面配对按"真图号"（管段号，如 `408.101.P02`）而不是页码** —— 因为每页标题栏都印同一个项目号，按页码配会全错。只在一侧出现的判为新增 / 删除；没有图号的页用线画指纹兜底。

> **自检口径**：同一份 PDF 与自身比对，必须得到 **0 图形差异 / 0 文字差异**。若不为 0，说明比对链路引入了伪差异，请连同图纸一并反馈。

### 4.4 区域提取 JSON 契约

界面导出的「区域」文件就是下面这个结构（字段名大小写不敏感），也可以手写或由脚本生成：

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

- `bbox` 单位是 **PDF 点（pt）**，**左上角为原点、y 向下**（与渲染图一致）；
- 返回的每个区域含 `kind`、`ocrUsed`、`pipeNumbers`（编号区）、`table`（表格区的行 × 列网格）、`words`（区域内全部字框，供叠加 / 调试）。

---

## 5. 配置与参数

抽取与差异的**全部参数都不写死在程序里**，而是放在随程序分发的配置文件中，**改完无需重新编译**（缺的字段会自动合并内置默认值）：

| 文件 | 内容 |
| --- | --- |
| `EGpdf2excel.Engine\config.json` | 抽取词表、列坐标、页面区间、分组小标题规则等 |
| `EGpdf2df.Engine\config.json` | 差异检测的 DPI、阈值、配准、标注样式等 |

读取优先级：`--config` 指定的文件 → 可执行文件同级 `config.json` → 内置默认。

**界面里可调的常用项**（等价于命令行开关）：输出格式、组件列布局（合并 / 独立）、分组小标题（嵌入 / 省略）、输出标签、参考词表工作簿、输出目录；差异页可调标注颜色、线宽、透明度、判定容差、图形差异方法等。

---

## 6. 在 Linux 上使用

### 6.1 安装

```bash
sudo dpkg -i egtools-<版本>-Linux.deb     # Debian / Ubuntu
sudo rpm  -i egtools-<版本>-Linux.rpm     # Fedora / RHEL
```

- 安装到 `/opt/egtools`，并创建 `/usr/bin/egtools` 软链、桌面项与图标；
- 之后在终端敲 `egtools` 或在应用菜单里搜 EGtools 即可启动；
- 三个内核在 `/opt/egtools/EGpdf2excel/` 等目录，用法与 Windows 完全一致（**可执行文件没有 `.exe` 后缀**）。

卸载：

```bash
sudo dpkg -r egtools        # 或：sudo rpm -e egtools
```

### 6.2 需要注意的差异

| 项目 | 说明 |
| --- | --- |
| 可执行文件名 | 无 `.exe` 后缀：`EGpdf2excel`、`EGexcel2df`、`EGpdf2df`。 |
| 免安装使用 | 把发布目录整体复制即可，同样自带 .NET 运行时与 Qt 运行库。 |
| 依赖 | 安装包已在**干净的 Ubuntu Server** 上验证：装完即可运行，无需额外装 Qt / .NET。 |
| 内嵌通道 | 目标机若**没装 .NET 运行时**，界面会显示"内嵌不可用，已回退命令行"——这是**设计内的回退，功能不受影响**。 |

---

## 7. 常见问题

**Q：双击程序，窗口闪一下就没了？**
先确认是从安装目录 / 解压目录直接运行，且该目录**完整**（不要只拷贝单个 exe）。然后从命令行启动看错误输出：

```bash
EGtoolsFrontend.exe            # Windows
egtools                        # Linux
```

**Q：提示缺少 DLL / 程序无法启动（错误码 `0xC0000135`）？**
说明发布目录不完整（少拷了 Qt 或 VC++ 运行库）。**整个目录一起复制**，不要只挑 exe。

**Q：画布一片黑？**
通常是远程桌面 / 虚拟机只提供很老的 OpenGL（GDI Generic 1.1），而 GPU 绘制需要 GL 2.0。处理：菜单 **视图 → 渲染后端：CPU 光栅（最稳，任何机器都不黑屏）**，或设环境变量 `EGTOOLS_NO_GPU=1`。默认后端本就是 CPU 光栅，正常情况下不该出现黑屏。

**Q：界面显示"内嵌不可用，已回退命令行"，功能会受影响吗？**
不会。这是设计内的降级：内嵌（进程内调库）不可用时自动改用命令行子进程调用，结果完全一致，只是每条命令慢几十毫秒。

**Q：Linux 上 `egtools` 报 `Permission denied`？**
内核可执行文件的执行位丢失。检查：

```bash
ls -l /opt/egtools/EGpdf2excel/EGpdf2excel      # 应为 -rwxr-xr-x
```

**Q：图纸差异报出大量"新增 / 删除"，但两版看着几乎一样？**
多半是**页面配对错位**。请确认两侧图纸的**图号（管段号）**能对应上；如果两侧图纸结构差异大（例如一版是 CAD 直出、另一版是打印重排），请把两份图纸一起反馈以便复现。

**Q：提取出来的描述文字没有空格 / 断词不对？**
MuPDF 的结构化文本本身不带空格，工具按字形间距重建。用 `-r ref.xlsx`（或界面里的「参考 xlsx」）提供一份描述词表，能让断词更贴近你的项目习惯。

**Q：中文 / 特殊字符乱码？**
Windows 命令行默认代码页可能不是 UTF-8。在 `cmd` 里先执行 `chcp 65001`，或用 `EGpdf2excel ... > out.txt` 落文件后以 UTF-8 打开。图形界面不受影响。

**Q：支持哪些图纸？**
面向 **CAD 等轴测（isometric）PDF**：文字为**可复制矢量文本**时效果最好。若是扫描件（整页是图片），只有"材料表 OCR 增强"模式能尝试识别，并且**准确率取决于扫描质量**——本工具的设计前提是**矢量文本**，不是 OCR 工具。

---

## 8. 版本与许可

| 项目 | 值 |
| --- | --- |
| 安装包 / 发布版本 | **4.4.0** |
| 三个 CLI 内核自报版本（`EGpdf2excel --version`） | 4.4.0 |
| 图形界面自报版本（`EGtoolsFrontend --version`） | 4.4.0 (Release) |
| 作者 / 签名 | HerryABU |
| 许可证 | **AGPL-3.0** |
| 运行依赖 | **无外部依赖** —— 内核为 .NET 9 **自包含**发布，Qt 与 VC++ 运行库随包分发 |
| 支持平台 | Windows 10 1809+ / 11（x64、arm64）、Linux x64（deb / rpm） |

> ✅ 自 4.4.0 起三处版本号已统一为 **4.4.0**：安装包/发布产物、内核程序集、图形界面。

---

### 附：相关文档

- 仓库根 README（开发 / 构建 / 架构）：`README.md`
- 精简英文说明：`docs\README_en.md`
- 技术文章（实现细节）：`docs\CSDN_等轴测PDF材料表提取与对比.md`

版权 © 2026 HerryABU。本项目以 **GNU Affero General Public License v3.0** 开源。
