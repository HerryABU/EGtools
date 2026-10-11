// EGtools Qt front-end entry point.
// Normal mode: launch the GUI. `--selftest` runs the headless QA path instead
// (offscreen platform), so the sandbox can verify the full CLI round-trip.
#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include "mainwindow.h"
#include "selftest.h"
#include "theme.h"
#ifdef EGTOOLS_WIN32
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    // Pre-scan argv: the headless self-test must force the offscreen platform
    // BEFORE QApplication is constructed (the platform plugin loads in the ctor).
    bool wantSelfTest = false, wantMathTest = false, wantRenderTest = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--selftest") == 0) wantSelfTest = true;
        else if (qstrcmp(argv[i], "--test-math") == 0) wantMathTest = true;
        else if (qstrcmp(argv[i], "--test-render") == 0) wantRenderTest = true;
    }
    // The front-end is built as a console subsystem so --selftest / --test-math can
    // print PASS/FAIL to stdout. For the normal GUI we DETACH that console
    // entirely (FreeConsole) so there is no black window to close — a hidden
    // console (ShowWindow SW_HIDE) can still be closed from the taskbar and that
    // terminates the whole process group, taking the GUI down with it. After
    // FreeConsole there is simply no console, so nothing to close. The child CLI
    // kernels are spawned with CREATE_NO_WINDOW (see CliRunner), so they never
    // show a console of their own either.
#ifdef EGTOOLS_WIN32
    if (!wantSelfTest && !wantMathTest && !wantRenderTest) {
        if (GetConsoleWindow()) FreeConsole();
    }
#endif
    if ((wantSelfTest || wantRenderTest) && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    // GPU 相关的进程级设置必须在 QApplication 构造之前生效：共享 GL 上下文是 Qt 对
    // QOpenGLWidget 的官方建议（多控件/多窗口共用纹理与着色器资源）。
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("EGtools"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
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
    QCommandLineOption optPage(QStringLiteral("page"),
        QStringLiteral("Page index for --test-render (default 0)."), QStringLiteral("n"), QStringLiteral("0"));
    parser.addOption(optSelf);
    parser.addOption(optCli);
    parser.addOption(optPdf);
    parser.addOption(optMath);
    parser.addOption(optRender);
    parser.addOption(optPage);
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

    if (parser.isSet(optSelf)) {
        // Headless: force offscreen rendering so no display is required.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runSelfTest(parser.value(optCli), parser.value(optPdf));
    }

    MainWindow w(parser.value(optCli));
    w.show();
    return app.exec();
}