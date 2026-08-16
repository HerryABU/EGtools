using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace EGtools.Core
{
    // ------------------------------------------------------------------
    //  Self-contained XLSX reader (no third-party NuGet dependency).
    //  Reads the first (or named/indexed) worksheet of a .xlsx file.
    // ------------------------------------------------------------------
    static class XlsxReader
    {
        static readonly XNamespace Ns = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";

        public static (List<string> Headers, List<Dictionary<string, string>> Rows) Read(string path, string? sheet = null)
        {
            var headers = new List<string>();
            var rows = new List<Dictionary<string, string>>();
            if (!File.Exists(path))
                throw new FileNotFoundException($"找不到文件: {path}", path);

            using var za = ZipFile.OpenRead(path);

            // shared strings table (if present)
            var shared = new Dictionary<int, string>();
            var ss = za.GetEntry("xl/sharedStrings.xml");
            if (ss != null)
            {
                var sdoc = XDocument.Load(ss.Open());
                int i = 0;
                foreach (var si in sdoc.Root!.Elements(Ns + "si"))
                {
                    var txt = string.Concat(si.Descendants(Ns + "t").Select(t => t.Value));
                    shared[i++] = txt;
                }
            }

            // pick the worksheet to read
            string sheetEntry = PickSheet(za, sheet);
            var ws = za.GetEntry(sheetEntry) ?? throw new InvalidDataException($"工作表不存在: {sheetEntry}");
            var doc = XDocument.Load(ws.Open());

            var rowEls = doc.Root!.Descendants(Ns + "row").ToList();
            if (rowEls.Count == 0) return (headers, rows);

            // header row (first <row>)
            headers = rowEls[0].Elements(Ns + "c")
                .OrderBy(c => ColIndex(c.Attribute("r")?.Value ?? "A1"))
                .Select(c => CellText(c, shared))
                .Select(t => t.Trim())
                .ToList();

            for (int r = 1; r < rowEls.Count; r++)
            {
                var dict = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (var c in rowEls[r].Elements(Ns + "c"))
                {
                    int col = ColIndex(c.Attribute("r")?.Value);
                    if (col < headers.Count)
                        dict[headers[col]] = CellText(c, shared);
                }
                rows.Add(dict);
            }
            return (headers, rows);
        }

        static string PickSheet(ZipArchive za, string? sheet)
        {
            var wb = za.GetEntry("xl/workbook.xml");
            var sheetEntries = za.Entries
                .Where(e => e.FullName.StartsWith("xl/worksheets/sheet") && e.FullName.EndsWith(".xml"))
                .OrderBy(e => e.FullName)
                .ToList();
            if (sheetEntries.Count == 0) throw new InvalidDataException("xlsx 中没有工作表");
            if (string.IsNullOrEmpty(sheet)) return sheetEntries[0].FullName;

            if (wb != null)
            {
                var wdoc = XDocument.Load(wb.Open());
                var wns = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
                var rns = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
                var sheetEls = wdoc.Root!.Elements(wns + "sheets").Elements(wns + "sheet").ToList();

                // by 1-based index
                if (int.TryParse(sheet, out int idx) && idx >= 1 && idx <= sheetEls.Count)
                    return ResolveRid(za, sheetEls[idx - 1].Attribute(rns + "id")?.Value);

                // by name
                var byName = sheetEls.FirstOrDefault(s => string.Equals(s.Attribute("name")?.Value, sheet, StringComparison.OrdinalIgnoreCase));
                if (byName != null)
                    return ResolveRid(za, byName.Attribute(rns + "id")?.Value);
            }
            // fallback: treat as file name index
            if (int.TryParse(sheet, out int i2) && i2 >= 1 && i2 <= sheetEntries.Count)
                return sheetEntries[i2 - 1].FullName;
            return sheetEntries[0].FullName;
        }

        static string ResolveRid(ZipArchive za, string? rid)
        {
            var rels = za.GetEntry("xl/_rels/workbook.xml.rels");
            if (rels != null && rid != null)
            {
                var rdoc = XDocument.Load(rels.Open());
                var rel = rdoc.Root!.Elements()
                    .FirstOrDefault(e => e.Attribute("Id")?.Value == rid);
                var target = rel?.Attribute("Target")?.Value;
                if (target != null)
                {
                    var full = target.StartsWith("/") ? target.Substring(1) : "xl/" + target;
                    var entry = za.GetEntry(full.Replace("/", "\\"));
                    if (entry != null) return entry.FullName;
                }
            }
            return za.Entries.First(e => e.FullName.StartsWith("xl/worksheets/sheet")).FullName;
        }

        static int ColIndex(string? cellRef)
        {
            int idx = 0;
            if (cellRef == null) return 0;
            foreach (char c in cellRef)
            {
                if (char.IsLetter(c)) idx = idx * 26 + (char.ToUpperInvariant(c) - 'A' + 1);
                else break;
            }
            return idx - 1;
        }

        static string CellText(XElement c, Dictionary<int, string> shared)
        {
            var t = c.Attribute("t")?.Value;
            if (t == "s")
            {
                var v = c.Element(Ns + "v")?.Value;
                if (v != null && int.TryParse(v, out int i)) return shared.TryGetValue(i, out var s) ? s : "";
                return "";
            }
            if (t == "inlineStr")
            {
                var isEl = c.Element(Ns + "is");
                if (isEl != null) return string.Concat(isEl.Descendants(Ns + "t").Select(x => x.Value));
                return "";
            }
            return c.Element(Ns + "v")?.Value ?? "";
        }
    }

    // ------------------------------------------------------------------
    //  Self-contained multi-sheet XLSX writer (inline strings + numbers).
    // ------------------------------------------------------------------
    static class XlsxWriter
    {
        public class Sheet
        {
            public string Name = "Sheet1";
            public List<string> Headers = new();
            public List<List<string>> Rows = new();
        }

        public static void Write(string path, List<Sheet> sheets)
        {
            var ct = new StringBuilder();
            ct.Append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
            ct.Append("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">");
            ct.Append("<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>");
            ct.Append("<Default Extension=\"xml\" ContentType=\"application/xml\"/>");
            ct.Append("<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>");
            for (int i = 1; i <= sheets.Count; i++)
                ct.Append($"<Override PartName=\"/xl/worksheets/sheet{i}.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>");
            ct.Append("</Types>");

            var rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>";

            var wb = new StringBuilder();
            wb.Append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>");
            for (int i = 1; i <= sheets.Count; i++)
                wb.Append($"<sheet name=\"{Escape(sheets[i - 1].Name)}\" sheetId=\"{i}\" r:id=\"rId{i}\"/>");
            wb.Append("</sheets></workbook>");

            var wrels = new StringBuilder();
            wrels.Append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
            for (int i = 1; i <= sheets.Count; i++)
                wrels.Append($"<Relationship Id=\"rId{i}\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet{i}.xml\"/>");
            wrels.Append("</Relationships>");

            using var za = ZipFile.Open(path, ZipArchiveMode.Create);
            Add(za, "[Content_Types].xml", ct.ToString());
            Add(za, "_rels/.rels", rels);
            Add(za, "xl/workbook.xml", wb.ToString());
            Add(za, "xl/_rels/workbook.xml.rels", wrels.ToString());

            for (int i = 0; i < sheets.Count; i++)
            {
                var s = sheets[i];
                var sb = new StringBuilder();
                sb.Append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
                sb.Append(RowXml(0, s.Headers.Select(h => (object)h).ToList()));
                for (int r = 0; r < s.Rows.Count; r++)
                    sb.Append(RowXml(r + 1, s.Rows[r].Select(v => (object)v).ToList()));
                sb.Append("</sheetData></worksheet>");
                Add(za, $"xl/worksheets/sheet{i + 1}.xml", sb.ToString());
            }
        }

        static void Add(ZipArchive za, string name, string content)
        {
            var e = za.CreateEntry(name);
            using var w = new StreamWriter(e.Open(), new UTF8Encoding(false));
            w.Write(content);
        }

        static string ColLetter(int idx)
        {
            idx++;
            var s = "";
            while (idx > 0) { int m = (idx - 1) % 26; s = (char)('A' + m) + s; idx = (idx - 1) / 26; }
            return s;
        }

        static string RowXml(int row0, List<object> vals)
        {
            var sb = new StringBuilder();
            sb.Append($"<row r=\"{row0 + 1}\">");
            for (int c = 0; c < vals.Count; c++)
            {
                var v = vals[c];
                string refc = ColLetter(c) + (row0 + 1);
                if (v is double d)
                    sb.Append($"<c r=\"{refc}\"><v>{d.ToString("G17", CultureInfo.InvariantCulture)}</v></c>");
                else
                {
                    string s = v?.ToString() ?? "";
                    sb.Append($"<c r=\"{refc}\" t=\"inlineStr\"><is><t xml:space=\"preserve\">{Escape(s)}</t></is></c>");
                }
            }
            sb.Append("</row>");
            return sb.ToString();
        }

        static string Escape(string s)
        {
            if (s == null) return "";
            var sb = new StringBuilder(s.Length);
            foreach (char c in s)
            {
                if (c == '&') sb.Append("&amp;");
                else if (c == '<') sb.Append("&lt;");
                else if (c == '>') sb.Append("&gt;");
                else if (c == '"') sb.Append("&quot;");
                else if (c == '\'') sb.Append("&apos;");
                else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') sb.Append(' ');
                else sb.Append(c);
            }
            return sb.ToString();
        }
    }

    // ------------------------------------------------------------------
    //  Comparison
    // ------------------------------------------------------------------
    public class ChangeRec
    {
        public string PipeNo = "";
        public string Table = "";
        public string No = "";
        public string DN = "";
        public string Description = "";
        public string DescFull = "";
        public string QtyOld = "";
        public string QtyNew = "";
        public string ChangeType = "";
        public string Detail = "";
    }

    static class Comparator
    {
        const string SEP = "\u001f"; // unit separator, unlikely in data

        static string Get(Dictionary<string, string> r, string name)
        {
            foreach (var kv in r)
                if (string.Equals(kv.Key, name, StringComparison.OrdinalIgnoreCase)) return (kv.Value ?? "").Trim();
            string nn = name.Replace(" ", "");
            foreach (var kv in r)
                if (kv.Key.Replace(" ", "").Equals(nn, StringComparison.OrdinalIgnoreCase)) return (kv.Value ?? "").Trim();
            return "";
        }

        static string KeyOf(Dictionary<string, string> r) =>
            $"{Get(r, "TABLE")}{SEP}{Get(r, "DN(mm)")}{SEP}{Get(r, "项目/DESCRIPTION")}{SEP}{Get(r, "NO")}";

        // Build a collision-safe key->row map: when the same (TABLE,DN,DESC,NO)
        // appears multiple times inside one PIPE, append "#2", "#3", ... so each
        // occurrence is addressable. Matching on the indexed key aligns
        // occurrences in order between old and new drawings.
        static Dictionary<string, Dictionary<string, string>> IndexItems(List<Dictionary<string, string>> rows)
        {
            var dict = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
            var counts = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            foreach (var row in rows)
            {
                string k = KeyOf(row);
                counts.TryGetValue(k, out int n); n++; counts[k] = n;
                string uk = n == 1 ? k : k + "#" + n;
                dict[uk] = row;
            }
            return dict;
        }

        static bool TryNum(string s, out double d)
        {
            d = 0;
            if (string.IsNullOrWhiteSpace(s)) return false;
            var m = Regex.Match(s.Trim(), @"^-?\d+(\.\d+)?");
            return m.Success && double.TryParse(m.Value, NumberStyles.Any, CultureInfo.InvariantCulture, out d);
        }

        static bool QtyEqual(string a, string b)
        {
            a = (a ?? "").Trim(); b = (b ?? "").Trim();
            if (TryNum(a, out double x) && TryNum(b, out double y)) return x == y;
            return a == b;
        }

        static (double diff, string dir) QtyDiff(string a, string b)
        {
            if (TryNum(a, out double x) && TryNum(b, out double y))
            {
                var d = y - x;
                return (d, d > 0 ? "增加" : (d < 0 ? "减少" : ""));
            }
            return (0, "");
        }

        public static List<ChangeRec> Compare(List<Dictionary<string, string>> oldRows, List<Dictionary<string, string>> newRows)
        {
            var oldByPipe = oldRows.GroupBy(r => Get(r, "PIPE NO")).ToDictionary(g => g.Key, g => g.ToList());
            var newByPipe = newRows.GroupBy(r => Get(r, "PIPE NO")).ToDictionary(g => g.Key, g => g.ToList());
            var allPipes = new SortedSet<string>(oldByPipe.Keys.Concat(newByPipe.Keys), StringComparer.OrdinalIgnoreCase);
            var changes = new List<ChangeRec>();

            foreach (var pipe in allPipes)
            {
                bool inOld = oldByPipe.ContainsKey(pipe), inNew = newByPipe.ContainsKey(pipe);
                if (inOld && !inNew)
                {
                    foreach (var row in oldByPipe[pipe])
                        changes.Add(Mk(pipe, row, Get(row, "QTY"), "不存在", "整个PIPE被删除",
                            $"PIPE {pipe} 在图纸2中完全删除，共 {oldByPipe[pipe].Count} 个项目"));
                    continue;
                }
                if (!inOld && inNew)
                {
                    foreach (var row in newByPipe[pipe])
                        changes.Add(Mk(pipe, row, "不存在", Get(row, "QTY"), "整个PIPE新增",
                            $"PIPE {pipe} 在图纸2中新增，共 {newByPipe[pipe].Count} 个项目"));
                    continue;
                }

                var oItems = IndexItems(oldByPipe[pipe]);
                var nItems = IndexItems(newByPipe[pipe]);
                var allKeys = new SortedSet<string>(oItems.Keys.Concat(nItems.Keys));
                foreach (var k in allKeys)
                {
                    bool ko = oItems.ContainsKey(k), kn = nItems.ContainsKey(k);
                    if (ko && !kn)
                    {
                        var row = oItems[k];
                        changes.Add(Mk(pipe, row, Get(row, "QTY"), "不存在", "项目被删除",
                            $"项目 \"{Get(row, "项目/DESCRIPTION")}\" 从图纸1中删除"));
                        continue;
                    }
                    if (!ko && kn)
                    {
                        var row = nItems[k];
                        changes.Add(Mk(pipe, row, "不存在", Get(row, "QTY"), "项目新增",
                            $"项目 \"{Get(row, "项目/DESCRIPTION")}\" 在图纸2中新增"));
                        continue;
                    }
                    var ro = oItems[k];
                    var rn = nItems[k];
                    var q1 = Get(ro, "QTY");
                    var q2 = Get(rn, "QTY");
                    if (!QtyEqual(q1, q2))
                    {
                        var (d, dir) = QtyDiff(q1, q2);
                        changes.Add(Mk(pipe, ro, q1, q2, $"QTY变化({dir}{Math.Abs(d)})",
                            $"数量从 {q1} 变为 {q2}，{dir}{Math.Abs(d)}个"));
                    }
                }
            }
            return changes;
        }

        static ChangeRec Mk(string pipe, Dictionary<string, string> row, string qOld, string qNew, string type, string detail)
        {
            string t = Get(row, "TABLE"), no = Get(row, "NO"), dn = Get(row, "DN(mm)"), desc = Get(row, "项目/DESCRIPTION");
            return new ChangeRec
            {
                PipeNo = pipe,
                Table = t,
                No = no,
                DN = dn,
                Description = desc,
                DescFull = $"TABLE:{t} | DN:{dn}mm | {desc} | NO:{no}",
                QtyOld = qOld,
                QtyNew = qNew,
                ChangeType = type,
                Detail = detail
            };
        }
    }

    // ------------------------------------------------------------------
    //  CLI
    // ------------------------------------------------------------------
    public class ExcelTools
    {
        const string VERSION = "3.0.0";
        const string APP = "EGexcel2df";

        private static int MainCore(string[] args)
        {
            try
            {
                var opts = ParseArgs(args, out var positional, out bool showHelp, out bool showVersion);
                if (showVersion) { Console.WriteLine($"{APP} {VERSION}"); return 0; }
                if (showHelp || positional.Count < 2)
                {
                    PrintHelp();
                    return showHelp ? 0 : 1;
                }

                string oldFile = positional[0];
                string newFile = positional[1];
                string output = opts.TryGetValue("output", out var o) ? o! : DefaultOutput();

                Console.WriteLine($"[{APP}] 旧图纸: {oldFile}");
                Console.WriteLine($"[{APP}] 新图纸: {newFile}");
                Console.WriteLine($"[{APP}] 输出  : {output}");

                var oldData = XlsxReader.Read(oldFile, opts.TryGetValue("sheet1", out var s1) ? s1 : null);
                var newData = XlsxReader.Read(newFile, opts.TryGetValue("sheet2", out var s2) ? s2 : null);
                var oldRows = oldData.Rows;
                var newRows = newData.Rows;

                var changes = Comparator.Compare(oldRows, newRows);
                WriteReport(output, oldFile, newFile, changes);

                if (changes.Count == 0)
                {
                    Console.WriteLine($"\n[{APP}] ✓ 两个图纸完全一致，没有任何变化！");
                }
                else
                {
                    var byType = changes.GroupBy(c => c.ChangeType)
                        .Select(g => $"{g.Key}: {g.Count()}").ToList();
                    Console.WriteLine($"\n[{APP}] 变化项目总数: {changes.Count}");
                    Console.WriteLine($"[{APP}] 涉及 PIPE 数量: {changes.Select(c => c.PipeNo).Distinct().Count()}");
                    Console.WriteLine($"[{APP}] 变化类型: {string.Join(", ", byType)}");
                }
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[{APP}] 错误: {ex.Message}");
                return 2;
            }
        }

        static string DefaultOutput()
        {
            var ts = DateTime.Now.ToString("yyyyMMdd_HHmmss");
            return $"图纸变化清单_{ts}.xlsx";
        }

        static Dictionary<string, string?> ParseArgs(string[] args, out List<string> positional, out bool help, out bool version)
        {
            var opts = new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase);
            positional = new List<string>();
            help = false; version = false;
            for (int i = 0; i < args.Length; i++)
            {
                var a = args[i];
                if (a == "-h" || a == "--help") { help = true; }
                else if (a == "-v" || a == "--version") { version = true; }
                else if (a == "-o" || a == "--output") { opts["output"] = args[++i]; }
                else if (a == "--sheet1") { opts["sheet1"] = args[++i]; }
                else if (a == "--sheet2") { opts["sheet2"] = args[++i]; }
                else if (a.StartsWith("--output=")) opts["output"] = a.Substring("--output=".Length);
                else if (!a.StartsWith("-")) positional.Add(a);
                else { Console.Error.WriteLine($"[{APP}] 未知选项: {a}"); }
            }
            return opts;
        }

        static void PrintHelp()
        {
            string NL = Environment.NewLine;
            var sb = new StringBuilder();
            sb.AppendLine($"{APP} {VERSION} —— 管道图纸(Excel)变化对比工具");
            sb.AppendLine($"{APP} {VERSION} —— Piping drawing (Excel) change-comparison tool");
            sb.AppendLine("Copyright © 2026 HerryABU");
            sb.AppendLine();
            sb.AppendLine("【基本用法 / Usage】");
            sb.AppendLine("  EGexcel2df <旧图纸.xlsx> <新图纸.xlsx> [选项]");
            sb.AppendLine("  EGexcel2df <old.xlsx> <new.xlsx> [options]");
            sb.AppendLine();
            sb.AppendLine("【选项 / Options】");
            sb.AppendLine("  -h, --help                 显示本帮助 / Show this help");
            sb.AppendLine("  -v, --version              显示版本号 (3.0.0) / Show version (3.0.0)");
            sb.AppendLine("  -o, --output <文件>        指定输出 Excel 路径 (默认: 图纸变化清单_<时间戳>.xlsx)");
            sb.AppendLine("                             Specify output Excel path (default: 图纸变化清单_<时间戳>.xlsx)");
            sb.AppendLine("      --sheet1 <名称|序号>   旧图纸读取的工作表 (默认第 1 个)");
            sb.AppendLine("                             Worksheet to read from old drawing (default: 1st)");
            sb.AppendLine("      --sheet2 <名称|序号>   新图纸读取的工作表 (默认第 1 个)");
            sb.AppendLine("                             Worksheet to read from new drawing (default: 1st)");
            sb.AppendLine();
            sb.AppendLine("【对比逻辑 / Comparison logic】");
            sb.AppendLine("  以 PIPE NO 为主键分组；在每个 PIPE NO 下，用");
            sb.AppendLine("  Group by PIPE NO; within each PIPE NO, use");
            sb.AppendLine("  TABLE + DN(mm) + 项目/DESCRIPTION + NO 作为项目唯一键。");
            sb.AppendLine("  TABLE + DN(mm) + ITEM/DESCRIPTION + NO as the item key.");
            sb.AppendLine("  - 某 PIPE NO 仅存在于一侧  -> 整个 PIPE 新增/删除");
            sb.AppendLine("    A PIPE NO on only one side -> whole PIPE added/removed");
            sb.AppendLine("  - 项目键仅存在于一侧        -> 项目新增/删除");
            sb.AppendLine("    An item key on only one side -> item added/removed");
            sb.AppendLine("  - 项目键两侧都有但 QTY 不同 -> QTY变化(增加/减少N)");
            sb.AppendLine("    Item key on both sides but different QTY -> QTY change (+N/-N)");
            sb.AppendLine("  - QTY 完全相同              -> 无变化(不输出)");
            sb.AppendLine("    Identical QTY -> no change (not output)");
            sb.AppendLine();
            sb.AppendLine("【输出 Excel 工作表 / Output Excel sheets】");
            sb.AppendLine("  有变化的项目  —— 仅列出发生变化的行(主表)");
            sb.AppendLine("  Changed items —— only rows that changed (main sheet)");
            sb.AppendLine("  PIPE_<编号>   —— 每个发生变化的 PIPE 单独一页");
            sb.AppendLine("  PIPE_<id>    —— one page per changed PIPE");
            sb.AppendLine("  变化类型统计 / PIPE变化统计 / 对比信息");
            sb.AppendLine("  Change-type stats / PIPE-change stats / comparison info");
            sb.AppendLine();
            sb.AppendLine("【示例 / Examples】");
            sb.AppendLine("  EGexcel2df 旧图纸.xlsx 新图纸.xlsx -o 变化清单.xlsx");
            sb.AppendLine("  EGexcel2df old.xlsx new.xlsx -o changes.xlsx");
            sb.AppendLine("  EGexcel2df 旧版.pdf提取.xlsx 新版.pdf提取.xlsx");
            sb.AppendLine("  EGexcel2df old_pdf.xlsx new_pdf.xlsx");
            Console.WriteLine(sb.ToString());
        }

        static void WriteReport(string output, string oldFile, string newFile, List<ChangeRec> changes)
        {
            var f1 = Path.GetFileName(oldFile);
            var f2 = Path.GetFileName(newFile);
            var qOld = $"QTY({f1})";
            var qNew = $"QTY({f2})";

            var main = new XlsxWriter.Sheet
            {
                Name = "有变化的项目",
                Headers = new() { "PIPE NO", "TABLE", "NO", "DN(mm)", "项目/DESCRIPTION", "项目描述", qOld, qNew, "变化类型", "变化详情" }
            };
            foreach (var c in changes)
                main.Rows.Add(new() { c.PipeNo, c.Table, c.No, c.DN, c.Description, c.DescFull, c.QtyOld, c.QtyNew, c.ChangeType, c.Detail });

            var sheets = new List<XlsxWriter.Sheet> { main };

            // 变化类型统计
            var typeSheet = new XlsxWriter.Sheet { Name = "变化类型统计", Headers = new() { "变化类型", "项目数量" } };
            foreach (var g in changes.GroupBy(c => c.ChangeType).OrderBy(g => g.Key))
                typeSheet.Rows.Add(new() { g.Key, g.Count().ToString() });
            sheets.Add(typeSheet);

            // PIPE 变化统计
            var pipeSheet = new XlsxWriter.Sheet { Name = "PIPE变化统计", Headers = new() { "PIPE NO", "变化项目数" } };
            foreach (var g in changes.GroupBy(c => c.PipeNo).OrderByDescending(g => g.Count()))
                pipeSheet.Rows.Add(new() { g.Key, g.Count().ToString() });
            sheets.Add(pipeSheet);

            // 对比信息
            var info = new XlsxWriter.Sheet
            {
                Name = "对比信息",
                Headers = new() { "信息", "内容" },
                Rows = new()
                {
                    new() { "对比时间", DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") },
                    new() { "旧图纸", f1 },
                    new() { "新图纸", f2 },
                    new() { "变化项目总数", changes.Count.ToString() },
                    new() { "涉及PIPE数量", changes.Select(c => c.PipeNo).Distinct().Count().ToString() }
                }
            };
            sheets.Add(info);

            // 每个变化的 PIPE 单独一页
            var used = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var pipe in changes.Select(c => c.PipeNo).Distinct())
            {
                string baseName = "PIPE_" + SanitizeSheetName(pipe);
                string name = TruncateSheet(baseName, used);
                used.Add(name);
                var ps = new XlsxWriter.Sheet
                {
                    Name = name,
                    Headers = new() { "项目描述", qOld, qNew, "变化类型", "变化详情" }
                };
                foreach (var c in changes.Where(c => c.PipeNo == pipe))
                    ps.Rows.Add(new() { c.DescFull, c.QtyOld, c.QtyNew, c.ChangeType, c.Detail });
                sheets.Add(ps);
            }

            if (changes.Count == 0)
            {
                sheets.Add(new XlsxWriter.Sheet
                {
                    Name = "无变化",
                    Headers = new() { "说明" },
                    Rows = new() { new() { "两个图纸完全一致，无任何变化" } }
                });
            }

            XlsxWriter.Write(output, sheets);
            Console.WriteLine($"[{APP}] 已生成对比报告: {output}");
        }

        static string SanitizeSheetName(string s)
        {
            var sb = new StringBuilder();
            foreach (char c in s ?? "")
            {
                if (":\\/?*[]".IndexOf(c) >= 0) sb.Append('_');
                else sb.Append(c);
            }
            return sb.ToString();
        }

        static string TruncateSheet(string name, HashSet<string> used)
        {
            string n = name.Length > 31 ? name.Substring(0, 31) : name;
            if (!used.Contains(n)) return n;
            int i = 1;
            while (used.Contains(n.Length > 28 ? n.Substring(0, 28) + i : n + i)) i++;
            return n.Length > 28 ? n.Substring(0, 28) + i : n + i;
        }
    
    public static int Run(string[] args, Action<string>? log = null)
    {
        TextWriter? prevErr = Console.Error, prevOut = Console.Out;
        ForwardingWriter? fw = null;
        if (log != null) { fw = new ForwardingWriter(log); Console.SetError(fw); Console.SetOut(fw); }
        try { return MainCore(args); }
        finally { fw?.Flush(); Console.SetError(prevErr); Console.SetOut(prevOut); }
    }

    /// <summary>One-shot compare used by the GUI. Returns change records.</summary>
    public static List<ChangeRec> CompareFiles(string oldFile, string newFile,
        string? sheet1 = null, string? sheet2 = null, Action<string>? log = null)
    {
        if (log != null) log($"[EGexcel2df] 旧图纸: {oldFile}");
        if (log != null) log($"[EGexcel2df] 新图纸: {newFile}");
        var oldData = XlsxReader.Read(oldFile, sheet1);
        var newData = XlsxReader.Read(newFile, sheet2);
        return Comparator.Compare(oldData.Rows, newData.Rows);
    }

    public static void WriteChanges(string output, string oldFile, string newFile, List<ChangeRec> changes)
    {
        WriteReport(output, oldFile, newFile, changes);
    }
}
}
