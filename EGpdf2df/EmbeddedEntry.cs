// 内嵌调用契约（EGpdf2df 内核）。
//
// ⚠️ 三个内核（EGpdf2df / EGpdf2excel / EGexcel2df）**类型名、方法名、签名必须完全一致**
//    （namespace EGtools / class EmbeddedEntry / Run / SetLogSink），
//    因为 Qt 侧用同一段 hostfxr 代码按"程序集 + 类型 + 方法"取函数指针 —— 统一契约让
//    "换一个内核"只是换一个 DLL 路径，宿主代码零改动。
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using EGtools.Core;

namespace EGtools
{
    /// <summary>原生宿主（Qt）通过 hostfxr 调用的入口。签名见 EGtools.Core.EmbeddedKernel.Serve。</summary>
    public static class EmbeddedEntry
    {
        /// <summary>eg_run(argv, argc, jsonUtf8, outBuf, outCap) → 结果长度 / 负值=错误。</summary>
        [UnmanagedCallersOnly]
        public static int Run(IntPtr argvPtr, int argc, IntPtr jsonUtf8, IntPtr outBuf, int outCap)
            => EmbeddedKernel.Serve(argvPtr, argc, jsonUtf8, outBuf, outCap);

        /// <summary>eg_set_log_sink(void(*)(void*, const char*), void*) —— 让内核日志实时回传宿主。</summary>
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

    /// <summary>程序集一被加载就把内核入口接到内嵌运行时上（无需先跑 Main）。</summary>
    internal static class EmbeddedBootstrap
    {
        [ModuleInitializer]
        internal static void Init()
            => EmbeddedKernel.Handler = (argv, json) =>
                   EmbeddedRunner.RunCaptured(
                       (a, log) => EGpdf2df.Program.Run(a, log),
                       argv ?? Array.Empty<string>(), json, supportsStdin: true);
    }
}
