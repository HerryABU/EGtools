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
    for (const QString& d : dirs)
    {
        const QString c = QDir(d).filePath(name);
        if (QFileInfo::exists(c)) return c;
    }
    return dirs.isEmpty() ? name : QDir(dirs.first()).filePath(name);
}

QString KernelClient::kernelDir(const QString& kernel)
{
    const QString dll = findKernelFile(kernel, QStringLiteral(".dll"));
    return QFileInfo(dll).absolutePath();
}

// 找 hostfxr.dll：不依赖 nethost（那是 SDK 期产物，用户机上通常没有）。
// 顺序：① 应用目录旁（可随包分发）② DOTNET_ROOT ③ 注册表安装位置
//      ④ 默认安装目录；然后在 host/fxr/<版本>/ 里取**版本最高**的那个。
QString KernelClient::findHostFxr()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString beside = QDir(appDir).filePath(QStringLiteral("hostfxr.dll"));
    if (QFileInfo::exists(beside)) return beside;

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
#endif
    roots << QStringLiteral("C:/Program Files/dotnet");

    for (const QString& root : roots)
    {
        const QString fxrRoot = QDir(root).filePath(QStringLiteral("host/fxr"));
        if (!QFileInfo::exists(fxrRoot)) continue;
        QString best;
        QDirIterator it(fxrRoot, QStringList() << QStringLiteral("hostfxr.dll"),
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString cand = it.next();
            // 目录名即版本号，取最大者（字符串比较对同一主版本足够；10.x > 9.x > 8.x 需按段比）
            const QString ver = QFileInfo(cand).dir().dirName();
            auto parts = [](const QString& v) {
                QList<int> p; for (const QString& s : v.split(QLatin1Char('.'))) p << s.toInt();
                while (p.size() < 3) p << 0; return p;
            };
            if (best.isEmpty() || parts(ver) > parts(QFileInfo(best).dir().dirName())) best = cand;
        }
        if (!best.isEmpty()) return best;
    }
    return QString();
}

// ---------------------------------------------------------------------------
// hostfxr 初始化（进程内只做一次）
// ---------------------------------------------------------------------------
static bool initRuntime(QString* why)
{
    if (g_runtimeReady) return true;
    if (!g_runtimeError.isEmpty()) { if (why) *why = g_runtimeError; return false; }

    auto fail = [&](const QString& msg) {
        g_runtimeError = msg;
        if (why) *why = msg;
        return false;
    };

    const QString fxr = KernelClient::findHostFxr();
    if (fxr.isEmpty())
        return fail(QStringLiteral("未找到 hostfxr.dll（需要目标机安装 .NET 运行时）"));

    g_hostfxrLib = new QLibrary(fxr);
    if (!g_hostfxrLib->load())
        return fail(QStringLiteral("hostfxr.dll 加载失败: %1").arg(g_hostfxrLib->errorString()));

    // 只校验必要导出是否齐全；真正初始化在 ensureRuntimeFor（要用某个内核的 runtimeconfig）。
    if (!g_hostfxrLib->resolve("hostfxr_initialize_for_runtime_config")
        || !g_hostfxrLib->resolve("hostfxr_get_runtime_delegate")
        || !g_hostfxrLib->resolve("hostfxr_close"))
        return fail(QStringLiteral("hostfxr.dll 缺少所需导出（版本过旧？）"));

    g_hostfxrPath = fxr;
    return true;
}

// 真正按"某个内核的 runtimeconfig"初始化（首次调用时绑定；之后复用同一 runtime）。
static bool ensureRuntimeFor(const QString& runtimeConfigPath, QString* why)
{
    QMutexLocker lk(&g_mutex);
    if (g_runtimeReady) return true;
    if (!initRuntime(why)) return false;

    auto initFn = reinterpret_cast<hfx::initialize_for_runtime_config_fn>(
        g_hostfxrLib->resolve("hostfxr_initialize_for_runtime_config"));
    auto setPropFn = reinterpret_cast<hfx::set_runtime_property_value_fn>(
        g_hostfxrLib->resolve("hostfxr_set_runtime_property_value"));
    auto getDelegFn = reinterpret_cast<hfx::get_runtime_delegate_fn>(
        g_hostfxrLib->resolve("hostfxr_get_runtime_delegate"));

    // ⚠️ 本工具的内核 target net9.0，但机器上可能只装了更新的主版本（本机就是只有 10.x）。
    // 命令行侧靠 `dotnet --roll-forward LatestMajor` 才跑得起来；内嵌侧必须在**第一次**
    // hostfxr 初始化之前就把等价开关设好。
    // 不要"先正常试一次、失败再带开关重试"：实测那样虽然第二次 init 返回成功，
    // 但随后加载程序集会莫名其妙地失败（hdt_load_assembly / 取类型都报 0x80131522
    // COR_E_TYPELOAD），而同一份 DLL 在托管侧用默认 ALC 加载完全正常 —— 说明是
    // "先失败一次"污染出来的坏状态。故这里一次性设好再 init。
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
    hfx::handle_t ctx = nullptr;
    int rc = initFn(cfg.c_str(), &params, &ctx);
    if (rc != 0 || ctx == nullptr)
    {
        g_runtimeError = QStringLiteral("hostfxr 初始化失败 (hr=0x%1): %2")
                             .arg(static_cast<quint32>(rc), 8, 16).arg(runtimeConfigPath);
        if (why) *why = g_runtimeError;
        return false;
    }

    // 说明：曾经在这里把 APP_CONTEXT_BASE_DIRECTORY 设成内核目录，想让依赖从内核目录解析。
    // 实测这会让"已按 runtimeconfig/deps.json 算好的默认 ALC 基目录"与运行时属性不一致，
    // 之后按路径加载内核程序集会失败（COR_E_TYPELOAD）。deps.json 本身已经让依赖解析到
    // 内核目录，不需要这个属性，故不设。
    (void)setPropFn;

    void* p = nullptr;
    rc = getDelegFn(ctx, hfx::hdt_load_assembly_and_get_function_pointer, &p);
    if (rc != 0 || p == nullptr)
    {
        g_closeFn(ctx);
        g_runtimeError = QStringLiteral("hostfxr_get_runtime_delegate 失败 (hr=0x%1)")
                             .arg(static_cast<quint32>(rc), 8, 16);
        if (why) *why = g_runtimeError;
        return false;
    }
    g_loadFn = reinterpret_cast<hfx::load_assembly_and_get_function_pointer_fn>(p);

    // 备用路径的两个委托：取不到就置空（只有主路径失败时才会用到）。
    void* pa = nullptr; void* pf = nullptr;
    if (getDelegFn(ctx, hfx::hdt_load_assembly, &pa) == 0) g_asmLoadFn = reinterpret_cast<hfx::load_assembly_fn>(pa);
    if (getDelegFn(ctx, hfx::hdt_get_function_pointer, &pf) == 0) g_fnPtrFn = reinterpret_cast<hfx::get_function_pointer_fn>(pf);
    g_ctx = ctx;
    g_runtimeReady = true;
    return true;
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
    if (findHostFxr().isEmpty())
    {
        if (why) *why = QStringLiteral("未找到 hostfxr.dll（目标机需安装 .NET 运行时）");
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
    const QString host = findHostFxr();
    return ok
        ? QStringLiteral("%1 内嵌可用（hostfxr: %2）").arg(kernel, host)
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
    const int rc = CliRunner::run(findKernelFile(kernel, QStringLiteral(".exe")), a, out, err, timeoutMs);
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
