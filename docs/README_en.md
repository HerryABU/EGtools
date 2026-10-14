# EGtools Manual (English)

**A toolkit for extracting and comparing engineering drawing material tables** — made by **HerryABU**.

EGtools targets **CAD isometric PDF drawings** and **piping material tables (Excel)**. It ships four parts: three **fully standalone** command-line kernels, plus a **unified graphical front-end** that drives them.

| Part | Purpose |
| --- | --- |
| **EGtools (GUI)** | Qt 6 desktop app. Open a PDF → region picking / batch extraction → drawing diff → preview the red-annotated diff. The only GUI. |
| **EGpdf2excel** | Reads the **editable vector text directly** from CAD isometric PDFs (no OCR) and extracts the `FABRICATION / ERECTION MATERIALS` tables to CSV / XLSX. |
| **EGexcel2df** | Compares two drawing Excel files (old vs new), keyed by `PIPE NO`, and reports changes (added / removed / QTY changed). |
| **EGpdf2df** | Compares two drawing PDFs (old vs new) and emits a **red-annotated** PDF plus a JSON summary. |

> Engine consistency: PDF parsing uses MuPDFCore (the .NET binding of MuPDF) — the same engine as the Python / Node / C++ references — so Chinese and multilingual text parse correctly.
>
> The GUI **bundles its whole runtime**: the kernels are published as .NET **self-contained**, and Qt / VC++ runtimes ship alongside ⇒ **no .NET, Qt or VC++ runtime installation is required on the target machine**.

**Contents**

