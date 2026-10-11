// 内嵌调用契约（EGpdf2excel 内核）。
// ⚠️ 类型名/方法名/签名必须与 EGpdf2df、EGexcel2df 的 EmbeddedEntry **完全一致**（见那边注释）。
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using EGtools.Core;
using EGpdf2excel.Engine;

namespace EGtools
{
    /// <summary>原生宿主（Qt）通过 hostfxr 调用的入口。</summary>
    public static class EmbeddedEntry
    {
        [UnmanagedCallersOnly]
        public static int Run(IntPtr argvPtr, int argc, IntPtr jsonUtf8, IntPtr outBuf, int outCap)
            => EmbeddedKernel.Serve(argvPtr, argc, jsonUtf8, outBuf, outCap);

        [UnmanagedCallersOnly]
        public static void SetLogSink(IntPtr fn, IntPtr ctx)
            => EmbeddedKernel.SetLogSink(fn, ctx);

        /// <summary>
        /// 健康检查：走 hostfxr 的**默认签名**（ComponentEntryPoint: int(IntPtr args, int sizeBytes)）路径。
        /// 宿主在真正调用前可先探一次，用来区分"运行时/程序集/类型 加载失败"与
        /// "[UnmanagedCallersOnly] 函数指针取不到"——两者报错很像，分层探一次就能定位。
        /// </summary>
        public static int Ping(IntPtr args, int sizeBytes) => unchecked((int)0x4547544Fu); // "EGTO"
    }

    internal static class EmbeddedBootstrap
    {
        [ModuleInitializer]
        internal static void Init()
            => EmbeddedKernel.Handler = (argv, json) =>
                   EmbeddedRunner.RunCaptured(
                       (a, log) => PdfExtractor.Run(a, log),
                       argv ?? Array.Empty<string>(), json, supportsStdin: true);
    }
}
