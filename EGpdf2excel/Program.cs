// EGpdf2excel — command-line front-end for the PDF -> Excel extraction engine.
// All extraction logic lives in EGpdf2excel.Engine.PdfExtractor; this wrapper only
// forwards argv and the process exit code, keeping the code-base DRY and the
// CLI independently runnable (also bundled inside the WINUI installer).
using EGpdf2excel.Engine;

namespace EGpdf2excel;

public static class Program
{
    public static int Main(string[] args)
    {
        return PdfExtractor.Run(args);
    }
}
