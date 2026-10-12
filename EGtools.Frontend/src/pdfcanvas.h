#pragma once

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include "iregioncanvas.h"
#include "types.h"

// ---------------------------------------------------------------------------
// PdfCanvas —— 页面画布的全部状态与逻辑（与具体控件无关）
//
// 为什么要与控件解耦：本工具的页面图很大（例如 2384x1684 起），显示它既可以用
// CPU 光栅（普通 QWidget），也可以用 GPU（QOpenGLWidget 把位图作为纹理绘制）。
// 两者只能靠**继承**区分（QOpenGLWidget 已经是 QWidget 的子类，无法再对同一个
// 对象二选一），所以把"逻辑"放在这个 QObject 里，由两个薄壳控件各自委托：
//   PdfView    —— QWidget（CPU 光栅，永远可用，是保底路径）
//   GpuPdfView —— QOpenGLWidget（GPU 合成 + 纹理过滤缩放）
// 启动时探测 OpenGL 是否真的可用，可用才选 GPU 壳；探测失败就退回光栅壳，
// 所以"开启硬件加速"绝不会以黑屏为代价。
//
// 坐标系（全 app 唯一的映射点，勿在别处再算一次）：
//   imagePx = pt * renderScale          （renderScale 由内核下发）
//   widgetPx = imagePx * zoom           （zoom 是界面缩放，默认 1.0）
//   ⇒ pt = widgetPx / (renderScale * zoom)
// ---------------------------------------------------------------------------
class PdfCanvas : public QObject, public IRegionCanvas
{
    Q_OBJECT
public:
    explicit PdfCanvas(QObject* parent = nullptr);

    // ---- 页面 ----
    void setPage(const QImage& img, double renderScale, int pageIndex);
    void setPageIndex(int p) { m_page = p; emit repaintNeeded(); }
    int  pageIndex() const { return m_page; }
    bool hasImage() const { return !m_img.isNull(); }
    QImage image() const { return m_img; }
    bool lastRenderFailed() const { return m_renderFailed; }
    void setRenderFailed(const QString& reason) { m_renderFailed = true; m_failReason = reason; emit repaintNeeded(); }
    void clearRenderFailed() { m_renderFailed = false; m_failReason.clear(); }

    // ---- 语义 / 框选 ----
    void setKind(const QString& kind) override { m_kind = kind; }
    QString kind() const override { return m_kind; }
    void addRegion(const Region& r) override { m_regions.push_back(r); emit repaintNeeded(); }
    void clearRegions() override { m_regions.clear(); m_words.clear(); emit repaintNeeded(); }
    QVector<Region> regions() const override { return m_regions; }
    // 跳页 = 单页视图里切换当前显示页（连续视图则由滚动实现）。
    void showPage(int page) override { setPageIndex(page); }

    void setWordBoxes(const QVector<WordBox>& w) override { m_words = w; emit repaintNeeded(); }
    void clearWordBoxes() override { m_words.clear(); emit repaintNeeded(); }
    void setPageWords(const QVector<WordBox>& w);
    QVector<WordBox> pageWords() const { return m_pageWords; }

    // ---- 缩放（GPU 路径下平滑缩放几乎零成本，光栅路径用平滑变换仍可用）----
    double zoom() const override { return m_zoom; }
    void setZoom(double z) override;
    void zoomIn() override  { setZoom(m_zoom * 1.25); }
    void zoomOut() override { setZoom(m_zoom / 1.25); }
    // 自适应宽度：把整页缩放到给定视口宽度（用于"适合宽度"按钮）。
    void fitToWidth(int viewportWidthPx) override;
    // 画布应有尺寸（供宿主控件 setFixedSize）。
    QSize canvasSize() const;

    // ---- 坐标映射（唯一映射点）----
    QRectF pxToPt(const QRectF& r) const;
    QRectF ptToPx(const QRectF& r) const;
    double renderScale() const { return m_scale; }

    // ---- 事件（由薄壳控件转发，坐标用控件像素）----
    void mousePress(const QPointF& pos);
    void mouseMove(const QPointF& pos);
    void mouseRelease(const QPointF& pos);
    // 滚轮缩放；返回 true 表示已消费（需要宿主重设控件尺寸）。
    bool wheelZoom(int angleDeltaY);

    // ---- 绘制（由薄壳控件的 paintEvent 调用）----
    void paint(QPainter& g, const QRect& widgetRect);
    // 自检用：绘制次数。用来区分"没画"与"画了但没落到屏幕上"。
    int paintCalls() const { return m_paintCalls; }

    static QColor colorForKind(const QString& kind);

signals:
    void repaintNeeded();
    void canvasResized(const QSize& size);   // 缩放/换页后宿主需要 setFixedSize
    void regionSelected(int page, const QString& kind, const QRectF& rectPt);
    void textPreview(int page, const QString& text);
    void zoomChanged(double zoom);

private:
    QImage  m_img;
    double  m_scale = 1.0;     // image px per PDF pt（内核下发）
    double  m_zoom  = 1.0;     // 界面缩放
    int     m_page  = 0;
    QString m_kind  = QStringLiteral("detail_table");
    QVector<Region> m_regions;
    QVector<WordBox> m_words;       // 抽取叠加层（提取后）
    QVector<WordBox> m_pageWords;   // 本页全部矢量词（实时识别用）
    QVector<WordBox> m_previewWords;// 当前橡皮筋内的词

    bool    m_dragging = false;
    QPointF m_pressPt;
    QPointF m_curPt;

    bool    m_renderFailed = false;
    QString m_failReason;
    int     m_paintCalls = 0;      // 自检用（paint() 被调用次数）
};
