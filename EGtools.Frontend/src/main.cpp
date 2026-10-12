// EGtools Qt front-end entry point.
// Normal mode: launch the GUI. `--selftest` runs the headless QA path instead
// (offscreen platform), so the sandbox can verify the full CLI round-trip.
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QIcon>
#include <QSurfaceFormat>
#include <cstdio>
#include "mainwindow.h"
#include "selftest.h"
#include "theme.h"
#ifdef EGTOOLS_WIN32
#include <windows.h>
#endif

#ifdef EGTOOLS_WIN32
// 本程序按 **GUI 子系统** 链接（CMake 里 WIN32_EXECUTABLE），所以双击启动时 Windows
// 不会先创建一个控制台窗口 —— 这正是以前"启动时黑窗口一闪"的来源：控制台子系统 +
// main() 里事后 FreeConsole()，用户看到的就是那个窗口短暂出现的瞬间。
//
// GUI 子系统的代价是默认没有 stdout，而 --selftest / --test-* 要靠它输出 PASS/FAIL。
// 所以只在**测试模式**下按需"借用"父进程的控制台：
// ⚠️ 判断条件是关键 —— 若标准输出句柄已经有效（父进程用 `> file` 或 `| Out-String`
//    把句柄给了我们），就绝不能再去 AttachConsole + freopen("CONOUT$")，那会把输出
//    从重定向目标挪到控制台，无头自检的输出捕获会立刻失效。
static void ensureStdoutUsable()
{
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) return;   // 已被重定向/继承 → 别动它
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;        // 无父控制台（双击启动）→ 无需处理
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$",  "r", stdin);
}
#endif

int main(int argc, char** argv)
{
    // Pre-scan argv: the headless self-test must force the offscreen platform
    // BEFORE QApplication is constructed (the platform plugin loads in the ctor).
    QString shotOut;
    bool wantSelfTest = false, wantMathTest = false, wantRenderTest = false,
         wantContTest = false, wantBatchTest = false, wantLayout = false,
         wantPaintTest = false, wantUiPaintTest = false, wantShot = false,
         wantDiffUiTest = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--selftest") == 0) wantSelfTest = true;
        else if (qstrcmp(argv[i], "--test-math") == 0) wantMathTest = true;
        else if (qstrcmp(argv[i], "--test-render") == 0) wantRenderTest = true;
        else if (qstrcmp(argv[i], "--test-continuous") == 0) wantContTest = true;
        else if (qstrcmp(argv[i], "--test-batch") == 0) wantBatchTest = true;
        else if (qstrcmp(argv[i], "--dump-layout") == 0) wantLayout = true;
        else if (qstrcmp(argv[i], "--test-paint") == 0) wantPaintTest = true;
        else if (qstrcmp(argv[i], "--test-ui-paint") == 0) wantUiPaintTest = true;
        else if (qstrcmp(argv[i], "--screenshot") == 0) wantShot = true;
        else if (qstrcmp(argv[i], "--test-diffui") == 0) wantDiffUiTest = true;
        else if (qstrcmp(argv[i], "--out") == 0 && i + 1 < argc) shotOut = QString::fromLocal8Bit(argv[++i]);
    }
    const bool anyTestMode = wantSelfTest || wantMathTest || wantRenderTest
                             || wantContTest || wantBatchTest || wantLayout
                             || wantPaintTest || wantUiPaintTest || wantShot
                             || wantDiffUiTest;

#ifdef EGTOOLS_WIN32
    // 只有测试模式需要 stdout；正常 GUI 模式什么都不做（GUI 子系统本来就没有控制台）。
    if (anyTestMode) ensureStdoutUsable();
#endif
    if ((wantSelfTest || wantRenderTest || wantContTest || wantBatchTest || wantLayout
         || wantPaintTest) && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    // GPU 相关的进程级设置必须在 QApplication 构造之前生效：共享 GL 上下文是 Qt 对
    // QOpenGLWidget 的官方建议（多控件/多窗口共用纹理与着色器资源）。
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // ⚠️ 默认 GL 格式也必须在这里定：QOpenGLWidget 需要 **GL 2.0 / ES 2** 才能工作
    // （它靠 FBO + 着色器合成）。部分老驱动默认只给 1.1（GDI Generic），表现出来就是
    // 一整块**黑画布**。显式请求 2.0 + NoProfile 是兼容面最广的写法：
    // 现代驱动会正常给出 2.0+ 兼容上下文，老驱动则要么给 2.0、要么创建失败——
    // 而"创建失败"会被 pdfview 的探测接住，自动退回 CPU 光栅，不会黑屏。
    {
        QSurfaceFormat fmt;
        fmt.setVersion(2, 0);
        fmt.setProfile(QSurfaceFormat::NoProfile);
        fmt.setDepthBufferSize(0);        // 我们只画 2D，不要深度/模板缓冲
        fmt.setStencilBufferSize(0);
        fmt.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(fmt);
    }

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("EGtools"));
    // 版本里带上构建配置：`EGtoolsFrontend.exe --version` 即可自证是 Release 还是 Debug。
#ifdef EGTOOLS_BUILD_CONFIG
    QCoreApplication::setApplicationVersion(
        QStringLiteral("1.0.0 (%1)").arg(QStringLiteral(EGTOOLS_BUILD_CONFIG)));
