#include "kernelclient.h"
#include "clirunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// hostfxr 最小 ABI 声明
// ---------------------------------------------------------------------------
// 官方定义在 hostfxr.h / coreclr_delegates.h（随 .NET SDK 一起装，见
// <dotnet>/packs/Microsoft.NETCore.App.Host.win-x64/<ver>/runtimes/<rid>/native/）。
// 这里只**只在运行时**用 QLibrary 解析 hostfxr.dll 的导出，故不需要任何链接期依赖，
// 也就不需要在 CMake 里写死一个带版本号的 SDK 路径（那会在换 SDK 时烂掉）。
// 结构体布局按官方头文件（hostfxr_initialize_parameters）原样声明。
namespace hfx
{
    typedef void* handle_t;

    // ⚠️ Windows 上 hostfxr 的字符串类型 char_t 是 **wchar_t（UTF-16）**，不是 UTF-8！
    // 官方 hostfxr.h 里 typedef char char_t / wchar_t char_t 就是按平台切的。
    // 踩过的坑：按 UTF-8 传路径时 hostfxr 报 "runtimeconfig.json [...] does not exist"，
    // 而且错误信息里路径显示成乱码（正是 UTF-16 字节被当单字节读），见 kr=0x80008093。
#if defined(_WIN32)
    typedef wchar_t char_t;
#else
    typedef char char_t;
#endif

    struct initialize_parameters
    {
        size_t        size;
        const char_t* host_path;
        const char_t* dotnet_root;
    };

    // hostfxr_delegate_type 枚举里我们需要的几个值。
    enum
    {
        hdt_load_assembly_and_get_function_pointer = 5,
        hdt_get_function_pointer                   = 6,
        hdt_load_assembly                          = 7
    };

    // 取函数指针时的特殊"委托类型名"：官方 macro UNMANAGEDCALLERSONLY_METHOD = (const char_t*)-1
    // 表示"目标方法是 [UnmanagedCallersOnly]，按原生调用约定直接调"——不生成托管委托包装。
    inline const char_t* unmanaged_callers_only_method()
    {
        return reinterpret_cast<const char_t*>(-1);
    }

    typedef int  (*initialize_for_runtime_config_fn)(const char_t* runtime_config_path,
                                                     const initialize_parameters* parameters,
                                                     handle_t* host_context_handle);
    typedef int  (*set_runtime_property_value_fn)(handle_t host_context_handle,
                                                  const char_t* name, const char_t* value);
    typedef int  (*get_runtime_delegate_fn)(handle_t host_context_handle, int type, void** delegate);
    typedef int  (*close_fn)(handle_t host_context_handle);

    typedef int  (*load_assembly_and_get_function_pointer_fn)(const char_t* assembly_path,
                                                              const char_t* type_name,
                                                              const char_t* method_name,
                                                              const char_t* delegate_type_name,
                                                              void* reserved,
                                                              void** delegate);

    // ---- 备用加载路径（见 KernelClient 里"两条托管加载路径"的说明）----
    // ⚠️ 签名要与官方 coreclr_delegates.h 完全一致，否则参数错位会返回
    //    0x80131502（COR_E_ARGUMENTOUTOFRANGE）这类莫名其妙的码：
    //      load_assembly_fn(assembly_path, load_context=0, reserved=0)
    //      get_function_pointer_fn(type, method, delegate_type, load_context=0, reserved=0, out_delegate)
    typedef int  (*load_assembly_fn)(const char_t* assembly_path, void* load_context, void* reserved);
    typedef int  (*get_function_pointer_fn)(const char_t* type_name,
                                            const char_t* method_name,
                                            const char_t* delegate_type_name,
                                            void* load_context,
                                            void* reserved,
                                            void** delegate);

    // QString → 平台字符串（Windows: UTF-16；其他平台：UTF-8）。
#if defined(_WIN32)
    inline std::wstring Native(const QString& s) { return s.toStdWString(); }
#else
    inline std::string Native(const QString& s) { return s.toStdString(); }
#endif
}

// 内核导出（三个内核同名同签名，见 .NET 侧 EGtools.EmbeddedEntry）
typedef int  (*eg_run_fn)(const char* const* argv, int argc,
                          const char* jsonUtf8, char* outBuf, int outCap);
typedef void (*eg_set_log_sink_fn)(void* fn, void* ctx);
typedef void (*eg_log_callback_fn)(void* ctx, const char* utf8Line);

