# 告别手工核图：基于 .NET + MuPDFCore 的 CAD 等轴测 PDF 材料表提取与新旧版本对比实战

> 关键词：C# / .NET、CAD 等轴测图、PDF 矢量文本提取、MuPDFCore、Excel 差异比对、WinUI 3
> 项目：EGtools（AGPL-3.0 开源，含 GUI + 两个 CLI）

---

## 一、引言

工程审图里，最枯燥也最容易出错的环节之一，就是要把几十页 CAD 等轴测（isometric）PDF 图纸里的 `FABRICATION / ERECTION MATERIALS` 材料表，手工敲进 Excel，再和上一版图纸逐行比对——哪些管段（`PIPE NO`）新增 / 删除、哪些管件数量变了。

本文要讲的 EGtools，就是用纯 C# 把这件事自动化：**一份 PDF 进去，矢量文本直接变 Excel；两份 Excel 一比，变化清单自动出来**。不堆术语，只讲真实工程里踩过的坑和能复用的代码。

---

## 二、背景：为什么这件事没那么简单

1. **等轴测 PDF 是"矢量图 + 矢量文本"，不是扫描件** → 不能用 OCR（慢、还丢精度），必须直接读取可编辑文本。
2. **文本是"被放进去"的字形（glyph）**，没有天然的词 / 列结构 → 需要按坐标自己聚合。
3. **很多 CAD 导出的英文描述没有空格**（`WELDOLET`、`WELDNECKFLANGE`），直接按词切会乱。
4. **中文与多语言混排**，CMap 处理不对就会乱码。
5. 最终还要和旧版对比，**按管线号对齐**。

---

## 三、核心实现思路

整体三步、三个组件，两个处理引擎各自独立成 DLL（`EGpdf2excel.Engine` 与 `EGexcel2df.Engine`），共享库 `EGtools.Core` 只放日志转发：

| 组件 | 作用 |
| --- | --- |
| **EGpdf2excel.Engine**（DLL） | PDF → 矢量文本提取 → CSV / XLSX 的核心引擎（MuPDFCore） |
| **EGexcel2df.Engine**（DLL） | 两份 Excel → 变化清单的核心引擎 |
| **EGpdf2excel**（CLI） | 上述 PDF 引擎的命令行前端 |
| **EGexcel2df**（CLI） | 上述对比引擎的命令行前端 |
| **EGtools**（WinUI 3 GUI） | 把两个引擎串起来，支持拖拽、批量、pipeline，中英文双语 |

关键决策：**用 MuPDFCore（PyMuPDF / C++ 同源引擎）做位置文本提取**，不用 PdfPig——后者的 CMap 处理在我们的中文样本上会乱码。跨语言做参考实现时，**务必保证引擎同源**，否则中文解析结果对不齐。

---

## 四、关键代码示例

### 1. 为什么不用 OCR：从字形重建词与行

MuPDFCore 给出的是单个字形及其原点坐标。我们把同一行（y 接近）的字形按 x 排序，x 间隔超过阈值才切词：

```csharp
static List<Word> GetWords(MuPDFStructuredTextPage st, double H)
{
    var gs = new List<(double x0, double y0, double x1, double y1, double yc, string text)>();
    foreach (var block in st)
    {
        if (block.Type != 0) continue;               // 0 = 文本块，过滤图片块
        foreach (var line in block)
        {
            var chars = line.Characters.ToList();
            for (int i = 0; i < chars.Count; i++)
            {
                var c = chars[i];
                double x0 = c.Origin.X;
                double baseline = c.Origin.Y;
                double sz = Convert.ToDouble(c.Size);
                double w = (i + 1 < chars.Count) ? (chars[i + 1].Origin.X - x0) : sz * 0.5;
                if (w < 0.3) w = sz * 0.5;
                gs.Add((x0, baseline - sz, x0 + w, baseline, baseline - sz / 2, c.Character.ToString()));
            }
        }
    }
    // 之后：按 y（容差 4pt）聚类成行，行内按 x 排序，x 间隔 > 8pt 切词
    // ...
}
```

说明：

- `block.Type != 0` 过滤掉图片块，只留文字。
- **词切分的 8pt 阈值不是拍脑袋**：PDF 表格里同一字段的字母间距远小于列间距，用"间距跳变"区分列，比正则更稳。
- MuPDF 的 y 已是自上而下（top-down），与 PyMuPDF / C++ 一致，省去翻转坐标的麻烦。

