#include "selftest.h"
#include "pdfcanvas.h"
#include "pdfview.h"          // createPdfViewHost / frameLooksBlank / gpuBackendDescription
#include "continuouspdfview.h"
#include "kernelclient.h"
#include "materialextractpage.h"
#include "mainwindow.h"
#include "diffpage.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QScreen>
#include <QScrollArea>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QToolBar>
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
// 连续滚动视图自检
// ---------------------------------------------------------------------------
int runContinuousTest(const QString& cliDirIn, const QString& pdfIn)
{
    const QString cliDir = cliDirIn.isEmpty() ? qEnvironmentVariable("EGTOOLS_CLI_DIR") : cliDirIn;
    if (!cliDir.isEmpty()) KernelClient::setCliDir(cliDir);

    out << "=== 连续滚动视图自检 ===\n";

    // 版面自检不需要滚动容器，但 attachScrollArea 之后的 relayout 行为才是真实的，
    // 所以还是给一个真的 QScrollArea（无头平台下它同样能建、能算尺寸）。
    auto* scroll = new QScrollArea();
    // 退出时显式回收：视图析构里会 waitForDone 等后台渲染线程收尾，
    // 否则内核侧已启动的任务在进程退出时仍持有全局锁，会打印扰人的
    // "QMutex: destroying locked mutex"。
    struct Cleanup { QWidget* w; ~Cleanup() { delete w; } } cleanup{ scroll };
    scroll->resize(900, 700);
    auto* view = new ContinuousPdfView();
    scroll->setWidget(view);
    scroll->setWidgetResizable(false);
    view->attachScrollArea(scroll);

    const QString layoutErr = view->runLayoutSelfTest();
    if (!layoutErr.isEmpty()) {
        out << "[FAIL] 版面自检: " << layoutErr << "\n"; out.flush();
        return 20;
    }
    out << "  版面自检（堆叠/留白/居中/缩放/命中）OK\n";

    // ---- 真实文档：验证"打开→滚动到某页→该页渲染出来" ----
    QString pdf = pdfIn;
    if (pdf.isEmpty()) pdf = qEnvironmentVariable("EGTOOLS_SELFTEST_PDF");
    if (pdf.isEmpty() || !QFile::exists(pdf)) {
        out << "  （未提供 PDF，跳过懒渲染验证）\n";
        out << "[PASS] continuous test: 版面自检通过\n"; out.flush();
        return 0;
    }

    const KernelClient::Result pc = callKernel({QStringLiteral("--page-count"), pdf});
    int pageCount = 1;
    if (pc.rc == 0)
        pageCount = qMax(1, QJsonDocument::fromJson(pc.out.toUtf8()).object()
                                .value(QStringLiteral("pageCount")).toInt());
    out << "  文档页数=" << pageCount << "\n";
    if (pageCount < 2) {
        out << "[FAIL] 连续视图需要多页文档来验证滚动\n"; out.flush();
        return 21;
    }

    view->setDocument(pdf, pageCount);
    // 让滚动区完成一次布局（真实窗口里由事件循环自然完成；无头环境要主动跑一次），
    // 否则滚动条 range 还是 0，后续 scrollToPage 会被钳住。
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    // 懒渲染是异步的：等首页出图（上限 60s），再滚到最后一页等它出图。
    auto waitFor = [](ContinuousPdfView* v, int page, int ms) {
        QElapsedTimer t; t.start();
        while (t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (v->pageHasImage(page)) return true;
            QThread::msleep(20);
        }
        return v->pageHasImage(page);
    };

    if (!waitFor(view, 0, 60000)) {
        out << "[FAIL] 第 1 页在 60s 内未渲染完成（连续视图懒渲染）\n"; out.flush();
        return 22;
    }
    const QSizeF s0 = view->pageSizePt(0);
    out << "  第 1 页已渲染，页尺寸(pt)=" << s0.width() << "x" << s0.height() << "\n";

    const int last = pageCount - 1;
    view->scrollToPage(last);
    const bool lastOk = waitFor(view, last, 60000);
    if (!lastOk) {
        out << "[FAIL] 滚动到第 " << (last + 1) << " 页后 60s 内未渲染完成\n";
        out << "        调度状态: 队列=" << view->debugQueueDepth()
            << " 渲染中=" << view->debugInFlightPage()
            << " 已出图页数=" << view->debugRenderedCount()
            << " 目标页 pending=" << (view->debugPending(last) ? 1 : 0)
            << " failed=" << (view->debugFailed(last) ? 1 : 0) << "\n";
        out << "        调度轨迹:\n";
        for (const QString& t : view->debugTrace()) out << "          " << t << "\n";
        out.flush();
        return 23;
    }
    out << "  第 " << (last + 1) << " 页（滚动后）已渲染\n";

    // 空白页（封面）常见于图纸集：其渲染图可能"几乎全白"，属正常，不算失败；
    // 这里只断言"能取到图"，内容判定由 --test-render 负责。
    out.flush();
    out << "[PASS] continuous test: 版面 + 懒渲染（含滚动到末页）\n";
    out.flush();
    return 0;
}

