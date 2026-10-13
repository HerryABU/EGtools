#pragma once

#include <QString>
#include <QStringList>
#include <functional>

// 内核调用融合层：把「调用库方法（进程内）」与「拼命令行（子进程）」两种方式统一到
// 同一个入口，宿主代码只描述"要哪个内核 + 参数 / JSON 请求"，由本层决定怎么送进去。
//
//   后端 Embedded —— 用 hostfxr（.NET 官方托管 API）把内核 DLL **载入本进程**直接调用：
//                    无子进程、无控制台窗口、启动快、日志可实时回调。
//   后端 Cli      —— 沿用 QProcess 起 {kernel}.exe（老路，兼容性最好，能隔离崩溃）。
//   后端 Auto     —— 先试 Embedded，不可用（缺 hostfxr / 缺 DLL / 初始化失败）就回退 Cli。
//
// ⚠️ 两条路**吃的是同一份内核 DLL 与同一套 JSON 契约**，所以不需要为内嵌另建产物，
//    行为也天然一致（内嵌只是把同一段 argv 交给同一段内核代码）。
//
// 关于依赖：本层**只用运行时动态加载**（QLibrary 找 hostfxr.dll），不引入链接期的
// nethost.lib / hostfxr.lib —— 部署时不必附带任何 .NET 开发期产物，只需目标机装了 .NET 运行时。
class KernelClient
{
public:
    enum class Backend { Auto, Embedded, Cli };

    struct Result
    {
        int     rc = -1;        // 0 = 成功（与命令行退出码同义）
        QString out;            // 内核 stdout（JSON 结果）
        QString err;            // 内核 stderr / 错误信息
        QString backend;        // 实际使用的后端："embedded" / "cli"
    };

    // ---- 全局默认后端与内核目录（由 MainWindow 从设置里下发）----
    static void setDefaultBackend(Backend b);
    static Backend defaultBackend();
    static void setCliDir(const QString& dir);
    static QString cliDir();

    static Backend backendFromString(const QString& s);
    static QString backendToString(Backend b);

    // 统一入口。
    //   kernel      —— "EGpdf2df" / "EGpdf2excel" / "EGexcel2df"
    //   args        —— 命令行参数（与过去完全一样）
    //   jsonRequest —— 可选：JSON 契约内容。非空时内嵌走 stdin（不落盘），命令行落临时文件
    //                  并补 --json。两者结果一致（内核里是同一段解析代码）。
    static Result run(const QString& kernel, const QStringList& args,
                      const QString& jsonRequest = QString(),
                      Backend backend = Backend::Auto, int timeoutMs = 600000);

    // 两个后端各自的实现（测试/显式选择用）。
    static Result runEmbedded(const QString& kernel, const QStringList& args,
                             const QString& jsonRequest);
    static Result runCli(const QString& kernel, const QStringList& args,
                         const QString& jsonRequest, int timeoutMs = 600000);

    // 内嵌后端是否可用；why 返回原因（供 UI 提示）。
    static bool embeddedAvailable(const QString& kernel, QString* why = nullptr);
    // 一行状态描述（供首页/状态栏显示）；kernel 省略则探测 EGpdf2df。
    static QString describe(const QString& kernel = QStringLiteral("EGpdf2df"));

    // 内核器目录探测（.dll 与 .exe 同目录）。
    static QString kernelDir(const QString& kernel);
    // 定位 hostfxr 库（不依赖 nethost；见 .cpp 里的搜索顺序说明）。
    static QString findHostFxr();

    // ---- 平台差异（移植 Linux/macOS 时唯二要注意的地方）------------------
    // hostfxr 库名按平台：hostfxr.dll / libhostfxr.so / libhostfxr.dylib。
    static QString hostFxrFileName();
    // 可执行文件后缀：Windows ".exe"，其他平台空串。
    // ⚠️ 托管程序集始终是 ".dll"（Linux 上也是），只有**可执行文件**没有后缀。
    static QString exeSuffix();
    // 内核 CLI 可执行文件名（含平台后缀），如 Windows "EGpdf2df.exe" / Linux "EGpdf2df"。
    static QString cliFileName(const QString& kernel);

    // 定位内核文件：kernel + ext（".exe" / ".dll" / ".runtimeconfig.json"）。
    static QString findKernelFile(const QString& kernel, const QString& ext);
    // 候选项能否当可执行文件用（必须是**可执行的文件**，不能是同名目录）。
    // ⚠️ Linux 上 `<app>/EGpdf2excel` 既是目录又是可执行文件名 ⇒ 只判 exists() 会误命中目录。
    static bool isUsableKernelExe(const QString& path);

    // 可选：把内核日志**实时**转出去（进度显示用）。传空函数清除。
    static void setLiveLogSink(std::function<void(const QString&)> sink);
};
