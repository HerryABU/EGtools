# EGtools v3.0.0

**工程图纸材料表提取与对比工具集** — 由 **HerryABU** 制作并签名。

EGtools 是一套面向 CAD 等轴测（isometric）PDF 图纸的桌面工具，包含两个可独立使用、也可在统一图形界面中串联/批量使用的命令行工具：

| 工具 | 作用 |
| --- | --- |
| **EGpdf2excel** | 从 CAD 等轴测 PDF 图纸中**直接读取可编辑矢量文本**（无 OCR），提取 `FABRICATION / ERECTION MATERIALS` 材料表，导出为 CSV / XLSX。 |
| **EGexcel2df** | 对比两份图纸 Excel（旧版 vs 新版），以 `PIPE NO` 为主键，输出变化清单（新增 / 删除 / QTY 变化）。 |
| **EGtools (GUI)** | WinUI 3 图形界面，整合上面两个工具，支持**拖拽上传、批量、串联（pipeline）**，界面**中英文双语**。 |

> 引擎一致性：EGpdf2excel 使用 MuPDFCore（MuPDF 的 .NET 绑定），与 Python / Node / C++ 参考实现同源，中文与多语言文本均正确解析。

---

## 1. 安装

### 1.1 安装包（推荐）

运行安装目录中的 **`EGtools-3.0.0-x64.exe`**（Inno Setup 制作）：

- 自动检测并安装 **Windows App Runtime**（WinUI 3 运行依赖，若已安装则跳过）。
- 默认安装到 `C:\Program Files\EGtools\`，并在桌面创建快捷方式。
- 安装完成后可勾选“立即运行”。

### 1.2 绿色运行（免安装）

将 `DIST\` 目录整体复制即可使用，需本机已安装 **Windows App Runtime 1.7.x**（或运行 `runtime\windowsappruntimeinstall.exe`）。
直接双击 `EGtools.exe` 启动图形界面；命令行工具见第 3 节。

---

## 2. 图形界面（EGtools GUI）

启动 `EGtools.exe` 后，左侧导航为：

- **提取 (Extract)** — 对应 EGpdf2excel。
- **对比 (Compare)** — 对应 EGexcel2df。
- **串联 (Pipeline)** — 先“提取”再“对比”的一键流水线。
- **设置 (Settings)** — 语言（中文 / English）、主题（跟随系统 / 浅色 / 深色）。
- **关于 (About)** — 版本、作者、许可证、使用文档入口。

### 2.1 提取页（Extract）

- **拖拽上传**：把 PDF 文件拖到虚线框；或点击框内按钮用文件选择器多选。
- **批量**：可一次加入多个 PDF，列表显示已选文件，支持单条删除与清空。
- **选项**：
  - `格式`：CSV（默认）/ XLSX / 两者。
  - `组件列布局`：合并（组件并入描述，8 列，默认）/ 独立（组件单列，9 列）。
  - `分组小标题`：嵌入（把 PDF 表内 FITTINGS/VALVES/FLANGES 等小标题前缀到描述，默认）/ 省略。
  - `标签`：输出文件名后缀（默认 `V3`）。
  - `参考 xlsx`（可选）：提供描述词表，使无空格 PDF 的描述分词更贴近参考样式。
  - `输出目录`（可选）：留空则输出到各 PDF 同目录。
- 点击 **运行**，进度条与日志实时显示；完成后 **打开** 按钮定位输出文件夹。

### 2.2 对比页（Compare）

- 分别拖入 / 选择**旧图纸**与**新图纸**两个 Excel。
- 可选指定输出报告路径（默认 `图纸变化清单_<时间戳>.xlsx`）。
- 点击 **运行** 生成变化清单。

### 2.3 串联页（Pipeline）

- 选择或拖入**旧文件（PDF 或 Excel）** 与 **新文件（PDF 或 Excel）**。
- 工具自动判断：PDF 先提取为临时 Excel，再与另一侧对比，输出统一变化报告。
- 支持组件列布局 / 分组小标题选项。

### 2.4 设置

- **语言**：中文 / English（即时切换，写入 `lang.txt` 记忆）。
- **主题**：跟随系统 / 浅色 / 深色。

---

## 3. 命令行（CLI）

两个 CLI 工具也可**完全独立运行**，且已被打包进安装程序。

### 3.1 EGpdf2excel

```
EGpdf2excel [输入...] [选项]
```

- **输入**：PDF 文件、目录或通配符；省略时默认当前目录 `*.pdf`。
- **选项**：

  | 选项 | 说明 |
  | --- | --- |
  | `-o, --output DIR\|FILE` | 输出目录；单输入时可为单个输出文件（扩展名由 `--format` 决定）。默认各输入同目录。 |
  | `-f, --format csv\|xlsx\|both` | 输出格式（默认 `csv`）。 |
  | `-r, --ref FILE.xlsx` | 参考工作簿；用其 DESCRIPTION 词表重建无空格 PDF 的描述分词空格。 |
  | `-G, --group-header MODE` | 表内分组小标题处理：`embed`（默认，前缀到描述）/ `omit`（省略）。 |
  | `-C, --component MODE` | 组件列布局：`merged`（默认，并入描述，8 列）/ `separate`（独立列，9 列）。 |
  | `--tag NAME` | 文件名标签（默认 `V3`）→ `<pdf>_<tag>.csv/.xlsx`。 |
  | `-v, --verbose` | 详细进度（每页、警告）输出到 stderr。 |
  | `-h, --help` | 显示帮助并退出。 |

- **示例**：

  ```
  EGpdf2excel -v *.pdf -f both -r ref.xlsx -o out/
  EGpdf2excel C:/scan/408-101-051*.pdf --tag V4 -f xlsx
  ```

### 3.2 EGexcel2df

```
EGexcel2df <旧图纸.xlsx> <新图纸.xlsx> [选项]
```

- **选项**：

  | 选项 | 说明 |
  | --- | --- |
  | `-h, --help` | 显示帮助。 |
  | `-v, --version` | 显示版本号（3.0.0）。 |
  | `-o, --output <文件>` | 输出 Excel 路径（默认 `图纸变化清单_<时间戳>.xlsx`）。 |
  | `--sheet1 <名称\|序号>` | 旧图纸读取的工作表（默认第 1 个）。 |
  | `--sheet2 <名称\|序号>` | 新图纸读取的工作表（默认第 1 个）。 |

- **对比逻辑**：以 `PIPE NO` 为主键分组；在每组内以 `TABLE + DN(mm) + 项目/DESCRIPTION + NO` 为项目唯一键：
  - 某 `PIPE NO` 仅存在于一侧 → 整 PIPE 新增 / 删除；
  - 项目键仅存在于一侧 → 项目新增 / 删除；
  - 两侧都有但 `QTY` 不同 → QTY 变化（增加/减少 N）；
  - `QTY` 完全相同 → 无变化（不输出）。

- **示例**：

  ```
  EGexcel2df old.xlsx new.xlsx -o report.xlsx
  EGexcel2df old.xlsx new.xlsx --sheet1 "材料表" --sheet2 1
  ```

---

## 4. 版本与许可

- **版本**：3.0.0
- **作者 / 签名**：HerryABU
- **许可证**：AGPL-3.0
- **运行依赖**：Windows 10/11 (x64)，.NET 10 运行时（GUI 由 Windows App Runtime 1.7 提供 WinUI 3）。