### 2. 按列边界归列：让每一格回到它该在的列

材料表列位置相对固定，但不同图纸会偏移。我们先在表头区探测 `NO / DN / QTY / WEIGHT` 的横坐标，作为列边界：

```csharp
double noX  = cols.ContainsKey("NO") ? cols["NO"] : 1123;
double dnX  = cols.ContainsKey("DN") ? cols["DN"] : 1142;
double qtyX = cols.ContainsKey("QTY") ? cols["QTY"] : 1548;
double wtX  = cols.ContainsKey("WEIGHT") ? cols["WEIGHT"] : qtyX + 58;

string ColOf(double xc)
{
    if (xc < (noX + dnX) / 2)   return "NO";
    if (xc < (dnX + compX) / 2) return "DN";
    if (xc < qtyX - 25)         return "DESC";
    if (xc < (qtyX + wtX) / 2)  return "QTY";
    return "WT";
}
```

每个数据字形按其 x 落入哪个区间，自动归到对应列。`NO` 列的数字是"锚点"，每一行以它对齐，`QTY / DN / DESC` 都按"离哪个锚点最近"分配到该行。

### 3. 难点：无空格描述怎么分词

很多 CAD 导出的英文描述是挤在一起的，比如 `WELDOLET`、`WELDNECKFLANGE`。我们用一份"参考工作簿"里人工规范的 `DESCRIPTION` 词表，做**最长匹配重分词**：

```csharp
// 从参考 xlsx 的 DESCRIPTION 列收集词表（含 # 数字模式，如 DN50 -> DN#）
static RefDict BuildRefDict(List<string> descriptions) { /* ... */ }

// 贪心最长匹配，把 "WELDOLET" 还原成 "WELD OLET"
static string ReconstructSpaces(string s, RefDict dict)
{
    string up = s.ToUpperInvariant();
    var outb = new StringBuilder();
    int pos = 0;
    while (pos < s.Length)
    {
        int L = dict.LongestMatch(up, pos);   // 当前位置取最长的已知词
        if (L > 0) { if (outb.Length > 0) outb.Append(' '); outb.Append(s.Substring(pos, L)); pos += L; }
        else { /* 跳过无法识别的片段，避免把数字拆成 2 6 3 3 */ ... }
    }
    return outb.ToString();
}
```

要点：参考词表带 `#` 数字模式（如 `DN50` → `DN#`），这样 `50` 不会被判成 `5 0`。这一步让机器输出直接对齐人工排版，下游对比才准。

### 4. 零依赖写 XLSX：别再为一个导出引一个库

输出 Excel 我们没用 EPPlus / NPOI，而是**直接构造 OOXML 的 zip 包**——其实只比写 CSV 多几十行：

```csharp
static void WriteXlsx(string path, List<RowOut> rows)
{
    using var zip = new ZipArchive(File.Create(path), ZipArchiveMode.Create);
    AddEntry(zip, "[Content_Types].xml", CT);
    AddEntry(zip, "_rels/.rels", RELS);
    AddEntry(zip, "xl/workbook.xml", WB);
    AddEntry(zip, "xl/_rels/workbook.xml.rels", WB_RELS);
    // sheet1.xml：<c r="A1" t="inlineStr"><is><t xml:space="preserve">...</t></is></c>
    AddEntry(zip, "xl/worksheets/sheet1.xml", BuildSheetXml(rows));
}

// 数字列写 <v>1.5</v>（用 InvariantCulture），文本列用 inlineStr
if (IsNumCol(name) && double.TryParse(val, NumberStyles.Any, CultureInfo.InvariantCulture, out var num))
    sb.Append($"<c r=\"{cl}\"><v>{num.ToString(CultureInfo.InvariantCulture)}</v></c>");
```

好处：发布体积更小、没有第三方许可（license）顾虑、不会因为某个库升级而炸。对比工具也同理，自己解析 `xl/sharedStrings.xml` + `sheet1.xml` 读、自己写多 sheet 报告。

### 5. 新旧对比：以管线号为主键

两份 Excel 都提取成统一列结构后，差异比对逻辑非常纯粹：

