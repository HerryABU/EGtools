# EGtools v3.0.0

**A toolkit for extracting and comparing engineering drawing material tables** — made and signed by **HerryABU**.

EGtools is a desktop toolkit for CAD isometric PDF drawings. It bundles two command-line tools that can run independently, or be chained / batched from a single graphical interface:

| Tool | Purpose |
| --- | --- |
| **EGpdf2excel** | Reads the **editable vector text directly** from CAD isometric PDFs (no OCR) and extracts the `FABRICATION / ERECTION MATERIALS` tables to CSV / XLSX. |
| **EGexcel2df** | Compares two drawing Excel files (old vs new) keyed by `PIPE NO` and produces a change report (added / removed / QTY changed). |
| **EGtools (GUI)** | A WinUI 3 app that integrates both tools with **drag-and-drop, batch, and chained (pipeline)** modes, in a **bilingual (中文 / English)** interface. |

> Engine consistency: EGpdf2excel uses MuPDFCore (the .NET binding of MuPDF), the same engine as the Python / Node / C++ references, so Chinese and multilingual text parse correctly.

---

## 1. Installation

### 1.1 Installer (recommended)

Run **`EGtools-3.0.0-x64.exe`** (built with Inno Setup):

- Automatically detects and installs the **Windows App Runtime** (the WinUI 3 dependency; skipped if already present).
- Installs to `C:\Program Files\EGtools\` by default and creates a desktop shortcut.
- Option to launch on finish.

### 1.2 Portable (no install)

Copy the `DIST\` folder as-is. Requires the **Windows App Runtime 1.7.x** on the machine (or run `runtime\windowsappruntimeinstall.exe`).
Double-click `EGtools.exe` to launch the GUI; see Section 3 for the CLI tools.

---

## 2. Graphical Interface (EGtools GUI)

After launching `EGtools.exe`, the left navigation shows:

- **Extract** — corresponds to EGpdf2excel.
- **Compare** — corresponds to EGexcel2df.
- **Pipeline** — a one-click flow that runs Extract then Compare.
- **Settings** — language (中文 / English), theme (system / light / dark).
- **About** — version, author, license, and the documentation entry point.

### 2.1 Extract page

- **Drag & drop**: drop PDF files onto the dashed box, or click the button to open a multi-select picker.
- **Batch**: add multiple PDFs at once; the list shows selected files with per-item remove and clear.
- **Options**:
  - `Format`: CSV (default) / XLSX / both.
  - `Component layout`: merged (component folded into DESCRIPTION, 8 cols, default) / separate (its own column, 9 cols).
  - `Group header`: embed (prefix PDF in-table banners like FITTINGS/VALVES/FLANGES onto each member row, default) / omit.
  - `Tag`: output filename suffix (default `V3`).
  - `Reference xlsx` (optional): supplies a DESCRIPTION vocabulary to improve word-spacing reconstruction for space-less PDFs.
  - `Output folder` (optional): defaults to each PDF's own directory.
- Click **Run**; the progress bar and log update live. When done, the **Open** button reveals the output folder.

### 2.2 Compare page

- Drag / pick the **old** and **new** drawing Excel files.
- Optionally set the output report path (default `图纸变化清单_<timestamp>.xlsx`).
- Click **Run** to generate the change report.

### 2.3 Pipeline page

- Select or drop the **old** (PDF or Excel) and **new** (PDF or Excel) files.
- The tool auto-detects: PDFs are first extracted to a temporary Excel, then compared with the other side, producing one unified report.
- Supports component-layout / group-header options.

### 2.4 Settings

- **Language**: 中文 / English (applies instantly, persisted to `lang.txt`).
- **Theme**: system / light / dark.

---

## 3. Command Line (CLI)

Both CLI tools can run **fully independently** and are bundled in the installer.

### 3.1 EGpdf2excel

```
EGpdf2excel [inputs...] [options]
```

- **inputs**: PDF file(s), a directory, or a wildcard glob; defaults to `*.pdf` in the current directory when omitted.
- **options**:

  | Option | Description |
  | --- | --- |
  | `-o, --output DIR\|FILE` | Output directory, or a single output file when one input is given (extension set by `--format`). Defaults to each input's directory. |
  | `-f, --format csv\|xlsx\|both` | Output format (default `csv`). |
  | `-r, --ref FILE.xlsx` | Reference workbook; its DESCRIPTION vocabulary reconstructs word spacing in space-less PDFs. |
  | `-G, --group-header MODE` | In-table group banner handling: `embed` (default, prefix to DESCRIPTION) / `omit`. |
  | `-C, --component MODE` | Component column layout: `merged` (default, folded into DESCRIPTION, 8 cols) / `separate` (own column, 9 cols). |
  | `--tag NAME` | Filename tag (default `V3`) → `<pdf>_<tag>.csv/.xlsx`. |
  | `-v, --verbose` | Detailed progress (per-page, warnings) to stderr. |
  | `-h, --help` | Show this help and exit. |

- **Examples**:

  ```
  EGpdf2excel -v *.pdf -f both -r ref.xlsx -o out/
  EGpdf2excel C:/scan/408-101-051*.pdf --tag V4 -f xlsx
  ```

### 3.2 EGexcel2df

```
EGexcel2df <old.xlsx> <new.xlsx> [options]
```

- **options**:

  | Option | Description |
  | --- | --- |
  | `-h, --help` | Show help. |
  | `-v, --version` | Show version (3.0.0). |
  | `-o, --output <file>` | Output Excel path (default `图纸变化清单_<timestamp>.xlsx`). |
  | `--sheet1 <name\|index>` | Worksheet to read from the old file (default first). |
  | `--sheet2 <name\|index>` | Worksheet to read from the new file (default first). |

- **Comparison logic**: groups by `PIPE NO`; within each group, the item key is `TABLE + DN(mm) + 项目/DESCRIPTION + NO`:
  - A `PIPE NO` present on only one side → whole PIPE added / removed.
  - An item key present on only one side → item added / removed.
  - Present on both sides but `QTY` differs → QTY changed (±N).
  - `QTY` identical → no change (not reported).

- **Examples**:

  ```
  EGexcel2df old.xlsx new.xlsx -o report.xlsx
  EGexcel2df old.xlsx new.xlsx --sheet1 "材料表" --sheet2 1
  ```

---

## 4. Version & License

- **Version**: 3.0.0
- **Author / Signer**: HerryABU
- **License**:AGPL-3.0
- **Runtime**: Windows 10/11 (x64); .NET 10 runtime (GUI uses WinUI 3 via Windows App Runtime 1.7).