namespace
{
    QMutex                 g_mutex;                 // 保护日志与 hostfxr 初始化
    QStringList            g_log;                   // 最近若干行内核日志（stderr 平替）
    std::function<void(const QString&)> g_liveSink;
    bool                   g_runtimeReady = false;
    QString                g_runtimeError;
    hfx::handle_t          g_ctx = nullptr;
    hfx::load_assembly_and_get_function_pointer_fn g_loadFn = nullptr;
    // 备用加载路径（hdt_load_assembly + hdt_get_function_pointer）。两条都留着：
    // 见 resolveKernel 里"两条托管加载路径"的说明。
    hfx::load_assembly_fn  g_asmLoadFn = nullptr;
    hfx::get_function_pointer_fn g_fnPtrFn = nullptr;
    hfx::close_fn          g_closeFn = nullptr;
    QLibrary*              g_hostfxrLib = nullptr;
    QString                g_hostfxrPath;

    const int kLogKeep = 200;

    // 内核日志桥：托管侧通过函数指针回调到这里（可能来自非 Qt 线程）。
    void eg_log_bridge(void* /*ctx*/, const char* utf8Line)
    {
        if (!utf8Line) return;
        const QString line = QString::fromUtf8(utf8Line);
        std::function<void(const QString&)> sink;
        {
            QMutexLocker lk(&g_mutex);
            g_log.append(line);
            while (g_log.size() > kLogKeep) g_log.removeFirst();
            sink = g_liveSink;
        }
        if (sink) sink(line);
    }
}

// ---------------------------------------------------------------------------
// 静态配置
// ---------------------------------------------------------------------------
static KernelClient::Backend g_backend = KernelClient::Backend::Auto;
static QString             g_cliDir;

void KernelClient::setDefaultBackend(Backend b) { g_backend = b; }
KernelClient::Backend KernelClient::defaultBackend() { return g_backend; }
void KernelClient::setCliDir(const QString& dir) { g_cliDir = dir; }
QString KernelClient::cliDir() { return g_cliDir; }

KernelClient::Backend KernelClient::backendFromString(const QString& s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("cli") || v == QLatin1String("command")
        || v == QLatin1String("命令行") || v == QLatin1String("进程"))
        return Backend::Cli;
    if (v == QLatin1String("embedded") || v == QLatin1String("inproc")
        || v == QLatin1String("内嵌") || v == QLatin1String("库"))
        return Backend::Embedded;
    return Backend::Auto;
}

QString KernelClient::backendToString(Backend b)
{
    switch (b)
    {
    case Backend::Cli:      return QStringLiteral("cli");
    case Backend::Embedded: return QStringLiteral("embedded");
    default:                return QStringLiteral("auto");
    }
}

void KernelClient::setLiveLogSink(std::function<void(const QString&)> sink)
{
    QMutexLocker lk(&g_mutex);
    g_liveSink = std::move(sink);
}

// ---------------------------------------------------------------------------
// 文件定位
// ---------------------------------------------------------------------------
QString KernelClient::findKernelFile(const QString& kernel, const QString& ext)
{
    const QString name = kernel + ext;
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList dirs;
    auto add = [&](const QString& d) { if (!d.isEmpty() && !dirs.contains(d)) dirs << d; };
    // 候选项与 MainWindow::cliExe 保持一致，保证 exe / dll / runtimeconfig 三者在同一处被找到：
    // ① 用户显式指定（设置项 / EGTOOLS_CLI_DIR）② 与 Qt exe 同级 ③ DIST 布局 <app>/<name>/
    // ④ <app>/cli ⑤ 开发布局 ../cli ⑥ 父目录 / 兄弟目录
    add(g_cliDir);
    add(appDir);
    add(QDir(appDir).filePath(kernel));
    add(QDir(appDir).filePath(QStringLiteral("cli")));
    add(QDir(appDir).filePath(QStringLiteral("../cli")));
    add(QDir(appDir).filePath(QStringLiteral("..")));
    add(QDir(appDir).filePath(QStringLiteral("../") + kernel));
    // ⚠️ 找**可执行文件**时必须要求"是可执行文件"，不能只判存在 —— 见 isUsableKernelExe。
    //    ext == exeSuffix() 即"这次要的就是可执行文件"（Windows ".exe"；Linux 两边都是空串）。
    const bool needExec = (ext == exeSuffix());
    for (const QString& d : dirs)
    {
        const QString c = QDir(d).filePath(name);
        if (needExec ? isUsableKernelExe(c) : QFileInfo::exists(c)) return c;
    }
    return dirs.isEmpty() ? name : QDir(dirs.first()).filePath(name);
}

QString KernelClient::kernelDir(const QString& kernel)
{
    const QString dll = findKernelFile(kernel, QStringLiteral(".dll"));
    return QFileInfo(dll).absolutePath();
}

// hostfxr 的**文件名按平台**不同（这是移植到 Linux 最容易漏的一处）：
//   Windows: hostfxr.dll      Linux: libhostfxr.so      macOS: libhostfxr.dylib
QString KernelClient::hostFxrFileName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("hostfxr.dll");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("libhostfxr.dylib");
#else
    return QStringLiteral("libhostfxr.so");
#endif
}

