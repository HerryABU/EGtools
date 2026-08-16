using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using MuPDFCore;

// Cross-language materials-table extractor in C# (.NET / MS toolchain).
// Uses MuPDFCore (the .NET binding of MuPDF -- the SAME engine as the
// Python/Node reference) for positional text extraction, so CJK glyphs are
// decoded correctly (unlike PdfPig, whose CMap handling garbled Chinese).
// MuPDF reports top-down y already, matching PyMuPDF/C++ constants.
//
// CLI:
//   MaterialsExtractor [inputs...] [-o DIR|FILE] [-f csv|xlsx|both] [-r REF.xlsx]
//                          [-C merged|separate] [-G embed|omit] [--tag V3] [-v] [-h]
//   inputs : PDF file(s), directory, or wildcard glob (e.g. "*.pdf", "C:/x/*.pdf").
//            If omitted, defaults to "*.pdf" in the current directory.
//   -o/--output : output directory, or a single output file when one input is given.
//   -f/--format : csv (default) | xlsx | both.
//   -r/--ref    : reference .xlsx workbook; its DESCRIPTION vocabulary is used to
//                 reconstruct word spacing in the extracted descriptions so the
//                 output mirrors the reference's spaced style.
//   -C/--component : merged (default) | separate. Layout of the component type:
//                 'merged' keeps COMPONENT inside the 项目/DESCRIPTION cell (8 cols);
//                 'separate' emits it as its own column between DN(mm) and
//                 项目/DESCRIPTION (9 cols), matching the reference workbook.
//   -G/--group-header : embed (default) | omit. The PDF's in-table group banners
//                 (FITTINGS, VALVES, FLANGES, ...) are detected and, when 'embed',
//                 prefixed onto every member row's DESCRIPTION cell.
//   --tag       : filename tag (default "V3") -> "<pdfbase>_<tag>.csv/.xlsx".
//   -v/--verbose: detailed progress (per-page, warnings) to stderr.
//   -h/--help   : show usage.
//   Output columns (merged)  : pipe no | TABLE | NO | DN(mm) | 项目/DESCRIPTION
//                             (COMPONENT merged in) | QTY | WEIGHT(kg) | PAGE.
//   Output columns (separate): pipe no | TABLE | NO | DN(mm) | COMPONENT |
//                             项目/DESCRIPTION | QTY | WEIGHT(kg) | PAGE.

namespace EGtools.Core
{
    public class PdfExtractor
{
    // ---- constants (identical to extract_fitz.py / mupdf_extract.cpp) ----
    const double B_NO_MAX = 1135, B_DN_MAX = 1172, B_DESC_MAX = 1537, B_QTY_MAX = 1567;
    static readonly Regex PIPE_RE = new Regex(@"408\.101\.P\d+", RegexOptions.IgnoreCase);
    static readonly HashSet<string> DN_HEADER_TOKENS = new HashSet<string> { "mm", "(mm)", "dn", "no", "n", "o" };
    static readonly HashSet<string> LABELS = new HashSet<string> { "no","dn","qty","weight","total","fabrication","erection","materials","component","description","summary" };
    static readonly string[] SUMMARY_SUBSTR = { "total","fabrication","erection","materials","weight","summary" };
    static readonly HashSet<string> COMP_KEYWORDS = new HashSet<string> { "PIPE","PIPES","TEE","ELBOW","ELBOWS","FLANGE","FLANGES","REDUCER","REDUCERS","OLET","OLETS","GASKET","GASKETS","BOLT","BOLTS","NUT","NUTS","VALVE","VALVES","SPECTACLE","BLIND","STUD","STUDS","WASHER","WASHERS","SUPPORT","SUPPORTS","WELD","NECK","NIPPLE","CAP","COUPLING","BUSH","UNION","CLAMP","BRANCH","SLEEVE","ADAPTER" };
    static readonly HashSet<string> SECTION_KEYWORDS = new HashSet<string> { "fittings","flanges","valves","pipes","supports","bolts","nuts","washers","gaskets","olets","components","studs","screws","connectors","spools","materials","fabrication","erection" };
    // Group sub-headers inside a table (FITTINGS, VALVES, FLANGES, GASKET, ...).
    // Detected as a line in the component/description x-zone whose English-letter
    // tokens include one of these AND which has NO NO-number anchor on its baseline.
    static readonly HashSet<string> GROUP_KEYWORDS = new HashSet<string>(COMP_KEYWORDS) { "FITTINGS" };
    // A "type banner" (PIPE / PIPES) is a section header whose keyword is itself a
    // concrete component type that also appears as the row's COMPONENT; the
    // reference workbook keeps it in the description. All other section headers
    // (FITTINGS, FLANGES, GASKETS, SUPPORTS, ...) are CATEGORY headers removed
    // from the description.
    static readonly HashSet<string> TYPE_BANNER = new HashSet<string> { "PIPE", "PIPES" };

    // Output column headers. When ComponentSeparate is set, a COMPONENT column is
    // inserted between DN(mm) and 项目/DESCRIPTION (9 cols); otherwise COMPONENT is
    // merged into the DESCRIPTION cell (8 cols, the default).
    static string[] HEADERS = { "pipe no", "TABLE", "NO", "DN(mm)", "项目/DESCRIPTION", "QTY", "WEIGHT(kg)", "PAGE" };
    // Component-column layout: "merged" (default) keeps COMPONENT inside the
    // DESCRIPTION cell; "separate" emits it as its own column (the 3rd layout mode).
    static bool ComponentSeparate = false;
    static bool Verbose = false;
    // Group sub-header behaviour: "embed" prefixes the PDF's group banner (e.g.
    // FITTINGS) onto each row's merged cell; "omit" leaves it out. CLI: -G/--group-header.
    static string GroupHeaderMode = "embed";
    static void LogV(string s) { if (Verbose) Console.Error.WriteLine(s); }

    static string Norm(string t) => Regex.Replace(t ?? "", @"[^\w一-鿿]", "").ToLowerInvariant();
    static string NormWs(string t) => Regex.Replace(t ?? "", @"\s+", " ").Trim();
    static bool HasCjk(string s) { foreach (var c in s ?? "") if (c >= 0x4E00 && c <= 0x9FFF) return true; return false; }
    static bool LooksNumeric(string text)
    {
        var t = Regex.Replace((text ?? "").Trim(), @"\s*(mm|kg|kgs)\s*$", "", RegexOptions.IgnoreCase);
        return Regex.IsMatch(t, @"^[\d.,\s/xX]+$") && t.Any(char.IsDigit);
    }
    static bool IsNumericNo(string text) => Regex.IsMatch((text ?? "").Trim(), @"^\d+(\.\d+)?$");
    static string ExtractComponent(string desc)
    {
        if (string.IsNullOrEmpty(desc)) return "";
        var outp = new List<string>();
        foreach (var tok in desc.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries))
        {
            var bare = new string(tok.Where(c => char.IsLetter(c)).ToArray()).ToUpperInvariant();
            if (COMP_KEYWORDS.Contains(bare)) outp.Add(tok); else break;
        }
        return string.Join(" ", outp);
    }

