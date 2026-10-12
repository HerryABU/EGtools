#include "pdfcanvas.h"

#include "pdfrender.h"

#include <QPainter>
#include <QPaintDevice>
#include <QtMath>

#include <algorithm>

PdfCanvas::PdfCanvas(QObject* parent) : QObject(parent)
{
    // 渲染服务是进程内单例（唯一持有 PDFium 文档、在自己的线程里渲染）。
    // 渲染完成 → 只在"是本页"时重绘，避免多页视图互相触发无谓刷新。
    auto* svc = PdfRenderService::instance();
    connect(svc, &PdfRenderService::regionReady, this, [this](int page) {
        if (page == m_page) emit repaintNeeded();
    });
    connect(svc, &PdfRenderService::documentReady, this, [this](const QString& p, int) {
        if (p != m_pdfPath) return;
        emit canvasResized(canvasSize());
        requestPreview(m_page);
        emit repaintNeeded();
    });
}

// ---------------------------------------------------------------------------
// 文档（矢量路径）
// ---------------------------------------------------------------------------
void PdfCanvas::setDocument(const QString& pdfPath, int pageCount)
{
    m_pdfPath = pdfPath;
    m_pageCount = pageCount;
    m_img = QImage();          // 切到矢量路径后不再需要旧位图
    m_scale = 1.0;
    m_renderFailed = false;
    m_failReason.clear();
    m_pageWords.clear();
    m_previewWords.clear();
    if (pageCount > 0 && m_page >= pageCount) m_page = 0;
    requestPreview(m_page);
    emit canvasResized(canvasSize());
    emit repaintNeeded();
}

void PdfCanvas::clearDocument()
{
    m_pdfPath.clear();
    m_pageCount = 0;
    m_img = QImage();
    emit canvasResized(canvasSize());
    emit repaintNeeded();
}

// ---------------------------------------------------------------------------
// 页面（光栅路径，兼容/自检用）
// ---------------------------------------------------------------------------
void PdfCanvas::setPage(const QImage& img, double renderScale, int pageIndex)
{
    m_img   = img;
    m_scale = (renderScale > 0.0) ? renderScale : 1.0;
    m_page  = pageIndex;
    m_dragging = false;
    m_pageWords.clear();     // 换页 => 由调用方重新加载本页词
    m_previewWords.clear();
    m_renderFailed = false;
    m_failReason.clear();
    emit canvasResized(canvasSize());
    emit repaintNeeded();
}

void PdfCanvas::setPageIndex(int p)
{
    if (p == m_page) { emit repaintNeeded(); return; }
    m_page = p;
    m_pageWords.clear();
    m_previewWords.clear();
    if (vectorMode()) {
        // 立刻要一版低倍率预览（内容先出现），可视区高清块由 paint 里按需请求。
        requestPreview(m_page);
    }
    emit canvasResized(canvasSize());
    emit repaintNeeded();
}

QSizeF PdfCanvas::pageSizePt() const
{
    if (vectorMode()) {
        auto* svc = PdfRenderService::instance();
        if (svc->path() == m_pdfPath && m_page >= 0 && m_page < svc->pageCount())
            return svc->pagePointSize(m_page);
        return QSizeF();
    }
    if (m_img.isNull() || m_scale <= 0.0) return QSizeF();
    return QSizeF(double(m_img.width()) / m_scale, double(m_img.height()) / m_scale);
}

bool PdfCanvas::hasImage() const
{
    if (vectorMode()) return !PdfRenderService::instance()->cachedRegions(m_page).isEmpty();
    return !m_img.isNull();
}

QImage PdfCanvas::image() const
{
    if (!vectorMode()) return m_img;
    // 取"最清晰的那块"作为代表图（首帧自检 / 截图要用）。
    const QVector<PdfRegion> regs = PdfRenderService::instance()->cachedRegions(m_page);
    if (regs.isEmpty()) return QImage();
    return regs.last().img;      // cachedRegions 升序 ⇒ 最后一块最清晰
}

int PdfCanvas::cachedRegionCountForTest() const
{
    if (!vectorMode()) return m_img.isNull() ? 0 : 1;
    return PdfRenderService::instance()->cachedRegions(m_page).size();
}

