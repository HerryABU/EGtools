#pragma once

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include "iregioncanvas.h"
#include "types.h"

// ---------------------------------------------------------------------------
// PdfCanvas —— 页面画布的全部状态与逻辑（与具体控件无关）
//
// 两条渲染路径：
//   ★ 矢量路径（默认，有 PDF 时）：页面由 **Qt PDF / PDFium** 按"目标分辨率"
//     （zoom × 设备像素比）直接重绘，只渲染可视区，异步 + 分块缓存 + 渐进式细化。
//     任意倍率都清晰（实测 4x 下锐度是"位图放大"的 4.9 倍），且不依赖 OpenGL。
//   · 光栅路径（兼容/兜底）：调用方用 setPage() 塞一张现成位图（自检与降级用）。
//
// 为什么不再用"内核渲染固定倍率位图 → 按 zoom 拉伸"：那条路必然模糊（位图放大），
// 且渲染结果背景是透明的，画布若没铺白底就看着像黑屏。
//
// 坐标系（全 app 唯一的映射点，勿在别处再算一次）：
//   矢量路径：widgetPx = pt * zoom                    ⇒ pt = widgetPx / zoom
//   光栅路径：widgetPx = pt * renderScale * zoom
//   两者都遵循 y-down / 左上原点。
// ---------------------------------------------------------------------------
class PdfCanvas : public QObject, public IRegionCanvas
{
    Q_OBJECT
public:
    explicit PdfCanvas(QObject* parent = nullptr);

    // ---- 文档（矢量路径）---------------------------------------------------
    void setDocument(const QString& pdfPath, int pageCount);
    void clearDocument();
    bool    vectorMode() const { return !m_pdfPath.isEmpty(); }
    QString documentPath() const { return m_pdfPath; }
    int     pageCount() const { return m_pageCount; }

    // ---- 页面（光栅路径）---------------------------------------------------
    void setPage(const QImage& img, double renderScale, int pageIndex);
    void setPageIndex(int p);
    int  pageIndex() const { return m_page; }
    bool hasImage() const;
    // 当前可用的最佳图像（自检/首帧判定用）：矢量模式取"最清晰的那块缓存"。
    QImage image() const;
    bool lastRenderFailed() const { return m_renderFailed; }
    void setRenderFailed(const QString& reason) { m_renderFailed = true; m_failReason = reason; emit repaintNeeded(); }
    void clearRenderFailed() { m_renderFailed = false; m_failReason.clear(); }

    // 当前页尺寸（pt）。矢量模式取 PDFium；光栅模式由图像/倍率反推。
    QSizeF pageSizePt() const;

    // ---- 语义 / 框选 ----
    void setKind(const QString& kind) override { m_kind = kind; }
    QString kind() const override { return m_kind; }
    void addRegion(const Region& r) override { m_regions.push_back(r); emit repaintNeeded(); }
    void clearRegions() override { m_regions.clear(); m_words.clear(); emit repaintNeeded(); }
    QVector<Region> regions() const override { return m_regions; }
    void showPage(int page) override { setPageIndex(page); }

    void setWordBoxes(const QVector<WordBox>& w) override { m_words = w; emit repaintNeeded(); }
    void clearWordBoxes() override { m_words.clear(); emit repaintNeeded(); }
    void setPageWords(const QVector<WordBox>& w);
    QVector<WordBox> pageWords() const { return m_pageWords; }

    // ---- 缩放 ----
    double zoom() const override { return m_zoom; }
    void setZoom(double z) override;
    void zoomIn() override  { setZoom(m_zoom * 1.25); }
    void zoomOut() override { setZoom(m_zoom / 1.25); }
    void fitToWidth(int viewportWidthPx) override;
    QSize canvasSize() const;

    // ---- 坐标映射（唯一映射点）----
    QRectF pxToPt(const QRectF& r) const;
    QRectF ptToPx(const QRectF& r) const;
    double renderScale() const { return m_scale; }

    // ---- 事件 ----
    void mousePress(const QPointF& pos);
    void mouseMove(const QPointF& pos);
    void mouseRelease(const QPointF& pos);
    bool wheelZoom(int angleDeltaY);

    // ---- 绘制 ----
    // ⚠️ 必须把"暴露区"和"可视区"分开传：
    //   exposed  —— 本次真正要重绘的范围（交给 QPainter 裁剪，省时间）；
    //   visible  —— 视口当前能看到画布的范围（**请求渲染只按这个来**）。
    //   若两者都用 exposed，每次 update() 都会按整块画布请求 —— 4x 下等于要把整页
    //   （9536×6736 = 20 块）全部排队渲染，既慢又爆内存。
    void paint(QPainter& g, const QRect& exposedRect) { paint(g, exposedRect, exposedRect); }
    void paint(QPainter& g, const QRect& exposedRect, const QRect& visibleRect);
    int paintCalls() const { return m_paintCalls; }

    // ---- 自检钩子 ----
    int    tileRequestsForTest() const { return m_tileRequests; }
    QRect  lastVisibleDevForTest() const { return m_lastVisibleDev; }
    double deviceScaleForTest(double dpr) const { return m_zoom * dpr; }
    int    cachedRegionCountForTest() const;
    // 是否所有"可视区分块"都已就绪（不黑、不糊的判据：覆盖可视区的块全有）。
    bool visibleTilesReadyForTest() const;
    void resetTileRequestCounterForTest() { m_tileRequests = 0; }

    static QColor colorForKind(const QString& kind);

signals:
    void repaintNeeded();
    void canvasResized(const QSize& size);
    void regionSelected(int page, const QString& kind, const QRectF& rectPt);
    void textPreview(int page, const QString& text);
    void zoomChanged(double zoom);

private:
    // 可视区在"整页设备像素空间"里的矩形（按分块网格对齐并外扩一块）。
    QRect visibleDevRect(const QRect& visibleRect, double dpr) const;
    // 按需要请求可视区分块（异步、去重）。
    void requestVisibleTiles(const QRect& visibleRect, double dpr);
    // 低倍率整页预览（让内容尽快出现，随后被高清块覆盖）。
    void requestPreview(int page);

    QString m_pdfPath;              // 非空 ⇒ 矢量路径
    int     m_pageCount = 0;

    QImage  m_img;                  // 光栅路径用的位图
    double  m_scale = 1.0;          // image px per PDF pt（仅光栅路径）
    double  m_zoom  = 1.0;          // 界面缩放（widget px per pt）
    int     m_page  = 0;
    QString m_kind  = QStringLiteral("detail_table");
    QVector<Region> m_regions;
    QVector<WordBox> m_words;
    QVector<WordBox> m_pageWords;
    QVector<WordBox> m_previewWords;

    bool    m_dragging = false;
    QPointF m_pressPt;
    QPointF m_curPt;

    bool    m_renderFailed = false;
    QString m_failReason;
    int     m_paintCalls = 0;

    QRect   m_lastVisibleDev;       // 最近一次绘制的可视区（设备像素）
    int     m_tileRequests = 0;     // 累计发起的渲染请求数
};