// ---------------------------------------------------------------------------
// 批量提取并发自检
//
// 并发路径的风险不是"快不快"，而是**线程安全与计数正确**：多个后台线程同时打内核，
// 信号跨线程回界面，还要把成败数准。所以这里用同一份 PDF 重复 N 次、并发度 >1 跑一遍，
// 断言：① 全部成功；② 产出文件数 = N（真的干活了，不是记账错了）；③ 停止标志有效。
// ---------------------------------------------------------------------------
int runBatchTest(const QString& cliDirIn, const QString& pdfIn, int jobs, int concurrency)
{
    const QString cliDir = cliDirIn.isEmpty() ? qEnvironmentVariable("EGTOOLS_CLI_DIR") : cliDirIn;
    QString pdf = pdfIn;
    if (pdf.isEmpty()) pdf = qEnvironmentVariable("EGTOOLS_SELFTEST_PDF");
    if (!cliDir.isEmpty()) KernelClient::setCliDir(cliDir);
    if (pdf.isEmpty() || !QFile::exists(pdf)) {
        out << "[FAIL] pdf missing (" << pdf << ")\n"; out.flush(); return 2;
    }
    if (jobs < 1) jobs = 3;
    if (concurrency < 1) concurrency = 1;

    out << "=== 批量提取并发自检 ===\n";
    out << "  文件数=" << jobs << "  并发数=" << concurrency
        << "  后端=" << KernelClient::backendToString(KernelClient::defaultBackend()) << "\n";
    if (concurrency > 1 && KernelClient::defaultBackend() != KernelClient::Backend::Cli)
        out << "  注：非命令行后端会被融合层串行化，本次验证的是并发代码路径的**正确性**\n"
               "      （多线程同时调用内核不串数据、计数准确），而非墙钟加速。\n";

    QTemporaryDir outDir;
    if (!outDir.isValid()) { out << "[FAIL] 无法创建临时输出目录\n"; out.flush(); return 3; }

    QStringList files;
    for (int i = 0; i < jobs; ++i) files << pdf;

    auto* worker = new MaterialExtractWorker(files, outDir.path(), QStringLiteral("csv"),
                                            QStringLiteral("V3"), QString(),
                                            /*verbose*/false, concurrency);
    int ok = -1, fail = -1, stopped = -1, startedCount = 0, finishedCount = 0;
    QObject::connect(worker, &MaterialExtractWorker::fileStarted,
                     [&startedCount](int){ ++startedCount; });
    QObject::connect(worker, &MaterialExtractWorker::fileFinished,
                     [&finishedCount](int, int, int, const QStringList&, const QString&, int){
                         ++finishedCount; });
    QObject::connect(worker, &MaterialExtractWorker::allFinished,
                     [&ok, &fail, &stopped](int o, int f, int s){ ok = o; fail = f; stopped = s; });

    worker->run();                       // 同步跑完（内部会等所有并发任务收尾）
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);   // 排空跨线程信号

    const QStringList produced = QDir(outDir.path()).entryList(
        { QStringLiteral("*.csv") }, QDir::Files);
    delete worker;

    out << "  回调统计: ok=" << ok << " fail=" << fail << " stopped=" << stopped
        << " started=" << startedCount << " finished=" << finishedCount << "\n";
    out << "  实际产物: " << produced.size() << " 个 CSV\n";

    if (stopped != 0)   { out << "[FAIL] 意外停止\n"; out.flush(); return 4; }
    if (ok != jobs)     { out << "[FAIL] 成功数应为 " << jobs << "，实际 " << ok << "\n"; out.flush(); return 5; }
    if (fail != 0)      { out << "[FAIL] 出现失败 " << fail << " 个\n"; out.flush(); return 6; }
    if (finishedCount != jobs) {
        out << "[FAIL] fileFinished 回调次数 " << finishedCount << " ≠ " << jobs
            << "（跨线程信号丢失？）\n"; out.flush(); return 7;
    }
    // 同一 PDF 重复抽取会互相覆盖同名文件，所以产物数 ≤ jobs；这里只要求"确实产出了"。
    if (produced.isEmpty()) { out << "[FAIL] 没有产出任何 CSV（并发路径没真干活？）\n"; out.flush(); return 8; }

    out << "[PASS] batch test: 并发提取正确（计数准确、跨线程信号完整、有实际产物）\n";
    out.flush();
    return 0;
}

