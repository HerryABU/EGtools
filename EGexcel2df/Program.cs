// EGexcel2df — command-line front-end for the Excel drawing-change comparison
// engine. All comparison logic lives in EGtools.Core.ExcelTools; this wrapper
// only forwards argv and the process exit code.
using EGtools.Core;

namespace EGexcel2df;

public static class Program
{
    public static int Main(string[] args)
    {
        return ExcelTools.Run(args);
    }
}
