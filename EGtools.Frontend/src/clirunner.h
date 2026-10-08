#pragma once

#include <QString>
#include <QStringList>

// Thin QProcess wrapper to run one of the .NET CLI kernels synchronously.
// The Qt shell owns no PDF logic; everything real happens in the kernels.
class CliRunner
{
public:
    // Runs exe with args. stdout/stderr are captured separately; returns the
    // process exit code (-1 if the process could not be started).
    static int run(const QString& exe, const QStringList& args,
                   QString& outStdout, QString& outStderr, int timeoutMs = 180000);
};