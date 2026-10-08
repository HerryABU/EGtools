#include "clirunner.h"
#include <QProcess>

int CliRunner::run(const QString& exe, const QStringList& args,
                   QString& outStdout, QString& outStderr, int timeoutMs)
{
    outStdout.clear();
    outStderr.clear();

    QProcess p;
    p.setProgram(exe);
    p.setArguments(args);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start();
    if (!p.waitForStarted(timeoutMs)) {
        outStderr = QStringLiteral("failed to start '%1': %2").arg(exe, p.errorString());
        return -1;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        outStderr = QStringLiteral("'%1' timed out").arg(exe);
        return -2;
    }
    outStdout = QString::fromLocal8Bit(p.readAllStandardOutput());
    outStderr = QString::fromLocal8Bit(p.readAllStandardError());
    return p.exitCode();
}