    // Compressed (space-less) multi-word component names that appear run together
    // in the PDF (e.g. "PIPESUPPORT", "WELDOLET"). Mapped to the canonical spaced
    // form the reference workbook uses. Checked as a PREFIX of the leading text
    // before the single-keyword greedy match.
    static readonly Dictionary<string, string> COMP_CANON = new Dictionary<string, string>
    {
        { "WELDOLET", "WELD OLET" },
        { "WELDNECKFLANGE", "WELD NECK FLANGE" },
        { "BLINDFLANGE", "BLIND FLANGE" },
        { "PIPESUPPORT", "PIPE SUPPORT" },
        { "SPECTACLEFLANGE", "SPECTACLE FLANGE" },
    };
    static string RecoverComponent(string raw)
    {
        if (string.IsNullOrEmpty(raw)) return "";
        var outp = new List<string>();
        foreach (var seg in Regex.Split(raw, @"[^A-Za-z]+"))
        {
            if (seg.Length == 0) continue;
            int pos = 0; string up = seg.ToUpperInvariant();
            bool yielded = false;
            while (pos < up.Length)
            {
                string canon = "";
                foreach (var kv in COMP_CANON)
                    if (up.Length - pos >= kv.Key.Length && up.Substring(pos, kv.Key.Length) == kv.Key && kv.Key.Length > canon.Length)
                        canon = kv.Key;
                if (canon.Length > 0) { outp.Add(COMP_CANON[canon]); pos += canon.Length; yielded = true; continue; }
                string hit = "";
                foreach (var kw in COMP_KEYWORDS)
                    if (up.Length - pos >= kw.Length && up.Substring(pos, kw.Length) == kw && kw.Length > hit.Length)
                        hit = kw;
                if (hit.Length == 0) break;
                // "WELD" is only a component when followed by NECK/OLET (e.g.
                // "WELD NECK FLANGE", "WELD OLET"); a bare "WELDED ..." (e.g.
                // "WELDED FEMALE SOCKET") is not a component -> leave COMPONENT empty.
                if (hit == "WELD")
                {
                    string rest = up.Substring(pos + hit.Length);
                    if (!(rest.StartsWith("NECK") || rest.StartsWith("OLET") || rest.Length == 0)) break;
                }
                outp.Add(seg.Substring(pos, hit.Length));
                pos += hit.Length; yielded = true;
            }
            if (!yielded) break;
        }
        return string.Join(" ", outp);
    }

    // How many raw characters at the START of `raw` form the leading COMPONENT
    // keyword(s). Used only to split the remainder into DESCRIPTION in 'separate'
    // mode; it mirrors RecoverComponent's matching but stops at the first letter
    // run (the component is always at the very start of the description).
    static int LeadingComponentConsumed(string raw)
    {
        if (string.IsNullOrEmpty(raw)) return 0;
        int i = 0; while (i < raw.Length && char.IsLetter(raw[i])) i++;
        string seg = raw.Substring(0, i);
        string up = seg.ToUpperInvariant();
        int pos = 0;
        while (pos < up.Length)
        {
            string canon = "";
            foreach (var kv in COMP_CANON)
                if (up.Length - pos >= kv.Key.Length && up.Substring(pos, kv.Key.Length) == kv.Key && kv.Key.Length > canon.Length)
                    canon = kv.Key;
            if (canon.Length > 0) { pos += canon.Length; continue; }
            string hit = "";
            foreach (var kw in COMP_KEYWORDS)
                if (up.Length - pos >= kw.Length && up.Substring(pos, kw.Length) == kw && kw.Length > hit.Length)
                    hit = kw;
            if (hit.Length == 0) break;
            if (hit == "WELD")
            {
                string rest = up.Substring(pos + hit.Length);
                if (!(rest.StartsWith("NECK") || rest.StartsWith("OLET") || rest.Length == 0)) break;
            }
            pos += hit.Length;
        }
        return pos;
    }

    // ---- reference-driven spacing reconstruction ----
    // A dictionary of tokens harvested from the reference workbook's DESCRIPTION
    // column. ReconstructSpaces() greedily segments a space-less extracted string
    // using the LONGEST known token (or digit-normalized pattern) at each position,
    // inserting a single space between recovered tokens. This makes the CSV/XLSX
    // mirror the reference's human-readable, word-spaced style.
    class RefDict
    {
        readonly HashSet<string> exact = new HashSet<string>();
        readonly HashSet<string> pat = new HashSet<string>();
        readonly Dictionary<char, List<string>> exactByFirst = new Dictionary<char, List<string>>();
        readonly Dictionary<char, List<string>> patByFirst = new Dictionary<char, List<string>>();
        public void Add(string token)
        {
            var t = (token ?? "").Trim();
            if (t.Length == 0) return;
            var ut = t.ToUpperInvariant();
            if (exact.Add(ut)) AddFirst(exactByFirst, ut[0], ut);
            var p = Regex.Replace(ut, @"[0-9]+", "#");
            if (p != ut && pat.Add(p)) AddFirst(patByFirst, p[0], p);
        }
        static void AddFirst(Dictionary<char, List<string>> d, char k, string v)
        {
            if (!d.TryGetValue(k, out var l)) { l = new List<string>(); d[k] = l; }
            if (!l.Contains(v)) l.Add(v);
        }
        public int LongestMatch(string up, int pos)
        {
            char key = up[pos];
            int best = 0;
            if (exactByFirst.TryGetValue(key, out var el))
                foreach (var t in el) if (StartsWith(up, pos, t) && t.Length > best) best = t.Length;
            if (patByFirst.TryGetValue(key, out var pl))
                foreach (var p in pl) if (MatchPattern(up, pos, p) && p.Length > best) best = p.Length;
            if (char.IsDigit(key) && patByFirst.TryGetValue('#', out var pl2))
                foreach (var p in pl2) if (MatchPattern(up, pos, p) && p.Length > best) best = p.Length;
            return best;
        }
        static bool StartsWith(string up, int pos, string t)
            => pos + t.Length <= up.Length && up.Substring(pos, t.Length) == t;
    static bool MatchPattern(string up, int pos, string p)
    {
        int i = 0, j = pos;
        while (i < p.Length && j < up.Length)
        {
            if (p[i] == '#')
            {
                // a '#' stands for a whole run of digits (e.g. "2633"), not a single
                // digit -- otherwise numbers get split into "2 6 3 3".
                if (!char.IsDigit(up[j])) return false;
                int run = 0;
                while (j + run < up.Length && char.IsDigit(up[j + run])) run++;
                j += run; i++;
            }
            else if (p[i] != up[j]) return false;
            else { j++; i++; }
        }
        return i == p.Length;
    }
    }
    static string ReconstructSpaces(string s, RefDict dict)
    {
        if (dict == null || s == null) return s ?? "";
        string up = s.ToUpperInvariant();
        int len = s.Length, pos = 0;
        var outb = new StringBuilder();
        while (pos < len)
        {
            int L = dict.LongestMatch(up, pos);
            if (L > 0) { if (outb.Length > 0) outb.Append(' '); outb.Append(s.Substring(pos, L)); pos += L; }
            else
            {
                int p = pos + 1;
                while (p < len && dict.LongestMatch(up, p) == 0) p++;
                if (outb.Length > 0) outb.Append(' ');
                outb.Append(s.Substring(pos, p - pos));
                pos = p;
            }
        }
        return outb.ToString();
    }
    static RefDict BuildRefDict(List<string> descriptions)
    {
        var d = new RefDict();
        foreach (var s in descriptions)
            foreach (var wt in s.Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
                foreach (var tok in wt.Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries))
                    d.Add(tok);
        return d;
    }
    static List<string> ReadRefDescriptions(string path)
    {
        var descs = new List<string>();
        try
        {
            using var zip = new ZipArchive(File.OpenRead(path), ZipArchiveMode.Read);
            XDocument wb = null, sheet = null, ss = null;
            foreach (var e in zip.Entries)
            {
                string n = e.FullName.Replace('\\', '/');
                if (n == "xl/workbook.xml") wb = XDocument.Load(e.Open());
                else if (n.EndsWith("/sheet1.xml")) sheet = XDocument.Load(e.Open());
                else if (n == "xl/sharedStrings.xml") ss = XDocument.Load(e.Open());
            }
            if (sheet == null) return descs;
            XNamespace ns = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
            List<string> shared = null;
            if (ss != null)
            {
                shared = new List<string>();
                foreach (var si in ss.Root.Elements(ns + "si"))
                {
                    var t = si.Descendants(ns + "t").FirstOrDefault();
                    shared.Add(t == null ? "" : t.Value);
                }
            }
            var rows = sheet.Root.Element(ns + "sheetData").Elements(ns + "row").ToList();
            int descCol = -1;
            var headerRow = rows.FirstOrDefault();
            if (headerRow != null)
                foreach (var c in headerRow.Elements(ns + "c"))
                {
                    string txt = CellText(c, ns, shared);
                    if (txt != null && txt.ToUpperInvariant().Contains("DESCRIPTION")) { descCol = ColIndex(ColLetters(c.Attribute("r")?.Value)); break; }
                }
            if (descCol < 0) return descs;
            foreach (var row in rows.Skip(1))
                foreach (var c in row.Elements(ns + "c"))
                    if (ColIndex(ColLetters(c.Attribute("r")?.Value)) == descCol)
                    {
                        string txt = CellText(c, ns, shared);
                        if (!string.IsNullOrEmpty(txt)) descs.Add(txt);
                    }
        }
        catch (Exception ex) { Console.Error.WriteLine($"  [warn] failed to read reference '{path}': {ex.Message}"); }
        return descs;
    }
    static string CellText(XElement c, XNamespace ns, List<string> shared)
    {
        var t = c.Attribute("t")?.Value;
        if (t == "s") { var v = c.Element(ns + "v")?.Value; if (v != null && shared != null && int.TryParse(v, out var i) && i < shared.Count) return shared[i]; return ""; }
        if (t == "inlineStr") { var tx = c.Descendants(ns + "t").FirstOrDefault(); return tx == null ? "" : tx.Value; }
        return c.Element(ns + "v")?.Value ?? "";
    }
    static string ColLetters(string r) { if (string.IsNullOrEmpty(r)) return ""; int i = 0; while (i < r.Length && char.IsLetter(r[i])) i++; return r.Substring(0, i); }
    static int ColIndex(string letters) { int v = 0; foreach (char c in letters) v = v * 26 + (c - 'A' + 1); return v - 1; }

