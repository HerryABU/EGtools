# EGtools

**工程图纸材料表提取与对比工具集** · A toolkit for extracting and comparing engineering drawing material tables

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)

EGtools 是一套面向 **CAD 等轴测（isometric）PDF 图纸** 与 **管道材料表（Excel）** 的桌面工具集。它采用 **三层架构**：**Qt 图形前端** 只负责交互与编排，**全部 PDF / Excel 实际处理** 都下沉到独立的 **.NET CLI 内核**，前后端通过 **JSON / 文件契约** 完全解耦。

| 组件 | 类型 | 作用 |
| --- | --- | --- |
| **EGpdf2excel** | CLI 内核 | 从 CAD 等轴测 PDF 中**直接读取可编辑矢量文本**（无 OCR），提取材料表 → CSV / XLSX；支持**区域框选提取**与**页面栅格化渲染**。 |
| **EGexcel2df** | CLI 内核 | 对比两份图纸 Excel（旧版 vs 新版），以 `PIPE NO` 为主键，输出变化清单（新增 / 删除 / QTY 变化）。 |
| **EGpdf2df** | CLI 内核 | 对比两份图纸 PDF（baseline vs target），输出**带红色差异标注**的新 PDF + JSON 摘要（图形差异与文字差异**分两步独立处理**）。 |
| **EGtools.Frontend** | Qt 6 应用 | 统一图形前端：打开 PDF → 三模式框选 → 导出区域 JSON → 调内核提取 / 图纸差异。 |

