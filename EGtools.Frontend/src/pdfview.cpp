#include "pdfview.h"

#include "pdfcanvas.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QSurfaceFormat>
#include <QWheelEvent>

// ---------------------------------------------------------------------------
// 光栅壳
// ---------------------------------------------------------------------------
PdfView::PdfView(QWidget* parent) : QWidget(parent)
{
    m_c = new PdfCanvas(this);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(true);

    // 画布尺寸 = 图像尺寸 * 缩放；滚动交给外层 QScrollArea。
    connect(m_c, &PdfCanvas::canvasResized, this, [this](const QSize& s){ setFixedSize(s); });
    connect(m_c, &PdfCanvas::repaintNeeded, this, qOverload<>(&QWidget::update));
}

QRect PdfView::visibleCanvasRect() const
{
    // 画布通常被塞进 QScrollArea，其直接父对象就是视口（QAbstractScrollArea 的行为）。
    // 视口可见区在画布坐标里 = mapFrom(视口, (0,0)) 为左上角 + 视口尺寸。
    if (QWidget* vp = parentWidget()) {
        if (vp->size().isValid() && !vp->size().isEmpty())
            return QRect(mapFrom(vp, QPoint(0, 0)), vp->size());
    }
    return rect();
}

void PdfView::paintEvent(QPaintEvent* e)
{
    QPainter g(this);
    m_c->paint(g, e->rect(), visibleCanvasRect());
}

void PdfView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    m_c->mousePress(e->position());
}

void PdfView::mouseMoveEvent(QMouseEvent* e) { m_c->mouseMove(e->position()); }

void PdfView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
    m_c->mouseRelease(e->position());
}

void PdfView::wheelEvent(QWheelEvent* e)
{
    // Ctrl+滚轮 = 缩放（与主流 CAD/PDF 阅读器一致）；不按 Ctrl 时让 QScrollArea 滚动。
    if (e->modifiers().testFlag(Qt::ControlModifier) && m_c->wheelZoom(e->angleDelta().y())) {
        e->accept();
        return;
    }
    QWidget::wheelEvent(e);
}

// ---------------------------------------------------------------------------
// GPU 壳（QOpenGLWidget）
//
// ⚠️ 绘制必须在本类的 paintGL() 里做。QOpenGLWidget 的绘制目标是它自己的 FBO，
//    Qt 只在 paintGL() 被调用那条路径上保证"上下文已 makeCurrent + FBO 已绑定"。
//    以前在 paintEvent() 里 new QPainter 画，内容不会落到 FBO 上，界面就是一块纯黑。
// ---------------------------------------------------------------------------
GpuPdfView::GpuPdfView(QWidget* parent) : QOpenGLWidget(parent)
{
    m_c = new PdfCanvas(this);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    connect(m_c, &PdfCanvas::canvasResized, this, [this](const QSize& s){ setFixedSize(s); });
    connect(m_c, &PdfCanvas::repaintNeeded, this, qOverload<>(&QWidget::update));
}

void GpuPdfView::initializeGL()
{
    // 清屏色取画布底色：即便某次绘制一筆未落，屏幕上也只是与光栅壳一致的深灰底，
    // 而不是 OpenGL 默认的纯黑 —— "全黑"是最难定位的故障形态，先从源头消灭。
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    QOpenGLFunctions* f = ctx ? ctx->functions() : nullptr;
    if (!f) return;

    f->glClearColor(0x3A / 255.0f, 0x3E / 255.0f, 0x44 / 255.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);

    const auto str = [f](GLenum e) {
        const char* s = reinterpret_cast<const char*>(f->glGetString(e));
        return QString::fromLatin1(s ? s : "?");
    };
    m_glInfo = QStringLiteral("%1 / %2 (GL %3)")
                   .arg(str(GL_VENDOR), str(GL_RENDERER), str(GL_VERSION));
}

void GpuPdfView::resizeGL(int, int)
{
    // 无需处理：QPainter 在本控件上绘制时会自行按当前 FBO 尺寸设视口/投影。
}

QRect GpuPdfView::visibleCanvasRect() const
{
    if (QWidget* vp = parentWidget()) {
        if (vp->size().isValid() && !vp->size().isEmpty())
            return QRect(mapFrom(vp, QPoint(0, 0)), vp->size());
    }
    return rect();
}