void PdfCanvas::requestPreview(int page)
{
    auto* svc = PdfRenderService::instance();
    if (!svc->ready() || page < 0 || page >= svc->pageCount()) return;
    const QSizeF pt = svc->pagePointSize(page);
    if (pt.isEmpty()) return;
    // 目标约 120 万像素。注意：单次渲染耗时与面积**几乎无关**（成本由内容流解释主导），
    // 所以低倍率预览并不比高清块便宜，但它能让内容**立刻**出现，是"渐进式细化"的前提。
    const double area = pt.width() * pt.height();
    double s = (area > 0.0) ? qSqrt(1.2e6 / area) : 1.0;
    s = qBound(0.05, s, 2.0);
    svc->requestWholePage(page, s);
    ++m_tileRequests;
}

// ---------------------------------------------------------------------------
// 缩放
// ---------------------------------------------------------------------------
void PdfCanvas::setZoom(double z)
{
    // 限幅避免把内存/纹理撑爆（400% 已经足够看清图元）。
    const double nz = std::clamp(z, 0.05, 4.0);
    if (qFuzzyCompare(nz, m_zoom)) return;
    m_zoom = nz;
    // 矢量模式无需在此重渲染：paint 会按新的"目标分辨率"请求可视区分块，
    // 期间先用已有（低倍率）缓存拉伸显示 —— 这就是渐进式细化。
    emit zoomChanged(m_zoom);
    emit canvasResized(canvasSize());
    emit repaintNeeded();
}

void PdfCanvas::fitToWidth(int viewportWidthPx)
{
    const QSizeF pt = pageSizePt();
    if (pt.isEmpty() || viewportWidthPx <= 0) return;
    const double k = vectorMode() ? 1.0 : m_scale;    // pt → widget：光栅路径要再乘 renderScale
    setZoom(double(viewportWidthPx - 8) / (pt.width() * k));
}

QSize PdfCanvas::canvasSize() const
{
    const QSizeF pt = pageSizePt();
    if (pt.isEmpty()) return QSize(1, 1);
    return QSize(qMax(1, qRound(pt.width()  * m_zoom)),
                 qMax(1, qRound(pt.height() * m_zoom)));
}

// pt = widgetPx / (缩放系数)；矢量路径 k = zoom，光栅路径 k = renderScale * zoom。
QRectF PdfCanvas::pxToPt(const QRectF& r) const
{
    const double k = vectorMode() ? m_zoom : m_scale * m_zoom;
    if (k <= 0.0) return r;
    return QRectF(r.left() / k, r.top() / k, r.width() / k, r.height() / k);
}

QRectF PdfCanvas::ptToPx(const QRectF& r) const
{
    const double k = vectorMode() ? m_zoom : m_scale * m_zoom;
    return QRectF(r.left() * k, r.top() * k, r.width() * k, r.height() * k);
}

QColor PdfCanvas::colorForKind(const QString& kind)
{
    if (kind == QLatin1String("global_table"))       return QColor(30, 120, 220);
    if (kind == QLatin1String("material_no_region")) return QColor(220, 120, 20);
    return QColor(30, 160, 90);                      // detail_table
}

