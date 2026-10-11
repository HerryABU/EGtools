#include "pdfcanvas.h"

#include <QPainter>
#include <algorithm>

PdfCanvas::PdfCanvas(QObject* parent) : QObject(parent) {}

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

QSize PdfCanvas::canvasSize() const
{
    if (m_img.isNull()) return QSize(1, 1);
    return QSize(qMax(1, qRound(m_img.width()  * m_zoom)),
                 qMax(1, qRound(m_img.height() * m_zoom)));
}

void PdfCanvas::setZoom(double z)
{
    // 限幅避免把内存/纹理撑爆（400% 已经足够看清图元）。
    const double nz = std::clamp(z, 0.05, 4.0);
    if (qFuzzyCompare(nz, m_zoom)) return;
    m_zoom = nz;
    emit zoomChanged(m_zoom);
    emit canvasResized(canvasSize());
    emit repaintNeeded();
}

void PdfCanvas::fitToWidth(int viewportWidthPx)
{
    if (m_img.isNull() || viewportWidthPx <= 0) return;
    setZoom(static_cast<double>(viewportWidthPx - 8) / m_img.width());
}

// pt = widgetPx / (renderScale * zoom)
QRectF PdfCanvas::pxToPt(const QRectF& r) const
{
    const double k = m_scale * m_zoom;
    if (k <= 0.0) return r;
    return QRectF(r.left() / k, r.top() / k, r.width() / k, r.height() / k);
}

QRectF PdfCanvas::ptToPx(const QRectF& r) const
{
    const double k = m_scale * m_zoom;
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
// 绘制
// ---------------------------------------------------------------------------
void PdfCanvas::paint(QPainter& g, const QRect& widgetRect)
{
    // 底色：渲染失败时给出**可读原因**而不是一块深灰（避免"黑屏但不知为何"）。
    const QColor bg(0x3A, 0x3E, 0x44);
    g.fillRect(widgetRect, bg);

    if (m_img.isNull()) {
        g.setPen(QColor(0xE8, 0xEC, 0xF2));
        QFont f = g.font();
        f.setPointSizeF(f.pointSizeF() + 1.5);
        g.setFont(f);
        const QString msg = m_renderFailed
            ? QStringLiteral("页面渲染失败\n%1").arg(m_failReason.isEmpty()
                                                     ? QStringLiteral("（详见「内核日志」页）") : m_failReason)
            : QStringLiteral("尚未渲染页面 — 打开一个 PDF 或点「重渲染」");
        g.drawText(widgetRect.adjusted(24, 24, -24, -24),
                   Qt::AlignCenter | Qt::TextWordWrap, msg);
        return;
    }

    // 缩放 != 100% 时开启平滑（GPU 路径由纹理过滤完成，几乎零成本）。
    g.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 0.999 || m_zoom > 1.001);
    g.drawImage(QRectF(0, 0, widgetRect.width(), widgetRect.height()), m_img,
                QRectF(0, 0, m_img.width(), m_img.height()));

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
    // 只画属于当前页的（叠加层现在收录所有页，供连续视图按页绘制）。
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
}