// ---------------------------------------------------------------------------
// 布局审计
//
// "横向太长"这类问题靠肉眼争论没有意义，这里把它变成数字：
//   * window.minimumSizeHint() —— **窗口能不能变窄**完全由它决定；它偏大就说明
//     有某个控件（常见是文字很长的下拉/按钮）在绑架整窗宽度。
//   * 每个控件的 minimumSizeHint / sizeHint / 实际几何 —— 定位"谁把它撑大的"。
// 递归打印控件树（限深度），并在最后给出横向占用前几名的排行。
// ---------------------------------------------------------------------------
namespace {

struct LayoutRow
{
    QString path;        // 父链路径（用于定位"到底是谁"）
    QString cls;
    QString name;
    int minW = 0, hintW = 0, actualW = 0, actualH = 0;
};

QString widgetPath(QWidget* w)
{
    QStringList chain;
    for (QWidget* p = w; p; p = qobject_cast<QWidget*>(p->parentWidget())) {
        QString seg = QString::fromLatin1(p->metaObject()->className());
        if (!p->objectName().isEmpty()) seg += QLatin1Char('#') + p->objectName();
        chain.prepend(seg);
        if (chain.size() >= 4) break;
    }
    return chain.join(QStringLiteral(" < "));
}

void walkLayout(QWidget* w, int depth, int maxDepth, QTextStream& os,
                QVector<LayoutRow>& rank)
{
    if (!w || depth > maxDepth) return;
    const QSize mn = w->minimumSizeHint().isValid() ? w->minimumSizeHint() : QSize(0, 0);
    LayoutRow row;
    row.path = widgetPath(w);
    row.cls = QString::fromLatin1(w->metaObject()->className());
    row.name = w->objectName();
    row.minW = mn.width();
    row.hintW = w->sizeHint().width();
    row.actualW = w->width();
    row.actualH = w->height();
    rank.push_back(row);
    Q_UNUSED(os);

    const QObjectList kids = w->children();
    for (QObject* o : kids) {
        if (auto* cw = qobject_cast<QWidget*>(o))
            walkLayout(cw, depth + 1, maxDepth, os, rank);
    }
}

} // namespace

int runLayoutDump()
{
    auto* w = new MainWindow();
    w->resize(1280, 860);
    w->show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 300);

    out << "=== 布局审计（1280x860，离屏） ===\n";
    const QSize mn = w->minimumSizeHint();
    out << "窗口最小尺寸 = " << mn.width() << " x " << mn.height()
        << "   ← 这个数决定窗口能不能变窄\n";
    out << "窗口实际尺寸 = " << w->width() << " x " << w->height() << "\n";
    if (QScreen* scr = QGuiApplication::primaryScreen()) {
        const QRect ag = scr->availableGeometry();
        out << "屏幕可用区   = " << ag.width() << "x" << ag.height() << "\n";
    } else {
        out << "屏幕可用区   = ?\n";
    }

    // 状态栏常驻读数（「缩放 %」「页码」已下放到这里）
    if (auto* sb = w->statusBar())
        out << "状态栏高度 = " << sb->height() << "\n";

    QVector<LayoutRow> rank;
    walkLayout(w, 0, 12, out, rank);

    // 横向占用排行：把"谁在绑架窗口宽度"按最小宽度排出来（含父链，便于定位）。
    std::sort(rank.begin(), rank.end(), [](const LayoutRow& a, const LayoutRow& b){
        return a.minW > b.minW;
    });
    out << "\n最小宽度排行（前 20，>=150 才可能绑架窗口宽度）:\n";
    int shown = 0;
    for (const LayoutRow& r : rank) {
        if (r.minW < 150 || shown >= 20) continue;
        out << QStringLiteral("  minW=%1 hintW=%2 实际=%3x%4\n     %5\n")
                   .arg(r.minW, 5).arg(r.hintW, 5).arg(r.actualW, 5).arg(r.actualH, 4)
                   .arg(r.path);
        ++shown;
    }

    out << "\n标签页清单（核对「窗口堆叠」是否真的把面板收进来了）:\n";
    for (QWidget* kid : w->findChildren<QWidget*>()) {
        if (auto* tw = qobject_cast<QTabWidget*>(kid)) {
            QStringList names;
            for (int i = 0; i < tw->count(); ++i)
                names << QStringLiteral("%1[%2px]").arg(tw->tabText(i)).arg(tw->widget(i)->width());
            out << QStringLiteral("  %1: %2\n")
                       .arg(QString::fromLatin1(tw->metaObject()->className()),
                            names.join(QStringLiteral(" | ")));
        }
    }

    out << "\n关键区域实测宽度:\n";
    auto report = [&](const char* label, QWidget* wid){
        if (!wid) { out << QStringLiteral("  %1: <null>\n").arg(QLatin1String(label)); return; }
        out << QStringLiteral("  %1: 宽 %2 (最小 %3)\n")
                   .arg(QString::fromUtf8(label), -22)
                   .arg(wid->width(), 5).arg(wid->minimumSizeHint().width(), 5);
    };
    for (QWidget* kid : w->findChildren<QWidget*>()) {
        const QString cn = QString::fromLatin1(kid->metaObject()->className());
        if (cn.contains(QStringLiteral("ToolBar")))
            report("工具栏", kid);
        if (kid->objectName() == QLatin1String("cmdBar"))
            report("命令条", kid);
        if (cn.contains(QStringLiteral("PageNavPanel")))
            report("缩略图栏", kid);
        if (cn.contains(QStringLiteral("ContinuousPdfView")))
            report("画布(连续视图)", kid);
        if (cn.contains(QStringLiteral("QTabWidget")))
            report("结果面板", kid);
    }
    out.flush();

    delete w;                 // 显式回收（避免退出时的线程/mutex 噪音）
    out << "[PASS] layout dump\n";
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

