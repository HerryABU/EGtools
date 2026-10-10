#include "pdfview.h"
#include <QPainter>
#include <QMouseEvent>
#include <algorithm>

PdfView::PdfView(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::gray);
    setPalette(pal);
}

void PdfView::setPage(const QImage& img, double renderScale, int pageIndex)
{
    m_img   = img;
    m_scale = (renderScale > 0.0) ? renderScale : 1.0;
    m_page  = pageIndex;
    m_dragging = false;
    m_pageWords.clear();     // a new page => words are reloaded by the caller
    m_previewWords.clear();
    // Display the rendered image 1:1 (scroll area handles overflow). Because the
    // widget is exactly the image size, widget px == image px, and pt = px/scale.
    setFixedSize(img.width(), img.height());
    update();
}

QRectF PdfView::pxToPt(const QRectF& r) const
{
    if (m_scale <= 0.0) return r;
    return QRectF(r.left() / m_scale, r.top() / m_scale,
                  r.width() / m_scale, r.height() / m_scale);
}

QRectF PdfView::ptToPx(const QRectF& r) const
{
    return QRectF(r.left() * m_scale, r.top() * m_scale,
                  r.width() * m_scale, r.height() * m_scale);
}

QColor PdfView::colorForKind(const QString& kind)
{
    if (kind == QLatin1String("global_table"))       return QColor(30, 120, 220);
    if (kind == QLatin1String("material_no_region")) return QColor(220, 120, 20);
    return QColor(30, 160, 90);                      // detail_table
}

void PdfView::paintEvent(QPaintEvent*)
{
    QPainter g(this);
    g.fillRect(rect(), QColor(90, 90, 90));
    if (!m_img.isNull())
        g.drawImage(0, 0, m_img);

    // Existing selections (drawn in pt -> px).
    for (const Region& r : m_regions) {
        if (r.page != m_page) continue;
        QColor c = colorForKind(r.kind);
        QRectF px = ptToPx(r.rectPt);
        g.setPen(QPen(c, 2));
        g.setBrush(QColor(c.red(), c.green(), c.blue(), 40));
        g.drawRect(px);
    }

    // Word segments returned by the kernel.
    g.setPen(QPen(QColor(200, 0, 0), 1));
    for (const WordBox& w : m_words) {
        QRectF px = ptToPx(w.rectPt);
        g.drawRect(px);
    }

    // Live-in-canvas recognition: words inside the current rubber band are
    // highlighted so the user sees exactly what will be recognised.
    if (!m_previewWords.isEmpty()) {
        g.setPen(QPen(QColor(255, 170, 0), 2));
        g.setBrush(QColor(255, 200, 0, 60));
        for (const WordBox& w : m_previewWords) {
            QRectF px = ptToPx(w.rectPt);
            g.drawRect(px);
        }
    }

    // Live rubber band.
    if (m_dragging) {
        QRect r = QRect(m_pressPt, m_curPt).normalized();
        g.setPen(QPen(QColor(255, 0, 0), 2, Qt::DashLine));
        g.setBrush(QColor(255, 0, 0, 30));
        g.drawRect(r);
    }
}

void PdfView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    m_dragging = true;
    m_pressPt = e->position().toPoint();
    m_curPt   = m_pressPt;
    update();
}

void PdfView::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging) { QWidget::mouseMoveEvent(e); return; }
    m_curPt = e->position().toPoint();
    // Live text recognition: gather vector words whose centre lies inside the
    // current rubber band, then announce the joined text to the front-end.
    QRectF bandPt = pxToPt(QRectF(QRect(m_pressPt, m_curPt).normalized()));
    m_previewWords.clear();
    QStringList txt;
    for (const WordBox& w : m_pageWords) {
        if (bandPt.contains(w.rectPt.center()) && !w.text.isEmpty()) {
            m_previewWords.append(w);
            txt.append(w.text);
        }
    }
    emit textPreview(m_page, txt.join(QStringLiteral(" ")));
    update();
}

void PdfView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !m_dragging) { QWidget::mouseReleaseEvent(e); return; }
    m_dragging = false;
    m_curPt = e->position().toPoint();
    QRect r = QRect(m_pressPt, m_curPt).normalized();
    update();
    // Clear the live preview highlight / announcement now that the band is gone.
    m_previewWords.clear();
    emit textPreview(m_page, QString());
    // Ignore accidental clicks / tiny drags.
    if (r.width() >= 3 && r.height() >= 3) {
        QRectF rectPt = pxToPt(QRectF(r));
        emit regionSelected(m_page, m_kind, rectPt);
    }
}