// 可执行文件后缀：Windows 有 ".exe"，Linux/macOS 没有。
// ⚠️ 注意与托管程序集区分：`EGpdf2excel.dll` 在任何平台都带 .dll，
//    只有**可执行文件**（self-contained 发布出来的那个）在 Linux 上无后缀。
QString KernelClient::exeSuffix()
{
#if defined(Q_OS_WIN)
    return QStringLiteral(".exe");
#else
    return QString();
#endif
}

QString KernelClient::cliFileName(const QString& kernel)
{
    return kernel + exeSuffix();
}

// ⚠️⚠️ 判断"这个候选项能不能当可执行文件用"，**不能只判 `QFileInfo::exists()`**：
//    安装/DIST 布局是 `<appDir>/<name>/<name>`：目录名与 Linux 上的可执行文件名
//    **完全同形**（Linux 可执行文件没有 .exe 后缀）。于是 `exists("<appDir>/EGpdf2excel")`
//    命中的是那个**目录**，被当成程序返回 ⇒ 运行时才炸 `execvp: Permission denied`（退出码 126）。
//    Windows 因为候选名都带 .exe 后缀（目录不带），"恰好"绕过了这个坑 —— 所以这个 bug
//    只在 Linux 暴露，而且暴露得很晚（走到 spawn 才报）。
//    再顺手在 Unix 上要求**可执行位**：包里漏了 exec 位（install(DIRECTORY) 默认不保留权限）
//    也在这里就暴露，而不是等到用户点"开始"。
bool KernelClient::isUsableKernelExe(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.isFile()) return false;          // 目录、或指向目录的符号链接，一律不算
#if defined(Q_OS_UNIX)
    return fi.isExecutable();
#else
    return true;
#endif
}

// ---------------------------------------------------------------------------
// hostfxr 候选链（按**使用优先级**排序、已去重）
//
// 为什么不是"选一个最好的"而是返回一条链：
//   ① 随包分发的 hostfxr 是 **self-contained 版**（self-contained 发布把 hostfxr 放在内核
//      目录里）。它连机器上的共享框架都**不会去搜**（COREHOST_TRACE 里 "Gathering FX
//      locations" 次数为 0）⇒ 用它初始化我们那份 framework-dependent 的宿主配置**必然失败**
//      （实测 hr=0x80008096 / "Initialization for framework-dependent components is not
//      supported"）。所以它**绝不能优先**，只在机器上完全找不到 .NET 时兜底。
//   ② 机器上不同大版本的 hostfxr 能力不同：实测同一台机器、同一份配置，
//      **.NET 10 的 hostfxr 拒绝** framework-dependent 组件初始化（同样 hr=0x80008096），
//      而 **6.0.36 的能成功**（内嵌 rc=0、实时日志 127 行、两路结果一致）。
//      ⇒ 必须能**跳过**不能用的那个（由 initRuntime 逐个试），否则"机器上装了 .NET 10 就
//        连 6.x/8.x/9.x 一起用不上"，内嵌通道会被整条废掉。
//   ③ 目标机若根本没装 .NET：内嵌做不到，由命令行后端接管（设计内行为，见 run() 的 Auto 回退）。
// ---------------------------------------------------------------------------
QStringList KernelClient::hostFxrCandidates()
{
    const QString fxrName = hostFxrFileName();
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList out;
    auto add = [&](const QString& p) {
        if (!p.isEmpty() && !out.contains(p) && QFileInfo::exists(p)) out << p;
    };

    // ① 应用目录旁（随包分发时可放；测试里也用它钉住某个特定 hostfxr）。
    add(QDir(appDir).filePath(fxrName));

    // ② 机器上的 .NET 安装（每个根下按**版本降序**）——能服务 framework-dependent 组件的那类。
    QStringList roots;
    const QByteArray envRoot = qgetenv("DOTNET_ROOT");
    if (!envRoot.isEmpty()) roots << QString::fromLocal8Bit(envRoot);

#if defined(Q_OS_WIN)
    {
        QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\dotnet\\Setup\\InstalledVersions\\x64"),
                      QSettings::NativeFormat);
        const QString loc = reg.value(QStringLiteral("InstallLocation")).toString();
        if (!loc.isEmpty()) roots << loc;
    }
    roots << QStringLiteral("C:/Program Files/dotnet");
