#pragma once

#include <QImage>
#include <QOpenGLWidget>
#include <QString>
#include <QWidget>

class PdfCanvas;

// ---------------------------------------------------------------------------
// 两种"壳"，承载同一个 PdfCanvas：
//   PdfView    —— 普通 QWidget：CPU 光栅合成，任何机器都能用（保底路径）。
//   GpuPdfView —— QOpenGLWidget：页面位图作为纹理交由 GPU 合成，缩放用纹理过滤，
//                 滚动/缩放不占 CPU（大图浏览收益明显）。
// 二者只能靠继承区分（QOpenGLWidget 本身就是 QWidget），所以逻辑都在 PdfCanvas，
// 这里只做事件转发 + 绘制调用。
//
// ⚠️⚠️ QOpenGLWidget 的绘制**必须**发生在 paintGL() 里。
//   早期版本在 paintEvent() 里 new 一个 QPainter 画，结果是：QOpenGLWidget 的绘制
//   目标是它自己的 FBO，而 paintEvent 阶段那条路径并没有把内容落到 FBO 上 ——
//   界面稳定显示为**纯黑**。致命之处在于它只在"GPU 可用"的机器上出现，而沙箱/CI
//   的 offscreen 平台探测不到 GL、自动走光栅壳，所以本地测试全绿、真机全黑。
//   现在：绘制在 paintGL()，并在首帧做一次"有没有画出东西"的健全性检查，
//   一旦发现异常就通知宿主热切换到光栅壳（见 frameSuspect）。
// ---------------------------------------------------------------------------
class PdfView : public QWidget
{
    Q_OBJECT
public:
    explicit PdfView(QWidget* parent = nullptr);
    PdfCanvas* canvas() const { return m_c; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    // 视口当前可见范围在本控件坐标系里的矩形。画布据此决定"要渲染哪一块" ——
    // 不区分 exposed/visible 就会按整块画布请求，4x 下等于把整页全部排队渲染。
    QRect visibleCanvasRect() const;
    PdfCanvas* m_c = nullptr;
};

class GpuPdfView : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit GpuPdfView(QWidget* parent = nullptr);
    PdfCanvas* canvas() const { return m_c; }

    // 实际生效的 GL 描述（vendor/renderer/version），写日志用。
    QString glInfo() const { return m_glInfo; }
    // 首帧健全性检查里如果判定"明明有页面图却整帧全黑"，值为 true。
    bool frameWasSuspect() const { return m_frameSuspect; }

signals:
    // 首帧画不出内容（全黑）——宿主应立刻热切换到光栅壳，绝不能给用户留一块黑画布。
    void frameSuspect(const QString& reason);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;              // ⚠️ QOpenGLWidget 只能在这里绘制
    // 仅为**复现旧写法**而存在（EGTOOLS_GL_LEGACY_PAINT=1）：旧版把 QPainter 画在
    // paintEvent() 里，FBO 上什么都不会有 ⇒ 纯黑。留着它，是为了让
    // `--test-paint` 能同时给出"旧写法黑屏 / 新写法正常"的对照证据，防止将来退回。
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    QRect visibleCanvasRect() const;
    PdfCanvas* m_c = nullptr;
    QString    m_glInfo;
    bool       m_selfCheckPending = true;   // 还没做过"首帧有没有画出东西"的检查
    bool       m_grabbingFbo      = false;  // 防 grabFramebuffer 重入
    bool       m_frameSuspect     = false;
};

// 画布宿主：实际的控件 + 其画布逻辑 + 是否启用了 GPU。
struct PdfViewHost
{
    QWidget*   widget = nullptr;
    PdfCanvas* canvas = nullptr;
    bool       gpu    = false;
};

// 渲染后端偏好：
//   Auto   —— 探测通过就用 GPU，否则 CPU 光栅（默认）。
//   Gpu    —— 强制 GPU（排查/自证用；若机器不支持，首帧自检会把画布热切回光栅）。
//   Raster —— 强制 CPU 光栅（最保底；远程桌面/老驱动机器上建议用这个）。
enum class RenderBackend { Auto, Gpu, Raster };

RenderBackend renderBackendPreference();
void setRenderBackendPreference(RenderBackend b);
RenderBackend renderBackendFromString(const QString& s);   // "auto"/"gpu"/"raster"
QString       renderBackendToString(RenderBackend b);

// 探测 OpenGL 是否真的可用（结果缓存；EGTOOLS_NO_GPU=1 可强制关闭）。
bool gpuRenderingAvailable();

// 实际渲染路径的一行描述（写进日志，便于用户确认到底走没走 GPU）。
QString gpuBackendDescription();

// 按探测结果选择壳：可用则 GPU，否则光栅。
PdfViewHost createPdfViewHost(QWidget* parent = nullptr);

// 强制新建一个**光栅**壳（热回退用：GPU 壳画不出东西时换掉它）。
PdfViewHost createRasterPdfViewHost(QWidget* parent = nullptr);

// 忽略当前偏好，判断这台机器是否存在可用的 GPU 壳（用于"强制 GPU"时的可用性判断）。
bool gpuShellUsable();

// 判定一帧是不是"什么都没有"（几乎全黑/整体单色）。
// 有页面图时正常帧应该以白底为主（平均亮度高、动态范围大），
// 因此"整体偏暗且几乎没有明暗变化"即判为异常。reason 里给出可读原因。
bool frameLooksBlank(const QImage& frame, QString* reason);

// 图像亮度统计（网格采样）：自检与首帧判定共用同一套度量，避免两处口径不一致。
struct LumaStats
{
    double avg        = 0.0;    // 平均亮度 0..255
    int    minLum     = 0;
    int    maxLum     = 0;
    double brightFrac = 0.0;    // 亮度 >= 180 的采样点占比（"白纸"占比）
    int    range() const { return maxLum - minLum; }
};
LumaStats imageLuminanceStats(const QImage& img);