    // ---- glyph / word ----
    class Word
    {
        public double x0, y0, x1, y1, xc, yc;
        public string text = "";
        public override string ToString() => $"[{text}] x0={x0:F1} xc={xc:F1} yc={yc:F1}";
    }

    static List<Word> GetWords(MuPDFStructuredTextPage st, double H)
    {
        var gs = new List<(double x0, double y0, double x1, double y1, double yc, string text)>();
        foreach (var block in st)
        {
            if (block.Type != 0) continue;
            foreach (var line in block)
            {
                var chars = line.Characters.ToList();
                for (int i = 0; i < chars.Count; i++)
                {
                    var c = chars[i];
                    string t = c.Character.ToString();
                    if (t == " " || t == "\u00a0") continue;
                    double x0 = c.Origin.X;
                    double baseline = c.Origin.Y;
                    double sz = Convert.ToDouble(c.Size);
                    double w = (i + 1 < chars.Count) ? (chars[i + 1].Origin.X - x0) : sz * 0.5;
                    if (w < 0.3) w = sz * 0.5;
                    double x1 = x0 + w;
                    double yc = baseline - sz / 2.0;
                    double y0 = baseline - sz;
                    double y1 = baseline;
                    gs.Add((x0, y0, x1, y1, yc, t));
                }
            }
        }
        if (gs.Count == 0) return new List<Word>();
        gs.Sort((a, b) =>
        {
            if (Math.Abs(a.yc - b.yc) > 4.0) return a.yc.CompareTo(b.yc);
            return a.x0.CompareTo(b.x0);
        });
        var rows = new List<List<(double x0,double y0,double x1,double y1,double yc,string text)>>();
        var cur = new List<(double,double,double,double,double,string)>();
        double rowY = -1;
        foreach (var g in gs)
        {
            if (rowY < 0 || Math.Abs(g.yc - rowY) > 4.0) { if (cur.Count > 0) rows.Add(cur); cur = new List<(double,double,double,double,double,string)>(); rowY = g.yc; }
            cur.Add(g);
        }
        if (cur.Count > 0) rows.Add(cur);
        var words = new List<Word>();
        foreach (var row in rows)
        {
            row.Sort((a, b) => a.x0.CompareTo(b.x0));
            Word w = null; bool has = false;
            void Push() { if (has) { w.xc = (w.x0 + w.x1) / 2; w.yc = (w.y0 + w.y1) / 2; words.Add(w); } }
            foreach (var g in row)
            {
                bool breakHere = false;
                if (has) { double dx = g.x0 - w.x1; breakHere = dx > 8.0; }
                if (breakHere) Push();
                if (!has || breakHere)
                {
                    w = new Word { x0 = g.x0, y0 = g.y0, x1 = g.x1, y1 = g.y1, text = g.text };
                    has = true;
                }
                else
                {
                    w.x1 = Math.Max(w.x1, g.x1); w.y0 = Math.Min(w.y0, g.y0); w.y1 = Math.Max(w.y1, g.y1);
                    w.text += g.text;
                }
            }
            Push();
        }
        foreach (var wd in words) { wd.xc = (wd.x0 + wd.x1) / 2; wd.yc = (wd.y0 + wd.y1) / 2; }
        return words;
    }

    static double PageH(List<Word> words) => words.Count > 0 ? words.Max(w => w.y1) : 1191;

    static string ExtractPipeNo(List<Word> words, double W, double H)
    {
        var tb = words.Where(w => w.x0 > 0.70 * W && w.y0 > 0.70 * H && PIPE_RE.IsMatch(w.text)).ToList();
        if (tb.Count > 0) { tb.Sort((a, b) => (b.x0 + b.y0).CompareTo(a.x0 + a.y0)); return tb[0].text.ToUpperInvariant(); }
        var anyp = words.Where(w => PIPE_RE.IsMatch(w.text)).ToList();
        if (anyp.Count > 0) { anyp.Sort((a, b) => (b.x0 + b.y0).CompareTo(a.x0 + a.y0)); return anyp[0].text.ToUpperInvariant(); }
        return null;
    }

