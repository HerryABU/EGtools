#include "selftest.h"
#include "pdfcanvas.h"
#include "kernelclient.h"

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

// 自检一律走**内核融合层**（内嵌优先，失败回退命令行）。这样即使在没有子进程权限
// 的环境里（沙箱/受限环境），渲染-映射-抽取全链路依然可验证 —— 这正是融合层的价值。
static KernelClient::Result callKernel(const QStringList& args, const QString& json = QString())
{
    return KernelClient::run(QStringLiteral("EGpdf2excel"), args, json,
                             KernelClient::defaultBackend());
}

// ---------------------------------------------------------------------------
// 渲染诊断：把一页渲染出来并统计像素分布
//
// 用途：把"黑屏"从主观描述变成可测量的结论。
//   - 读图失败 / 渲染失败  ⇒ 前端拿不到图像（黑屏在"取图"这一步）
//   - 平均亮度极低、max 也极低 ⇒ 内核真的渲染出了一张黑图（内容/PDF 问题）
//   - 亮度正常但界面看不到   ⇒ 问题在显示侧（GPU/控件/缩放）
// ---------------------------------------------------------------------------
int runRenderTest(const QString& cliDirIn, const QString& pdfIn, int page)
{
    const QString cliDir = cliDirIn.isEmpty() ? qEnvironmentVariable("EGTOOLS_CLI_DIR") : cliDirIn;
    QString pdf = pdfIn;
    if (pdf.isEmpty()) pdf = qEnvironmentVariable("EGTOOLS_SELFTEST_PDF");
    if (!cliDir.isEmpty()) KernelClient::setCliDir(cliDir);
    if (pdf.isEmpty() || !QFile::exists(pdf)) {
        out << "[FAIL] pdf missing (" << pdf << ")\n"; out.flush(); return 2;
    }

    out << "=== 渲染诊断 ===\n";
    out << "PDF: " << pdf << "\n";
    out << "内核: " << KernelClient::describe(QStringLiteral("EGpdf2excel")) << "\n";

    QTemporaryDir tmp;
    const QString bmp = QDir(tmp.path()).filePath(QStringLiteral("page.bmp"));

    const KernelClient::Result kr = callKernel(
        {QStringLiteral("--render"), QStringLiteral("--page"), QString::number(page),
         QStringLiteral("--scale"), QStringLiteral("1"),
         QStringLiteral("--render-out"), bmp, pdf});
    out << "渲染 rc=" << kr.rc << " 后端=" << kr.backend << "\n";
    out << "  stdout 长度=" << kr.out.size() << "  预览=[" << kr.out.left(200) << "]\n";
    out << "  stderr 长度=" << kr.err.size() << "\n";
    if (kr.rc != 0) {
        out << "[FAIL] 渲染失败: " << kr.err.trimmed() << "\n"; out.flush(); return 3;
    }
    const qint64 bmpBytes = QFileInfo(bmp).size();
    out << "BMP: " << bmpBytes << " 字节\n";

    QImageReader reader(bmp);
    QImage img = reader.read();
    if (img.isNull()) {
        out << "[FAIL] 读取渲染图失败: " << reader.errorString()
            << "  文件大小=" << bmpBytes
            << "  识别格式=" << (reader.format().isEmpty() ? "<空>" : QString::fromLatin1(reader.format()))
            << "\n";
        out.flush();
        return 4;
    }
    img = img.copy();

    // 全图统计（按 1/4 抽样即可反映分布，避免大图逐像素开销）。
    quint64 lum = 0; quint64 n = 0; int minL = 255, maxL = 0;
    QHash<int, int> histogram;   // 16 桶
    const int step = 2;
    for (int y = 0; y < img.height(); y += step) {
        for (int x = 0; x < img.width(); x += step) {
            const QRgb c = img.pixel(x, y);
            const int l = (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000;
            lum += static_cast<quint64>(l); ++n;
            minL = qMin(minL, l); maxL = qMax(maxL, l);
            ++histogram[l / 16];
        }
    }
    const int bias = n ? static_cast<int>(lum / n) : 0;
    out << "尺寸: " << img.width() << "x" << img.height()
        << "  格式#: " << static_cast<int>(img.format())
        << "  色深: " << img.depth() << "bpp\n";
    out << "亮度: 平均 " << bias << "  min " << minL << "  max " << maxL << "\n";
    out << "直方图(16档, 第k档=亮度[k*16,(k+1)*16)): ";
    for (int k = 0; k < 16; ++k) out << (histogram.value(k) * 100 / qMax<quint64>(1, n)) << " ";
    out << "(%)\n";

    // JSON 元信息（页尺寸/缩放）也应一致。
    const QJsonObject meta = QJsonDocument::fromJson(kr.out.toUtf8()).object();
    out << "内核元信息: pageWidthPt=" << meta.value(QStringLiteral("pageWidthPt")).toDouble()
        << " pageHeightPt=" << meta.value(QStringLiteral("pageHeightPt")).toDouble()
        << " scale=" << meta.value(QStringLiteral("scale")).toDouble() << "\n";

    const bool allBlack = maxL < 24;
    const bool blank   = (bias > 250 && maxL - minL < 6);
    out << "判定: " << (allBlack ? "全黑（内核渲染出的图就是黑的 → 请看 PDF 内容/背景）"
                                 : (blank ? "全白（页面可能确实无内容）" : "正常（有内容）")) << "\n";
    out.flush();
    if (allBlack) return 5;
    out << "[PASS] render test: 渲染图可读且包含内容\n";
    out.flush();
    return 0;
}

// ---------------------------------------------------------------------------
// 全链路自检（渲染 -> 坐标映射 -> 区域抽取），走融合层
// ---------------------------------------------------------------------------
int runSelfTest(const QString& cliDirIn, const QString& pdfIn)
{
    QString cliDir = cliDirIn.isEmpty() ? qEnvironmentVariable("EGTOOLS_CLI_DIR") : cliDirIn;
    QString pdf    = pdfIn;
    if (pdf.isEmpty()) pdf = qEnvironmentVariable("EGTOOLS_SELFTEST_PDF");
    if (!cliDir.isEmpty()) KernelClient::setCliDir(cliDir);
    if (pdf.isEmpty() || !QFile::exists(pdf)) {
        out << "[FAIL] cliDir/pdf missing (cliDir=" << cliDir << " pdf=" << pdf << ")\n";
        out.flush();
        return 2;
    }

    // ---- 1. 先找一个**有矢量文字**的页，再渲染它 ----
    // 封面/空白页没有文字（--words 返回 0），拿它做断言会得到假失败；
    // 扫描前若干页取第一页有词的，测试才有意义，且对不同 PDF 都稳健。
    QTemporaryDir tmp;
    const QString bmp = QDir(tmp.path()).filePath(QStringLiteral("page.bmp"));
    QJsonArray allWords;
    QString outJson, errText;
    int page = -1;
    for (int p = 0; p < 30; ++p) {
        const KernelClient::Result wr = callKernel(
            {QStringLiteral("--words"), QString::number(p), pdf});
        if (wr.rc != 0) break;                       // 越界即停
        const QJsonArray ws = QJsonDocument::fromJson(wr.out.toUtf8())
                                 .object().value(QStringLiteral("words")).toArray();
        if (!ws.isEmpty()) { page = p; allWords = ws; break; }
    }
    if (page < 0) { out << "[FAIL] no page with vector words found\n"; out.flush(); return 9; }
    out << "  选中页 = " << page << "  矢量词数 = " << allWords.size() << "\n";

    const KernelClient::Result rr = callKernel(
        {QStringLiteral("--render"), QStringLiteral("--page"), QString::number(page),
         QStringLiteral("--scale"), QStringLiteral("1"),
         QStringLiteral("--render-out"), bmp, pdf});
    if (rr.rc != 0) { out << "[FAIL] render rc=" << rr.rc << " " << rr.err << "\n"; out.flush(); return 3; }
    outJson = rr.out;
    errText = rr.err;

    const QJsonObject meta = QJsonDocument::fromJson(outJson.toUtf8()).object();
    const double scale = meta.value(QStringLiteral("scale")).toDouble(1.0);
    const double pw    = meta.value(QStringLiteral("pageWidthPt")).toDouble();
    const double ph    = meta.value(QStringLiteral("pageHeightPt")).toDouble();
    if (scale <= 0.0 || pw <= 0.0 || ph <= 0.0) {
        out << "[FAIL] 渲染元信息缺失（内嵌模式下 stdout 未回传？）out=" << outJson.left(120)
            << " err=" << errText.left(120) << "\n";
        out.flush();
        return 11;
    }
    QImage img = QImageReader(bmp).read();
    if (img.isNull()) { out << "[FAIL] cannot read rendered BMP\n"; out.flush(); return 4; }

    // ---- 2. 坐标映射必须是推导出来的，不能硬编码 ----
    PdfCanvas canvas;
    canvas.setPage(img, scale, page);
    canvas.setKind(QStringLiteral("global_table"));
    const QRectF px(80, 60, 300, 220);
    const QRectF pt0 = canvas.pxToPt(px);
    const QRectF back = canvas.ptToPx(pt0);
    const bool mapOk = qAbs(back.x() - px.x()) < 0.01 && qAbs(back.y() - px.y()) < 0.01 &&
                       qAbs(back.width() - px.width()) < 0.01 && qAbs(back.height() - px.height()) < 0.01;
    out << "  page pt=" << pw << "x" << ph << "  img px=" << img.width() << "x" << img.height()
        << "  scale=" << scale << "\n";
    out << "  px->pt->px roundtrip " << (mapOk ? "OK" : "BAD") << "\n";

    // ---- 3. 用一个**真实文字**的位置构造框选（避免落空导致假失败）----
    const QJsonObject w0 = allWords.first().toObject();
    const double wx0 = w0.value(QStringLiteral("x0")).toDouble();
    const double wy0 = w0.value(QStringLiteral("y0")).toDouble();
    const double wx1 = w0.value(QStringLiteral("x1")).toDouble();
    const double wy1 = w0.value(QStringLiteral("y1")).toDouble();
    // 以该词为中心扩一点，确保框内有内容。
    const QRectF pt(wx0 - 2, wy0 - 2, (wx1 - wx0) + 4, (wy1 - wy0) + 4);
    out << "  取首个词构造框选: pt=[" << pt.x() << "," << pt.y() << " " << pt.width()
        << "x" << pt.height() << "]\n";
    QJsonObject bbox;
    bbox.insert(QStringLiteral("x"), pt.x());  bbox.insert(QStringLiteral("y"), pt.y());
    bbox.insert(QStringLiteral("width"), pt.width()); bbox.insert(QStringLiteral("height"), pt.height());
    QJsonObject reg;
    reg.insert(QStringLiteral("page"), page);
    reg.insert(QStringLiteral("kind"), QStringLiteral("global_table"));
    reg.insert(QStringLiteral("bbox"), bbox);
    QJsonArray regs; regs.append(reg);
    QJsonObject req;
    req.insert(QStringLiteral("pdf"), pdf);
    req.insert(QStringLiteral("pages"), QJsonArray{page});
    req.insert(QStringLiteral("regions"), regs);

    // JSON 通过 stdin 直接交给内核（不落盘）——融合层负责投递方式。
    const KernelClient::Result er = callKernel(
        {QStringLiteral("--mode"), QStringLiteral("region")},
        QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact)));
    if (er.rc != 0) { out << "[FAIL] region rc=" << er.rc << " " << er.err << "\n"; out.flush(); return 5; }

    const QJsonObject resp = QJsonDocument::fromJson(er.out.toUtf8()).object();
    const QJsonArray rarr = resp.value(QStringLiteral("regions")).toArray();
    const bool wordsOk = !rarr.isEmpty()
                         && rarr[0].toObject().value(QStringLiteral("words")).toArray().size() > 0;
    out << "  region results=" << rarr.size() << "  words="
        << (rarr.isEmpty() ? 0 : rarr[0].toObject().value(QStringLiteral("words")).toArray().size()) << "\n";

    out.flush();
    if (!mapOk)   { out << "[FAIL] coordinate roundtrip\n"; out.flush(); return 6; }
    if (!wordsOk) { out << "[FAIL] kernel returned no word segments\n"; out.flush(); return 7; }
    out << "[PASS] selftest: render+map+extract via the kernel fusion layer\n";
    out.flush();
    return 0;
}

