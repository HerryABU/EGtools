// 内嵌调用（in-process embedding）：让原生宿主（Qt/C++ 前端）不必 spawn 子进程，
// 直接把内核 DLL 载入**本进程**调用。
//
// 为什么用 hostfxr 而不是 NativeAOT 导出：
//   本内核依赖 iText（大量反射）、BouncyCastle、MuPDFCore/OpenCvSharp（原生库），
//   NativeAOT 对这类依赖不友好（反射裁剪、动态范型、AOT 不支持的 API）；
//   而 hostfxr 是 .NET 官方托管 API，**直接吃我们已经在发布的同一份 DLL**，
//   不需要改引擎、不需要新构建产物 ⇒ 命令行与内嵌两条路永远行为一致。
//
// 契约（原生侧看到的）：
//   int  eg_run(const char* const* argv, int argc, const char* jsonUtf8, char* outBuf, int outCap);
//   void eg_set_log_sink(void (*fn)(void* ctx, const char* utf8), void* ctx);
//   • argv/argc  —— 与命令行**完全一样**的参数（不含 exe 名）。纯参数用法（如 --render）也能走内嵌。
//   • jsonUtf8   —— 可选：直接把 JSON 契约传进来，宿主无需落临时文件（内核支持 --stdin 时走 stdin，
//                   否则由本层落临时文件并补 --json）。
//   • outBuf/outCap —— 结果（stdout）；两段式协议见 Serve 的注释。
//   • 日志（stderr）通过 log sink 实时回调给宿主，宿主可显示进度。
using System;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;

namespace EGtools.Core
{
    /// <summary>
    /// 内嵌调用的宿主侧运行时。各内核在自己的程序集里放一个同名同签名的
    /// <c>EGtools.EmbeddedEntry</c>（[UnmanagedCallersOnly]），把调用转发到这里；
    /// Qt 侧因此可以用同一段 hostfxr 代码加载任意一个内核。
    /// </summary>
    public static unsafe class EmbeddedKernel
    {
        /// <summary>处理器：argv + 可选 JSON 请求 → 结果字符串（stdout）。由各内核注册。</summary>
        public static Func<string[]?, string?, string>? Handler;

        static Action<string>? _managedLog;
        static delegate* unmanaged[Cdecl]<IntPtr, IntPtr, void> _nativeLog;
        static IntPtr _nativeLogCtx;

        /// <summary>注册托管日志接收器（单元测试/诊断用；与原生 sink 可并存）。</summary>
        public static void SetManagedLogSink(Action<string>? sink) => _managedLog = sink;

        /// <summary>原生宿主注册日志回调：<c>void (*)(void* ctx, const char* utf8Line)</c>。</summary>
        public static void SetLogSink(IntPtr fn, IntPtr ctx)
        {
            _nativeLog = fn == IntPtr.Zero
                ? null
                : (delegate* unmanaged[Cdecl]<IntPtr, IntPtr, void>)fn;
            _nativeLogCtx = ctx;
        }

        /// <summary>转发一行日志到宿主（托管 sink + 原生 sink 都发）。</summary>
        public static void Log(string? line)
        {
            if (line == null) return;
            try { _managedLog?.Invoke(line); } catch { /* 宿主 sink 异常不能影响内核 */ }
            if (_nativeLog == null) return;
            IntPtr p = IntPtr.Zero;
            try
            {
                p = Marshal.StringToCoTaskMemUTF8(line);
                _nativeLog(_nativeLogCtx, p);
            }
            catch { /* 同上 */ }
            finally { if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p); }
        }