> 引擎一致性：PDF 相关内核统一使用 [MuPDFCore](https://www.nuget.org/packages/MuPDFCore)（MuPDF 的 .NET 绑定），与 Python / Node / C++ 参考实现同源，中文与多语言文本均正确解析。图像/差异计算使用 [OpenCvSharp4](https://www.nuget.org/packages/OpenCvSharp4)。
>
> 配置外部化：抽词表 / 列坐标 / 页面区间集中在 `EGpdf2excel.Engine/config.json`；差异检测的全部 DPI / 阈值 / 配准 / 标注参数集中在 `EGpdf2df.Engine/config.json`。二者随产物分发，**改动后无需重新编译**，缺失字段自动合并内置默认值。

---

## 📁 仓库结构

```
EGtools/
├── EGtools.Core/              # 共享库：日志转发 ForwardingWriter + 文本几何 TextGeometry
├── EGpdf2excel.Engine/        # PDF 抽取引擎（DLL，MuPDFCore）
│   ├── PdfExtractor.cs        #   主抽取流程（整页表格 → CSV/XLSX）
│   ├── RegionContracts.cs     #   区域框选 JSON 契约（请求/响应）
│   ├── RegionExtractor.cs     #   区域框选提取（动态、零硬编码坐标）
│   ├── PdfRenderer.cs         #   页面栅格化（BMP，供前端显示）
│   ├── ExtractConfig.cs       #   配置加载（config.json）
│   └── config.json            #   抽取配置（词表/列坐标/页面区间）
├── EGexcel2df.Engine/         # Excel 图纸对比引擎（DLL）
├── EGpdf2df.Engine/           # PDF 图纸差异引擎（DLL，MuPDFCore + OpenCvSharp4）
│   ├── DiffEngine.cs          #   两步差异：图形(排除文字,描实际形状)→ 文字；按图纸号配对
│   ├── PdfVectorDiff.cs       #   矢量差异 PDF 写出（iText7：保留原图矢量+矢量标注层）
│   └── config.json            #   差异检测配置（DPI/阈值/配准/标注）
├── EGpdf2excel/               # CLI：PDF 材料表抽取 / 区域提取 / 页面渲染
├── EGexcel2df/                # CLI：两份 Excel 图纸对比
├── EGpdf2df/                  # CLI：两份 PDF 图纸差异
├── EGtools.Frontend/          # Qt 6 图形前端（唯一 GUI）
│   ├── src/pdfview.*          #   页面显示 + 三模式橡皮筋框选 + 坐标映射
│   ├── src/mainwindow.*       #   工具栏 / 结果面板 / 内核编排
│   ├── src/clirunner.*        #   QProcess 调用 CLI 内核
│   ├── src/selftest.*         #   无头自检（--selftest / --test-math）
│   └── build_qt.bat           #   Qt+MSVC+Ninja 构建脚本（真机 cmd）
├── docs/                      # 详细使用文档（中文 / English / CSDN 文章）
├── redist/                    # 第三方运行库（gitignore，见 redist/README.md）
├── build_all.ps1              # 一键构建 + 打包（.NET 内核 + 可选 Qt 前端）
├── build_installer.iss        # Inno Setup 安装包定义
└── LICENSE                    # AGPL-3.0
```

构建产物（`bin/`、`obj/`、`build/`、`DIST/`、`installer/` 等）与临时日志均已被 `.gitignore` 排除。

---

## 🛠 先决条件

- **Windows 10 1809+ / Windows 11 (x64)**
- **.NET SDK 9 或 10**（构建 CLI 内核；本仓库 `net9.0-windows`）
- **Qt 6.11.2 msvc2022_64**（仅构建图形前端需要）
- **Visual Studio 2022 / 2026**（含「使用 C++ 的桌面开发」工作负载；Qt 前端需要）
- **Ninja + CMake**（Qt 前端构建；VS 自带 CMake 亦可）
- **Inno Setup 6**（仅打包安装程序时需要）
- 第三方运行库见 [`redist/README.md`](redist/README.md)

---

## 🔨 构建

### 一键构建（.NET 内核 + 可选 Qt 前端）

```powershell
# 1) 准备 redist/（首次构建前，按 redist/README.md 放入 VC++ 运行库等）
# 2) 构建全部内核 + 安装包
.\build_all.ps1
```

`build_all.ps1` 会：
1. 清理并还原全部项目（Core + 三个 Engine + 三个 CLI）；
2. 发布三个 **自包含** CLI 到 `DIST/EGpdf2excel`、`DIST/EGexcel2df`、`DIST/EGpdf2df`；
3. **若检测到 Qt 6**，构建 `EGtools.Frontend` 到 `DIST/EGtoolsFrontend`（未装 Qt 则跳过，不报错）；
4. 复制 VC++ 运行库、文档、图标；
5. 校验产物并用 Inno Setup 生成安装包。

> Qt 前端是**可选步骤**：没有 Qt / MSVC 时 .NET 内核仍可独立构建成功。

### 单独构建

```powershell
# 单个 CLI 内核（自包含）
dotnet publish EGpdf2excel/EGpdf2excel.csproj -c Release -r win-x64 --self-contained true
dotnet publish EGpdf2df/EGpdf2df.csproj    -c Release -r win-x64 --self-contained true

# Qt 前端（真机 cmd 环境）
cd EGtools.Frontend
.\build_qt.bat          # 内部：vcvars64 + Ninja + CMake，产物 build\EGtoolsFrontend.exe
```

---

## 🖥 图形前端（EGtools.Frontend）

Qt 6 前端是**交互壳**，不含任何 PDF 处理逻辑：

1. **打开 PDF** → 通过 `EGpdf2excel --render` 把当前页栅格化为 BMP 显示；
2. **框选区域** → 在页面上拖拽橡皮筋矩形，顶部切换三种语义模式：
   - **全域表** `global_table` —— 整张材料表；
   - **细表** `detail_table` —— 子表 / 明细块；
   - **材料编号区** `material_no_region` —— 图纸中管道/管线编号出现的区域；
3. **导出区域 JSON** → 把所有框选（按页 + 语义 + PDF 点矩形）写成 `RegionRequest`，可存盘后交给内核；
4. **提取** → `QProcess` 调 `EGpdf2excel --mode region`，解析返回的 `RegionResponse`，表格/管线号分栏展示，**字框回叠**到画布；
5. **图纸差异** → 选基准图与目标图，调 `EGpdf2df` 生成红标注 `diff.pdf` 并内嵌预览。

### 坐标系（重要）

前端、内核、契约**统一**采用与渲染图像一致的坐标约定：

- **左上原点，y 向下增长，单位 = PDF 点**（与 MuPDF 结构化文本坐标一致）；
- 像素 → PDF 点映射：**`pt = px / renderScale`**，`renderScale` 由内核 `--render` 下发的 JSON 提供，**不硬编码任何图纸坐标**；
- 区域 bbox JSON 字段为 `x / y / width / height`。

---

## 🔧 CLI 内核

三个内核均可**独立运行**，也可被前端以子进程方式调用（stdout 输出 JSON，stderr 输出日志）。

### EGpdf2excel — PDF 材料表抽取 / 区域提取 / 页面渲染

```bash
# 整页表格抽取 → CSV（默认）
EGpdf2excel drawing.pdf
EGpdf2excel "C:/docs/*.pdf" -o out/ -f xlsx

# 页面栅格化渲染（输出 BMP + JSON 摘要）
EGpdf2excel --render --page 0 --scale 2 --render-out page0.bmp drawing.pdf

# 区域框选提取（请求 JSON 来自文件或 stdin）
EGpdf2excel --mode region --json request.json
echo '{...}' | EGpdf2excel --mode region --stdin
```

主要选项：`-o/--output`、`-f/--format csv|xlsx|both`、`-r/--ref`（参考词表）、`--config`、`-G/--group-header embed|omit`、`-C/--component merged|separate`、`--tag`、`--render/--page/--scale/--render-out`、`--mode region/--region/--json/--stdin`。

**区域提取 JSON 契约**（`RegionRequest`，字段大小写不敏感）：

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

内核返回 `RegionResponse`：每个区域含 `kind`、`ocrUsed`、`pipeNumbers`（编号区）、`table`（表格区的行×列网格）、`words`（区域内全部字框，用于叠加/调试）。**表格分列完全由字形几何涌现**（行按 y 聚类、列按墨盒水平间隙切分），**无任何硬编码列坐标**；矢量文本优先，仅在区域无矢量文字时用 MuPDF 内置 **Tesseract OCR**（专业库，**非** Windows 系统 OCR）兜底。

### EGexcel2df — 两份 Excel 图纸对比

```bash
EGexcel2df old.xlsx new.xlsx -o changes.xlsx
```

以 `PIPE NO` 为主键分组，组内用 `TABLE + DN(mm) + 项目/DESCRIPTION + NO` 作为项目键，识别项目新增/删除与 QTY 变化，输出多工作表 Excel。

### EGpdf2df — 两份 PDF 图纸差异

```bash
EGpdf2df baseline.pdf target.pdf -o diff.pdf
EGpdf2df --json request.json      # 或 stdin 契约
```

差异检测**分两步、彼此独立**：

1. **图形/图元差异**——先按两页**文字包围盒涂白排除**文字区域，再做边缘/像素比对，**确保文字变化不被误报为图形变化**；配准优先 ORB + 单应性（纯线条图特征不足时降级为不平移）。
2. **文字/注释差异**——独立于像素对比，按位置匹配基线/目标文字，识别**增 / 删 / 改 / 移动**。

输出带**红色标注**的 `diff.pdf` 与 JSON 摘要；所有参数（DPI、阈值、配准、标注样式）由 `EGpdf2df.Engine/config.json` 驱动。

---

## 📖 详细文档

- 中文使用说明：[docs/README_zh.md](docs/README_zh.md)
- English：[docs/README_en.md](docs/README_en.md)
- 技术文章（CSDN 风格）：[docs/CSDN_等轴测PDF材料表提取与对比.md](docs/CSDN_等轴测PDF材料表提取与对比.md)

---

## ⚖️ 许可证

本项目以 **GNU Affero General Public License v3.0 (AGPL-3.0)** 开源。详见 [LICENSE](LICENSE)。

版权 © 2026 HerryABU。