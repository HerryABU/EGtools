#pragma once

#include <QOpenGLWidget>
#include <QWidget>

class PdfCanvas;

// ---------------------------------------------------------------------------
// 两种"壳"，承载同一个 PdfCanvas：
//   PdfView    —— 普通 QWidget：CPU 光栅合成，任何机器都能用（保底路径）。
//   GpuPdfView —— QOpenGLWidget：页面位图作为纹理交由 GPU 合成，缩放用纹理过滤，
//                 滚动/缩放不占 CPU（大图浏览收益明显）。
// 二者只能靠继承区分（QOpenGLWidget 本身就是 QWidget），所以逻辑都在 PdfCanvas，
// 这里只做事件转发 + 绘制调用。
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
    PdfCanvas* m_c = nullptr;
};

class GpuPdfView : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit GpuPdfView(QWidget* parent = nullptr);
    PdfCanvas* canvas() const { return m_c; }

protected:
    void paintEvent(QPaintEvent*) override;   // QOpenGLWidget 里 QPainter 走 GL 引擎
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    PdfCanvas* m_c = nullptr;
};

// 画布宿主：实际的控件 + 其画布逻辑 + 是否启用了 GPU。
struct PdfViewHost
{
    QWidget*   widget = nullptr;
    PdfCanvas* canvas = nullptr;
    bool       gpu    = false;
};

// 探测 OpenGL 是否真的可用（结果缓存；EGTOOLS_NO_GPU=1 可强制关闭）。
bool gpuRenderingAvailable();

// 实际渲染路径的一行描述（写进日志，便于用户确认到底走没走 GPU）。
QString gpuBackendDescription();

// 按探测结果选择壳：可用则 GPU，否则光栅。
PdfViewHost createPdfViewHost(QWidget* parent = nullptr);