void PdfCanvas::setPageWords(const QVector<WordBox>& w)
{
    m_pageWords = w;
    emit repaintNeeded();
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------
void PdfCanvas::mousePress(const QPointF& pos)
{
    m_dragging = true;
    m_pressPt  = pos;
    m_curPt    = pos;
    emit repaintNeeded();
}

void PdfCanvas::mouseMove(const QPointF& pos)
{
    if (!m_dragging) return;
    m_curPt = pos;
    // 实时识别：收集橡皮筋内（中心落在框内）的矢量词并上报。
    const QRectF bandPt = pxToPt(QRectF(m_pressPt, m_curPt).normalized());
    m_previewWords.clear();
    QStringList txt;
    for (const WordBox& w : m_pageWords) {
        if (bandPt.contains(w.rectPt.center()) && !w.text.isEmpty()) {
            m_previewWords.append(w);
            txt.append(w.text);
        }
    }
    emit textPreview(m_page, txt.join(QStringLiteral(" ")));
    emit repaintNeeded();
}

void PdfCanvas::mouseRelease(const QPointF& pos)
{
    if (!m_dragging) return;
    m_dragging = false;
    m_curPt = pos;
    const QRectF r = QRectF(m_pressPt, m_curPt).normalized();
    m_previewWords.clear();
    emit textPreview(m_page, QString());
    emit repaintNeeded();
    // 忽略误点/过小的拖拽。
    if (r.width() >= 3 && r.height() >= 3)
        emit regionSelected(m_page, m_kind, pxToPt(r));
}

bool PdfCanvas::wheelZoom(int angleDeltaY)
{
    if (angleDeltaY == 0) return false;
    setZoom(m_zoom * (angleDeltaY > 0 ? 1.15 : 1.0 / 1.15));
    return true;
}

// ---------------------------------------------------------------------------
// 可视区分块请求
// ---------------------------------------------------------------------------
QRect PdfCanvas::visibleDevRect(const QRect& visibleRect, double dpr) const
{
    const double scale = m_zoom * dpr;      // 设备像素 / pt
    if (scale <= 0.0 || m_zoom <= 0.0) return QRect();
    // widget px → pt → 设备像素
    const QRectF dev(visibleRect.x() / m_zoom * scale, visibleRect.y() / m_zoom * scale,
                     visibleRect.width() / m_zoom * scale, visibleRect.height() / m_zoom * scale);
    const int T = PdfRenderService::instance()->tileSizeDevPx();
    // 按分块网格对齐并**往外扩一块**：这样小幅滚动仍在同一批块内，不会反复重渲染。
    const int x0 = int(std::floor(dev.left()   / T)) * T - T;
    const int y0 = int(std::floor(dev.top()    / T)) * T - T;
    const int x1 = int(std::ceil (dev.right()  / T)) * T + T;
    const int y1 = int(std::ceil (dev.bottom() / T)) * T + T;
    return QRect(QPoint(x0, y0), QPoint(x1, y1));
}

void PdfCanvas::requestVisibleTiles(const QRect& visibleRect, double dpr)
{
    auto* svc = PdfRenderService::instance();
    if (!svc->ready() || svc->path() != m_pdfPath) return;
    const QSizeF pt = svc->pagePointSize(m_page);
    if (pt.isEmpty()) return;

    const double scale = m_zoom * dpr;
    const QRect vis = visibleDevRect(visibleRect, dpr);
    if (!vis.isValid()) return;
    m_lastVisibleDev = vis;

    // 页面在设备像素空间的范围：请求前先夹一次，保证"请求的矩形"与"回来的矩形"一致
    // （否则边缘那些被夹掉的块会因为 rect 不等而反复请求）。
    const QRect pageDev(0, 0, qMax(1, qRound(pt.width() * scale)),
                                qMax(1, qRound(pt.height() * scale)));
    const QRect need = vis.intersected(pageDev);
    if (need.isEmpty()) return;

    const int T = svc->tileSizeDevPx();
    for (int y = need.top(); y < need.bottom(); y += T) {
        for (int x = need.left(); x < need.right(); x += T) {
            const QRect tile = QRect(x, y, T, T).intersected(pageDev);
            if (tile.isEmpty()) continue;
            if (svc->hasRegionCovering(m_page, scale, tile)) continue;
            svc->requestRegion(m_page, scale, tile);
            ++m_tileRequests;
        }
    }
}

bool PdfCanvas::visibleTilesReadyForTest() const
{
    auto* svc = PdfRenderService::instance();
    if (!vectorMode() || !svc->ready() || m_lastVisibleDev.isEmpty()) return false;
    const QSizeF pt = svc->pagePointSize(m_page);
    if (pt.isEmpty()) return false;
    const double scale = deviceScaleForTest(1.0);   // 自检在 dpr=1 下跑
    const QRect pageDev(0, 0, qMax(1, qRound(pt.width() * scale)),
                                qMax(1, qRound(pt.height() * scale)));
    const QRect need = m_lastVisibleDev.intersected(pageDev);
    const int T = svc->tileSizeDevPx();
    for (int y = need.top(); y < need.bottom(); y += T)
        for (int x = need.left(); x < need.right(); x += T) {
            const QRect tile = QRect(x, y, T, T).intersected(pageDev);
            if (tile.isEmpty()) continue;
            if (!svc->hasRegionCovering(m_page, scale, tile)) return false;
        }
    return true;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void PdfCanvas::paint(QPainter& g, const QRect& exposedRect, const QRect& visibleRect)
{
    ++m_paintCalls;
    const QRect& widgetRect = exposedRect;
    // 底色：画布（页面之外那一圈）。用深灰而不是纯黑 —— "全黑"是最难定位的故障形态。
    const QColor bg(0x3A, 0x3E, 0x44);
    g.fillRect(widgetRect, bg);

    const QSizeF pt = pageSizePt();
    if (pt.isEmpty()) {
        g.setPen(QColor(0xE8, 0xEC, 0xF2));
        QFont f = g.font();
        f.setPointSizeF(f.pointSizeF() + 1.5);
        g.setFont(f);
        const QString msg = m_renderFailed
            ? QStringLiteral("页面渲染失败\n%1").arg(m_failReason.isEmpty()
                                                     ? QStringLiteral("（详见「内核日志」页）") : m_failReason)
            : (vectorMode() ? QStringLiteral("正在打开文档…")
                            : QStringLiteral("尚未渲染页面 — 打开一个 PDF 或点「重渲染」"));
        g.drawText(widgetRect.adjusted(24, 24, -24, -24),
                   Qt::AlignCenter | Qt::TextWordWrap, msg);
        return;
    }

    // ⚠️ 纸面白底。PDFium 渲染出的页面背景是**透明**的（既不是白也不是黑），
    //    少了这一层，深色画布就会从页面下面透出来 —— 用户看到的就是"黑页面"。
    //    （连续视图早就铺了白底，单页画布一直漏了这一条。）
    g.fillRect(QRectF(0, 0, pt.width() * m_zoom, pt.height() * m_zoom).intersected(QRectF(widgetRect)),
               Qt::white);

    const double dpr = g.device() ? g.device()->devicePixelRatioF() : 1.0;

    if (vectorMode()) {
        // 已缓存的区域：cachedRegions 按倍率**升序**返回 ⇒ 先画糊的、后画清的，
        // 天然形成"渐进式细化"，不需要任何额外的状态机。
        g.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const PdfRegion& r : PdfRenderService::instance()->cachedRegions(m_page)) {
            const double k = m_zoom / r.scale;    // 设备像素 → widget px
            const QRectF dst(r.devRect.x() * k, r.devRect.y() * k,
                             r.devRect.width() * k, r.devRect.height() * k);
            if (!dst.intersects(QRectF(widgetRect))) continue;
            g.drawImage(dst, r.img);
        }
    } else if (!m_img.isNull()) {
        g.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 0.999 || m_zoom > 1.001);
        const QRectF dst(0, 0, m_img.width() * m_zoom, m_img.height() * m_zoom);
        g.drawImage(dst, m_img, QRectF(m_img.rect()));
    }

    // 已有框选（pt -> px）。
    for (const Region& r : m_regions) {
        if (r.page != m_page) continue;
        const QColor c = colorForKind(r.kind);
        const QRectF px = ptToPx(r.rectPt);
        g.setPen(QPen(c, 2));
        g.setBrush(QColor(c.red(), c.green(), c.blue(), 40));
        g.drawRect(px);
    }

    // 内核返回的词盒：绿色=矢量文本，橙色=OCR 补位。
    for (const WordBox& w : m_words) {
        if (w.page != m_page) continue;
        const QRectF px = ptToPx(w.rectPt);
        g.setPen(QPen(w.source == QLatin1String("ocr") ? QColor(230, 120, 0)
                                                       : QColor(20, 150, 80), 1));
        g.drawRect(px);
    }

    // 橡皮筋内的词高亮。
    if (!m_previewWords.isEmpty()) {
        g.setPen(QPen(QColor(255, 170, 0), 2));
        g.setBrush(QColor(255, 200, 0, 60));
        for (const WordBox& w : m_previewWords) {
            const QRectF px = ptToPx(w.rectPt);
            g.drawRect(px);
        }
    }

    // 进行中的橡皮筋。
    if (m_dragging) {
        const QRectF r = QRectF(m_pressPt, m_curPt).normalized();
        g.setPen(QPen(QColor(255, 0, 0), 2, Qt::DashLine));
        g.setBrush(QColor(255, 0, 0, 30));
        g.drawRect(r);
    }

    // 最后再按需请求可视区的高清块（异步；这次的画面用的是已有缓存）。
    // ⚠️ 用 visibleRect 而不是 exposedRect：见头文件里的说明。
    if (vectorMode()) requestVisibleTiles(visibleRect, dpr);
}