    static List<double> FindHeaderRows(List<Word> words)
    {
        var cand = words.Where(w => w.x0 > 1050 && w.x0 < 1660 && w.yc < 0.92 * PageH(words)).ToList();
        cand.Sort((a, b) => a.yc.CompareTo(b.yc));
        var clusters = new List<List<Word>>(); var cur = new List<Word>(); double? cy = null;
        foreach (var w in cand)
        {
            if (cy == null || Math.Abs(w.yc - cy.Value) <= 4) { cur.Add(w); cy = cy == null ? w.yc : (cy + w.yc) / 2; }
            else { clusters.Add(cur); cur = new List<Word> { w }; cy = w.yc; }
        }
        if (cur.Count > 0) clusters.Add(cur);
        var headers = new List<double>();
        foreach (var cl in clusters)
        {
            var joined = Norm(string.Join(" ", cl.OrderBy(w => w.x0).Select(w => w.text)));
            bool hasNo = joined.Contains("no"), hasDn = joined.Contains("dn");
            bool hasQty = joined.Contains("qty"), hasWt = joined.Contains("weight");
            if ((hasNo || hasDn) && (hasQty || hasWt))
                headers.Add(cl.Average(w => w.yc));
        }
        return headers.Distinct().OrderBy(h => h).Select(h => Math.Round(h, 1)).ToList();
    }

    static List<double> FindLabelBands(List<Word> words)
    {
        var cand = words.Where(w => w.x0 > 1050 && w.x0 < 1660 && w.yc < 0.95 * PageH(words)).ToList();
        cand.Sort((a, b) => a.yc.CompareTo(b.yc));
        var clusters = new List<List<Word>>(); List<Word> cur = null; double? cy = null;
        foreach (var w in cand)
        {
            if (cy == null || Math.Abs(w.yc - cy.Value) <= 5) { if (cur == null) cur = new List<Word>(); cur.Add(w); cy = cy == null ? w.yc : (cy + w.yc) / 2; }
            else { clusters.Add(cur); cur = new List<Word> { w }; cy = w.yc; }
        }
        if (cur != null) clusters.Add(cur);
        var bands = new List<double>();
        foreach (var cl in clusters)
        {
            var toks = new HashSet<string>();
            string clusterText = string.Join("", cl.OrderBy(w => w.x0).Select(w => w.text.ToLowerInvariant()));
            foreach (var w in cl)
            {
                var t = w.text.ToLowerInvariant();
                foreach (Match m in Regex.Matches(t, @"[a-z0-9]+")) toks.Add(m.Value);
            }
            foreach (var lab in SUMMARY_SUBSTR) if (clusterText.Contains(lab)) toks.Add(lab);
            if (toks.Any(t => LABELS.Contains(t)))
                bands.Add(cl.Average(w => w.yc));
        }
        return bands;
    }

    static Dictionary<string, double> ColsFromWords(List<Word> cl)
    {
        var cols = new Dictionary<string, double>();
        var map = new Dictionary<string, string> { {"NO","NO"},{"DN","DN"},{"COMPONENT","COMPONENT"},{"DESCRIPTION","DESCRIPTION"},{"QTY","QTY"},{"WEIGHT(KG)","WEIGHT"} };
        foreach (var w in cl)
        {
            if (map.TryGetValue(w.text.ToUpperInvariant(), out var name))
            {
                if (!cols.ContainsKey(name) || w.xc < cols[name]) cols[name] = w.xc;
            }
        }
        return cols;
    }
    static Dictionary<string, double> ColsFromLineStr(List<Word> cl)
    {
        var order = cl.OrderBy(w => w.x0).ToList();
        string s = string.Join("", order.Select(w => w.text));
        var charWord = new List<int>();
        foreach (var w in order) for (int i = 0; i < w.text.Length; i++) charWord.Add(order.IndexOf(w));
        var map = new Dictionary<string, string> { {"NO","NO"},{"DN","DN"},{"COMPONENT","COMPONENT"},{"DESCRIPTION","DESCRIPTION"},{"QTY","QTY"},{"WEIGHT(KG)","WEIGHT"} };
        var cols = new Dictionary<string, double>();
        foreach (var kv in map)
        {
            int idx = s.IndexOf(kv.Key);
            if (idx < 0) continue;
            int wi0 = charWord[idx], wi1 = charWord[idx + kv.Key.Length - 1];
            double xc = (order[wi0].x0 + order[wi1].x1) / 2;
            if (!cols.ContainsKey(kv.Value) || xc < cols[kv.Value]) cols[kv.Value] = xc;
        }
        return cols;
    }
    static Dictionary<string, double> DetectTableColumns(List<Word> words, double titleY)
    {
        var zone = words.Where(w => w.x0 > 1000 && w.x0 < 1700 && titleY - 12 <= w.yc && w.yc <= titleY + 45).ToList();
        zone.Sort((a, b) => a.yc.CompareTo(b.yc));
        var clusters = new List<List<Word>>(); var cur = new List<Word>(); double? cy = null;
        foreach (var w in zone)
        {
            if (cy == null || Math.Abs(w.yc - cy.Value) <= 5) { cur.Add(w); cy = cy == null ? w.yc : (cy + w.yc) / 2; }
            else { clusters.Add(cur); cur = new List<Word> { w }; cy = w.yc; }
        }
        if (cur.Count > 0) clusters.Add(cur);
        foreach (var cl in clusters)
        {
            var cols = ColsFromWords(cl);
            if (cols.ContainsKey("NO") && (cols.ContainsKey("QTY") || cols.ContainsKey("WEIGHT")))
                return cols.OrderBy(kv => kv.Value).ToDictionary(kv => kv.Key, kv => kv.Value);
            var cols2 = ColsFromLineStr(cl);
            if (cols2.ContainsKey("NO") && (cols2.ContainsKey("QTY") || cols2.ContainsKey("WEIGHT")))
                return cols2.OrderBy(kv => kv.Value).ToDictionary(kv => kv.Key, kv => kv.Value);
        }
        return null;
    }

    static List<List<Word>> ClusterByY(List<Word> words, double yTol = 4.5)
    {
        if (words.Count == 0) return new List<List<Word>>();
        var order = words.OrderBy(w => w.yc).ThenBy(w => w.x0).ToList();
        var groups = new List<List<Word>> { new List<Word> { order[0] } };
        foreach (var w in order.Skip(1))
        {
            if (w.yc - groups.Last().Last().yc <= yTol) groups.Last().Add(w);
            else groups.Add(new List<Word> { w });
        }
        return groups;
    }

    static bool IsSummary(Dictionary<string, string> cells)
    {
        var t = Norm(string.Join(" ", cells.Values));
        return t.Contains("total") || t.Contains("spool") || t.Contains("summary") ||
            t.StartsWith("cutpipe") || t.Contains("cutlength") || t.StartsWith("pipespool") || t == "spools";
    }
    static bool IsSectionHeader(string txt)
    {
        var bare = Regex.Replace(Norm(txt), @"一-鿿", "");
        return SECTION_KEYWORDS.Contains(bare);
    }
    static string CleanDesc(string text) => Regex.Replace(text, @"TOTAL(\s*\S*)*$", "", RegexOptions.IgnoreCase).Trim();
    static string JoinDesc(List<(double ly, string txt)> pairs)
    {
        if (pairs.Count == 0) return "";
        var ordered = pairs.OrderBy(p => p.ly).ToList();
        var outp = new List<string>(); string prev = null;
        foreach (var (ly, txt) in ordered)
        {
            if (IsSectionHeader(txt)) continue;
            var nt = Norm(txt);
            if (nt == prev) continue;
            outp.Add(txt); prev = nt;
        }
        return CleanDesc(string.Join(" ", outp));
    }
    static string PickClosest(List<(double ly, string txt)> pairs, double anchorYc)
    {
        if (pairs.Count == 0) return "";
        var sorted = pairs.OrderBy(p => Math.Abs(p.Item1 - anchorYc)).ToList();
        double thr = Math.Abs(sorted[0].Item1 - anchorYc) + 1.5;
        return string.Join(" ", sorted.Where(p => Math.Abs(p.Item1 - anchorYc) <= thr).Select(p => p.Item2)).Trim();
    }

