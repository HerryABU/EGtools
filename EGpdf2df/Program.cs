// EGpdf2df — command-line kernel that compares two drawing PDFs (baseline vs
// target) and emits a red-annotated difference PDF plus a JSON summary. Fully
// decoupled from any UI: it reads a JSON contract from a file or stdin and writes
// the JSON result to stdout.
using EGpdf2df.Engine;
using System;
using System.IO;
using System.Linq;
using System.Text.Json;

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
                case "-o": case "--output": output = take(""); break;
                case "-p": case "--pages": pagesArg = take(""); break;
                default:
                    if (a.StartsWith("-")) { L($"Unknown option: {a} (use --help)"); return 2; }
                    if (baseline == null) baseline = a; else if (target == null) target = a; else L($"Ignoring extra argument: {a}");
                    break;
            }
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
        return DiffEngine.Run(req, cfg, L);
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
        Console.WriteLine("  --json FILE      从文件读取 JSON 契约 (read request JSON from file)");
        Console.WriteLine("  --stdin          从标准输入读取 JSON 契约 (read JSON from stdin)");
        Console.WriteLine("  -o, --output F   差异 PDF 输出路径 (diff PDF output path)");
        Console.WriteLine("  -p, --pages L    对比页码(0基,逗号分隔) (0-based pages, comma list)");
        Console.WriteLine("  --config FILE    差异检测配置 (pdf2df config JSON)");
        Console.WriteLine("  -h, --help       显示帮助 (show help)");
        Console.WriteLine();
        Console.WriteLine("JSON 契约示例 / Example contract (stdin or --json):");
        Console.WriteLine("  { \"baseline\":\"old.pdf\", \"target\":\"new.pdf\", \"pages\":[0,1],");
        Console.WriteLine("    \"options\": { \"renderDpi\":200, \"graphicDiff\":true, \"textDiff\":true } }");
    }
}