void GpuPdfView::paintGL()
{
    // 复现开关：旧版把绘制写在 paintEvent 里，paintGL 里什么都不做 ⇒ 画面全黑。
    // 仅用于 `--test-paint` 做 A/B 对照，绝不要在产品路径上打开。
    if (qEnvironmentVariableIsSet("EGTOOLS_GL_LEGACY_PAINT"))
        return;
    // 故障注入（仅测试）：故意不画，用来验证"首帧自检能发现画不出并自动回退光栅"。
    // 没有这条注入，"自愈"就只是一句愿望，永远没被证明过。
    const bool injectBlack = qEnvironmentVariableIsSet("EGTOOLS_GL_INJECT_BLACK");

    if (!injectBlack) {
        // QPainter 析构时才把 GL 命令 flush 出去，所以首帧抓图必须在它作用域之外。
        QPainter g(this);
        g.setRenderHint(QPainter::SmoothPixmapTransform, true);
        m_c->paint(g, rect(), visibleCanvasRect());
    }

    // ---- 首帧健全性检查 ----
    // 判定依据是"与已知页面图对比"，不是"页面一定很白"：只要有页面图且**页面图本身
    // 有明暗变化**（图纸/文字/线条必然有），而抓到的帧却是**单色平帧**，就说明这条
    // GPU 路径在这台机器上根本没把内容画出来（驱动/合成异常）→ 立刻通知宿主热切换到
    // 光栅壳，自愈而不是让用户对着黑屏。
    if (m_selfCheckPending && m_c->hasImage() && !m_grabbingFbo
        && !qEnvironmentVariableIsSet("EGTOOLS_GPU_SELFCHECK_OFF")) {
        m_selfCheckPending = false;
        m_grabbingFbo = true;
        const QImage fb = grabFramebuffer();
        m_grabbingFbo = false;

        const LumaStats fs = imageLuminanceStats(fb);
        const LumaStats ps = imageLuminanceStats(m_c->image());
        QString why;
        bool suspect = false;
        if (fb.isNull()) {
            suspect = true;
            why = QStringLiteral("抓不到帧缓冲");
        } else if (ps.range() > 40 && fs.range() <= 20) {
            // 页面图有明显内容，帧却是单色平帧 —— 铁证。
            suspect = true;
            why = QStringLiteral("页面有内容（明暗范围 %1）但绘制结果是一张单色平帧（范围 %2，平均亮度 %3）")
                      .arg(ps.range()).arg(fs.range()).arg(fs.avg, 0, 'f', 1);
        } else if (frameLooksBlank(fb, &why)) {
            suspect = true;
        }
        if (suspect) {
            m_frameSuspect = true;
            emit frameSuspect(why);
        }
    }
}

void GpuPdfView::paintEvent(QPaintEvent* e)
{
    // 正常路径：交回基类 —— 基类会在"上下文 current + FBO 已绑定"的前提下调用
    // paintGL()，内容才能真正落到 FBO 上。**不要**在这里自己 new QPainter 画画。
    if (qEnvironmentVariableIsSet("EGTOOLS_GL_LEGACY_PAINT")) {
        QPainter g(this);                     // 旧写法（必然画不出，见文档注释）
        g.setRenderHint(QPainter::SmoothPixmapTransform, true);
        m_c->paint(g, rect(), visibleCanvasRect());
        return;
    }
    QOpenGLWidget::paintEvent(e);
}

void GpuPdfView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { QOpenGLWidget::mousePressEvent(e); return; }
    m_c->mousePress(e->position());
}

void GpuPdfView::mouseMoveEvent(QMouseEvent* e) { m_c->mouseMove(e->position()); }

void GpuPdfView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { QOpenGLWidget::mouseReleaseEvent(e); return; }
    m_c->mouseRelease(e->position());
}

void GpuPdfView::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers().testFlag(Qt::ControlModifier) && m_c->wheelZoom(e->angleDelta().y())) {
        e->accept();
        return;
    }
    QOpenGLWidget::wheelEvent(e);
}

// ---------------------------------------------------------------------------
// 帧内容判定
// ---------------------------------------------------------------------------
LumaStats imageLuminanceStats(const QImage& imgIn)
{
    LumaStats st;
    if (imgIn.isNull() || imgIn.width() < 2 || imgIn.height() < 2) return st;

    QImage img = imgIn;
    if (img.format() != QImage::Format_RGB32 && img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_RGB32);

    // 网格采样（最多约 200x200 点）：判定"有没有内容"足够，且不拖慢首帧。
    const int stepX = qMax(1, img.width() / 200);
    const int stepY = qMax(1, img.height() / 200);
    quint64 sum = 0;
    int n = 0, lo = 255, hi = 0, bright = 0;
    for (int y = 0; y < img.height(); y += stepY) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += stepX) {
            const QRgb p = line[x];
            const int lum = (qRed(p) * 299 + qGreen(p) * 587 + qBlue(p) * 114) / 1000;
            sum += static_cast<quint64>(lum); ++n;
            lo = qMin(lo, lum); hi = qMax(hi, lum);
            if (lum >= 180) ++bright;
        }
    }
    if (n == 0) return st;
    st.avg        = double(sum) / n;
    st.minLum     = lo;
    st.maxLum     = hi;
    st.brightFrac = double(bright) / n;
    return st;
}

