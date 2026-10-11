// EGpdf2df — command-line kernel that compares two drawing PDFs (baseline vs
// target) and emits a red-annotated difference PDF plus a JSON summary. Fully
// decoupled from any UI: it reads a JSON contract from a file or stdin and writes
// the JSON result to stdout.
using EGpdf2df.Engine;
using EGtools.Core;
using MuPDFCore;
using MuPDFCore.StructuredText;
using System;
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
        bool readStdin = false;
        bool locateMode = false;
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
                case "-o": case "--output": output = take(""); break;
                case "-p": case "--pages": pagesArg = take(""); break;
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
            if (!string.IsNullOrWhiteSpace(c.Circle)) cfg.Annotation.CircleColor = c.Circle;
            if (!string.IsNullOrWhiteSpace(c.Text)) cfg.Annotation.TextColor = c.Text;
        }
        var o = req?.Options;
        if (o?.LineWidth != null) cfg.Annotation.LineWidth = (int)Math.Max(0.5, o.LineWidth.Value);
        if (o?.FillOpacity != null) cfg.Annotation.FillOpacity = Math.Min(1.0, Math.Max(0.0, o.FillOpacity.Value));
        if (o?.Outline != null) cfg.Annotation.Outline = o.Outline.Value;
        if (o?.HaloPx != null) cfg.Annotation.HaloPx = Math.Max(0, o.HaloPx.Value);
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