- [1. Installation](#1-installation)
- [2. Quick start](#2-quick-start)
- [3. The graphical interface](#3-the-graphical-interface)
- [4. Command line (CLI)](#4-command-line-cli)
- [5. Configuration](#5-configuration)
- [6. Using it on Linux](#6-using-it-on-linux)
- [7. FAQ](#7-faq)
- [8. Version & license](#8-version--license)

---

## 1. Installation

### 1.1 Windows installer (recommended)

1. Run `EGtools-<version>-x64.exe` (64-bit Intel/AMD) or `EGtools-<version>-arm64.exe` (Windows on ARM).
2. The installer will:
   - install to `Program Files\EGtools` by default;
   - create Start-menu entries: the GUI, the three CLI tools, plus the "使用文档 (中文)" and "Documentation (English)" shortcuts;
   - copy this manual, the Chinese manual and the technical article into `<install dir>\docs`;
   - tick **"add the command-line tools (EGpdf2excel / EGexcel2df / EGpdf2df) to PATH" by default** — with it, you can run `EGpdf2excel` etc. from any prompt, and **the PATH entries are removed again on uninstall** (the option is only offered for an all-users install, since a per-user install may not write the system PATH);
   - offer a **desktop shortcut as an optional extra** (the checkbox under "Additional icons" is **not** ticked by default);
   - offer to launch the app when finished.
3. Uninstall via Windows "Apps & features", or run `unins000.exe` in the install directory. Uninstalling also removes the four program folders (GUI + three kernels) and cleans up the PATH entries as described above.

> **Want a per-user install** (no Program Files, no admin)? Change the install location in the wizard, or pass `/CURRENTUSER` for a silent install.
>
> ⚠️ **Do not mix up architectures**: the installers carry an architecture constraint — the arm64 package is **refused** on a plain x64 machine (and vice versa). This is intentional, so you never install a build that cannot start.
>
> ⚠️ Installing to Program Files requires **administrator rights**. If you are a standard user the installer will offer a per-user install instead.

### 1.2 Windows portable

Copy the whole `DIST\` folder to the target machine and run `EGtoolsFrontend\EGtoolsFrontend.exe`. The three kernels live in `DIST\EGpdf2excel\`, `DIST\EGexcel2df\` and `DIST\EGpdf2df\` and can be used straight from a prompt.

### 1.3 Verify the installation

```bash
EGpdf2excel --version          # prints the version
EGpdf2df --help                # shows help
EGexcel2df --help              # shows help
```

> ⚠️ Of the three kernels **only `EGpdf2excel` supports `--version`**; the other two reply `Unknown option` and exit with code 2. **Use `--help` for liveness checks in scripts.**

---

## 2. Quick start

**Scenario 1 — export one drawing's material table to Excel**

1. Launch EGtools; on the **Home** page click "open a PDF and pick regions" and choose the drawing.
2. Drag a rectangle over the material table (pick `global_table` in the mode combo at the top).
3. Click "Extract (kernel)"; the table and pipe numbers are shown in separate panes, and the word boxes are overlaid back onto the canvas.
4. Click "Export" in the results panel to get CSV / XLSX.

**Scenario 2 — extract a batch of drawings**

1. In the left navigation go to **Material extraction**.
2. Add several PDFs (or a whole folder).
3. Choose the output format (CSV / XLSX / both) and the output folder, then click "Start batch extraction".
4. When it finishes, click "Open" to reveal the output folder.

**Scenario 3 — compare old vs new drawings**

- **PDF vs PDF**: navigation → **Drawing diff** → pick baseline (old) and target (new) → get a red-annotated `diff.pdf` previewed in-app. Click any row in the diff list to jump to and highlight that change.
- **Excel vs Excel**: navigation → **Excel compare** → pick two workbooks → get a change report.

---

## 3. The graphical interface

### 3.1 Navigation

Left navigation tree (`Ctrl+B` collapses it):

| Item | Description |
| --- | --- |
| **Home** | Entry cards; also shows the **kernel status** (is the embedded channel available, where does hostfxr come from) and the **render path** (GPU or CPU raster) — check these two lines first when the picture looks wrong. |
| **Material extraction** | Batch workbench (multiple files, format, column layout, progress and log). |
| **PDF tools → Render & region extract** | Main workspace: canvas + region picking + export region JSON + extract, with word boxes overlaid back onto the canvas. |
| **PDF tools → Material-table OCR boost** | Same workspace, running with the OCR fallback enabled — the recognizer kicks in only when a picked region has **no vector text**. |
| **Drawing diff** | PDF drawing comparison + diff list + in-app preview. |
| **Excel compare** | Change report for two Excel workbooks. |
| **Region schemes** | Manage named region schemes (stored in `Documents\EGtools\schemes.json`) and re-apply them to the canvas in one click. |

### 3.2 Shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+B` | Collapse / expand the navigation pane (cycles 180 / 220 / 280 px) |
| `Ctrl+Shift+B` | Collapse / expand the thumbnail pane (140 / 180 / 240 px) |
| `Ctrl+J` | Collapse / expand the results pane (160 / 240 / 360 px) |
| `Ctrl+1` / `Ctrl+2` | Canvas view: single page / continuous |
| `F1` | Open this manual |

> All splitters are **deliberately not draggable** — adjust panes with the presets and the collapse shortcuts so the layout never gets dragged into a mess.

### 3.3 The three region semantics

When you drag a region, the mode combo at the top tells the kernel how to interpret it:

| Semantic | Internal name | Used for |
| --- | --- | --- |
| **Global table** | `global_table` | the whole material table |
| **Detail table** | `detail_table` | a sub-table / detail block |
| **Material number region** | `material_no_region` | where the piping / line numbers appear (used for the one-to-one check against the material table) |

### 3.4 View and theme

- **View → Render backend**: three choices — `Auto (use GPU if the probe passes, otherwise raster)` / `GPU accelerated` / **`CPU raster (most stable — cannot go black on any machine)`**. The default is CPU raster (tiled vector rendering — no GPU needed, and it cannot go black). Switching applies immediately and persists.
- **View → Pane sizes**: set the navigation width, thumbnail width and results-pane height individually — pairs with the collapse shortcuts.
- **Theme**: light / dark / follow system. The **entire** UI (including the Home page) follows, and the choice is remembered.
- **Layout**: sidebar / vertically stacked (handier on narrow or portrait screens).

---

## 4. Command line (CLI)

All three kernels run standalone: **stdout carries JSON results, stderr carries logs**, which makes them easy to script, batch or schedule.

### 4.1 EGpdf2excel — PDF material-table extraction

```
EGpdf2excel [inputs...] [options]
```

- **inputs**: PDF file(s), a directory, or a wildcard glob; defaults to `*.pdf` in the current directory.

| Option | Description |
| --- | --- |
| `-o, --output DIR\|FILE` | Output directory, or a single output file when one input is given (extension set by `--format`). Defaults to each input's own directory. |
| `-f, --format csv\|xlsx\|both` | Output format (default `csv`). |
| `-r, --ref FILE.xlsx` | Reference workbook; its `DESCRIPTION` vocabulary reconstructs word spacing in space-less PDFs. |
| `--config FILE.json` | Extraction config (vocab / column coords / page ranges); defaults to the `config.json` next to the exe, then to the built-in defaults. |
| `-G, --group-header embed\|omit` | How to treat in-table group banners (FITTINGS / VALVES / FLANGES…): `embed` (default, prefix onto each row's description) / `omit`. |
| `-C, --component merged\|separate` | Component column layout: `merged` (default, folded into the description, 8 cols) / `separate` (its own column, 9 cols). |
| `--tag NAME` | Output filename tag (default `V3`) → `<pdf>_<tag>.csv/.xlsx`. |
| `--render --page N --scale S --render-out F.bmp` | Rasterize a page (for the GUI, or for diagnosis). |
| `--mode region --json FILE` / `--stdin` | Region extraction: the request JSON may come from a file or stdin. |
| `-v, --verbose` | Per-page progress and warnings to stderr. |
| `--version` / `-h, --help` | Version / help. |

**Examples**

```bash
EGpdf2excel drawing.pdf                              # one file  → CSV
EGpdf2excel -v *.pdf -f both -r ref.xlsx -o out/     # batch     → CSV + XLSX
EGpdf2excel "C:/scan/408-101-051*.pdf" --tag V4 -f xlsx
```

### 4.2 EGexcel2df — compare two drawing spreadsheets

```
EGexcel2df <old.xlsx> <new.xlsx> [options]
```

| Option | Description |
| --- | --- |
| `-o, --output <file>` | Output workbook path (default `图纸变化清单_<timestamp>.xlsx`). |
| `--sheet1 <name\|index>` | Worksheet to read from the old file (default: the first one). |
| `--sheet2 <name\|index>` | Worksheet to read from the new file (default: the first one). |
| `-v, --version` / `-h, --help` | Version / help. |

**Comparison logic**: group by `PIPE NO`; inside each group the item key is `TABLE + DN(mm) + 项目/DESCRIPTION + NO`:

- a `PIPE NO` present on only one side → the whole PIPE is added / removed;
- an item key present on only one side → item added / removed;
- present on both sides but `QTY` differs → QTY changed (±N);
- identical `QTY` → not reported.

```bash
EGexcel2df old.xlsx new.xlsx -o report.xlsx
EGexcel2df old.xlsx new.xlsx --sheet1 "材料表" --sheet2 1
```

### 4.3 EGpdf2df — compare two drawing PDFs

```
EGpdf2df <baseline.pdf> <target.pdf> -o diff.pdf
EGpdf2df --json request.json        # or --stdin
```

| Option | Description |
| --- | --- |
| `-o, --output F` | Annotated PDF output path. |
| `-p, --pages L` | Limit the pages compared; default `filter` semantics = pair all pages by drawing number first, then keep the listed pages. |
| `--page-mode filter\|pin` | Semantics of `-p`: `filter` (default) / `pin` (only these pages are compared). `--pin-pages` is an alias of `pin`. |
| `--graphic-method vector\|pixel` | Graphic-diff method: `vector` (default, pure vector detection) / `pixel` (rasterize + image comparison). |
| `--mark-style semi\|solid\|outline` | Graphic mark style (default `semi`: translucent fill + colored stroke). |
| `--mark-stroke-color #RRGGBB` | Graphic mark color (defaults to an automatically chosen contrasting color). |
| `--stroke-width PT` / `--stroke-opacity A` | Vector mark line width (default 1.0 pt) / opacity (default 0.55). |
| `--vector-tol PT` | Vector segment match tolerance (default 3.0 pt). |
| `--near-ink-tol PT` | Ink-overlap tolerance (default 0.5 pt). |
| `--text-added-color` / `--text-removed-color #RRGGBB` | Added (default `#FF0000` red) / removed (default `#0000FF` blue) text colors. |
| `--text-insert-direction down\|up\|right\|left` | Insert direction for added text (default `down`). |
| `--report-mode cluster\|run` | Graphic-diff reporting (default `cluster`). |
| `--config FILE` | Diff config (pdf2df config JSON). |
| `-h, --help` | Help. |

**The diff runs as two independent passes** so they never mis-report each other:

1. **Graphics / geometry** — pure **vector detection** by default (geometric calibration + collinear coverage matching); no pixel comparison.
2. **Text / annotation** — matches text on both sides by position and classifies **added / removed / changed / moved**. The matched glyphs are lifted out of the original PDF content stream and re-drawn self-contained (reusing the **glyphs already embedded in the drawing** — so Chinese and other scripts annotate fine, and **no system font is required**).

**Pages are paired by the real drawing number** (the line/sheet number, e.g. `408.101.P02`), **not by page index** — every page's title block prints the same project number, so pairing by page index would be completely wrong. Anything present on one side only is reported as added / removed; pages without a drawing number fall back to a line-art fingerprint.

> **Self-check convention**: comparing a PDF against itself must yield **0 graphic diffs / 0 text diffs**. Anything else means the comparison chain introduced phantom differences — please report it together with the drawing.

### 4.4 Region-extraction JSON contract

The "regions" file exported by the GUI is exactly this structure (field names are case-insensitive); you can also write it by hand or generate it from a script:

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

- `bbox` is in **PDF points (pt)** with a **top-left origin and y growing downwards** (same as the rendered image);
- every returned region carries `kind`, `ocrUsed`, `pipeNumbers` (number regions), `table` (the row × column grid of a table region) and `words` (all word boxes inside the region, for overlay / debugging).

---

## 5. Configuration

Extraction and diffing parameters are **never hard-coded**; they live in config files shipped with the app, and **changes take effect without recompiling** (missing fields fall back to the built-in defaults):

| File | Contents |
| --- | --- |
| `EGpdf2excel.Engine\config.json` | extraction vocabulary, column coordinates, page ranges, group-banner rules |
| `EGpdf2df.Engine\config.json` | diff DPI, thresholds, registration, annotation styling |

Lookup order: the file given via `--config` → `config.json` next to the executable → built-in defaults.

**Common settings exposed in the GUI** (equivalent to CLI switches): output format, component column layout (merged / separate), group banners (embed / omit), output tag, reference vocabulary workbook, output folder; on the diff page — annotation colors, stroke width, opacity, tolerances and the graphic-diff method.

---

## 6. Using it on Linux

### 6.1 Install

```bash
sudo dpkg -i egtools-<version>-Linux.deb     # Debian / Ubuntu
sudo rpm  -i egtools-<version>-Linux.rpm     # Fedora / RHEL
```

- Installs to `/opt/egtools` and creates the `/usr/bin/egtools` symlink, a desktop entry and an icon;
- Afterwards run `egtools` from a terminal or search for "EGtools" in the application menu;
- The kernels live in `/opt/egtools/EGpdf2excel/` etc. and behave exactly as on Windows (the executables simply have **no `.exe` suffix**).

Uninstall:

```bash
sudo dpkg -r egtools        # or: sudo rpm -e egtools
```

### 6.2 Differences to be aware of

| Item | Note |
| --- | --- |
| Executable names | No `.exe` suffix: `EGpdf2excel`, `EGexcel2df`, `EGpdf2df`. |
| Portable use | Copy the release folder as-is; it still bundles the .NET runtime and the Qt runtime. |
| Dependencies | The packages have been verified on a **clean Ubuntu Server**: they run right after install, with no extra Qt / .NET packages. |
| Embedded channel | If the machine has **no .NET runtime installed**, the GUI shows "embedded unavailable, fell back to the command line" — that is a **by-design fallback and functionality is unaffected**. |

---

## 7. FAQ

**Q: I double-click the app and the window flashes and disappears.**
Make sure you launch it from its install / extracted directory and that the directory is **complete** (do not copy a single exe). Then start it from a terminal to see the error output:

```bash
EGtoolsFrontend.exe            # Windows
egtools                        # Linux
```

**Q: It reports a missing DLL / fails to start (error `0xC0000135`).**
The release folder is incomplete (a Qt or VC++ runtime DLL is missing). **Copy the whole folder**, don't cherry-pick the exe.

**Q: The canvas is all black.**
Usually a remote-desktop / virtual-machine session that only offers a very old OpenGL (GDI Generic 1.1), while GPU drawing needs GL 2.0. Fix: menu **View → Render backend: CPU raster (most stable — cannot go black on any machine)**, or set `EGTOOLS_NO_GPU=1`. The default backend is CPU raster, so this should not normally happen.

**Q: The UI says "embedded unavailable, fell back to the command line" — does that break anything?**
No. It is a by-design degradation: when the embedded channel (in-process call into the kernel DLL) is unavailable, the app spawns the CLI instead. Results are identical; each call is just a few dozen milliseconds slower.

**Q: On Linux, `egtools` reports `Permission denied`.**
The kernel executable lost its execute bit. Check:

```bash
ls -l /opt/egtools/EGpdf2excel/EGpdf2excel      # should be -rwxr-xr-x
```

**Q: The drawing diff reports lots of "added / removed" even though the two revisions look almost identical.**
Most likely a **page pairing mismatch**. Check that the drawing numbers (line/sheet numbers) of the two sides correspond. If the two revisions are structurally very different (e.g. one is CAD-direct, the other re-printed), please send both drawings so it can be reproduced.

**Q: The extracted descriptions have no spaces / the word breaks are wrong.**
MuPDF's structured text carries no spaces; the tool reconstructs them from glyph spacing. Pass `-r ref.xlsx` (or use the "reference xlsx" field in the GUI) with a description vocabulary to match your project's wording.

**Q: Chinese / special characters come out garbled.**
The Windows console code page may not be UTF-8. Run `chcp 65001` in `cmd` first, or redirect output to a file (`EGpdf2excel ... > out.txt`) and open it as UTF-8. The GUI is unaffected.

**Q: Which drawings are supported?**
It targets **CAD isometric PDFs** and works best when the text is **copyable vector text**. For scanned drawings (a page that is just an image) only the "material-table OCR boost" mode can attempt recognition, and accuracy then depends on scan quality — the design premise of this tool is **vector text**, not OCR.

---

## 8. Version & license

| Item | Value |
| --- | --- |
| Installer / release version | **4.3.0** |
| CLI kernel self-reported version (`EGpdf2excel --version`) | 4.2.1 |
| GUI self-reported version (`EGtoolsFrontend --version`) | 1.0.0 (Release) |
| Author / signer | HerryABU |
| License | **AGPL-3.0** |
| Runtime dependencies | **None externally** — the kernels are .NET 9 **self-contained**, Qt and VC++ runtimes ship with the package |
| Supported platforms | Windows 10 1809+ / 11 (x64, arm64), Linux x64 (deb / rpm) |

> ⚠️ The three version numbers have historically not been unified: the installer and release artefacts use **4.3.0**, while the kernel assemblies still self-report 4.2.1 and the GUI 1.0.0. Treat the installer version (4.3.0) as the public release version; future releases will unify all three.

---

### See also

- Repository README (development / build / architecture): `README.md`
- Full Chinese manual: `docs\README_zh.md`
- Technical article (implementation details, Chinese): `docs\CSDN_等轴测PDF材料表提取与对比.md`

Copyright © 2026 HerryABU. Released under the **GNU Affero General Public License v3.0**.
