# EGtools

**工程图纸材料表提取与对比工具集** · A toolkit for extracting and comparing engineering drawing material tables

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)

EGtools 是一套面向 **CAD 等轴测（isometric）PDF 图纸** 的桌面工具，包含三个可独立运行、也可在统一图形界面中串联/批量使用的部件：

| 组件 | 类型 | 作用 |
| --- | --- | --- |
| **EGpdf2excel** | CLI | 从 CAD 等轴测 PDF 中**直接读取可编辑矢量文本**（无 OCR），提取 `FABRICATION / ERECTION MATERIALS` 材料表 → CSV / XLSX。 |
| **EGexcel2df** | CLI | 对比两份图纸 Excel（旧版 vs 新版），以 `PIPE NO` 为主键，输出变化清单（新增 / 删除 / QTY 变化）。 |
| **EGtools (GUI)** | WinUI 3 桌面应用 | 整合上面两个工具，支持**拖拽上传、批量、串联（pipeline）**，界面**中英文双语**。 |

> 引擎一致性：EGpdf2excel 使用 [MuPDFCore](https://www.nuget.org/packages/MuPDFCore)（MuPDF 的 .NET 绑定），与 Python / Node / C++ 参考实现同源，中文与多语言文本均正确解析。

---

## 📁 仓库结构

```
EGtools/
├── EGtools.Core/        # 共享引擎库（PDF 提取、日志、Excel 工具）
├── EGpdf2excel/         # CLI：PDF 材料表 → CSV/XLSX
├── EGexcel2df/          # CLI：两份 Excel 图纸对比
├── EGtools.Gui/         # WinUI 3 图形界面（提取 / 对比 / 串联 / 设置 / 关于）
├── docs/                # 完整使用文档（中文 / English）
│   ├── README_zh.md
│   └── README_en.md
├── redist/              # 第三方运行库（被 .gitignore 忽略，见 redist/README.md）
├── build_all.ps1        # 一键构建 + 打包脚本（PowerShell）
├── build_installer.iss  # Inno Setup 安装包定义
├── .gitignore
├── LICENSE              # AGPL-3.0
└── README.md            # 本文件
```

构建产物（`bin/`、`obj/`、`DIST/`、`installer/` 等）与临时日志均已被 `.gitignore` 排除，不会进入版本库。

---

## 🛠 先决条件

- **Windows 10 1809+ / Windows 11 (x64)**
- **.NET SDK 9**（构建 CLI 与 GUI 需要；GUI 运行依赖下方 Windows App Runtime）
- **Windows App Runtime 1.6**（WinUI 3 运行依赖，安装包会自动补装；绿色运行需本机已装）
- **Visual Studio 2022**（含“使用 C++ 的桌面开发”与“.NET 桌面开发”工作负载）可选，用于 GUI 设计
- **Inno Setup 6**（仅打包安装程序时需要）
- 第三方运行库见 [`redist/README.md`](redist/README.md)（VC++ 2022 x64 运行库 + Windows App Runtime 1.6 安装包）

---

## 🔨 构建

> `build_all.ps1` 使用脚本所在目录作为仓库根，并调用 PATH 中的 `dotnet`，因此可在任意克隆位置运行。

```powershell
# 1) 准备 redist/（首次构建前，按 redist/README.md 放入运行库）
# 2) 一键构建 Core + 两个 CLI（自包含发布）+ GUI + 安装包
.\build_all.ps1
```

构建完成后：

- CLI 自包含产物：`DIST/EGpdf2excel/`、`DIST/EGexcel2df/`
- GUI 产物：`DIST/EGtools/`
- 安装包：`installer/EGtools-3.0.0-x64.exe`

也可单独构建某个项目：

```powershell
dotnet publish EGpdf2excel/EGpdf2excel.csproj -c Release -r win-x64 --self-contained
dotnet publish EGtools.Gui/EGtools.Gui.csproj -c Release -r win-x64 -p:PublishProfile=FolderProfile
```

---

## 📖 使用文档

完整的使用说明（安装、图形界面各页、命令行参数、对比逻辑）见：

- 中文：[docs/README_zh.md](docs/README_zh.md)
- English：[docs/README_en.md](docs/README_en.md)

---

## ⚖️ 许可证

本项目以 **GNU Affero General Public License v3.0 (AGPL-3.0)** 开源。详见 [LICENSE](LICENSE)。

版权 © 2026 HerryABU。