    class RowOut { public string pipeNo, table, no, dn, comp, desc, qty, wt; public int page; }
    static string[] RowFields(RowOut r)
        => ComponentSeparate
            ? new[] { r.pipeNo, r.table, r.no, r.dn, r.comp, r.desc, r.qty, r.wt, r.page.ToString() }
            : new[] { r.pipeNo, r.table, r.no, r.dn, r.desc, r.qty, r.wt, r.page.ToString() };

    static List<RowOut> BuildTableRows(List<Word> dataWords, string pipeNo, Dictionary<string, double> cols, string tableType, RefDict dict)
    {
        var bands = FindLabelBands(dataWords);
        var filt = dataWords.Where(w => !bands.Any(b => Math.Abs(w.yc - b) <= 4)).ToList();
        var headerWords = new HashSet<string> { "mm", "(mm)" };
        filt = filt.Where(w => !headerWords.Contains(Norm(w.text))).ToList();
        filt = filt.Where(w => !(w.xc > B_NO_MAX && w.xc < B_DN_MAX && DN_HEADER_TOKENS.Contains(Norm(w.text)))).ToList();

        double noX = cols.ContainsKey("NO") ? cols["NO"] : 1123;
        double dnX = cols.ContainsKey("DN") ? cols["DN"] : 1142;
        double qtyX = cols.ContainsKey("QTY") ? cols["QTY"] : 1548;
        double wtX = cols.ContainsKey("WEIGHT") ? cols["WEIGHT"] : qtyX + 58;
        double descX = cols.ContainsKey("DESCRIPTION") ? cols["DESCRIPTION"] : (dnX + qtyX) / 2;
        double compX = cols.ContainsKey("COMPONENT") ? cols["COMPONENT"] : (dnX + descX) / 2;
        double bNoDn = (noX + dnX) / 2, bDnDesc = (dnX + compX) / 2, bDescQty = qtyX - 25, bQtyWt = (qtyX + wtX) / 2;
        string ColOf(double xc)
        {
            if (xc < bNoDn) return "NO";
            if (xc < bDnDesc) return "DN";
            if (xc < bDescQty) return "DESC";
            if (xc < bQtyWt) return "QTY";
            return "WT";
        }

        var noCells = filt.Where(w => ColOf(w.xc) == "NO" && IsNumericNo(w.text)).ToList();
        if (noCells.Count == 0) return new List<RowOut>();
        noCells.Sort((a, b) => a.yc.CompareTo(b.yc));
        var anchors = noCells;
        {
            var merged = new List<Word>();
            int i = 0;
            while (i < anchors.Count)
            {
                var cur = anchors[i];
                int j = i + 1;
                while (j < anchors.Count)
                {
                    var nx = anchors[j];
                    bool curSingle = cur.text.Length == 1 && char.IsDigit(cur.text[0]);
                    bool nxSingle = nx.text.Length == 1 && char.IsDigit(nx.text[0]);
                    double gap = nx.yc - cur.yc;
                    if (curSingle && nxSingle && gap > 0 && gap < 9) { cur.text = cur.text + nx.text; cur.yc = nx.yc; j++; }
                    else break;
                }
                merged.Add(cur);
                i = j;
            }
            anchors = merged;
        }

        // ---- detect group sub-headers (e.g. FITTINGS, VALVES, FLANGES) ----
        // A group banner is a line in the component/description x-zone whose
        // English-letter tokens include a category keyword AND which has NO NO
        // number anchor on its baseline (real data rows always carry a NO).
        var bannerWords = new HashSet<Word>();
        // A banner is a "type banner" (PIPE / PIPES) when its keyword is itself a
        // concrete component type that also appears as the item's COMPONENT (the
        // reference workbook keeps such a header in the description and as the
        // component). Every other banner (FITTINGS, FLANGES, GASKETS, SUPPORTS, ...)
        // is a CATEGORY header: it is removed from the description so the member
        // rows read with their own true component (TEE, WELD NECK FLANGE, GASKET,
        // PIPE SUPPORT, ...). Note VALVES is NOT a banner at all (its Latin part
        // "VALVES / IN-LINE ITEMS" is not all-keyword), so it naturally stays in
        // the description and yields COMPONENT=VALVES.
        var banners = new List<(double yc, string label, bool isType)>();
        {
            var zone = filt.Where(w => w.xc >= bDnDesc - 15 && w.xc <= bDescQty + 30).ToList();
            foreach (var grp in ClusterByY(zone, 4.5))
            {
                var g = grp.OrderBy(w => w.x0).ToList();
                double ly = g.Average(w => w.yc);
                // A genuine group banner is a SHORT line whose Latin tokens are ALL
                // category keywords (e.g. "FITTINGS", "FLANGES", "PIPE" -- possibly
                // with a CJK translation like "FITTINGS配件"/"FLANGES法兰") AND which
                // carries NO NO-number anchor. A real item row also begins with a
                // keyword (PIPE/WELD NECK FLANGE/...) but continues with descriptive
                // text (dimensions, DIN codes, materials, CJK), so its Latin tokens
                // are NOT all keywords -- it is correctly left as a data row.
                // We deliberately do NOT reject CJK here: the bilingual banners carry
                // a CJK translation, and rejecting them would let them leak into the
                // description and corrupt the COMPONENT column.
                var latin = new List<string>();
                foreach (var w in g)
                    foreach (var tk in Regex.Split(w.text, @"[^A-Za-z]+"))
                        if (tk.Length >= 2 && tk.All(char.IsLetter)) latin.Add(tk.ToUpperInvariant());
                bool allKeyword = latin.Count > 0 && latin.All(t => GROUP_KEYWORDS.Contains(t));
                if (!allKeyword) continue;            // has descriptive Latin text -> real row, not a banner
                if (anchors.Any(a => Math.Abs(a.yc - ly) <= 8)) continue; // real data row, not a banner
                bool isType = latin.Any(t => TYPE_BANNER.Contains(t));
                // Category banners are removed from the description; type banners
                // (PIPE) are left in so they become the row's component + leading
                // description text (matching the reference workbook).
                if (!isType) bannerWords.UnionWith(g);
                banners.Add((ly, string.Join(" ", latin.Distinct()), isType));
            }
            banners = banners.OrderBy(b => b.yc).ToList();
        }

        var colLines = new Dictionary<string, List<Word>> { ["DN"] = new(), ["DESC"] = new(), ["QTY"] = new(), ["WT"] = new() };
        foreach (var w in filt)
        {
            var c = ColOf(w.xc);
            if ((c == "QTY" || c == "WT") && !LooksNumeric(w.text)) c = "DESC";
            if (c == "NO") continue;
            if (bannerWords.Contains(w)) continue; // keep banners out of the description cell
            if (colLines.ContainsKey(c)) colLines[c].Add(w);
        }
        var colLineTxt = new Dictionary<string, List<(double, string)>>();
        foreach (var col in colLines.Keys.ToList())
        {
            var cw = colLines[col]; if (cw.Count == 0) { colLineTxt[col] = new(); continue; }
            var lines = new List<(double, string)>();
            foreach (var grp in ClusterByY(cw, 4.5))
            {
                var g = grp.OrderBy(w => w.x0).ToList();
                double ly = g.Average(w => w.yc);
                string txt = NormWs(string.Join(" ", g.Select(w => w.text)));
                if (!string.IsNullOrEmpty(txt)) lines.Add((ly, txt));
            }
            colLineTxt[col] = lines;
        }

        var assigned = anchors.Select(a => new { a, NO = a.text, DN = new List<(double,string)>(), DESC = new List<(double,string)>(), QTY = new List<(double,string)>(), WT = new List<(double,string)>() }).ToList();
        foreach (var (ly, txt) in colLineTxt["DESC"])
        {
            double bd = 1e9; Word best = anchors[0];
            foreach (var a in anchors) { double d = Math.Abs(a.yc - ly); if (d < bd) { bd = d; best = a; } }
            assigned.First(x => x.a == best).DESC.Add((ly, txt));
        }
        AssignCol(assigned, colLineTxt["DN"], "DN");
        AssignCol(assigned, colLineTxt["QTY"], "QTY");
        AssignCol(assigned, colLineTxt["WT"], "WT");

        var rows = new List<RowOut>();
        foreach (var b in assigned)
        {
            var a = b.a;
            var no = NormWs(b.NO);
            if (string.IsNullOrEmpty(no) || !IsNumericNo(no)) continue;
            var dn = NormWs(PickClosest(b.DN, a.yc));
            var qty = NormWs(PickClosest(b.QTY, a.yc));
            var wt = NormWs(PickClosest(b.WT, a.yc));
            var rawDesc = JoinDesc(b.DESC);
            var descRaw = ReconstructSpaces(rawDesc, dict);
            // attach the nearest group sub-header above this row (if any)
            string group = "";
            foreach (var bg in banners) { if (bg.yc < a.yc) group = bg.label; else break; }
            // The component type is the leading keyword(s) of the description
            // (e.g. "TEE", "WELD OLET", "PIPE SUPPORT"). If the item text carries
            // no explicit component word, inherit the enclosing section banner
            // (e.g. "PIPE") as the type -- this mirrors the reference workbook,
            // where pipe-section rows with no leading keyword still read COMPONENT='PIPE'.
            string comp = RecoverComponent(descRaw);

            string desc;
            if (ComponentSeparate)
            {
                // 'separate' mode: the leading COMPONENT keyword is lifted into its
                // own column, so DESCRIPTION holds only the remaining text (the rest
                // of the merged string after the type). This is the requested
                // "re-classify from the merged version" compromise.
                int px = LeadingComponentConsumed(rawDesc);
                desc = (px > 0 && px < rawDesc.Length)
                    ? ReconstructSpaces(rawDesc.Substring(px).TrimStart(), dict)
                    : descRaw;
            }
            else
            {
                // 'merged' mode: DESCRIPTION keeps the full (reconstructed) text.
                // Optionally prefix the group banner (e.g. "FITTINGS") so every
                // member row carries its section context.
                desc = descRaw;
                if (GroupHeaderMode == "embed" && !string.IsNullOrEmpty(group) && !Norm(desc).StartsWith(Norm(group)))
                    desc = group + " " + desc;
            }
            var cells = new Dictionary<string, string> { ["NO"] = no, ["DN"] = dn, ["DESC"] = desc, ["QTY"] = qty, ["WEIGHT"] = wt };
            if (IsSummary(cells)) continue;
            if (string.IsNullOrEmpty(desc) && string.IsNullOrEmpty(dn) && string.IsNullOrEmpty(qty) && string.IsNullOrEmpty(wt)) continue;
            rows.Add(new RowOut { pipeNo = pipeNo ?? "UNKNOWN", table = tableType, no = no, dn = dn, comp = comp, desc = desc, qty = qty, wt = wt });
        }
        return rows;
    }

