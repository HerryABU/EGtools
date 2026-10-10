#include "clirunner.h"
#include <QProcess>

// CREATE_NO_WINDOW: spawn the child CLI WITHOUT a visible console window on
// Windows. This is the fix for two real bugs: (1) the .NET CLI is a console
// subsystem app, so QProcess::start() would pop a black window that the user
// closes — killing the in-flight render/extract and leaving the PDF "unrendered";
// (2) keeping the child windowless also stops a stray console from stealing focus.
#ifndef CREATE_NO_WINDOW
#define CREATE_NO_WINDOW 0x08000000
#endif

int CliRunner::run(const QString& exe, const QStringList& args,
                   QString& outStdout, QString& outStderr, int timeoutMs)
{
    outStdout.clear();
    outStderr.clear();

    QProcess p;
    p.setProgram(exe);
    p.setArguments(args);
    p.setProcessChannelMode(QProcess::SeparateChannels);

#ifdef Q_OS_WIN
    // Never show a console window for the child kernel (see CREATE_NO_WINDOW).
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif

    p.start();
    if (!p.waitForStarted(timeoutMs)) {
        outStderr = QStringLiteral("failed to start '%1': %2").arg(exe, p.errorString());
        return -1;
    }
    if (!p.waitForFinished(timeoutMs)) {
        // The child hung or was killed (e.g. user closed its window) — we MUST
        // NOT let this take down the front-end. Kill it, report, and return so
        // the caller can show an error instead of crashing/exiting.
        p.kill();
        p.waitForFinished(2000);
        outStderr = QStringLiteral("'%1' timed out or was terminated").arg(exe);
        return -2;
    }
    outStdout = QString::fromLocal8Bit(p.readAllStandardOutput());
    outStderr = QString::fromLocal8Bit(p.readAllStandardError());
    return p.exitCode();
}
