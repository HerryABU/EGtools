using Microsoft.UI.Xaml;
using Microsoft.Windows.ApplicationModel.DynamicDependency;
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using WinRT;

namespace EGtools.Gui;

/// <summary>
/// Explicit entry point for the self-contained (embedded-runtime) WinUI 3 app.
///
/// The SDK's auto-initializer (MddBootstrapAutoInitializer, a module initializer)
/// calls <c>Environment.Exit(hr)</c> on bootstrap failure — a silent,
/// undiagnosable exit that left the GUI "unstartable" with no log or message.
/// We strip that initializer (see csproj &lt;Compile Remove&gt;) and call
/// Bootstrap.TryInitialize() ourselves, capture the HRESULT, and surface any
/// failure in a readable way (boot log + MessageBox) instead of dying silently.
/// </summary>
public static class Program
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int MessageBoxW(IntPtr hWnd, string text, string caption, uint type);

    [STAThread]
    public static void Main(string[] args)
    {
        // Catch anything that escapes our try/catch below (incl. native crashes
        // during window/CLR teardown) so the user gets a readable message.
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            Fatal("AppDomain.UnhandledException", e.ExceptionObject as Exception);

        Boot("Program.Main start");

        // ---- Explicitly initialize the self-contained (embedded) Windows App
        //      Runtime. This IS the "运行时嵌入" step. ----
        try
        {
            uint majorMinorVersion = Microsoft.WindowsAppSDK.Release.MajorMinor;
            string versionTag = Microsoft.WindowsAppSDK.Release.VersionTag;
            var minVersion = new PackageVersion(Microsoft.WindowsAppSDK.Runtime.Version.UInt64);
            // None (not OnNoMatch_ShowUI): we ship the runtime locally, so a
            // "no match" is a real error, not a hint to install an external one.
            var options = Bootstrap.InitializeOptions.None;
            if (!Bootstrap.TryInitialize(majorMinorVersion, versionTag, minVersion, options, out int hr))
            {
                Fatal($"Windows App Runtime 初始化失败 / bootstrap failed (hr=0x{hr:X8})", null);
                return;
            }
            Boot($"Bootstrap.TryInitialize OK (hr=0x{hr:X8})");
            // Record which Windows App Runtime / WinUI 3 framework will actually
            // be loaded. A native version mismatch here (vs the compile-time
            // metadata) is the classic cause of a silent 0xC000027B assert during
            // App.InitializeComponent — which bypasses every managed handler.
            LogRuntimeInfo();
        }
        catch (Exception ex)
        {
            Fatal("Bootstrap.TryInitialize threw", ex);
            return;
        }

        // ---- Standard WinUI 3 unpackaged startup (mirrors the SDK-generated Main). ----
        try
        {
            ComWrappersSupport.InitializeComWrappers();
            Application.Start((p) =>
            {
                var context = new Microsoft.UI.Dispatching.DispatcherQueueSynchronizationContext(
                    Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread());
                System.Threading.SynchronizationContext.SetSynchronizationContext(context);
                Boot("Application.Start -> new App()");
                new App();
            });
        }
        catch (Exception ex)
        {
            Fatal("Application.Start failed", ex);
        }
    }

    // Lightweight boot-trace: records how far startup got, even for native /
    // bootstrap failures that never raise a managed exception. Read
    // %TEMP%/EGtools_boot.log on the target machine to see the last milestone.
    public static void Boot(string step)
    {
        try
        {
            File.AppendAllText(
                Path.Combine(Path.GetTempPath(), "EGtools_boot.log"),
                $"{DateTime.Now:HH:mm:ss.fff}  {step}{Environment.NewLine}");
        }
        catch { }
    }

    // Record the framework bits that will be loaded on THIS machine. Appended to
    // %TEMP%/EGtools_boot.log. Even if the subsequent XAML load asserts natively
    // (0xC000027B, silent), this line is already on disk and tells us exactly
    // which Microsoft.ui.xaml.dll / WindowsAppRuntime the process is using.
    public static void LogRuntimeInfo()
    {
        try
        {
            var sb = new StringBuilder();
            sb.AppendLine("---- runtime info ----");
            sb.AppendLine($"Is64BitProcess : {Environment.Is64BitProcess}");
            sb.AppendLine($"Exe            : {Environment.ProcessPath}");
            sb.AppendLine($"BaseDirectory   : {AppContext.BaseDirectory}");
            try
            {
                using var key = Microsoft.Win32.Registry.LocalMachine
                    .OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
                var build = key?.GetValue("CurrentBuild")?.ToString();
                var ubr = key?.GetValue("UBR")?.ToString();
                sb.AppendLine($"OS             : {Environment.OSVersion} build={build}.{ubr}");
            }
            catch { sb.AppendLine("OS registry read failed"); }

            // The framework DLL sitting next to the EXE is what will be loaded.
            foreach (var name in new[] { "Microsoft.ui.xaml.dll", "Microsoft.UI.Xaml.Controls.dll", "Microsoft.WindowsAppRuntime.dll", "WindowsAppRuntime.dll" })
            {
                var p = Path.Combine(AppContext.BaseDirectory, name);
                if (File.Exists(p))
                    sb.AppendLine($"local {name} v={FileVersionInfo.GetVersionInfo(p).FileVersion}");
            }

            // Any already-loaded module that matches — reveals if a SYSTEM copy
            // (not the local one) is being used, which would explain a mismatch.
            try
            {
                foreach (ProcessModule m in Process.GetCurrentProcess().Modules)
                {
                    if (m.ModuleName.IndexOf("ui.xaml", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        m.ModuleName.IndexOf("windowsappruntime", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        m.ModuleName.IndexOf("windowsappsdk", StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        try { sb.AppendLine($"LOADED {m.ModuleName} v={m.FileVersionInfo.FileVersion} <- {m.FileName}"); }
                        catch { sb.AppendLine($"LOADED {m.ModuleName} <- {m.FileName}"); }
                    }
                }
            }
            catch { sb.AppendLine("(module enumeration failed)"); }
            sb.AppendLine("----------------------");
            File.AppendAllText(Path.Combine(Path.GetTempPath(), "EGtools_boot.log"), sb.ToString());
        }
        catch { }
    }

    // Write the failure to %TEMP%/EGtools_crash.log and pop a MessageBox so the
    // user can tell us exactly what failed instead of a silent exit.
    public static void Fatal(string where, Exception? ex)
    {
        var sb = new StringBuilder();
        sb.AppendLine("EGtools GUI 启动失败 / Failed to start.");
        sb.AppendLine($"位置 / Where : {where}");
        sb.AppendLine($"版本 / Version: {App.Version}");
        sb.AppendLine($"时间 / Time  : {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        sb.AppendLine();
        Exception? cur = ex;
        int depth = 0;
        while (cur != null && depth < 8)
        {
            sb.AppendLine($"[{depth}] {cur.GetType().FullName}: {cur.Message}");
            sb.AppendLine(cur.StackTrace ?? "(no stack trace)");
            sb.AppendLine("----");
            cur = cur.InnerException;
            depth++;
        }
        if (ex == null)
            sb.AppendLine("(no managed exception — native/bootstrap failure; see hr above)");

        string logPath = Path.Combine(Path.GetTempPath(), "EGtools_crash.log");
        try { File.WriteAllText(logPath, sb.ToString()); } catch { }

        string msg = $"EGtools 启动失败 / Failed to start.\n\n" +
                     $"位置 / Where: {where}\n" +
                     $"{(ex?.GetType().FullName ?? "n/a")}: {ex?.Message}\n\n" +
                     $"崩溃日志已写入 / Crash log:\n{logPath}";
        try { MessageBoxW(IntPtr.Zero, msg, "EGtools", 0x10 /* MB_ICONERROR */); } catch { }
    }
}
