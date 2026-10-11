// 内核调用融合层的无头验证程序。
//
//   kernelclient_test <cliDir> <request.json> [kernel]
//
// 同一份 JSON 请求分别走：
//   ① 内嵌（进程内调库，hostfxr 把内核 DLL 载入本进程）
//   ② 命令行（QProcess 起 {kernel}.exe --json ...）
// 然后比对两条路的 stdout JSON 是否一致、并打印各自的耗时与后端状态。
//
// 这是"两种调用方式融合"的验收手段：GUI 在无头环境跑不起来，而这件事必须有证据。
#include "kernelclient.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextStream>

#include <cstdio>

static QTextStream& err() { static QTextStream s(stderr); return s; }

// 内核输出里带"输出路径"之类会随文件名变化，比对时先做一层归一。
static QString normalize(const QString& json)
{
    QJsonParseError e{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &e);
    if (e.error != QJsonParseError::NoError) return json.simplified();
    // 只比对决定性的计数与统计字段，路径/耗时之类的噪音不参与。
    const QJsonObject o = doc.object();
    const QJsonObject s = o.value(QStringLiteral("summary")).toObject();
    QString summary = QStringLiteral("graphic=%1 text=%2")
                          .arg(s.value(QStringLiteral("graphicDiffs")).toInt())
                          .arg(s.value(QStringLiteral("textDiffs")).toInt());
    summary += QStringLiteral(" matched=%1 added=%2 removed=%3")
                   .arg(s.value(QStringLiteral("matchedPages")).toArray().size())
                   .arg(s.value(QStringLiteral("addedPages")).toArray().size())
                   .arg(s.value(QStringLiteral("removedPages")).toArray().size());
    return summary;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QStringList a = app.arguments();
    if (a.size() < 3)
    {
        err() << "usage: kernelclient_test <cliDir> <request.json> [kernel] [--offscreen-gui-not-needed]\n";
        return 2;
    }
    const QString cliDir  = a.at(1);
    const QString reqPath = a.at(2);
    const QString kernel  = a.size() > 3 ? a.at(3) : QStringLiteral("EGpdf2df");

    QFile rf(reqPath);
    if (!rf.open(QIODevice::ReadOnly))
    {
        err() << "cannot read request: " << reqPath << "\n";
        return 2;
    }
    const QString request = QString::fromUtf8(rf.readAll());
    rf.close();

    KernelClient::setCliDir(cliDir);

    out << "== kernelclient_test ==\n";
    out << "kernel   : " << kernel << "\n";
    out << "cliDir   : " << cliDir << "\n";
    out << "request  : " << reqPath << "\n";
    out << "status   : " << KernelClient::describe(kernel) << "\n\n";

    // 实时日志回调 —— 内嵌模式下内核 stderr 会被即时转到这里（命令行模式没有，因为日志
    // 是进程结束后一次性取的；这正是内嵌的一个额外好处）。
    int liveLines = 0;
    KernelClient::setLiveLogSink([&](const QString& line) {
        if (liveLines < 6) out << "   [live] " << line << "\n";
        ++liveLines;
        out.flush();
    });

    // ---- ① 内嵌（进程内调库）----
    QElapsedTimer t1; t1.start();
    const KernelClient::Result rEmb =
        KernelClient::run(kernel, {}, request, KernelClient::Backend::Embedded);
    const qint64 msEmb = t1.elapsed();
    out << "[embedded] rc=" << rEmb.rc << "  " << msEmb << " ms  out=" << rEmb.out.size()
        << " bytes  liveLog=" << liveLines << " lines\n";
    if (rEmb.rc != 0) out << "   err: " << rEmb.err.left(600) << "\n";

    // ---- ② 命令行（QProcess）----
    QElapsedTimer t2; t2.start();
    const KernelClient::Result rCli =
        KernelClient::run(kernel, {}, request, KernelClient::Backend::Cli);
    const qint64 msCli = t2.elapsed();
    out << "[cli     ] rc=" << rCli.rc << "  " << msCli << " ms  out=" << rCli.out.size() << " bytes\n";
    if (rCli.rc != 0) out << "   err: " << rCli.err.left(600) << "\n";

    // ---- ③ Auto（先内嵌，失败回退命令行）----
    const KernelClient::Result rAuto =
        KernelClient::run(kernel, {}, request, KernelClient::Backend::Auto);
    out << "[auto    ] rc=" << rAuto.rc << "  实际后端=" << rAuto.backend << "\n\n";

    const QString n1 = normalize(rEmb.out);
    const QString n2 = normalize(rCli.out);
    out << "内嵌统计: " << n1 << "\n";
    out << "命令统计: " << n2 << "\n";

    // 命令行后端"根本起不来"要跟"两条路结果不一致"分开报：前者是环境问题
    // （例如沙箱禁止派生子进程），后者才是融合层真的错了。
    const bool cliUnavailable = (rCli.rc == -1
        && (rCli.err.contains(QStringLiteral("failed to start"))
            || rCli.err.contains(QStringLiteral("pipe:"))));
    if (rEmb.rc != 0)
    {
        out << "==> 内嵌后端失败 ✗\n";
        out.flush();
        return 1;
    }
    if (cliUnavailable)
    {
        out << "==> 内嵌后端已通过 ✓；命令行后端在本环境不可用（无法派生子进程），未参与比对\n";
        out.flush();
        return 0;
    }
    const bool same = (rCli.rc == 0 && n1 == n2);
    out << (same ? "==> 两条路结果一致 ✓\n" : "==> 两条路结果不一致 ✗\n");
    out.flush();
    return same ? 0 : 1;
}