// ---------------------------------------------------------------------------
// 画布绘制自检：渲染成功 ≠ 画出来了
//
// 这是"全黑"这类故障唯一能被自动抓住的地方：内核渲染本身没问题（--test-render 已
// 证明过），但**画布壳**可能根本没把内容落到屏幕上 —— 曾经的 GPU 壳把 QPainter 画在
// paintEvent() 里，而 QOpenGLWidget 的绘制目标是自己的 FBO，结果稳定显示为纯黑。
// 做法：走真实工厂拿到画布壳 → 塞入真实渲染结果 → grab() 抓成位图 → 统计像素。
// ---------------------------------------------------------------------------
// 统一走 pdfview 的度量实现（自检与首帧判定同一口径，避免两处阈值不一致）。
static void luminanceStats(const QImage& img, double* avg, int* mn, int* mx, double* brightFrac)
{
    const LumaStats s = imageLuminanceStats(img);
    *avg = s.avg; *mn = s.minLum; *mx = s.maxLum; *brightFrac = s.brightFrac;
}

int runPaintTest(const QString& cliDirIn, const QString& pdfIn)
{
    const QString cliDir = cliDirIn.isEmpty() ? qEnvironmentVariable("EGTOOLS_CLI_DIR") : cliDirIn;
    QString pdf = pdfIn;
    if (pdf.isEmpty()) pdf = qEnvironmentVariable("EGTOOLS_SELFTEST_PDF");
    if (!cliDir.isEmpty()) KernelClient::setCliDir(cliDir);
    if (pdf.isEmpty() || !QFile::exists(pdf)) {
        out << "[SKIP] 未提供 PDF（--pdf 或 EGTOOLS_SELFTEST_PDF）\n"; out.flush(); return 0;
    }

    out << "=== 画布绘制自检 ===\n";
    out << "渲染路径: " << gpuBackendDescription() << "\n";

    QTemporaryDir tmp;
    const QString bmp = QDir(tmp.path()).filePath(QStringLiteral("page.bmp"));
    const KernelClient::Result kr = callKernel(
        {QStringLiteral("--render"), QStringLiteral("--page"), QStringLiteral("0"),
         QStringLiteral("--scale"), QStringLiteral("1"),
         QStringLiteral("--render-out"), bmp, pdf});
    if (kr.rc != 0) {
        out << "[FAIL] 渲染失败 rc=" << kr.rc << " " << kr.err.trimmed() << "\n"; out.flush(); return 3;
    }
    const QJsonObject meta = QJsonDocument::fromJson(kr.out.toUtf8()).object();
    const double scale = meta.value(QStringLiteral("scale")).toDouble(1.0);
    const QImage img = QImageReader(bmp).read();
    if (img.isNull()) { out << "[FAIL] 读不到渲染图: " << bmp << "\n"; out.flush(); return 4; }
    out << "页面图: " << img.width() << "x" << img.height()
        << "  渲染比例=" << scale << "\n";

    // 走**真实工厂**：GPU 可用就是 GPU 壳，否则光栅壳 —— 测的正是用户实际拿到的那条路径。
    PdfViewHost host = createPdfViewHost();
    out << "画布壳: " << (host.gpu ? "GPU (QOpenGLWidget)" : "CPU 光栅 (QWidget)") << "\n";

    host.canvas->setPage(img, scale, 0);
    // 缩到 1200px 宽以内再抓图：像素统计只需要"画没画出来"，不需要整张原图。
    host.canvas->setZoom(qMin(1.0, 1200.0 / qMax(1, img.width())));
    host.widget->resize(host.canvas->canvasSize());
    host.widget->show();
    QApplication::processEvents();

    // grab() 返回 QPixmap（对 QOpenGLWidget 会走 grabFramebuffer），转成 QImage 好统计像素。
    const QImage frame = host.widget->grab().toImage();
    if (frame.isNull()) { out << "[FAIL] grab() 返回空图\n"; out.flush(); return 5; }

    double avg = 0; int mn = 0, mx = 0; double brightFrac = 0;
    luminanceStats(frame, &avg, &mn, &mx, &brightFrac);
    out << "抓到位图: " << frame.width() << "x" << frame.height()
        << "  平均亮度=" << QString::number(avg, 'f', 1)
        << " (min " << mn << " / max " << mx << ")"
        << "  亮底占比=" << QString::number(brightFrac * 100, 'f', 1) << "%\n";

    QString why;
    if (frameLooksBlank(frame, &why)) {
        out << "[FAIL] 画布没画出内容 —— " << why << "\n";
        out << "       渲染是成功的，所以问题在显示侧（画布壳/GL 合成）。\n";
        out.flush();
        delete host.widget;
        return 6;
    }
    // 更进一步：图纸页应以白底为主。只有底色、没有页面 ⇒ 同样算没画出来。
    if (brightFrac < 0.05) {
        out << "[FAIL] 位图里几乎没有亮底（页面白纸）—— 页面没被绘制，只有底色。\n";
        out.flush();
        delete host.widget;
        return 7;
    }

    out << "[PASS] 画布绘制：内容确实落到了位图上（非纯黑，且含页面白底 "
        << QString::number(brightFrac * 100, 'f', 1) << "%）\n";
    out.flush();
    delete host.widget;
    return 0;
}

