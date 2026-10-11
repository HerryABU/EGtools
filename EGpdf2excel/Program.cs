// EGpdf2excel — command-line front-end for the PDF -> Excel extraction engine.
// All extraction logic lives in EGpdf2excel.Engine.PdfExtractor; this wrapper only
// forwards argv and the process exit code, keeping the code-base DRY and the
// CLI independently runnable (also bundled inside the WINUI installer).
using System;
using System.Linq;
using EGpdf2excel.Engine;

namespace EGpdf2excel;

public static class Program
{
    public static int Main(string[] args)
    {
        // --embed-probe：用「内嵌调用」的同一段捕获实现跑一次，并把捕获到的 stdout
        // 原样打印出来。用途：当原生宿主（Qt）走内嵌通道拿不到内核 stdout 时，
        // 用它一步区分【托管侧捕获有问题】还是【原生侧取回有问题】——
        // 前者会打印 length=0，后者会打印正确的 JSON。
        if (args.Length > 0 && args[0] == "--embed-probe")
        {
            // ① 最小用例：RunCaptured 里直接 Console.WriteLine，验证捕获机制本身。
            var basic = EGtools.Core.EmbeddedRunner.RunCaptured(
                (a, log) => { Console.WriteLine("{\"hello\":\"world\"}"); return 0; },
                Array.Empty<string>(), null, supportsStdin: true);
            Console.Error.WriteLine($"[probe] basic capture length = {basic.Length}  value=<<<{basic}>>>");

            // ② 真实用例：渲染模式。
            var inner = args.Skip(1).ToArray();
            var captured = EGtools.Core.EmbeddedRunner.RunCaptured(
                (a, log) => PdfExtractor.Run(a, log), inner, null, supportsStdin: true);
            Console.Error.WriteLine($"[probe] render capture length = {captured.Length}");
            Console.Error.WriteLine($"[probe] render capture value = <<<{captured}>>>");
            return 0;
        }
        return PdfExtractor.Run(args);
    }
}