#else
    // Linux：发行版包把运行时放在 /usr/lib/dotnet（Debian/Ubuntu 的 dotnet-runtime-*），
    // 微软官方脚本装到 /usr/share/dotnet，snap 另一处；macOS 官方装到
    // /usr/local/share/dotnet。这些路径都可能只有 host/fxr 而没有 SDK。
    roots << QStringLiteral("/usr/share/dotnet")
          << QStringLiteral("/usr/lib/dotnet")
          << QStringLiteral("/usr/local/share/dotnet")
          << QStringLiteral("/opt/dotnet")
          << QStringLiteral("/snap/dotnet-sdk/current/dotnet");
    // 最后再从 `dotnet` 可执行文件反推安装根（最能适应自定义安装位置）。
    {
        const QString dotnetExe = QStandardPaths::findExecutable(QStringLiteral("dotnet"));
        if (!dotnetExe.isEmpty()) {
            // /usr/bin/dotnet 常是符号链接 → 解析到真实路径后再取上两级（bin/ 与根）。
            const QString real = QFileInfo(dotnetExe).canonicalFilePath().isEmpty()
                                     ? dotnetExe : QFileInfo(dotnetExe).canonicalFilePath();
            roots << QFileInfo(QFileInfo(real).absolutePath()).absolutePath();
        }
    }
#endif

    // 目录名即版本号；按段比排序（字符串比较会在 10 < 9 上翻车）。
    auto verParts = [](const QString& v) {
        QList<int> p; for (const QString& s : v.split(QLatin1Char('.'))) p << s.toInt();
        while (p.size() < 3) p << 0; return p;
    };
    for (const QString& root : roots)
    {
        if (root.isEmpty()) continue;
        const QString fxrRoot = QDir(root).filePath(QStringLiteral("host/fxr"));
        if (!QFileInfo::exists(fxrRoot)) continue;
        QList<QPair<QList<int>, QString>> found;
        QDirIterator it(fxrRoot, QStringList() << fxrName, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString cand = it.next();
            found.append(qMakePair(verParts(QFileInfo(cand).dir().dirName()), cand));
        }
        // 版本高的先试（新版本没有已知缺陷时才想用它）。
        std::stable_sort(found.begin(), found.end(),
                         [](const QPair<QList<int>, QString>& a, const QPair<QList<int>, QString>& b) {
                             return a.first > b.first;
                         });
        for (const auto& f : found) add(f.second);
    }

    // ③ 最后兜底：随包分发的内核目录（self-contained 版，见本节开头 ①）。
    {
        QStringList kdirs;
        if (!g_cliDir.isEmpty()) kdirs << g_cliDir;
        const QStringList names{QStringLiteral("EGpdf2excel"), QStringLiteral("EGexcel2df"),
                                QStringLiteral("EGpdf2df")};
        for (const QString& k : names) kdirs << QDir(appDir).filePath(k);
        for (const QString& d : kdirs) add(QDir(d).filePath(fxrName));
    }
    return out;
}

// 找 hostfxr 库（不依赖 nethost —— 那是 SDK 期产物，用户机上通常没有）。
// 兼容旧语义：返回链上优先级最高的那个；真正的初始化见 initRuntime（它会逐个试）。
QString KernelClient::findHostFxr()
{
    const QStringList c = hostFxrCandidates();
    return c.isEmpty() ? QString() : c.first();
}

