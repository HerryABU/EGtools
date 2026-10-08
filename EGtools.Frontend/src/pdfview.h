#pragma once

#include <QWidget>
#include <QImage>
#include <QPoint>
#include <QVector>
#include "types.h"

// Displays one rendered PDF page (a QImage produced by the CLI --render mode)
// and lets the user rubber-band boxes on it. This is the coordinate hub of the
// front-end: it owns the ONLY px<->pt mapping in the app, derived purely from
// the render scale the kernel reports — nothing is hard-coded.
class PdfView : public QWidget
{
    Q_OBJECT
public:
    explicit PdfView(QWidget* parent = nullptr);

    void setPage(const QImage& img, double renderScale, int pageIndex);
    void setPageIndex(int p) { m_page = p; update(); }
    int  pageIndex() const { return m_page; }

    void setKind(const QString& kind) { m_kind = kind; }
    QString kind() const { return m_kind; }

    void addRegion(const Region& r) { m_regions.push_back(r); update(); }
    void clearRegions() { m_regions.clear(); m_words.clear(); update(); }
    QVector<Region> regions() const { return m_regions; }

    void setWordBoxes(const QVector<WordBox>& w) { m_words = w; update(); }
    void clearWordBoxes() { m_words.clear(); update(); }

    // renderScale == image pixels per PDF point (as reported by the kernel).
    double renderScale() const { return m_scale; }

    // The single source of truth for the coordinate mapping.
    QRectF pxToPt(const QRectF& r) const;
    QRectF ptToPx(const QRectF& r) const;

signals:
    void regionSelected(int page, const QString& kind, const QRectF& rectPt);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    static QColor colorForKind(const QString& kind);

    QImage  m_img;
    double  m_scale = 1.0;     // image px per PDF pt
    int     m_page  = 0;
    QString m_kind  = QStringLiteral("detail_table");
    QVector<Region> m_regions;
    QVector<WordBox> m_words;

    bool    m_dragging = false;
    QPoint  m_pressPt;
    QPoint  m_curPt;
};