```csharp
// 项目唯一键：TABLE + DN(mm) + 项目/DESCRIPTION + NO
static string KeyOf(Dictionary<string, string> r) =>
    $"{Get(r, "TABLE")}\u001f{Get(r, "DN(mm)")}\u001f{Get(r, "项目/DESCRIPTION")}\u001f{Get(r, "NO")}";

static List<ChangeRec> Compare(List<Dictionary<string, string>> oldRows,
                               List<Dictionary<string, string>> newRows)
{
    var oldByPipe = oldRows.GroupBy(r => Get(r, "PIPE NO")).ToDictionary(g => g.Key, g => g.ToList());
    var newByPipe = newRows.GroupBy(r => Get(r, "PIPE NO")).ToDictionary(g => g.Key, g => g.ToList());
    // 1) PIPE NO 只在旧版 -> 整条删除
    // 2) PIPE NO 只在新版 -> 整条新增
    // 3) 同一 PIPE 内，项目键缺失 -> 项目增/删
    // 4) 项目键都在但 QTY 不同 -> QTY 变化(±N)
    // 5) QTY 相同 -> 不输出
}
```

一个容易忽略的细节：同一管线里可能有多行完全一样的 `(TABLE, DN, DESC, NO)`（比如两根同规格螺栓）。我们在键后追加 `#2`、`#3` 解决碰撞，并按出现顺序对齐，避免"新增 N 行 / 删除 N 行"的误报。报告还会按"变化类型统计 / PIPE 变化统计 / 每个变化管段单独一页"多 sheet 输出，方便审图人直接定位。

---

## 五、实践经验总结（踩过的坑）

1. **绝不降级为 OCR / 像素比对**。CAD PDF 的文本是矢量的、可复制的，直接读 `MuPDFStructuredTextPage` 才是正道；OCR 既慢又丢坐标，下游无法对齐列。本文所有提取都基于**可编辑矢量文本**，无损、可复制。
2. **中文别用错引擎**。PdfPig 的 CMap 在我们的样本上把中文解成乱码；换成 MuPDF 同源的 MuPDFCore 后正常。跨语言（Python / Node / C++）做参考实现时，务必保证引擎同源。
3. **WinUI 3 非打包部署的 `0xC000027B` 崩溃**。self-contained 会把 `Microsoft.ui.xaml.dll` 等松散框架 DLL 塞进 app 目录；若目标机已注册更新的 WindowsAppRuntime，bootstrap 解析到系统包，进程里同时加载两个不兼容版本 → 原生断言**静默崩溃**（无窗口、无日志）。我们改回 **framework-dependent**（只吃系统单一运行时）解决。
4. **`dotnet publish` 会丢 `PRI / XBF`**。非打包 WinUI 3 靠 app 的 `*.pri` + 松散 `*.xbf` 解析 `ms-appx:///Xxx.xaml`；`dotnet build` 有，但 `publish` 会丢掉 → 发布后 `LoadComponent` 抛 `XamlParseException`。修复：加一个 `AfterTargets="Publish"` 的 target 把 `*.xbf` 和 `*.pri` 拷回发布目录。**排查口诀：bin 能跑、DIST 不能跑 = publish 丢资源**。
5. **输出编码**。CSV 写 `UTF-8 BOM`（`\uFEFF`），Excel 打开中文才不乱码；XLSX 数字列务必用 `InvariantCulture`，否则某些区域设置下 `1.5` 变成 `1,5` 导致下游解析失败。

---

## 六、结语与互动

工具已开源（AGPL-3.0），包含 WinUI 3 GUI 与两个独立 CLI，支持中英文界面、批量与一键 pipeline。把"读图 — 提取 — 对比"从小时级降到秒级，审图的人只需要看一眼变化清单。

如果你也在处理工程图纸 / 等轴测 PDF，欢迎在评论区聊聊你们的方案：是用 OCR、iText、PdfPig 还是 MuPDF？遇到中文乱码、列对不齐、版本对比怎么做的？如果觉得本文的"位置提取 + 词表重分词"思路有用，点个赞或收藏，后续我再写 GUI（WinUI 3）那部分的部署坑与无头环境调试技巧。

---

*作者：HerryABU · 转载请注明出处。项目仓库：https://github.com/HerryABU/EGtools.git（AGPL-3.0）。*