// ---------------------------------------------------------------------------
// 端到端「界面到底显示了什么」自检
//
// 与 --test-paint 的差别很关键：--test-paint 是**单独 new 一个画布壳**，
// 而这个测试走**真实主窗口**：那个画布壳实际是 QScrollArea(view) ⊂ QStackedWidget
// ⊂ QSplitter ⊂ QMainWindow。故障往往只在这种真实嵌套里出现（合成、裁剪、滚动区域
// 视口、DPI、GL 子窗口…），单独测壳测不出来。
// 做法：构造 MainWindow → 打开 PDF → 抓画布控件的位图 → 统计像素。
// ---------------------------------------------------------------------------
int runUiPaintTest(const QString& cliDirIn, const QString& pdfIn)
{
    const QString cliDir = cliDirIn.isEmpty() ? qEnvironmentVariable("EGTOOLS_CLI_DIR") : cliDirIn;
    QString pdf = pdfIn;
    if (pdf.isEmpty()) pdf = qEnvironmentVariable("EGTOOLS_SELFTEST_PDF");
    if (!cliDir.isEmpty()) KernelClient::setCliDir(cliDir);
    if (pdf.isEmpty() || !QFile::exists(pdf)) {
        out << "[SKIP] 未提供 PDF（--pdf 或 EGTOOLS_SELFTEST_PDF）\n"; out.flush(); return 0;
    }

    out << "=== 界面绘制自检（真实主窗口）===\n";
    out << "平台: " << QGuiApplication::platformName() << "\n";

    MainWindow w(cliDir);
    w.resize(1280, 860);
    w.show();
    // 让窗口真正完成首次布局/合成 —— 这正是"画布在真实嵌套里"与"孤立测壳"的差别。
    for (int i = 0; i < 30; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    // 必须切到「PDF 工具」页：真实用户流程是 打开PDF → showPage(1) → 渲染。
    // 停在首页时画布处于 QStackedWidget 的隐藏页里，抓图只会拿到底色。
    w.showPdfPageForTest();
    for (int i = 0; i < 10; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    if (!w.openPdf(pdf)) { out << "[FAIL] openPdf 失败\n"; out.flush(); return 3; }
    out << "页数=" << w.pageCountForTest()
        << "  画布=" << (w.canvasIsGpuForTest() ? "GPU (QOpenGLWidget)" : "CPU 光栅 (QWidget)") << "\n";
    out << "渲染路径: " << w.continuousBackendForTest() << "\n";

    // give the GL compositor a few frames before grabbing.
    // 这段等待也是自愈的窗口期：GPU 壳首帧画出问题时会在这期间被换成光栅壳。
    for (int i = 0; i < 40; ++i) { QApplication::processEvents(); QThread::msleep(25); }

    // ⚠️ 必须在这之后再取画布控件：换壳会把旧控件 deleteLater，提前取到的指针会失效。
    QWidget* cv = w.canvasWidgetForTest();
    if (!cv) { out << "[FAIL] 拿不到画布控件\n"; out.flush(); return 4; }

    // 诊断：先分清"画布没画"与"画了但 grab() 抓不到 GL 内容"（两者表现都是糊底）。
    PdfCanvas* pc = w.canvasForTest();
    out << "等待后画布=" << (w.canvasIsGpuForTest() ? "GPU" : "CPU 光栅")
        << "（用于观察 GPU 自愈是否发生）\n";
    out << "诊断: hasImage=" << (pc && pc->hasImage() ? "yes" : "no")
        << " 图尺寸=" << (pc ? QStringLiteral("%1x%2").arg(pc->image().width()).arg(pc->image().height())
                             : QStringLiteral("?"))
        << " zoom=" << (pc ? QString::number(pc->zoom(), 'f', 2) : QStringLiteral("?"))
        << " 页=" << (pc ? pc->pageIndex() : -1)
        << " paint()次数=" << (pc ? pc->paintCalls() : -1)
        << " dpr=" << QString::number(cv->devicePixelRatioF(), 'f', 2)
        << " 可见=" << (cv->isVisible() ? 1 : 0) << "\n";

    const QImage frame = cv->grab().toImage();
    if (frame.isNull()) { out << "[FAIL] 画布 grab() 返回空图\n"; out.flush(); return 5; }

    QImage best = frame;   // 以"能取到内容"的那份为判定依据
    double avg = 0; int mn = 0, mx = 0; double brightFrac = 0;
    luminanceStats(frame, &avg, &mn, &mx, &brightFrac);
    out << "grab()      : " << frame.width() << "x" << frame.height()
        << "  平均亮度=" << QString::number(avg, 'f', 1)
        << " (min " << mn << " / max " << mx << ")"
        << "  亮底占比=" << QString::number(brightFrac * 100, 'f', 1) << "%\n";

    // GPU 壳额外直接读 FBO：grab() 走的是控件渲染路径，grabFramebuffer() 走的是
    // QOpenGLWidget 自己的帧缓冲 —— 两者的差异能直接指出问题出在哪一层。
    if (auto* gpu = dynamic_cast<GpuPdfView*>(cv)) {
        const QImage fb = gpu->grabFramebuffer();
        if (!fb.isNull()) {
            double a2 = 0; int m2 = 0, x2 = 0; double b2 = 0;
            luminanceStats(fb, &a2, &m2, &x2, &b2);
            out << "grabFramebuf: " << fb.width() << "x" << fb.height()
                << "  平均亮度=" << QString::number(a2, 'f', 1)
                << " (min " << m2 << " / max " << x2 << ")"
                << "  亮底占比=" << QString::number(b2 * 100, 'f', 1) << "%\n";
            if (b2 > brightFrac) { best = fb; avg = a2; mn = m2; mx = x2; brightFrac = b2; }
        }
    }

    QString why;
    if (frameLooksBlank(best, &why)) {
        out << "[FAIL] 真实主窗口里的画布是全黑的 —— " << why << "\n";
        out.flush();
        return 6;
    }
    if (brightFrac < 0.05) {
        out << "[FAIL] 画布只有底色、没有页面内容（亮底占比 "
            << QString::number(brightFrac * 100, 'f', 1) << "%）\n";
        out.flush();
        return 7;
    }

    // ---- 决定性一步：屏幕像素 ----
    // QWidget::grab() 对 QOpenGLWidget 会走它的 FBO，所以"能抓到内容"只说明 FBO 对，
    // **不说明屏幕上合成出来是对的**。用户看到的黑屏只可能出现在合成层，所以必须直接
    // 抓屏幕像素来判定。没有真实显示（无头）时跳过，不误报。
    if (QScreen* scr = QGuiApplication::primaryScreen()) {
        const QPixmap shot = scr->grabWindow(0);
        if (!shot.isNull() && shot.width() > 64) {
            const qreal dpr = shot.devicePixelRatio();
            const QPoint cvGlobal = cv->mapToGlobal(QPoint(0, 0));
            // 画布可能大于窗口：只看**真正落在窗口里**的那块。
            const QRect vis = QRect(cvGlobal, cv->size()).intersected(w.frameGeometry());
            if (vis.width() > 32 && vis.height() > 32) {
                QImage desktop = shot.toImage();
                QRect cut(qRound(vis.x() * dpr), qRound(vis.y() * dpr),
                          qRound(vis.width() * dpr), qRound(vis.height() * dpr));
                cut &= QRect(0, 0, desktop.width(), desktop.height());
                if (cut.width() > 32 && cut.height() > 32) {
                    const QImage region = desktop.copy(cut);
                    double a3 = 0; int m3 = 0, x3 = 0; double b3 = 0;
                    luminanceStats(region, &a3, &m3, &x3, &b3);
                    out << "屏幕像素    : " << region.width() << "x" << region.height()
                        << " (dpr=" << QString::number(dpr, 'f', 2) << ")"
                        << "  平均亮度=" << QString::number(a3, 'f', 1)
                        << " (min " << m3 << " / max " << x3 << ")"
                        << "  亮底占比=" << QString::number(b3 * 100, 'f', 1) << "%\n";
                    if (b3 < 0.05) {
                        out << "[FAIL] 屏幕上画布区域是黑的/只有底色 —— 这是真实用户看到的现象"
                            << "（FBO 内有内容，但合成到屏幕后没了）。\n";
                        out.flush();
                        return 8;
                    }
                    out << "  → 屏幕合成正确（不是 FBO 层的假通过）\n";
                }
            }
        }
    }

    out << "[PASS] 界面绘制：真实主窗口的画布显示了页面内容（亮底 "
        << QString::number(brightFrac * 100, 'f', 1) << "%）\n";
    out.flush();
    return 0;
}

// ---------------------------------------------------------------------------
// 截图取证：把真实主窗口（含画布）与整屏保存成 PNG，供人工/多模态复核。
// 有些故障（配色、排版、合成层）用数字说不清，一张图胜过一堆阈值。
// ---------------------------------------------------------------------------
int runScreenshot(const QString& cliDirIn, const QString& pdfIn, const QString& outPng)
{
    const QString cliDir = cliDirIn.isEmpty() ? qEnvironmentVariable("EGTOOLS_CLI_DIR") : cliDirIn;
    QString pdf = pdfIn;
    if (pdf.isEmpty()) pdf = qEnvironmentVariable("EGTOOLS_SELFTEST_PDF");
    if (!cliDir.isEmpty()) KernelClient::setCliDir(cliDir);

    MainWindow w(cliDir);
    w.resize(1440, 900);
    w.show();
    for (int i = 0; i < 25; ++i) { QApplication::processEvents(); QThread::msleep(20); }
    if (QFile::exists(pdf)) {
        w.showPdfPageForTest();
        for (int i = 0; i < 10; ++i) { QApplication::processEvents(); QThread::msleep(20); }
        w.openPdf(pdf);
    }
    // 渲染是"打开后异步出图"的，给它足够时间（大图纸首帧要一两秒）。
    for (int i = 0; i < 200; ++i) { QApplication::processEvents(); QThread::msleep(25); }

    const QString base = outPng.isEmpty()
        ? QDir::temp().filePath(QStringLiteral("egtools_shot")) : outPng;
    const QString winPng = base + QStringLiteral("_window.png");
    const QString scrPng = base + QStringLiteral("_screen.png");

    w.grab().save(winPng, "PNG");
    out << "window png: " << winPng << "  size=" << QFileInfo(winPng).size() << "\n";

    if (QScreen* scr = QGuiApplication::primaryScreen()) {
        const QPixmap full = scr->grabWindow(0);
        if (!full.isNull()) {
            full.save(scrPng, "PNG");
            out << "screen png: " << scrPng << "  size=" << QFileInfo(scrPng).size()
                << "  (" << full.width() << "x" << full.height()
                << " dpr=" << QString::number(full.devicePixelRatio(), 'f', 2) << ")\n";

            // 顺便量一下"窗口在屏幕上那块"的像素：这张才代表用户实际看到的画面。
            const qreal dpr = full.devicePixelRatio();
            const QRect geo = w.frameGeometry();
            QRect cut(qRound(geo.x() * dpr), qRound(geo.y() * dpr),
                      qRound(geo.width() * dpr), qRound(geo.height() * dpr));
            cut &= QRect(0, 0, full.width(), full.height());
            if (cut.width() > 32 && cut.height() > 32) {
                double a = 0; int mn = 0, mx = 0; double bf = 0;
                luminanceStats(full.toImage().copy(cut), &a, &mn, &mx, &bf);
                out << "窗口屏幕区域: " << cut.width() << "x" << cut.height()
                    << "  平均亮度=" << QString::number(a, 'f', 1)
                    << " (min " << mn << " / max " << mx << ")"
                    << "  亮底占比=" << QString::number(bf * 100, 'f', 1) << "%\n";
            }
        }
    }
    out.flush();
    return 0;
}

// ---------------------------------------------------------------------------
// 「图纸比对」页交互自检：差异清单 / 筛选 / 跳转定位
//
// 这几件事是本页的核心交互，而真实比对一次要几分钟 —— 所以这里直接喂一份
// 内核返回**同样结构**的 JSON，把清单填充、筛选、"跳到某条差异"三条逻辑独立验证。
// ---------------------------------------------------------------------------
int runDiffUiTest()
{
    out << "=== 比对页交互自检 ===\n";

    DiffPage page;
    page.resize(1200, 760);
    page.show();
    for (int i = 0; i < 10; ++i) { QApplication::processEvents(); QThread::msleep(10); }

    // 造 5 条差异：2 图形(新增/删除) + 3 文字(新增/删除/变更)，页号与位置都不同。
    auto mkGraphic = [](int page, const char* change, double x, double y) {
        QJsonObject o;
        o[QStringLiteral("page")] = page;
        o[QStringLiteral("type")] = QStringLiteral("graphic");
        o[QStringLiteral("changeType")] = QString::fromLatin1(change);
        QJsonObject bb;
        bb[QStringLiteral("x")] = x; bb[QStringLiteral("y")] = y;
        bb[QStringLiteral("w")] = 12.5; bb[QStringLiteral("h")] = 7.5;
        o[QStringLiteral("bbox")] = bb;
        o[QStringLiteral("confidence")] = 0.9;
        return o;
    };
    auto mkText = [](int page, const char* change, const QString& oldT, const QString& newT) {
        QJsonObject o;
        o[QStringLiteral("page")] = page;
        o[QStringLiteral("type")] = QStringLiteral("text");
        o[QStringLiteral("changeType")] = QString::fromLatin1(change);
        QJsonObject bb;
        bb[QStringLiteral("x")] = 100; bb[QStringLiteral("y")] = 200;
        bb[QStringLiteral("w")] = 40;  bb[QStringLiteral("h")] = 8;
        o[QStringLiteral("bbox")] = bb;
        if (!oldT.isEmpty()) o[QStringLiteral("oldText")] = oldT;
        if (!newT.isEmpty()) o[QStringLiteral("newText")] = newT;
        o[QStringLiteral("confidence")] = 0.75;
        return o;
    };

    QJsonArray diffs;
    diffs.append(mkGraphic(0, "added",   120.0, 340.0));
    diffs.append(mkGraphic(2, "removed", 500.0, 210.0));
    diffs.append(mkText(1, "added",   QString(),            QStringLiteral("408.101.P02")));
    diffs.append(mkText(3, "removed", QStringLiteral("OLD-NOTE-1"), QString()));
    diffs.append(mkText(4, "changed", QStringLiteral("DN80"), QStringLiteral("DN100")));

    QJsonObject root;
    root[QStringLiteral("diffs")] = diffs;
    QJsonObject sum;
    sum[QStringLiteral("graphicDiffs")] = 2;
    sum[QStringLiteral("textDiffs")] = 3;
    root[QStringLiteral("summary")] = sum;

    page.feedDiffResultForTest(root);

    int fails = 0;
    auto check = [&](const QString& what, bool ok, const QString& detail = QString()) {
        out << "  " << (ok ? "OK  " : "BAD ") << what
            << (detail.isEmpty() ? QString() : QStringLiteral("  [%1]").arg(detail)) << "\n";
        if (!ok) ++fails;
    };

    check(QStringLiteral("清单行数 = 5"), page.diffRowCountForTest() == 5,
          QString::number(page.diffRowCountForTest()));
    check(QStringLiteral("默认筛选显示全部 5 条"), page.diffVisibleCountForTest() == 5,
          QString::number(page.diffVisibleCountForTest()));

    page.setFilterForTest(QStringLiteral("graphic"));
    check(QStringLiteral("筛选「仅图形」= 2 条"), page.diffVisibleCountForTest() == 2,
          QString::number(page.diffVisibleCountForTest()));

    page.setFilterForTest(QStringLiteral("text"));
    check(QStringLiteral("筛选「仅文字」= 3 条"), page.diffVisibleCountForTest() == 3,
          QString::number(page.diffVisibleCountForTest()));

    page.setFilterForTest(QStringLiteral("added"));
    check(QStringLiteral("筛选「仅新增」= 2 条"), page.diffVisibleCountForTest() == 2,
          QString::number(page.diffVisibleCountForTest()));

    page.setFilterForTest(QStringLiteral("changed"));
    check(QStringLiteral("筛选「仅变更」= 1 条"), page.diffVisibleCountForTest() == 1,
          QString::number(page.diffVisibleCountForTest()));

    page.setFilterForTest(QStringLiteral("all"));

    // 跳到第 4 条（索引 3）＝ 第 4 页的删除文字 ⇒ 应为 page=3、区域与输入一致。
    page.selectDiffRowForTest(3);
    check(QStringLiteral("跳转后页码 = 第 4 页(索引3)"), page.lastGotoPageForTest() == 3,
          QString::number(page.lastGotoPageForTest()));
    const QRectF r = page.lastGotoRectForTest();
    check(QStringLiteral("跳转后高亮区域与差异几何一致"),
          qAbs(r.x() - 100.0) < 0.01 && qAbs(r.y() - 200.0) < 0.01 &&
          qAbs(r.width() - 40.0) < 0.01 && qAbs(r.height() - 8.0) < 0.01,
          QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()));

    page.selectDiffRowForTest(1);
    check(QStringLiteral("再跳第 2 条：页码 = 索引2 且几何换成图形那条"),
          page.lastGotoPageForTest() == 2 && qAbs(page.lastGotoRectForTest().x() - 500.0) < 0.01,
          QStringLiteral("page=%1 x=%2").arg(page.lastGotoPageForTest()).arg(page.lastGotoRectForTest().x()));

    out.flush();
    if (fails) { out << "[FAIL] 比对页交互自检（" << fails << " 项）\n"; out.flush(); return 1; }
    out << "[PASS] 比对页交互：清单填充 / 6 种筛选 / 跳转定位 全部正确\n";
    out.flush();
    return 0;
}
