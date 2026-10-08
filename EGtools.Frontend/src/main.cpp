// EGtools Qt front-end entry point.
// Normal mode: launch the GUI. `--selftest` runs the headless QA path instead
// (offscreen platform), so the sandbox can verify the full CLI round-trip.
#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include "mainwindow.h"
#include "selftest.h"

int main(int argc, char** argv)
{
    // Pre-scan argv: the headless self-test must force the offscreen platform
    // BEFORE QApplication is constructed (the platform plugin loads in the ctor).
    bool wantSelfTest = false;
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--selftest") == 0) { wantSelfTest = true; break; }
    if (wantSelfTest && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("EGtools"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    // Window / taskbar / Start-menu icon (embedded Qt resource). The EXE file
    // icon itself is embedded separately via res/app_icon.rc.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));

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
    parser.addOption(optSelf);
    parser.addOption(optCli);
    parser.addOption(optPdf);
    parser.addOption(optMath);
    parser.process(app);

    if (parser.isSet(optMath)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return runMathTest();
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