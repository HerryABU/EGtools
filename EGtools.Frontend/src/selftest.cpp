#include "selftest.h"
#include "pdfview.h"
#include "clirunner.h"

#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QApplication>

static QTextStream out(stdout);

static QString exePath(const QString& cliDir, const QString& name)
{
#ifdef EGTOOLS_WIN32
    return QDir(cliDir).filePath(name + QStringLiteral(".exe"));
#else
    return QDir(cliDir).filePath(name);
#endif
}

int runSelfTest(const QString& cliDirIn, const QString& pdfIn)
{
    QString cliDir = cliDirIn.isEmpty() ? qEnvironmentVariable("EGTOOLS_CLI_DIR") : cliDirIn;
    QString pdf    = pdfIn;
    if (pdf.isEmpty()) pdf = qEnvironmentVariable("EGTOOLS_SELFTEST_PDF");
    if (cliDir.isEmpty() || pdf.isEmpty() || !QFile::exists(pdf)) {
        out << "[FAIL] cliDir/pdf missing (cliDir=" << cliDir << " pdf=" << pdf << ")\n";
        out.flush();
        return 2;
    }

    QString err; Q_UNUSED(err);

    // ---- 1. render page 0 via the CLI kernel ----
    QTemporaryDir tmp;
    QString bmp = QDir(tmp.path()).filePath(QStringLiteral("page.bmp"));
    QString so, se;
    int rc = CliRunner::run(exePath(cliDir, QStringLiteral("EGpdf2excel")),
        {QStringLiteral("--render"), QStringLiteral("--page"), QStringLiteral("0"),
         QStringLiteral("--scale"), QStringLiteral("1"),
         QStringLiteral("--render-out"), bmp, pdf}, so, se);
    if (rc != 0) { out << "[FAIL] render rc=" << rc << " " << se << "\n"; out.flush(); return 3; }

    QJsonObject meta = QJsonDocument::fromJson(so.toUtf8()).object();
    double scale = meta.value(QStringLiteral("scale")).toDouble(1.0);
    double pw    = meta.value(QStringLiteral("pageWidthPt")).toDouble();
    double ph    = meta.value(QStringLiteral("pageHeightPt")).toDouble();
    QImage img = QImageReader(bmp).read();
    if (img.isNull()) { out << "[FAIL] cannot read rendered BMP\n"; out.flush(); return 4; }

    // ---- 2. coordinate mapping is derived, not hard-coded ----
    PdfView view;
    view.setPage(img, scale, 0);
    view.setKind(QStringLiteral("global_table"));
    QRectF px(80, 60, 300, 220);
    QRectF pt = view.pxToPt(px);
    QRectF back = view.ptToPx(pt);
    bool mapOk = qAbs(back.x() - px.x()) < 0.01 && qAbs(back.y() - px.y()) < 0.01 &&
                 qAbs(back.width() - px.width()) < 0.01 && qAbs(back.height() - px.height()) < 0.01;
    out << "  page pt=" << pw << "x" << ph << "  img px=" << img.width() << "x" << img.height()
        << "  scale=" << scale << "\n";
    out << "  px->pt->px roundtrip " << (mapOk ? "OK" : "BAD") << "\n";

    // ---- 3. build a RegionRequest from a synthetic selection, call the kernel ----
    QJsonObject bbox;
    bbox.insert(QStringLiteral("x"), pt.x());  bbox.insert(QStringLiteral("y"), pt.y());
    bbox.insert(QStringLiteral("width"), pt.width()); bbox.insert(QStringLiteral("height"), pt.height());
    QJsonObject reg;
    reg.insert(QStringLiteral("page"), 0);
    reg.insert(QStringLiteral("kind"), QStringLiteral("global_table"));
    reg.insert(QStringLiteral("bbox"), bbox);
    QJsonArray regs; regs.append(reg);
    QJsonObject req;
    req.insert(QStringLiteral("pdf"), pdf);
    req.insert(QStringLiteral("pages"), QJsonArray{0});
    req.insert(QStringLiteral("regions"), regs);

    QString reqPath = QDir(tmp.path()).filePath(QStringLiteral("req.json"));
    QFile rf(reqPath);
    if (!rf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { out << "[FAIL] cannot write request\n"; out.flush(); return 8; }
    rf.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    rf.close();

    QString eo, ee;
    int rc2 = CliRunner::run(exePath(cliDir, QStringLiteral("EGpdf2excel")),
        {QStringLiteral("--mode"), QStringLiteral("region"),
         QStringLiteral("--json"), reqPath}, eo, ee);
    if (rc2 != 0) { out << "[FAIL] region rc=" << rc2 << " " << ee << "\n"; out.flush(); return 5; }

    QJsonObject resp = QJsonDocument::fromJson(eo.toUtf8()).object();
    QJsonArray rr = resp.value(QStringLiteral("regions")).toArray();
    bool wordsOk = !rr.isEmpty() && rr[0].toObject().value(QStringLiteral("words")).toArray().size() > 0;
    out << "  region results=" << rr.size() << "  words="
        << (rr.isEmpty() ? 0 : rr[0].toObject().value(QStringLiteral("words")).toArray().size()) << "\n";

    out.flush();
    if (!mapOk) { out << "[FAIL] coordinate roundtrip\n"; out.flush(); return 6; }
    if (!wordsOk) { out << "[FAIL] kernel returned no word segments\n"; out.flush(); return 7; }
    out << "[PASS] selftest: render+map+extract via CLI kernels\n";
    out.flush();
    return 0;
}

int runMathTest()
{
    // Verifies the ONLY coordinate mapping in the app, with no child processes.
    auto near = [](double a, double b){ return qAbs(a - b) < 0.01; };
    int fails = 0;

    // scale 1.0 : px == pt
    {
        QImage img(595, 842, QImage::Format_RGB32);
        img.fill(Qt::white);
        PdfView v; v.setPage(img, 1.0, 0);
        QRectF px(80, 60, 300, 220);
        QRectF pt = v.pxToPt(px);
        bool ok = near(pt.x(),80)&&near(pt.y(),60)&&near(pt.width(),300)&&near(pt.height(),220);
        out << "scale=1.0 px->pt " << (ok?"OK":"BAD") << "\n";
        if (!ok) ++fails;
    }
    // scale 2.0 : pt == px/2
    {
        QImage img(1190, 1684, QImage::Format_RGB32);
        img.fill(Qt::white);
        PdfView v; v.setPage(img, 2.0, 0);
        QRectF px(160, 120, 600, 440);
        QRectF pt = v.pxToPt(px);
        bool ok = near(pt.x(),80)&&near(pt.y(),60)&&near(pt.width(),300)&&near(pt.height(),220);
        out << "scale=2.0 px->pt " << (ok?"OK":"BAD") << "\n";
        if (!ok) ++fails;
        // round trip px->pt->px
        QRectF back = v.ptToPx(pt);
        bool rt = near(back.x(),px.x())&&near(back.y(),px.y())&&near(back.width(),px.width())&&near(back.height(),px.height());
        out << "roundtrip     " << (rt?"OK":"BAD") << "\n";
        if (!rt) ++fails;
    }
    out.flush();
    if (fails) { out << "[FAIL] math test (" << fails << ")\n"; out.flush(); return 1; }
    out << "[PASS] math test: coordinate mapping is derived from render scale, not hard-coded\n";
    out.flush();
    return 0;
}