        /// <summary>
        /// 统一内嵌入口（各内核的 EmbeddedEntry 转发到这里）。原生侧用
        /// <c>load_assembly_and_get_function_pointer</c> + <c>UNMANAGEDCALLERSONLY_METHOD</c> 取地址调用。
        ///
        /// 缓冲区协议（两段式，避免跨边界内存所有权问题）：
        ///   返回 need ≥ 0 → 结果 UTF-8 字节数（**不含**结尾 NUL）；若 <c>need &gt;= outCap</c> 说明被截断，
        ///                   宿主应把 outCap 放大到 need+1 再调一次。
        ///   返回 need &lt; 0 → 出错；错误文本长度 = <c>-need - 1</c>（同样可能被截断，按上面重试即可）。
        /// 无论成败都会写 NUL 结尾（只要 outCap &gt; 0），宿主可直接当 C 字符串看。
        /// </summary>
        public static int Serve(IntPtr argvPtr, int argc, IntPtr jsonUtf8, IntPtr outBuf, int outCap)
        {
            string payload;
            bool isError = false;
            try
            {
                var h = Handler ?? throw new InvalidOperationException(
                    "embedded handler not registered — 内核程序集的模块初始化器没有跑（请确认加载的是 CLI 内核 DLL）");

                string[] argv = Array.Empty<string>();
                if (argvPtr != IntPtr.Zero && argc > 0)
                {
                    argv = new string[argc];
                    var slots = (IntPtr*)argvPtr;
                    for (int i = 0; i < argc; i++)
                        argv[i] = slots[i] == IntPtr.Zero ? "" : (Marshal.PtrToStringUTF8(slots[i]) ?? "");
                }
                string? json = jsonUtf8 == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(jsonUtf8);

                payload = h(argv, json) ?? "";
            }
            catch (Exception ex)
            {
                payload = ex.ToString();
                isError = true;
            }

            byte[] bytes = Encoding.UTF8.GetBytes(payload);
            int need = bytes.Length;
            int room = Math.Max(0, outCap - 1);           // 预留结尾 NUL
            int n = Math.Min(need, room);
            if (outBuf != IntPtr.Zero)
            {
                if (n > 0) Marshal.Copy(bytes, 0, outBuf, n);
                Marshal.WriteByte(outBuf, n, 0);
            }
            return isError ? -need - 1 : need;
        }
    }

    /// <summary>
    /// 共享的"跑一次内核调用并捕获输出"实现：把 stdout/stderr 换到内存里再取回，
    /// 于是内核里的 <c>Console.WriteLine(json)</c> 与 <c>Console.Error</c> 日志都能被拿住 ——
    /// **内核业务逻辑一行都不用改**，命令行与内嵌两条路自然一致。
    /// </summary>
    public static class EmbeddedRunner
    {
        /// <param name="run">内核入口，签名同 <c>Program.Run(args, log)</c>。</param>
        /// <param name="argv">命令行参数（不含 exe 名）。</param>
        /// <param name="jsonInline">可选的内联 JSON 契约。</param>
        /// <param name="supportsStdin">该内核是否支持 <c>--stdin</c>；不支持时内联 JSON 落临时文件走 <c>--json</c>。</param>
        public static string RunCaptured(
            Func<string[], Action<string>?, int> run,
            string[] argv,
            string? jsonInline,
            bool supportsStdin)
        {
            string? tmp = null;
            TextWriter realOut = Console.Out, realErr = Console.Error;
            TextReader realIn = Console.In;
            var so = new StringWriter();
            var se = new ForwardingWriter(line => EmbeddedKernel.Log(line));
            try
            {
                Console.SetOut(TextWriter.Synchronized(so));
                Console.SetError(TextWriter.Synchronized(se));

                var args = argv ?? Array.Empty<string>();
                if (jsonInline != null)
                {
                    if (supportsStdin)
                    {
                        Console.SetIn(new StringReader(jsonInline));
                    }
                    else
                    {
                        tmp = Path.Combine(Path.GetTempPath(),
                                           "egtools_embed_" + Guid.NewGuid().ToString("N") + ".json");
                        File.WriteAllText(tmp, jsonInline, new UTF8Encoding(false));
                        var list = new System.Collections.Generic.List<string>(args) { "--json", tmp };
                        args = list.ToArray();
                    }
                }

                int rc = run(args, EmbeddedKernel.Log);
                if (rc != 0)
                    throw new InvalidOperationException(
                        $"kernel exit code {rc}; stderr:\n{se}".TrimEnd());
                return so.ToString();
            }
            finally
            {
                Console.SetOut(realOut);
                Console.SetError(realErr);
                Console.SetIn(realIn);
                if (tmp != null) { try { File.Delete(tmp); } catch { } }
            }
        }
    }
}