// ---------------------------------------------------------------------------
// hostfxr 初始化（进程内只做一次）
// ---------------------------------------------------------------------------
// hostfxr 初始化（进程内只做一次）：**逐个候选试**，取第一个能真正初始化成功的。
// 为什么要试而不是"选一个"：见 hostFxrCandidates 的说明 —— 机器上同时装了 6.x/8.x/9.x 与
// 10.x 时，只有低版本那份能服务 framework-dependent 组件；随包分发的 self-contained 那份
// 则从来不行。每个失败候选的代价只有几十毫秒。
static bool initRuntime(const QString& runtimeConfigPath, QString* why)
{
    if (g_runtimeReady) return true;
    if (!g_runtimeError.isEmpty()) { if (why) *why = g_runtimeError; return false; }

    auto fail = [&](const QString& msg) {
        g_runtimeError = msg;
        if (why) *why = msg;
        return false;
    };

    const QStringList cands = KernelClient::hostFxrCandidates();
    if (cands.isEmpty())
        return fail(QStringLiteral("未找到 %1（需要目标机安装 .NET 运行时）").arg(KernelClient::hostFxrFileName()));

    // ⚠️ 本工具的内核 target net9.0，但机器上可能只装了更新的主版本（本机就是 6/8/10）。
    //    命令行侧靠 `dotnet --roll-forward LatestMajor` 才跑得起来；内嵌侧必须在**第一次**
    //    hostfxr 初始化之前就把等价开关设好。
    //    不要"先正常试一次、失败再带开关重试"：实测那样虽然第二次 init 返回成功，但随后
    //    加载程序集会莫名其妙地失败（hdt_load_assembly / 取类型都报 0x80131522 COR_E_TYPELOAD）
    //    —— 那是"先失败一次"污染出来的坏状态。故这里一次性设好，再依次试候选。
    if (!qEnvironmentVariableIsSet("DOTNET_ROLL_FORWARD"))
        qputenv("DOTNET_ROLL_FORWARD", "LatestMajor");

    hfx::initialize_parameters params{};
    params.size = sizeof(hfx::initialize_parameters);
    // host_path 指向**宿主 exe**（官方文档要求宿主自报家门；留空时运行时按当前进程猜）。
    const auto hostPath = hfx::Native(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    params.host_path = hostPath.c_str();
    params.dotnet_root = nullptr;
    // ⚠️ 路径必须按平台字符类型传（Windows = UTF-16），见 hfx::char_t 的说明。
    const auto cfg = hfx::Native(QDir::toNativeSeparators(runtimeConfigPath));

    QStringList attempts;   // 失败明细：全失败时一次性报出来，省得逐个手工试
    for (const QString& fxr : cands)
    {
        QLibrary* lib = new QLibrary(fxr);
        if (!lib->load())
        {
            attempts << QStringLiteral("%1 → 加载失败").arg(fxr);
            delete lib;
            continue;
        }
        if (!lib->resolve("hostfxr_initialize_for_runtime_config")
            || !lib->resolve("hostfxr_get_runtime_delegate")
            || !lib->resolve("hostfxr_close"))
        {
            attempts << QStringLiteral("%1 → 缺少所需导出（版本过旧？）").arg(fxr);
            lib->unload();
            delete lib;
            continue;
        }
        auto closeFn = reinterpret_cast<hfx::close_fn>(lib->resolve("hostfxr_close"));
        auto initFn  = reinterpret_cast<hfx::initialize_for_runtime_config_fn>(
            lib->resolve("hostfxr_initialize_for_runtime_config"));

        hfx::handle_t ctx = nullptr;
        const int rc = initFn(cfg.c_str(), &params, &ctx);
        if (rc != 0 || ctx == nullptr)
        {
            if (ctx) closeFn(ctx);
            attempts << QStringLiteral("%1 → hr=0x%2").arg(fxr).arg(static_cast<quint32>(rc), 8, 16);
            lib->unload();
            delete lib;
            continue;
        }

        auto getDelegFn = reinterpret_cast<hfx::get_runtime_delegate_fn>(
            lib->resolve("hostfxr_get_runtime_delegate"));
        void* p = nullptr;
        const int rc2 = getDelegFn(ctx, hfx::hdt_load_assembly_and_get_function_pointer, &p);
        if (rc2 != 0 || p == nullptr)
        {
            closeFn(ctx);
            attempts << QStringLiteral("%1 → get_runtime_delegate hr=0x%2")
                            .arg(fxr).arg(static_cast<quint32>(rc2), 8, 16);
            lib->unload();
            delete lib;
            continue;
        }

        // 成功：把这份 hostfxr 与上下文固定下来（后续所有调用都走它）。
        g_hostfxrLib = lib;
        g_hostfxrPath = fxr;
        g_closeFn = closeFn;
        g_loadFn = reinterpret_cast<hfx::load_assembly_and_get_function_pointer_fn>(p);

        // 备用路径的两个委托：取不到就置空（只有主路径失败时才会用到）。
        void* pa = nullptr; void* pf = nullptr;
        if (getDelegFn(ctx, hfx::hdt_load_assembly, &pa) == 0) g_asmLoadFn = reinterpret_cast<hfx::load_assembly_fn>(pa);
        if (getDelegFn(ctx, hfx::hdt_get_function_pointer, &pf) == 0) g_fnPtrFn = reinterpret_cast<hfx::get_function_pointer_fn>(pf);
        g_ctx = ctx;
        g_runtimeReady = true;
        return true;
    }

    return fail(QStringLiteral("所有 %1 候选都无法初始化 —— %2")
                    .arg(KernelClient::hostFxrFileName(), attempts.join(QStringLiteral("；"))));
}

// 真正按"某个内核的 runtimeconfig"初始化（首次调用时绑定；之后复用同一 runtime）。
// 具体选哪份 hostfxr、以及"为什么必须逐个试"全在 initRuntime 里（含 hostFxrCandidates 的说明）。
static bool ensureRuntimeFor(const QString& runtimeConfigPath, QString* why)
{
    QMutexLocker lk(&g_mutex);
    if (g_runtimeReady) return true;
    return initRuntime(runtimeConfigPath, why);
}

// 取某个内核的导出函数指针。
//
// **两条托管加载路径**（都是官方 hostfxr 的公开 API，任一条能用即可）：
//   ① load_assembly_and_get_function_pointer —— 一步到位，内部用组件式加载
//      （AssemblyDependencyResolver + 默认 ALC）。官方文档首推。
//   ② load_assembly + get_function_pointer —— 先加载程序集、再按"类型+方法"取指针。
//      两步分开，对"程序集是应用（有入口点）"这类形态更宽容。
// 实测（.NET 10 + 我们自己的内核 DLL）：路径①在**按类型取词**这一步返回 0x80131522
// (COR_E_TYPELOAD)。真正的原因见下面 typeName 处的注释（类型名必须带程序集限定名）。
// 这里保留**双路径尝试**，
// 并把每步的 HRESULT 记进错误信息（方式② 的 load_assembly hr 曾直接把问题定位到
// "程序集已加载、只是按名字取不到类型"）。
static bool resolveKernel(const QString& kernelDll, void** runFn, void** logSinkFn, QString* why)
{
    if (!g_loadFn) { if (why) *why = QStringLiteral("运行时未初始化"); return false; }
    const auto asmPath = hfx::Native(QDir::toNativeSeparators(kernelDll));
    // ⚠️ 类型名必须是 **assembly qualified**（"命名空间.类型, 程序集简单名"）！
    // 只给 "EGtools.EmbeddedEntry" 时：hdt_load_assembly 成功(hr=0)，但按名字取函数指针
    // 会以 COR_E_TYPELOAD(0x80131522) 失败——因为那条路径用 Type.GetType() 解析，
    // 没有程序集限定名就找不到类型。加限定名后即通。
    const auto typeName = hfx::Native(
        QStringLiteral("EGtools.EmbeddedEntry, %1").arg(QFileInfo(kernelDll).completeBaseName()));
    auto hex = [](int rc) { return QString::number(static_cast<quint32>(rc), 16); };

    // 先探健康检查：走"默认签名"（ComponentEntryPoint）路径，只关心类型能否取到。
    // 它成功 ⇒ 运行时/程序集/类型这层没问题，后面只剩 [UnmanagedCallersOnly] 这一层。
    int pingHr1 = 0, asmLoadHr = 0, fnPtrHr = 0;
    {
        void* ping = nullptr;
        const auto pingName = hfx::Native(QStringLiteral("Ping"));
        if (g_loadFn)
            pingHr1 = g_loadFn(asmPath.c_str(), typeName.c_str(), pingName.c_str(), nullptr, nullptr, &ping);
        if ((pingHr1 != 0 || ping == nullptr) && g_asmLoadFn && g_fnPtrFn)
        {
            // hdt_load_assembly 把程序集载入**默认 ALC**（两个扩展参数必须为 0），
            // 随后 get_function_pointer 也以 load_context=0 在默认 ALC 里按名字取。
            asmLoadHr = g_asmLoadFn(asmPath.c_str(), nullptr, nullptr);
            fnPtrHr = (asmLoadHr == 0)
                ? g_fnPtrFn(typeName.c_str(), pingName.c_str(), nullptr, nullptr, nullptr, &ping)
                : asmLoadHr;
        }
        if (ping == nullptr)
        {
            if (why) *why = QStringLiteral(
                "内核程序集/类型加载失败（方式①hr=0x%1；方式② load_assembly hr=0x%2, get_function_pointer hr=0x%3）")
                .arg(hex(pingHr1), hex(asmLoadHr), hex(fnPtrHr));
            return false;
        }
    }

    auto get = [&](const char* method, void** out) -> bool {
        const auto methodName = hfx::Native(QString::fromLatin1(method));
        void* fn = nullptr;
        int rc1 = g_loadFn
            ? g_loadFn(asmPath.c_str(), typeName.c_str(), methodName.c_str(),
                       hfx::unmanaged_callers_only_method(), nullptr, &fn)
            : -1;
        if (rc1 == 0 && fn != nullptr) { *out = fn; return true; }

        int rc2 = -1;
        if (g_asmLoadFn && g_fnPtrFn)
        {
            int h = g_asmLoadFn(asmPath.c_str(), nullptr, nullptr);
            rc2 = (h == 0)
                ? g_fnPtrFn(typeName.c_str(), methodName.c_str(),
                            hfx::unmanaged_callers_only_method(), nullptr, nullptr, &fn)
                : h;
        }
        if (rc2 == 0 && fn != nullptr) { *out = fn; return true; }

        if (why) *why = QStringLiteral("取 EmbeddedEntry.%1 失败（方式①hr=0x%2，方式②hr=0x%3）")
                            .arg(QString::fromLatin1(method), hex(rc1), hex(rc2));
        return false;
    };
    return get("Run", runFn) && get("SetLogSink", logSinkFn);
}

// ---------------------------------------------------------------------------
// 可用性探测
// ---------------------------------------------------------------------------
// hostfxr 初始化用哪份 runtimeconfig：
//   首选**宿主自己的**（<exe 同名>.runtimeconfig.json，由 CMake 生成）—— 于是内核 DLL 是
//   "组件"身份被加载，依赖由它自己的 deps.json 解析，这是 hostfxr 组件模型的正常用法。
//   兜底才用内核那份（等于把内核当宿主，实测会加载失败，仅为兼容老部署）。
static QString hostRuntimeConfigPath(const QString& kernelCfgFallback)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString exeBase = QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName();
    const QString own = QDir(appDir).filePath(exeBase + QStringLiteral(".runtimeconfig.json"));
    if (QFileInfo::exists(own)) return own;
    return kernelCfgFallback;
}