    static void AssignCol(dynamic assigned, List<(double, string)> lines, string col)
    {
        var sorted = lines.OrderBy(p => p.Item1).ToList();
        if (sorted.Count == 0) return;
        if (sorted.Count == assigned.Count)
        {
            for (int i = 0; i < sorted.Count; i++) AddTo(assigned[i], col, sorted[i]);
        }
        else
        {
            foreach (var (ly, txt) in sorted)
            {
                double bd = 1e9; object best = assigned[0];
                foreach (var x in assigned) { double d = Math.Abs(((Word)x.a).yc - ly); if (d < bd) { bd = d; best = x; } }
                AddTo(best, col, (ly, txt));
            }
        }
    }
    static void AddTo(dynamic x, string col, (double ly, string txt) p)
    {
        switch (col) { case "DN": x.DN.Add(p); break; case "COMP": x.COMP.Add(p); break; case "QTY": x.QTY.Add(p); break; case "WT": x.WT.Add(p); break; }
    }

    // MuPDFCore's native document-open marshals the path as ANSI, so non-ASCII
    // paths fail. Work around it by copying any such PDF to a temp ASCII-named
    // file, opening that, then deleting the temp afterwards.
    static List<RowOut> ProcessPdf(string pdfPath, RefDict dict)
    {
        var all = new List<RowOut>();
        string workPath = pdfPath;
        string tmp = null;
        if (pdfPath.Any(c => c > 127))
        {
            tmp = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N") + ".pdf");
            File.Copy(pdfPath, tmp, true);
            workPath = tmp;
            LogV($"  (copied unicode path to temp '{tmp}')");
        }
        try
        {
        using var ctx = new MuPDFContext(512L * 1024 * 1024);
        using var doc = new MuPDFDocument(ctx, workPath);
        int n = doc.Pages.Count;
        for (int pno = 0; pno < n; pno++)
        {
            if (Verbose && pno % 10 == 0) Console.Error.WriteLine($"  page {pno + 1}/{n}");
            var st = doc.GetStructuredTextPage(pno, false);
            double Hpage = doc.Pages[pno].Bounds.Height;
            var words = GetWords(st, Hpage);
            if (words.Count == 0) continue;
            double Wmax = words.Max(w => w.x1), H = PageH(words);
            var pipeNo = ExtractPipeNo(words, Wmax, H);
            var headers = FindHeaderRows(words);
            if (headers.Count == 0) { LogV($"  page {pno + 1}: no table header, skipped"); continue; }
            string pn = pipeNo;
            if (pn == null && pno <= 5) pn = "408.101.P01";
            var pageBands = FindLabelBands(words);
            for (int i = 0; i < headers.Count; i++)
            {
                double hy = headers[i];
                double yTop = hy + 6;
                double hardBottom = 850;
                double yBottom = (i + 1 < headers.Count) ? headers[i + 1] - 6 : Math.Min(H - 5, hardBottom);
                foreach (var b in pageBands) if (b > yTop) yBottom = Math.Min(yBottom, b - 4);
                yBottom = Math.Min(yBottom, hardBottom);
                if (yBottom <= yTop) continue;
                var cols = DetectTableColumns(words, hy);
                if (cols == null) cols = new Dictionary<string, double> { ["NO"] = 1123, ["DN"] = 1142, ["COMPONENT"] = 1221, ["DESCRIPTION"] = 1276, ["QTY"] = 1548, ["WEIGHT"] = 1606 };
                double xLo = cols.Values.Min() - 45, xHi = cols.Values.Max() + 45;
                var dataWords = words.Where(w => w.xc > xLo && w.xc < xHi && w.yc >= yTop && w.yc <= yBottom).ToList();
                string tableType = i == 0 ? "FABRICATION" : "ERECTION";
                foreach (var item in BuildTableRows(dataWords, pn, cols, tableType, dict))
                {
                    item.page = pno + 1;
                    all.Add(item);
                }
            }
        }
        Console.Error.WriteLine($"  rows: {all.Count}");
        return all;
        }
        finally
        {
            if (tmp != null) { try { File.Delete(tmp); } catch { } }
        }
    }

