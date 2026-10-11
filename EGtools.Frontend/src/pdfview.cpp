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

void PdfView::paintEvent(QPaintEvent*)
{
    QPainter g(this);
    m_c->paint(g, rect());
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
// GPU 壳
// ---------------------------------------------------------------------------
GpuPdfView::GpuPdfView(QWidget* parent) : QOpenGLWidget(parent)
{
    m_c = new PdfCanvas(this);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    connect(m_c, &PdfCanvas::canvasResized, this, [this](const QSize& s){ setFixedSize(s); });
    connect(m_c, &PdfCanvas::repaintNeeded, this, qOverload<>(&QWidget::update));
}

void GpuPdfView::paintEvent(QPaintEvent*)
{
    // QOpenGLWidget 上使用 QPainter：Qt 会走 OpenGL 绘制引擎，位图作为纹理上传，
    // 之后每次滚动/缩放都是 GPU 采样，不再逐像素重算。
    QPainter g(this);
    g.setRenderHint(QPainter::SmoothPixmapTransform, true);
    m_c->paint(g, rect());
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

} // namespace

bool gpuRenderingAvailable()
{
    GpuProbe& p = probe();
    if (p.attempted) return p.ok;
    p.attempted = true;

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
        vendor   = QString::fromLatin1(reinterpret_cast<const char*>(f->glGetString(GL_VENDOR)));
        renderer = QString::fromLatin1(reinterpret_cast<const char*>(f->glGetString(GL_RENDERER)));
        version  = QString::fromLatin1(reinterpret_cast<const char*>(f->glGetString(GL_VERSION)));
    }
    ctx.doneCurrent();

    // 软件光栅（llvmpipe/softpipe/Mesa 软件栈）不产生 GPU 收益，但功能可用；
    // 仍然选 GPU 壳没有意义，直接退回光栅壳更省事。
    const QString rl = renderer.toLower();
    const bool software = rl.contains(QStringLiteral("llvmpipe"))
                       || rl.contains(QStringLiteral("softpipe"))
                       || rl.contains(QStringLiteral("software"))
                       || rl.contains(QStringLiteral("swiftshader"));
    if (software) {
        p.desc = QStringLiteral("仅软件 OpenGL（%1）→ 使用 CPU 光栅").arg(renderer);
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

PdfViewHost createPdfViewHost(QWidget* parent)
{
    PdfViewHost h;
    if (gpuRenderingAvailable()) {
        auto* v = new GpuPdfView(parent);
        h.widget = v;
        h.canvas = v->canvas();
        h.gpu    = true;
    } else {
        auto* v = new PdfView(parent);
        h.widget = v;
        h.canvas = v->canvas();
        h.gpu    = false;
    }
    return h;
}