#else
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
#endif
    // Window / taskbar / Start-menu icon (embedded Qt resource). The EXE file
    // icon itself is embedded separately via res/app_icon.rc.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));
    // 主题在窗口构造前套用，避免启动瞬间出现"闪一下默认样式"。
    Theme::apply(Theme::current());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("EGtools Qt front-end (region extraction + drawing diff)"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption optSelf(QStringLiteral("selftest"),
        QStringLiteral("Run headless self-test (render+map+extract) and exit."));
    QCommandLineOption optCli(QStringLiteral("cli-dir"),
        QStringLiteral("Directory containing EGpdf2excel.exe / EGpdf2df.exe."), QStringLiteral("dir"));
    QCommandLineOption optPdf(QStringLiteral("pdf"),
        QStringLiteral("PDF to use for --selftest."), QStringLiteral("file"));
    QCommandLineOption optMath(QStringLiteral("test-math"),
        QStringLiteral("Run the pure coordinate-mapping test (no child processes)."));
    QCommandLineOption optRender(QStringLiteral("test-render"),
        QStringLiteral("Render one page and report image size/format/luminance (black-page diagnosis)."));
    QCommandLineOption optCont(QStringLiteral("test-continuous"),
        QStringLiteral("Self-test the continuous multi-page view (layout, hit-test, lazy render)."));
    QCommandLineOption optBatch(QStringLiteral("test-batch"),
        QStringLiteral("Self-test concurrent batch extraction (thread safety + accurate counts)."));
    QCommandLineOption optJobs(QStringLiteral("jobs"),
        QStringLiteral("How many files for --test-batch (default 3)."), QStringLiteral("n"), QStringLiteral("3"));
    QCommandLineOption optConc(QStringLiteral("concurrency"),
        QStringLiteral("Concurrency for --test-batch (default 3)."), QStringLiteral("n"), QStringLiteral("3"));
    QCommandLineOption optLayout(QStringLiteral("dump-layout"),
        QStringLiteral("Audit the window layout (minimum sizes + per-pane widths) and exit."));
    QCommandLineOption optDiffUi(QStringLiteral("test-diffui"),
        QStringLiteral("Self-test the PDF-compare page: difference list, filters, jump-to-difference."));
    QCommandLineOption optShot(QStringLiteral("screenshot"),
        QStringLiteral("Open the GUI, load the PDF and save window/screen PNGs (visual evidence)."));
    QCommandLineOption optOut(QStringLiteral("out"),
        QStringLiteral("Output base path for --screenshot."), QStringLiteral("base"));
    QCommandLineOption optUiPaint(QStringLiteral("test-ui-paint"),
        QStringLiteral("Open a real MainWindow, load the PDF and measure the pixels the canvas "
                       "control actually shows (catches an all-black canvas in its real nesting)."));
    QCommandLineOption optPaint(QStringLiteral("test-paint"),
        QStringLiteral("Render a page, paint it into the real canvas shell and measure the grabbed "
                       "pixels (catches an all-black canvas that a plain render test cannot)."));
    QCommandLineOption optPage(QStringLiteral("page"),
        QStringLiteral("Page index for --test-render (default 0)."), QStringLiteral("n"), QStringLiteral("0"));
    QCommandLineOption optOpen(QStringLiteral("open"),
        QStringLiteral("Open this PDF in the GUI on startup (same as passing it as a file argument)."),
        QStringLiteral("file"));
    parser.addOption(optSelf);
    parser.addOption(optCli);
    parser.addOption(optPdf);
    parser.addOption(optMath);
    parser.addOption(optRender);
    parser.addOption(optCont);
    parser.addOption(optBatch);
    parser.addOption(optJobs);
    parser.addOption(optConc);
    parser.addOption(optLayout);
    parser.addOption(optPaint);
    parser.addOption(optUiPaint);
    parser.addOption(optShot);
    parser.addOption(optDiffUi);
    parser.addOption(optOut);
    parser.addOption(optPage);
    parser.addOption(optOpen);
    parser.process(app);

    if (parser.isSet(optMath)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runMathTest();
    }

    if (parser.isSet(optRender)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runRenderTest(parser.value(optCli), parser.value(optPdf),
                             parser.value(optPage).toInt());
    }

    if (parser.isSet(optCont)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runContinuousTest(parser.value(optCli), parser.value(optPdf));
    }

    if (parser.isSet(optDiffUi)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runDiffUiTest();
    }

    if (parser.isSet(optShot)) {
        return runScreenshot(parser.value(optCli), parser.value(optPdf),
                             parser.value(optOut).isEmpty() ? shotOut : parser.value(optOut));
    }

    if (parser.isSet(optUiPaint)) {
        // 注意：**不**强制离屏 —— 本项就是要走真实平台，才能碰到 GPU 合成路径。
        return runUiPaintTest(parser.value(optCli), parser.value(optPdf));
    }

    if (parser.isSet(optPaint)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runPaintTest(parser.value(optCli), parser.value(optPdf));
    }

    if (parser.isSet(optLayout)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runLayoutDump();
    }

    if (parser.isSet(optBatch)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runBatchTest(parser.value(optCli), parser.value(optPdf),
                            parser.value(optJobs).toInt(), parser.value(optConc).toInt());
    }

    if (parser.isSet(optSelf)) {
        // Headless: force offscreen rendering so no display is required.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runSelfTest(parser.value(optCli), parser.value(optPdf));
    }

    MainWindow w(parser.value(optCli));
    w.show();

    // 启动即打开一个 PDF：--open <file> 或直接 EGtoolsFrontend.exe <file>（文件关联/拖拽）。
    QString toOpen = parser.value(optOpen);
    if (toOpen.isEmpty() && parser.positionalArguments().size() > 0)
        toOpen = parser.positionalArguments().first();
    if (!toOpen.isEmpty() && QFile::exists(toOpen))
        w.openPdf(toOpen);
    else if (!toOpen.isEmpty())
        qWarning("cannot open '%s': file not found", qPrintable(toOpen));

    return app.exec();
}