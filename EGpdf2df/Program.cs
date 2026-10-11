// EGpdf2df — command-line kernel that compares two drawing PDFs (baseline vs
// target) and emits a red-annotated difference PDF plus a JSON summary. Fully
// decoupled from any UI: it reads a JSON contract from a file or stdin and writes
// the JSON result to stdout.
using EGpdf2df.Engine;
using EGtools.Core;
using MuPDFCore;
using MuPDFCore.StructuredText;
using System;
using System.Globalization;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace EGpdf2df;

public static class Program
{
    public static int Main(string[] args)
    {
        return Run(args);
    }

    public static int Run(string[] args, Action<string>? log = null)
    {
        var L = log ?? Console.Error.WriteLine;

        string? jsonFile = null;
        string? configPath = null;
        string? baseline = null, target = null, output = null;
        string? pagesArg = null;
        string? textAddedColor = null, textRemovedColor = null, textInsertDirection = null;
        string? markStyle = null, markStrokeColor = null;
        string? graphicMethod = null, reportMode = null;
        double? vectorTol = null;
        bool readStdin = false;
        bool locateMode = false;
        bool vecDiag = false;
        bool vecScan = false;
        bool sheetNos = false;
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i];
            string take(string d) => i + 1 < args.Length ? args[++i] : d;
            switch (a)
            {
                case "-h": case "--help":
                    PrintHelp();
                    return 0;
                case "--json": jsonFile = take(""); break;
                case "--config": configPath = take(""); break;
                case "--stdin": readStdin = true; break;
                case "--locate": locateMode = true; break;
                case "--vecdiag": vecDiag = true; break;
                case "--vecscan": vecScan = true; break;
                case "--sheetnos": sheetNos = true; break;
                case "-o": case "--output": output = take(""); break;
                case "-p": case "--pages": pagesArg = take(""); break;
                case "--text-added-color": textAddedColor = take(""); break;
                case "--text-removed-color": textRemovedColor = take(""); break;
                case "--text-insert-direction": textInsertDirection = take(""); break;
                case "--mark-style": markStyle = take(""); break;
                case "--mark-stroke-color": markStrokeColor = take(""); break;
                case "--graphic-method": graphicMethod = take(""); break;
                case "--vector-tol": if (double.TryParse(take(""), NumberStyles.Float, CultureInfo.InvariantCulture, out var vt)) vectorTol = vt; break;
                case "--report-mode": reportMode = take(""); break;
                default:
                    if (a.StartsWith("-")) { L($"Unknown option: {a} (use --help)"); return 2; }
                    if (baseline == null) baseline = a; else if (target == null) target = a; else L($"Ignoring extra argument: {a}");
                    break;
            }
        }

        // --json 既可给路径也可给名称（名称按软件目录查找）。
        jsonFile = ResolveJsonArgument(jsonFile);

        // ---- locate mode: find a text string's coordinates in both PDFs ----
        if (locateMode)
            return RunLocate(jsonFile, readStdin, L);

        // ---- sheetnos mode: 列出每页提取到的图号（核对配对是否合理）----
        if (sheetNos)
        {
            if (target == null) { L("[error] --sheetnos needs <pdf> [<pdf2>]"); return 2; }
            var cfg0 = Pdf2dfConfig.Load(null);
            using var ctx0 = new MuPDFContext(512u * 1024 * 1024);
            using var d1 = new MuPDFDocument(ctx0, baseline!);
            L($"== {Path.GetFileName(baseline!)} ({d1.Pages.Count} 页) ==");
            for (int i = 0; i < d1.Pages.Count; i++)
                L($"   {i,4}: {DiffEngine.SheetNoOf(d1, i, cfg0.SheetNoRegionX, cfg0.SheetNoRegionY, cfg0.SheetNoMaxDistinct) ?? "(无)"}");
            if (target != null)
            {
                using var d2 = new MuPDFDocument(ctx0, target);
                L($"== {Path.GetFileName(target)} ({d2.Pages.Count} 页) ==");
                for (int i = 0; i < d2.Pages.Count; i++)
                    L($"   {i,4}: {DiffEngine.SheetNoOf(d2, i, cfg0.SheetNoRegionX, cfg0.SheetNoRegionY, cfg0.SheetNoMaxDistinct) ?? "(无)"}");
            }
            return 0;
        }
        if (vecDiag)
        {
            if (baseline == null || target == null) { L("[error] --vecdiag needs <baseline> <target> [-p page]"); return 2; }
            return RunVecDiag(baseline, target, pagesArg, L);
        }

        // ---- vecscan mode: 用【线画自身】判定两版页面对应关系 ----
        // 每页标题栏都是同一个项目号，按图纸号配对会退化成按页序配对；一旦新版插入/删除页，
        // 后续全体错位。此模式对一个基准页扫描全部目标页，命中率最高者 = 真实对应页。
        if (vecScan)
        {
            if (baseline == null || target == null) { L("[error] --vecscan needs <baseline> <target> [-p basePages]"); return 2; }
            return RunVecScan(baseline, target, pagesArg, L);
        }

        DiffRequest? req = null;
        if (jsonFile != null)
        {
            req = JsonSerializer.Deserialize<DiffRequest>(File.ReadAllText(jsonFile), new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        }
        else if (readStdin || (baseline == null && target == null))
        {
            var stdin = Console.In.ReadToEnd();
            if (string.IsNullOrWhiteSpace(stdin)) { PrintHelp(); return 2; }
            req = JsonSerializer.Deserialize<DiffRequest>(stdin, new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        }

        if (req == null)
        {
            if (baseline == null || target == null) { L("baseline and target PDF are required."); PrintHelp(); return 2; }
            req = new DiffRequest { Baseline = baseline, Target = target, Output = output };
            if (pagesArg != null)
                req.Pages = pagesArg.Split(',').Select(s => int.Parse(s.Trim())).ToList();
        }

        var cfg = Pdf2dfConfig.Load(configPath);
        if (output != null) req.Output = output;
        // 前端颜色选择器 / 参数覆盖默认标注样式。
        if (req?.Options?.Colors != null)
        {
            var c = req.Options.Colors;
            if (!string.IsNullOrWhiteSpace(c.Trace)) cfg.Annotation.TraceColor = c.Trace;
            if (!string.IsNullOrWhiteSpace(c.Change)) cfg.Annotation.ChangeColor = c.Change;
            if (!string.IsNullOrWhiteSpace(c.TextAdded)) cfg.Annotation.TextAddedColor = c.TextAdded;
            if (!string.IsNullOrWhiteSpace(c.TextRemoved)) cfg.Annotation.TextRemovedColor = c.TextRemoved;
            if (!string.IsNullOrWhiteSpace(c.MarkStroke)) cfg.Annotation.MarkStrokeColor = c.MarkStroke;
        }
        var o = req?.Options;
        // 图形标记形式：semi(半透明填充+深色描边，默认) / solid(实心填充) / outline(仅描边)
        if (!string.IsNullOrWhiteSpace(o?.MarkStyle)) cfg.Annotation.MarkStyle = NormalizeMarkStyle(o!.MarkStyle);
        if (!string.IsNullOrWhiteSpace(markStyle)) cfg.Annotation.MarkStyle = NormalizeMarkStyle(markStyle);
        if (!string.IsNullOrWhiteSpace(markStrokeColor)) cfg.Annotation.MarkStrokeColor = markStrokeColor;
        if (o?.LineWidth != null) cfg.Annotation.LineWidth = (int)Math.Max(0.5, o.LineWidth.Value);
        if (o?.FillOpacity != null) cfg.Annotation.FillOpacity = Math.Min(1.0, Math.Max(0.0, o.FillOpacity.Value));
        if (o?.Outline != null) cfg.Annotation.Outline = o.Outline.Value;
        if (o?.HaloPx != null) cfg.Annotation.HaloPx = Math.Max(0, o.HaloPx.Value);
        if (!string.IsNullOrWhiteSpace(textAddedColor)) cfg.Annotation.TextAddedColor = textAddedColor;
        if (!string.IsNullOrWhiteSpace(textRemovedColor)) cfg.Annotation.TextRemovedColor = textRemovedColor;
        if (!string.IsNullOrWhiteSpace(textInsertDirection)) cfg.Annotation.TextInsertDirection = NormalizeDirection(textInsertDirection);
        if (!string.IsNullOrWhiteSpace(o?.TextInsertDirection)) cfg.Annotation.TextInsertDirection = NormalizeDirection(o!.TextInsertDirection);
        // 图形检测路径与矢量参数（前端 / 命令行可调）。
        if (!string.IsNullOrWhiteSpace(o?.GraphicMethod)) cfg.GraphicDiff.GraphicMethod = o!.GraphicMethod!;
        if (!string.IsNullOrWhiteSpace(graphicMethod)) cfg.GraphicDiff.GraphicMethod = graphicMethod;
        if (o?.VectorTolPt != null) cfg.GraphicDiff.VectorTolPt = Math.Max(0.1, o.VectorTolPt.Value);
        if (vectorTol != null) cfg.GraphicDiff.VectorTolPt = Math.Max(0.1, vectorTol.Value);
        if (o?.VectorAngleTolDeg != null) cfg.GraphicDiff.VectorAngleTolDeg = Math.Clamp(o.VectorAngleTolDeg.Value, 1, 60);
        if (o?.PageMatchMinSim != null) cfg.GraphicDiff.PageMatchMinSim = Math.Clamp(o.PageMatchMinSim.Value, 0.1, 0.999);
        if (o?.ChangeRadiusPt != null) cfg.GraphicDiff.ChangeRadiusPt = Math.Max(0, o.ChangeRadiusPt.Value);
        if (!string.IsNullOrWhiteSpace(o?.ReportMode)) cfg.GraphicDiff.ReportMode = o!.ReportMode!;
        if (!string.IsNullOrWhiteSpace(reportMode)) cfg.GraphicDiff.ReportMode = reportMode;
        if (o?.MinRunLengthPt != null) cfg.GraphicDiff.MinRunLengthPt = Math.Max(0, o.MinRunLengthPt.Value);
        if (o?.ClusterRadiusPt != null) cfg.GraphicDiff.ClusterRadiusPt = Math.Max(0, o.ClusterRadiusPt.Value);
        if (o?.MinClusterLengthPt != null) cfg.GraphicDiff.MinClusterLengthPt = Math.Max(0, o.MinClusterLengthPt.Value);
        if (o?.MinClusterRuns != null) cfg.GraphicDiff.MinClusterRuns = Math.Max(0, o.MinClusterRuns.Value);
        // ---- 文字差异参数 ----
        if (!string.IsNullOrWhiteSpace(o?.TextBoxMode)) cfg.Annotation.TextBoxMode = o!.TextBoxMode!;
        if (o?.TextBoxPadPt != null) cfg.Annotation.TextBoxPadPt = Math.Max(0, o.TextBoxPadPt.Value);
        if (!string.IsNullOrWhiteSpace(o?.TextSerialMatchMode)) cfg.Annotation.TextSerialMatchMode = o!.TextSerialMatchMode!;
        // ---- 图号配对参数 ----
        if (o?.SheetNoRegionX != null) cfg.SheetNoRegionX = Math.Clamp(o.SheetNoRegionX.Value, 0.0, 1.0);
        if (o?.SheetNoRegionY != null) cfg.SheetNoRegionY = Math.Clamp(o.SheetNoRegionY.Value, 0.0, 1.0);
        if (o?.SheetNoMaxDistinct != null) cfg.SheetNoMaxDistinct = Math.Max(1, o.SheetNoMaxDistinct.Value);
        return DiffEngine.Run(req, cfg, L);
    }

    /// <summary>
    /// 定位模式：在 baseline / target 两份 PDF 的指定页查找字符串，返回所有命中的
    /// 字形包围盒坐标。请求 JSON：
    /// { "baseline":"old.pdf","target":"new.pdf","page":0,"text":"X","caseSensitive?":false }
    /// </summary>
    static int RunLocate(string? jsonFile, bool readStdin, Action<string> L)
    {
        string json;
        try
        {
            if (readStdin) json = Console.In.ReadToEnd();
            else if (!string.IsNullOrEmpty(jsonFile)) json = File.ReadAllText(jsonFile);
            else { L("[error] locate needs --json FILE or --stdin"); return 2; }
        }
        catch (Exception ex) { L($"[error] reading request: {ex.Message}"); return 2; }

        JsonDocument doc;
        try { doc = JsonDocument.Parse(json); }
        catch (Exception ex) { L($"[error] invalid JSON: {ex.Message}"); return 2; }
        var root = doc.RootElement;
        string baseline = root.TryGetProperty("baseline", out var b) ? (b.GetString() ?? "") : "";
        string target = root.TryGetProperty("target", out var t) ? (t.GetString() ?? "") : "";
        string text = root.TryGetProperty("text", out var tv) ? (tv.GetString() ?? "") : "";
        int page = root.TryGetProperty("page", out var pv) && pv.TryGetInt32(out var pi) ? pi : 0;
        bool cs = root.TryGetProperty("caseSensitive", out var csv) && csv.GetBoolean();
        if (string.IsNullOrEmpty(baseline) || !File.Exists(baseline)
            || string.IsNullOrEmpty(target) || !File.Exists(target))
        { L($"[error] locate needs valid baseline/target ({baseline} | {target})"); return 2; }

        var outArr = new JsonArray();
        foreach (var (label, path) in new[] { ("baseline", baseline), ("target", target) })
        {
            var matches = new JsonArray();
            try
            {
                using var ctx = new MuPDFContext(512u * 1024 * 1024);
                string? tmp = null;
                MuPDFDocument pdf;
                if (path.Any(c => c > 127)) { tmp = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N") + ".pdf"); File.Copy(path, tmp, true); pdf = new MuPDFDocument(ctx, tmp); }
                else pdf = new MuPDFDocument(ctx, path);
                try
                {
                    if (page >= 0 && page < pdf.Pages.Count)
                    {
                        using var st = pdf.GetStructuredTextPage(page, false, StructuredTextFlags.None);
                        double H = pdf.Pages[page].Bounds.Y1 - pdf.Pages[page].Bounds.Y0;
                        var words = TextGeometry.GetWords(st, H, 0.18);
                        string key = cs ? text : text.ToLowerInvariant();
                        foreach (var w in words)
                        {
                            string src = cs ? (w.text ?? "") : (w.text ?? "").ToLowerInvariant();
                            if (src.Contains(key))
                                matches.Add(new JsonObject { ["x0"] = Math.Round(w.x0, 2), ["y0"] = Math.Round(w.y0, 2), ["x1"] = Math.Round(w.x1, 2), ["y1"] = Math.Round(w.y1, 2), ["text"] = w.text });
                        }
                    }
                }
                finally { pdf.Dispose(); if (tmp != null) { try { File.Delete(tmp); } catch { } } }
            }
            catch (Exception ex) { L($"[warn] locate on {label} failed: {ex.Message}"); }
            outArr.Add(new JsonObject { ["file"] = label, ["page"] = page, ["text"] = text, ["matchCount"] = matches.Count, ["matches"] = matches });
        }
        var result = new JsonObject { ["baseline"] = baseline, ["target"] = target, ["results"] = outArr };
        Console.WriteLine(result.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        return 0;
    }

    /// <summary>
    /// 矢量线画诊断：报告两版指定页的线段数、标定结果与覆盖匹配率、以及最大的若干变更折线。
    /// 用于快速判断"某页为什么报这么多/这么少差异"。
    /// </summary>
    /// <summary>
    /// 用线画自身判定页面对应：对给定基准页扫描全部目标页，报告覆盖命中率最高的目标页。
    /// 这是"两版页序是否真的对齐"的权威判据——命中率 ~100% 即真实对应页。
    /// </summary>
    static int RunVecScan(string baseline, string target, string? pagesArg, Action<string> L)
    {
        var cfg = Pdf2dfConfig.Load(null);
        var bases = pagesArg != null
            ? pagesArg.Split(',').Select(s => int.Parse(s.Trim())).ToList()
            : new List<int> { 0, 1, 2, 3, 4, 5 };
        using var bSrc = new VectorPageSource(baseline);
        using var tSrc = new VectorPageSource(target);
        int tMax = Math.Min(tSrc.PageCount, 14);
        L($"[scan] 扫描目标页 0..{tMax - 1}");

        // 目标页线画预先提取并缓存（提取是主要开销）
        var tCache = new Dictionary<int, List<VectorLinework.Seg>>();
        for (int j = 0; j < tMax; j++) tCache[j] = VectorLinework.Extract(tSrc, j);

        foreach (var bi in bases)
        {
            var b = VectorLinework.Extract(bSrc, bi);
            var scored = new List<(int t, double hit, int runs)>();
            for (int j = 0; j < tMax; j++)
            {
                var t = tCache[j];
                if (t.Count == 0 || b.Count == 0) continue;
                var r = VectorGeomDiff.Compare(b, t, cfg.GraphicDiff.VectorTolPt, cfg.GraphicDiff.VectorAngleTolDeg, null, quick: true);
                scored.Add((j, r.CalibHitRate, r.Runs.Count));
            }
            scored.Sort((x, y) => y.hit.CompareTo(x.hit));
            string top = string.Join("  ", scored.Take(4).Select(s => $"tgt{s.t}: {s.hit * 100:F1}% runs={s.runs}"));
            L($"[scan] base {bi} (segs={b.Count}) -> {top}");
        }
        return 0;
    }

    static string Span(List<VectorLinework.Seg> segs)
    {
        double x0 = double.MaxValue, y0 = double.MaxValue, x1 = double.MinValue, y1 = double.MinValue;
        foreach (var s in segs)
        {
            x0 = Math.Min(x0, Math.Min(s.X0, s.X1)); x1 = Math.Max(x1, Math.Max(s.X0, s.X1));
            y0 = Math.Min(y0, Math.Min(s.Y0, s.Y1)); y1 = Math.Max(y1, Math.Max(s.Y0, s.Y1));
        }
        if (x0 > x1) return "(empty)";
        return $"x[{x0:F1},{x1:F1}] y[{y0:F1},{y1:F1}]";
    }

    static int RunVecDiag(string baseline, string target, string? pagesArg, Action<string> L)
    {
        var pages = new List<int>();
        // 真实工作流是【按图纸号配对】，两版页序常有偏移（实测新版多插一页 → 后续整体错位一位）。
        // 故诊断支持 "基准页:目标页" 形式，如 -p 5:6,6:7，避免用错位的同号页得出"整页都变了"的假结论。
        var pairs2 = new List<(int b, int t)>();
        if (pagesArg != null)
            foreach (var seg in pagesArg.Split(','))
            {
                var kv = seg.Trim().Split(':');
                pairs2.Add(kv.Length == 2
                    ? (int.Parse(kv[0]), int.Parse(kv[1]))
                    : (int.Parse(kv[0]), int.Parse(kv[0])));
            }
        else pairs2.Add((0, 0));
        var cfg = Pdf2dfConfig.Load(null);
        using var bSrc = new VectorPageSource(baseline);
        using var tSrc = new VectorPageSource(target);
        foreach (var (p, q) in pairs2)
        {
            if (p < 0 || p >= bSrc.PageCount || q < 0 || q >= tSrc.PageCount) { L($"[vec] page {p}:{q} out of range"); continue; }
            var b = VectorLinework.Extract(bSrc, p);
            var t = VectorLinework.Extract(tSrc, q);
            L($"[vec] page {p}:{q}: contentBase={bSrc.Content(p).Length}B contentTgt={tSrc.Content(q).Length}B");
            L($"[vec] page {p}:{q}: baseSegs={b.Count} tgtSegs={t.Count} rotBase={bSrc.Rotation(p)} rotTgt={tSrc.Rotation(q)}");
            L($"[vec] page {p}:{q}: bboxBase={Span(b)}  bboxTgt={Span(t)}");
            var r = VectorGeomDiff.Compare(b, t, cfg.GraphicDiff.VectorTolPt, cfg.GraphicDiff.VectorAngleTolDeg, L,
                filter: new VectorGeomDiff.FilterCfg
                {
                    MinRunLengthPt = cfg.GraphicDiff.MinRunLengthPt,
                    ClusterRadiusPt = cfg.GraphicDiff.ClusterRadiusPt,
                    MinClusterLengthPt = cfg.GraphicDiff.MinClusterLengthPt,
                    MinClusterRuns = cfg.GraphicDiff.MinClusterRuns,
                });
            var byDir = r.Runs.GroupBy(x => x.Dir).ToDictionary(g => g.Key, g => g.Count());
            L($"[vec] page {p}:{q}: runs={r.Runs.Count} " +
              string.Join(" ", byDir.Select(kv => $"{kv.Key}={kv.Value}")) +
              $"  clusters={r.Clusters.Count}");
            foreach (var run in r.Runs.OrderByDescending(x => x.Length).Take(5))
                L($"[vec]    {run.Dir,-8} len={run.Length:F1} pts={run.Pts.Count / 2}");
        }
        return 0;
    }

    /// <summary>--json 既可是路径，也可是名称（在软件目录 / 子目录 / 当前目录查找 &lt;name&gt;.json）。</summary>
    static string ResolveJsonArgument(string? arg)
    {
        if (string.IsNullOrWhiteSpace(arg)) return null;
        if (arg!.IndexOfAny(new[] { '/', '\\', ':' }) >= 0 || File.Exists(arg)) return arg;
        var dir = AppContext.BaseDirectory ?? ".";
        foreach (var cand in new[]
        {
            Path.Combine(dir, arg + ".json"),
            Path.Combine(dir, "json", arg + ".json"),
            Path.Combine(Directory.GetCurrentDirectory(), arg + ".json"),
        })
            if (File.Exists(cand)) return cand;
        return arg;
    }

    // 把用户传入的方向归一化为合法值；非法值回退 "down"。
    static string NormalizeDirection(string? d)
    {
        if (string.IsNullOrWhiteSpace(d)) return "down";
        var v = d.Trim().ToLowerInvariant();
        return v switch
        {
            "up" or "above" or "top" => "up",
            "right" or "r" => "right",
            "left" or "l" => "left",
            _ => "down"
        };
    }

    // 标记形式归一化：semi(默认) / solid / outline；非法值回退 semi。
    static string NormalizeMarkStyle(string? s)
    {
        if (string.IsNullOrWhiteSpace(s)) return "semi";
        return s.Trim().ToLowerInvariant() switch
        {
            "solid" or "fill" or "实心" => "solid",
            "outline" or "stroke" or "line" or "描边" => "outline",
            _ => "semi"
        };
    }

    static void PrintHelp()
    {
        Console.WriteLine("EGpdf2df v1.0.0 - 对比两份图纸 PDF，输出带红色差异标注的新 PDF");
        Console.WriteLine("EGpdf2df v1.0.0 - compare two drawing PDFs, emit a red-annotated diff PDF");
        Console.WriteLine("Copyright © 2026 HerryABU");
        Console.WriteLine();
        Console.WriteLine("用法 / Usage:");
        Console.WriteLine("  EGpdf2df [baseline.pdf] [target.pdf] [options]");
        Console.WriteLine("  EGpdf2df --json request.json");
        Console.WriteLine("  EGpdf2df --stdin      (read JSON contract from stdin)");
        Console.WriteLine();
        Console.WriteLine("选项 / Options:");
        Console.WriteLine("  --json FILE      从文件读取 JSON 契约 (read request JSON from file；可给路径或名称)");
        Console.WriteLine("  --stdin          从标准输入读取 JSON 契约 (read JSON from stdin)");
        Console.WriteLine("  -o, --output F   差异 PDF 输出路径 (diff PDF output path)");
        Console.WriteLine("  -p, --pages L    对比页码(0基,逗号分隔) (0-based pages, comma list)");
        Console.WriteLine("  --config FILE    差异检测配置 (pdf2df config JSON)");
        Console.WriteLine("  --text-added-color H    新增文字改色 #RRGGBB (default #FF0000 红)");
        Console.WriteLine("  --text-removed-color H  删除文字改色 #RRGGBB (default #0000FF 蓝)");
        Console.WriteLine("  --text-insert-direction D  变更时旧(蓝)文字插入方向 down/up/right/left (default down)");
        Console.WriteLine("  --mark-style M       图形标记形式 semi(半透明填充+深色描边,默认)/solid(实心填充)/outline(仅描边)");
        Console.WriteLine("  --mark-stroke-color H     图形标记描边色 #RRGGBB (default 自动取标记色深色版)");
        Console.WriteLine("  --graphic-method M   图形检测路径 vector(默认,纯矢量)/pixel(渲染+OpenCV 回退)");
        Console.WriteLine("  --vector-tol PT      矢量比对容差(pt, default 3.0)：两版同名线画允许的位置偏差");
        Console.WriteLine("  --report-mode M      图形差异报告粒度 cluster(按区域汇总,默认)/run(逐条折线)");
        Console.WriteLine("  --locate FILE    定位模式：在两份 PDF 指定页查找字符串坐标 (--json 可给路径或名称)");
        Console.WriteLine("  -h, --help       显示帮助 (show help)");
        Console.WriteLine();
        Console.WriteLine("JSON 契约示例 / Example contract (stdin or --json):");
        Console.WriteLine("  { \"baseline\":\"old.pdf\", \"target\":\"new.pdf\", \"pages\":[0,1],");
        Console.WriteLine("    \"options\": { \"renderDpi\":200, \"graphicDiff\":true, \"textDiff\":true } }");
        Console.WriteLine();
        Console.WriteLine("定位示例 / Locate example:");
        Console.WriteLine("  { \"baseline\":\"old.pdf\", \"target\":\"new.pdf\", \"page\":0, \"text\":\"TOTAL\" }");
    }
}