bool frameLooksBlank(const QImage& frame, QString* reason)
{
    const LumaStats s = imageLuminanceStats(frame);
    if (frame.isNull() || frame.width() < 2 || frame.height() < 2) {
        if (reason) *reason = QStringLiteral("抓到的帧为空");
        return true;
    }
    // 判定标准：整体极暗 + 几乎没有任何明暗变化 ⇒ 没画出内容。
    // 取阈值 16/10 是因为 CAD 图纸页是白底（平均亮度 ≈248），
    // 而"没画上"的 FBO 是纯黑（平均 0、动态范围 0），两者相距极远，不会误伤。
    if (s.avg <= 16.0 && s.range() <= 10) {
        if (reason)
            *reason = QStringLiteral("整帧近乎全黑（平均亮度 %1，明暗范围 %2）")
                          .arg(s.avg, 0, 'f', 1).arg(s.range());
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// GPU 探测 / 工厂
// ---------------------------------------------------------------------------
namespace {

struct GpuProbe
{
    bool    attempted = false;
    bool    ok        = false;
    QString desc;
};

GpuProbe& probe()
{
    static GpuProbe p;
    return p;
}

RenderBackend& backendPref()
{
    static RenderBackend b = RenderBackend::Auto;
    return b;
}

// 从 "4.6.0 - Build 31.0.101.2140" / "2.1 Mesa 22.0" 这类 GL_VERSION 串里取主次版本。
void parseGlVersion(const QString& v, int* major, int* minor)
{
    *major = *minor = 0;
    const QString head = v.section(QLatin1Char(' '), 0, 0);        // 只取最前面那段 "4.6.0"
    const QStringList parts = head.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (parts.size() >= 1) *major = parts[0].toInt();
    if (parts.size() >= 2) *minor = parts[1].toInt();
}

} // namespace

RenderBackend renderBackendPreference() { return backendPref(); }

void setRenderBackendPreference(RenderBackend b)
{
    backendPref() = b;
    // 探测结果按偏好作废，下次 createPdfViewHost 会重新判定。
    probe() = GpuProbe{};
}

RenderBackend renderBackendFromString(const QString& s)
{
    const QString t = s.trimmed().toLower();
    if (t == QLatin1String("gpu") || t == QLatin1String("embedded")) return RenderBackend::Gpu;
    if (t == QLatin1String("raster") || t == QLatin1String("cpu") || t == QLatin1String("cli"))
        return RenderBackend::Raster;
    return RenderBackend::Auto;
}

QString renderBackendToString(RenderBackend b)
{
    switch (b) {
    case RenderBackend::Gpu:    return QStringLiteral("gpu");
    case RenderBackend::Raster: return QStringLiteral("raster");
    default:                    return QStringLiteral("auto");
    }
}

bool gpuRenderingAvailable()
{
    GpuProbe& p = probe();
    if (p.attempted) return p.ok;
    p.attempted = true;

    if (backendPref() == RenderBackend::Raster) {
        p.desc = QStringLiteral("按设置强制使用 CPU 光栅");
        p.ok = false;
        return false;
    }

    // ⚠️ Auto 默认走 **CPU 光栅**（2026-10-12 改）：
    //   预览已改为 Qt PDF（PDFium）**矢量分块渲染** —— 每帧只贴几张小图，CPU 光栅
    //   完全够用；而 GPU 壳（QOpenGLWidget）在远程桌面 / 虚拟机 / 无厂商驱动的机器上
    //   会因 GL 版本不足变成**纯黑画布**，长期是"预览发黑"投诉的主要来源之一。
    //   所以默认不再启用 GPU；用户显式选「GPU」时仍然可用（见下面的真探测）。
    if (backendPref() == RenderBackend::Auto) {
        p.desc = QStringLiteral("CPU 光栅（矢量分块渲染，不需要 GPU，也不会黑屏）");
        p.ok = false;
        return false;
    }

    // 显式关闭开关（排查显卡驱动问题时用）。
    if (qEnvironmentVariableIsSet("EGTOOLS_NO_GPU")) {
        p.desc = QStringLiteral("已按 EGTOOLS_NO_GPU 关闭 GPU，使用 CPU 光栅");
        p.ok = false;
        return false;
    }
    // 无头/离屏平台没有真实 GL 上下文，别去尝试（否则只会打印一堆警告）。
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen"), Qt::CaseInsensitive)) {
        p.desc = QStringLiteral("离屏平台（无头）→ 使用 CPU 光栅");
        p.ok = false;
        return false;
    }

    // 真探测：能创建上下文并 makeCurrent 才算可用 —— 只看"有显卡"不算数，
    // 远程桌面/驱动异常时会创建失败，那种情况下必须老实退回光栅。
    QOpenGLContext ctx;
    if (!ctx.create()) {
        p.desc = QStringLiteral("OpenGL 上下文创建失败 → 使用 CPU 光栅");
        p.ok = false;
        return false;
    }
    QOffscreenSurface surf;
    surf.setFormat(ctx.format());
    surf.create();
    if (!surf.isValid() || !ctx.makeCurrent(&surf)) {
        p.desc = QStringLiteral("OpenGL 上下文无法激活 → 使用 CPU 光栅");
        p.ok = false;
        return false;
    }

    QString vendor = QStringLiteral("?"), renderer = QStringLiteral("?"), version = QStringLiteral("?");
    if (auto* f = ctx.functions()) {
        const auto str = [f](GLenum e) {
            const char* s = reinterpret_cast<const char*>(f->glGetString(e));
            return QString::fromLatin1(s ? s : "?");
        };
        vendor   = str(GL_VENDOR);
        renderer = str(GL_RENDERER);
        version  = str(GL_VERSION);
    }
    ctx.doneCurrent();

    // ⚠️⚠️ 版本闸门 —— 这是"画布全黑"最可能的真凶。
    // 远程桌面 / 虚拟机 / 未装厂商驱动的机器上，Windows 只提供 Microsoft 的
    // "GDI Generic" **OpenGL 1.1**：上下文能创建、makeCurrent 也成功，所以只测
    // "能不能建上下文"会把这种环境误判成"有 GPU"。而 QOpenGLWidget 需要 **GL 2.0 / ES 2**
    // （它靠 FBO + 着色器合成），拿到 1.1 的唯一结果就是一块**纯黑**画布。
    const QString rl = renderer.toLower();
    const bool software = rl.contains(QStringLiteral("llvmpipe"))
                       || rl.contains(QStringLiteral("softpipe"))
                       || rl.contains(QStringLiteral("software"))
                       || rl.contains(QStringLiteral("swiftshader"))
                       || rl.contains(QStringLiteral("gdi generic"));
    int major = 0, minor = 0;
    parseGlVersion(version, &major, &minor);
    const bool tooOld = (major < 2) || (major == 2 && minor == 0 && version.contains(QStringLiteral("1.1")));

    if (software || tooOld) {
        p.desc = QStringLiteral("OpenGL 版本/实现不满足 QOpenGLWidget 要求（%1 / %2）→ 使用 CPU 光栅")
                     .arg(renderer, version);
        p.ok = false;
        return false;
    }

    p.ok = true;
    p.desc = QStringLiteral("GPU 加速已启用 — %1 / %2 (GL %3)").arg(vendor, renderer, version);
    return true;
}

QString gpuBackendDescription()
{
    gpuRenderingAvailable();     // 触发/复用探测
    return probe().desc;
}

PdfViewHost createRasterPdfViewHost(QWidget* parent)
{
    PdfViewHost h;
    auto* v = new PdfView(parent);
    h.widget = v;
    h.canvas = v->canvas();
    h.gpu    = false;
    return h;
}

PdfViewHost createPdfViewHost(QWidget* parent)
{
    if (gpuRenderingAvailable()) {
        PdfViewHost h;
        auto* v = new GpuPdfView(parent);
        h.widget = v;
        h.canvas = v->canvas();
        h.gpu    = true;
        return h;
    }
    return createRasterPdfViewHost(parent);
}

// 独立于偏好探测，判断"这台机器有没有可用的 GPU 壳"（用于强制 GPU 时的可用性判断）。
bool gpuShellUsable()
{
    const RenderBackend saved = renderBackendPreference();
    // ⚠️ 必须临时设成 Gpu 而不是 Auto：Auto 现在语义是"默认光栅"，拿它探测永远为假。
    if (saved != RenderBackend::Gpu) setRenderBackendPreference(RenderBackend::Gpu);
    const bool ok = gpuRenderingAvailable();
    setRenderBackendPreference(saved);
    return ok;
}