// ---------------------------------------------------------------------------
// 纯坐标映射自检（不派生任何子进程，任何环境都能跑）
// ---------------------------------------------------------------------------
int runMathTest()
{
    auto near = [](double a, double b){ return qAbs(a - b) < 0.01; };
    int fails = 0;

    // scale 1.0 / zoom 100% : px == pt
    {
        QImage img(595, 842, QImage::Format_RGB32);
        img.fill(Qt::white);
        PdfCanvas c; c.setPage(img, 1.0, 0);
        const QRectF px(80, 60, 300, 220);
        const QRectF pt = c.pxToPt(px);
        const bool ok = near(pt.x(),80)&&near(pt.y(),60)&&near(pt.width(),300)&&near(pt.height(),220);
        out << "scale=1.0 px->pt " << (ok?"OK":"BAD") << "\n";
        if (!ok) ++fails;
    }
    // scale 2.0 : pt == px/2
    {
        QImage img(1190, 1684, QImage::Format_RGB32);
        img.fill(Qt::white);
        PdfCanvas c; c.setPage(img, 2.0, 0);
        const QRectF px(160, 120, 600, 440);
        const QRectF pt = c.pxToPt(px);
        const bool ok = near(pt.x(),80)&&near(pt.y(),60)&&near(pt.width(),300)&&near(pt.height(),220);
        out << "scale=2.0 px->pt " << (ok?"OK":"BAD") << "\n";
        if (!ok) ++fails;

        const QRectF back = c.ptToPx(pt);
        const bool rt = near(back.x(),px.x())&&near(back.y(),px.y())&&near(back.width(),px.width())&&near(back.height(),px.height());
        out << "roundtrip     " << (rt?"OK":"BAD") << "\n";
        if (!rt) ++fails;
    }
    // zoom 200% 必须参与映射，否则缩放后框选会整体偏掉（GPU/缩放的correctness前提）
    {
        QImage img(595, 842, QImage::Format_RGB32);
        img.fill(Qt::white);
        PdfCanvas c; c.setPage(img, 1.0, 0);
        c.setZoom(2.0);
        const QRectF px(160, 120, 600, 440);       // 2x 显示下的像素
        const QRectF pt = c.pxToPt(px);            // 应折算回 pt：px/(scale*zoom)
        const bool ok = near(pt.x(),80)&&near(pt.y(),60)&&near(pt.width(),300)&&near(pt.height(),220);
        out << "zoom=2.0 px->pt " << (ok?"OK":"BAD") << "\n";
        if (!ok) ++fails;

        const QSize cs = c.canvasSize();           // 画布尺寸 = 图像尺寸 * zoom
        const bool sizeOk = cs.width() == 1190 && cs.height() == 1684;
        out << "zoom canvas size " << (sizeOk?"OK":"BAD") << "\n";
        if (!sizeOk) ++fails;
    }
    out.flush();
    if (fails) { out << "[FAIL] math test (" << fails << ")\n"; out.flush(); return 1; }
    out << "[PASS] math test: coordinate mapping derived from render scale + zoom, not hard-coded\n";
    out.flush();
    return 0;
}