bool KernelClient::embeddedAvailable(const QString& kernel, QString* why)
{
    const QString dll = findKernelFile(kernel, QStringLiteral(".dll"));
    const QString cfg = findKernelFile(kernel, QStringLiteral(".runtimeconfig.json"));
    if (!QFileInfo::exists(dll))
    {
        if (why) *why = QStringLiteral("未找到 %1.dll").arg(kernel);
        return false;
    }
    if (!QFileInfo::exists(cfg))
    {
        if (why) *why = QStringLiteral("未找到 %1.runtimeconfig.json（内嵌需要它）").arg(kernel);
        return false;
    }
    // ⚠️ 别再用 findHostFxr() 判"可用"：它只返回**候选之一**的路径，而候选里可能包含
    //    随包分发的 self-contained hostfxr（那份永远初始化不了，见 hostFxrCandidates）。
    if (hostFxrCandidates().isEmpty())
    {
        if (why) *why = QStringLiteral("未找到 %1（目标机需安装 .NET 运行时）").arg(KernelClient::hostFxrFileName());
        return false;
    }
    if (!g_runtimeError.isEmpty())
    {
        if (why) *why = g_runtimeError;
        return false;
    }
    return true;
}

QString KernelClient::describe(const QString& kernel)
{
    QString why;
    const bool ok = embeddedAvailable(kernel, &why);
    // 候选链上可能有多份 hostfxr（其中还可能有**初始化不了**的那份，如 .NET 10 或随包分发的
    // self-contained 版）⇒ 这里只说"找到了哪些候选"，真正的可用性由首次调用的逐个试探决定。
    const QStringList c = hostFxrCandidates();
    return ok
        ? QStringLiteral("%1 内嵌可用（hostfxr: %2%3）")
              .arg(kernel,
                   c.isEmpty() ? QString() : c.first(),
                   c.size() > 1 ? QStringLiteral("，另有 %1 个候选").arg(c.size() - 1) : QString())
        : QStringLiteral("%1 内嵌不可用：%2").arg(kernel, why);
}