    // ---- output writers ----
    static string Csv(string v)
    {
        v = v ?? "";
        return v.Contains(',') || v.Contains('"') || v.Contains('\n') ? "\"" + v.Replace("\"", "\"\"") + "\"" : v;
    }
    static void WriteCsv(string path, List<RowOut> rows)
    {
        var sb = new StringBuilder(); sb.Append('\uFEFF');
        sb.AppendLine(string.Join(",", HEADERS));
        foreach (var r in rows) sb.AppendLine(string.Join(",", RowFields(r).Select(Csv)));
        File.WriteAllText(path, sb.ToString(), new UTF8Encoding(false));
        Console.Error.WriteLine($"Saved CSV  {path} ({rows.Count} rows)");
    }
    static string ColLetter(int idx) { string s = ""; idx++; while (idx > 0) { int m = (idx - 1) % 26; s = (char)('A' + m) + s; idx = (idx - 1) / 26; } return s; }
    static string XEsc(string v) => v.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;").Replace("\"", "&quot;");
    static bool IsNumCol(string name) => name == "DN(mm)" || name == "QTY" || name == "WEIGHT(kg)" || name == "PAGE";
    const string CT =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n" +
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">" +
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>" +
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>" +
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>" +
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>" +
        "</Types>";
    const string RELS =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n" +
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" +
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>" +
        "</Relationships>";
    const string WB =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n" +
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">" +
        "<sheets><sheet name=\"Materials\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
    const string WB_RELS =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n" +
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" +
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>" +
        "</Relationships>";
    static void AddEntry(ZipArchive zip, string name, string content)
    {
        var e = zip.CreateEntry(name);
        using var w = new StreamWriter(e.Open(), new UTF8Encoding(false));
        w.Write(content);
    }
    static void WriteXlsx(string path, List<RowOut> rows)
    {
        var sb = new StringBuilder();
        sb.Append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n");
        sb.Append("<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\r\n<sheetData>\r\n");
        sb.Append("<row r=\"1\">");
        for (int c = 0; c < HEADERS.Length; c++)
            sb.Append($"<c r=\"{ColLetter(c)}1\" t=\"inlineStr\"><is><t xml:space=\"preserve\">{XEsc(HEADERS[c])}</t></is></c>");
        sb.Append("</row>\r\n");
        for (int r = 0; r < rows.Count; r++)
        {
            int rn = r + 2;
            sb.Append($"<row r=\"{rn}\">");
            var f = RowFields(rows[r]);
            for (int c = 0; c < f.Length; c++)
            {
                string cl = ColLetter(c) + rn;
                string val = f[c] ?? "";
                if (IsNumCol(HEADERS[c]) && double.TryParse(val, NumberStyles.Any, CultureInfo.InvariantCulture, out var num))
                    sb.Append($"<c r=\"{cl}\"><v>{num.ToString(CultureInfo.InvariantCulture)}</v></c>");
                else
                    sb.Append($"<c r=\"{cl}\" t=\"inlineStr\"><is><t xml:space=\"preserve\">{XEsc(val)}</t></is></c>");
            }
            sb.Append("</row>\r\n");
        }
        sb.Append("</sheetData>\r\n</worksheet>");
        using var zip = new ZipArchive(File.Create(path), ZipArchiveMode.Create);
        AddEntry(zip, "[Content_Types].xml", CT);
        AddEntry(zip, "_rels/.rels", RELS);
        AddEntry(zip, "xl/workbook.xml", WB);
        AddEntry(zip, "xl/_rels/workbook.xml.rels", WB_RELS);
        AddEntry(zip, "xl/worksheets/sheet1.xml", sb.ToString());
        Console.Error.WriteLine($"Saved XLSX {path} ({rows.Count} rows)");
    }

    // ---- argument parsing / CLI ----
    static void PrintHelp()
    {
        Console.WriteLine("EGpdf2excel v3.0.0 - 从等轴测 PDF 抽取预制/安装材料表");
        Console.WriteLine("EGpdf2excel v3.0.0 - extract fabrication/erection material tables from isometric PDFs");
        Console.WriteLine("Copyright © 2026 HerryABU");
        Console.WriteLine();
        Console.WriteLine("用法 / Usage:");
        Console.WriteLine("  EGpdf2excel [inputs...] [options]");
        Console.WriteLine();
        Console.WriteLine("  inputs            输入 PDF 文件、目录或通配符");
        Console.WriteLine("                     PDF file(s), a directory, or a wildcard glob.");
        Console.WriteLine("                     例: *.pdf   C:/docs/*.pdf   C:/scan   file1.pdf file2.pdf");
        Console.WriteLine("                     e.g.  *.pdf   C:/docs/*.pdf   C:/scan   file1.pdf file2.pdf");
        Console.WriteLine("                     省略时默认处理当前目录下的 \"*.pdf\"。");
        Console.WriteLine("                     If omitted, defaults to \"*.pdf\" in the current directory.");
        Console.WriteLine();
        Console.WriteLine("选项 / Options:");
        Console.WriteLine("  -o, --output DIR|FILE   输出目录，或单文件(扩展名由 --format 决定)");
        Console.WriteLine("                           Output directory, or a single output file when one");
        Console.WriteLine("                           input is given (extension is set by --format).");
        Console.WriteLine("                           默认: 与各输入同目录");
        Console.WriteLine("                           Default: same directory as each input.");
        Console.WriteLine("  -f, --format csv|xlsx|both   输出格式 (默认 csv)");
        Console.WriteLine("                           Output format (default csv).");
        Console.WriteLine("  -r, --ref FILE.xlsx     参考工作簿；用其 DESCRIPTION 词表还原描述词间距");
        Console.WriteLine("                           Reference workbook; its DESCRIPTION vocabulary is");
        Console.WriteLine("                           used to reconstruct word spacing in descriptions.");
        Console.WriteLine("  -G, --group-header MODE 处理 PDF 表内分组横幅(FITTINGS/VALVES/FLANGES):");
        Console.WriteLine("                           How to treat the PDF's in-table group banners");
        Console.WriteLine("                           'embed' 把横幅前缀到每行 DESCRIPTION");
        Console.WriteLine("                           (e.g. FITTINGS, VALVES, FLANGES). 'embed' prefixes");
        Console.WriteLine("                           the banner onto every member row's DESCRIPTION cell;");
        Console.WriteLine("                           'omit' 不添加 (默认 embed)");
        Console.WriteLine("                           'omit' leaves it out (default: embed).");
        Console.WriteLine("  -C, --component MODE    构件类型列布局: 'merged' 并入 DESCRIPTION(8列,默认)");
        Console.WriteLine("                           Component-type column layout: 'merged' keeps");
        Console.WriteLine("                           COMPONENT inside the DESCRIPTION cell (8 cols, default);");
        Console.WriteLine("                           'separate' 单独成列(位于 DN(mm) 与 DESCRIPTION 间,9列)");
        Console.WriteLine("                           'separate' emits it as its own column between DN(mm)");
        Console.WriteLine("                           and DESCRIPTION (9 cols).");
        Console.WriteLine("  --tag NAME              文件名标签 (默认 V3) -> <pdf>_<tag>.csv/.xlsx");
        Console.WriteLine("                           Filename tag (default V3) -> <pdf>_<tag>.csv/.xlsx.");
        Console.WriteLine("  --version               显示版本号并退出 / Show version and exit.");
        Console.WriteLine("  -v, --verbose           详细进度(每页/警告)输出到 stderr");
        Console.WriteLine("                           Detailed progress (per-page, warnings) to stderr.");
        Console.WriteLine("  -h, --help              显示本帮助并退出");
        Console.WriteLine("                           Show this help and exit.");
        Console.WriteLine();
        Console.WriteLine("示例 / Examples:");
        Console.WriteLine("  EGpdf2excel -v *.pdf -f both -r ref.xlsx -o out/");
        Console.WriteLine("  EGpdf2excel C:/scan/408-101-051*.pdf --tag V4 -f xlsx");
    }

    static List<string> ExpandInputs(List<string> inputs)
    {
        var files = new List<string>();
        foreach (var arg in inputs)
        {
            if (arg.Contains('*') || arg.Contains('?'))
            {
                string dir = Path.GetDirectoryName(arg); if (string.IsNullOrEmpty(dir)) dir = ".";
                string pat = Path.GetFileName(arg); if (string.IsNullOrEmpty(pat)) pat = "*";
                foreach (var f in Directory.GetFiles(dir, pat, SearchOption.TopDirectoryOnly))
                    if (f.EndsWith(".pdf", StringComparison.OrdinalIgnoreCase)) files.Add(Path.GetFullPath(f));
                if (files.Count == 0) LogV($"  [warn] no PDF matched glob '{arg}'");
            }
            else if (Directory.Exists(arg))
            {
                foreach (var f in Directory.GetFiles(arg, "*.pdf", SearchOption.TopDirectoryOnly))
                    files.Add(Path.GetFullPath(f));
                if (files.Count == 0) LogV($"  [warn] no PDF in directory '{arg}'");
            }
            else if (File.Exists(arg))
            {
                files.Add(Path.GetFullPath(arg));
            }
            else LogV($"  [warn] input not found: '{arg}'");
        }
        return files.Distinct().ToList();
    }

    private static int MainCore(string[] args)
    {
        var inputs = new List<string>();
        string outputDir = null, singleOut = null, format = "csv", tag = "V3", refPath = null;
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i];
            string take(string def) => (i + 1 < args.Length) ? args[++i] : def;
            switch (a)
            {
                case "-h": case "--help": PrintHelp(); return 0;
                case "--version": Console.WriteLine("EGpdf2excel v3.0.0"); return 0;
                case "-v": case "--verbose": Verbose = true; break;
                case "-o": case "--output": { string v = take(""); if (v.EndsWith(".csv", StringComparison.OrdinalIgnoreCase) || v.EndsWith(".xlsx", StringComparison.OrdinalIgnoreCase)) singleOut = v; else outputDir = v; break; }
                case "-f": case "--format": format = take("csv").ToLowerInvariant(); break;
                case "-r": case "--ref": refPath = take(""); break;
                case "-G": case "--group-header": { string v = take("embed").ToLowerInvariant(); if (v != "embed" && v != "omit") { Console.Error.WriteLine($"Invalid --group-header '{v}' (embed|omit)"); return 2; } GroupHeaderMode = v; break; }
                case "-C": case "--component": { string v = take("merged").ToLowerInvariant(); if (v != "merged" && v != "separate") { Console.Error.WriteLine($"Invalid --component '{v}' (merged|separate)"); return 2; } ComponentSeparate = (v == "separate"); break; }
                case "--tag": tag = take("V3"); break;
                default:
                    if (a.StartsWith("-")) { Console.Error.WriteLine($"Unknown option: {a} (use -h for help)"); return 2; }
                    inputs.Add(a); break;
            }
        }
        if (format != "csv" && format != "xlsx" && format != "both") { Console.Error.WriteLine($"Invalid --format '{format}' (csv|xlsx|both)"); return 2; }

        if (ComponentSeparate)
            HEADERS = new[] { "pipe no", "TABLE", "NO", "DN(mm)", "COMPONENT", "项目/DESCRIPTION", "QTY", "WEIGHT(kg)", "PAGE" };

        if (inputs.Count == 0) inputs.Add("*.pdf");
        var files = ExpandInputs(inputs);
        if (files.Count == 0) { Console.Error.WriteLine("No input PDF files found."); return 1; }
        if (singleOut != null && files.Count > 1) { Console.Error.WriteLine("A single output FILE was given but multiple inputs matched; using it as a directory."); outputDir = Path.GetDirectoryName(singleOut); if (string.IsNullOrEmpty(outputDir)) outputDir = "."; }

        RefDict dict = null;
        if (!string.IsNullOrEmpty(refPath))
        {
            Console.Error.WriteLine($"Loading reference vocabulary from {refPath} ...");
            var descs = ReadRefDescriptions(refPath);
            dict = BuildRefDict(descs);
            Console.Error.WriteLine($"  reference tokens: {descs.Count} description rows");
        }

        int total = 0;
        foreach (var pdf in files)
        {
            Console.Error.WriteLine($"Processing {pdf} ...");
            var rows = ProcessPdf(pdf, dict);
            string baseName = Path.GetFileNameWithoutExtension(pdf);
            string outDir = outputDir ?? Path.GetDirectoryName(pdf);
            if (string.IsNullOrEmpty(outDir)) outDir = ".";
            Directory.CreateDirectory(outDir);
            string stem = singleOut != null && files.Count == 1
                ? Path.Combine(Path.GetDirectoryName(singleOut), Path.GetFileNameWithoutExtension(singleOut))
                : Path.Combine(outDir, baseName + "_" + tag);
            if (format == "csv" || format == "both") WriteCsv(stem + ".csv", rows);
            if (format == "xlsx" || format == "both") WriteXlsx(stem + ".xlsx", rows);
            total += rows.Count;
        }
        Console.Error.WriteLine($"Done. {files.Count} file(s), {total} total rows.");
        return 0;
    }

    /// <summary>
    /// Entry point used by both the CLI and the GUI. When `log` is provided,
    /// all engine output (Console.Out / Console.Error) is forwarded to it, so
    /// the back-end stays completely decoupled from any front-end.
    /// </summary>
    public static int Run(string[] args, Action<string>? log = null)
    {
        TextWriter? prevErr = Console.Error, prevOut = Console.Out;
        ForwardingWriter? fw = null;
        if (log != null)
        {
            fw = new ForwardingWriter(log);
            Console.SetError(fw);
            Console.SetOut(fw);
        }
        try { return MainCore(args); }
        finally { fw?.Flush(); Console.SetError(prevErr); Console.SetOut(prevOut); }
    }
    }
}