// ---------------------------------------------------------------------------
// 内嵌后端
// ---------------------------------------------------------------------------
KernelClient::Result KernelClient::runEmbedded(const QString& kernel, const QStringList& args,
                                               const QString& jsonRequest)
{
    Result r;
    r.backend = QStringLiteral("embedded");
    QString why;

    const QString dll = findKernelFile(kernel, QStringLiteral(".dll"));
    const QString cfg = findKernelFile(kernel, QStringLiteral(".runtimeconfig.json"));
    if (!QFileInfo::exists(dll) || !QFileInfo::exists(cfg))
    {
        r.err = QStringLiteral("内嵌失败：缺少 %1.dll / .runtimeconfig.json").arg(kernel);
        return r;
    }
    if (!ensureRuntimeFor(hostRuntimeConfigPath(cfg), &why))
    {
        r.err = QStringLiteral("内嵌失败：%1").arg(why);
        return r;
    }

    void* pRun = nullptr; void* pLog = nullptr;
    if (!resolveKernel(dll, &pRun, &pLog, &why))
    {
        r.err = QStringLiteral("内嵌失败：%1").arg(why);
        return r;
    }
    auto runFn = reinterpret_cast<eg_run_fn>(pRun);
    auto logFn = reinterpret_cast<eg_set_log_sink_fn>(pLog);

    // 注册日志回调（内核 stderr 会实时回到 eg_log_bridge）。
    {
        QMutexLocker lk(&g_mutex);
        g_log.clear();
    }
    logFn(reinterpret_cast<void*>(&eg_log_bridge), nullptr);

    // argv → C 数组（QByteArray 必须活着到调用结束）。
    std::vector<QByteArray> keep;
    keep.reserve(static_cast<size_t>(args.size()));
    for (const QString& a : args) keep.push_back(a.toUtf8());
    std::vector<const char*> argv;
    argv.reserve(keep.size());
    for (const QByteArray& k : keep) argv.push_back(k.constData());

    const QByteArray json = jsonRequest.isEmpty() ? QByteArray() : jsonRequest.toUtf8();
    const char* jsonPtr = json.isEmpty() ? nullptr : json.constData();

    // 两段式缓冲区协议：先给 64KB，不够按返回的 need 放大重来。
    QByteArray buf;
    buf.resize(64 * 1024);
    int need = runFn(argv.empty() ? nullptr : argv.data(), static_cast<int>(argv.size()),
                     jsonPtr, buf.data(), buf.size());
    if (need >= buf.size())
    {
        buf.resize(need + 1);
        need = runFn(argv.empty() ? nullptr : argv.data(), static_cast<int>(argv.size()),
                     jsonPtr, buf.data(), buf.size());
    }

    if (need < 0)
    {
        int errLen = -need - 1;
        if (errLen + 1 > buf.size())
        {
            buf.resize(errLen + 1);
            int n2 = runFn(argv.empty() ? nullptr : argv.data(), static_cast<int>(argv.size()),
                           jsonPtr, buf.data(), buf.size());
            errLen = n2 < 0 ? -n2 - 1 : n2;
        }
        r.rc = -1;
        r.err = QString::fromUtf8(buf.constData(), errLen);
        // 把内核日志一并带上（前面可能已经写了很多行）
        QMutexLocker lk(&g_mutex);
        if (!g_log.isEmpty()) r.err += QStringLiteral("\n--- 内核日志 ---\n") + g_log.join(QLatin1Char('\n'));
        return r;
    }

    r.rc = 0;
    r.out = QString::fromUtf8(buf.constData(), need);
    {
        QMutexLocker lk(&g_mutex);
        r.err = g_log.join(QLatin1Char('\n'));
    }
    return r;
}

// ---------------------------------------------------------------------------
// 命令行后端
// ---------------------------------------------------------------------------
KernelClient::Result KernelClient::runCli(const QString& kernel, const QStringList& args,
                                          const QString& jsonRequest, int timeoutMs)
{
    Result r;
    r.backend = QStringLiteral("cli");

    QStringList a = args;
    QString tmpPath;
    if (!jsonRequest.isEmpty() && !a.contains(QStringLiteral("--json"))
        && !a.contains(QStringLiteral("--stdin")))
    {
        auto* tf = new QTemporaryFile(QDir(QStandardPaths::writableLocation(
            QStandardPaths::TempLocation)).filePath(QStringLiteral("egtools_req_XXXXXX.json")));
        tf->setAutoRemove(false);
        if (!tf->open())
        {
            delete tf;
            r.err = QStringLiteral("无法创建请求 JSON 临时文件");
            return r;
        }
        tf->write(jsonRequest.toUtf8());
        tmpPath = tf->fileName();
        tf->close();
        delete tf;
        a << QStringLiteral("--json") << tmpPath;
    }

    QString out, err;
    const int rc = CliRunner::run(findKernelFile(kernel, exeSuffix()), a, out, err, timeoutMs);
    r.rc = rc;
    r.out = out;
    r.err = err;
    if (!tmpPath.isEmpty()) QFile::remove(tmpPath);
    return r;
}

// ---------------------------------------------------------------------------
// 融合入口（Auto：先内嵌，再命令行）
// ---------------------------------------------------------------------------
KernelClient::Result KernelClient::run(const QString& kernel, const QStringList& args,
                                       const QString& jsonRequest, Backend backend, int timeoutMs)
{
    // ------------------------------------------------------------------
    // 调用串行化
    //
    // 内嵌后端把内核 DLL 载入本进程，内核用的是**进程级单例**资源：MuPDFContext、
    // g_log（日志缓冲）、以及 hostfxr 绑定的默认 ALC。并发调用会互相污染日志、
    // 甚至同时持有 MuPDF 上下文导致随机崩溃。命令行后端起的是独立进程，天然隔离，
    // 并发是安全的（也才有真正的并行收益）。
    // 因此：内嵌路径全局串行；命令行路径不加锁。
    // "并行"体现在：界面线程绝不被内核调用阻塞（所有调用都在后台线程），
    // 且命令行后端可多文件并行。
    // ------------------------------------------------------------------
    static QMutex s_embeddedMutex;
    const bool needsLock = backend != Backend::Cli;
    if (needsLock) s_embeddedMutex.lock();
    struct Unlocker { QMutex* m; bool on; ~Unlocker() { if (on) m->unlock(); } } unlocker{ &s_embeddedMutex, needsLock };

    if (backend == Backend::Embedded)
        return runEmbedded(kernel, args, jsonRequest);

    if (backend == Backend::Cli)
        return runCli(kernel, args, jsonRequest, timeoutMs);

    // Auto：内嵌优先（无子进程、启动快、日志实时）。失败则回退命令行，并在 err 里说明原因。
    QString why;
    if (embeddedAvailable(kernel, &why))
    {
        Result r = runEmbedded(kernel, args, jsonRequest);
        if (r.rc == 0) return r;

        // 内嵌能起但**内核本身报错**时不要回退（那是真实错误，重试没意义）：
        // 只有"内嵌通道自身失败"（错误文本以 内嵌失败： 开头）才回退。
        if (!r.err.startsWith(QStringLiteral("内嵌失败：")))
            return r;

        Result c = runCli(kernel, args, jsonRequest, timeoutMs);
        c.err = QStringLiteral("[内嵌不可用，已回退命令行] %1\n").arg(r.err) + c.err;
        return c;
    }

    Result c = runCli(kernel, args, jsonRequest, timeoutMs);
    c.err = QStringLiteral("[内嵌不可用，已回退命令行] %1\n").arg(why) + c.err;
    return c;
}
