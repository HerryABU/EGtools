#pragma once

#include <QHash>
#include <QImage>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include "iregioncanvas.h"
#include "types.h"

class QFutureWatcherBase;
class QScrollArea;
class QThreadPool;

// ---------------------------------------------------------------------------
// ContinuousPdfView —— Adobe 式「连续滚动多页」视图
//
// 一屏内纵向排布**所有页**（页间留白 + 页边框 + 页码标签），滚动即翻页；
// 任意页上都可直接橡皮筋框选。要点：
//
//  1) **懒渲染**：只为「可视区 ±1 页」请求内核渲染，滚到哪渲染哪；
//     离开视野较远的页会释放图像（大图纸单页就是几十 MB，必须回收）。
//  2) **渲染在后台线程**（单线程池）。内核调用在 KernelClient 内部已全局串行，
//     所以这里串行排队既安全、也能给出稳定的进度顺序。
//  3) **坐标映射只有一条式子**：`widgetPx = pt * zoom`（zoom=1 时 1pt=1px）。
//     渲染比例只影响图像清晰度，不参与坐标换算 —— 与单页视图的约定一致。
//  4) 缩放保持**视口锚点**（缩放前后视口中心对准同一处内容），这是专业阅读器的
//     基本手感；Ctrl+滚轮同样以光标为锚。
// ---------------------------------------------------------------------------
class ContinuousPdfView : public QWidget, public IRegionCanvas
{
    Q_OBJECT
public:
    explicit ContinuousPdfView(QWidget* parent = nullptr);
    ~ContinuousPdfView() override;

    // 必须在加入 QScrollArea 之后调用：用于监听滚动/视口尺寸，自己做懒加载与居中。
    void attachScrollArea(QScrollArea* area);

    void setCliDir(const QString& dir);
    void setKernelBackend(const QString& mode) { m_backend = mode; }

    void setDocument(const QString& pdfPath, int pageCount);
    void clearDocument();
    QString pdfPath() const { return m_pdfPath; }

    // 当前「页」（取视口中心所在页；用于页码显示与缩略图高亮）。
    int currentPage() const;
    bool hasDocument() const { return !m_pdfPath.isEmpty() && !m_slots.isEmpty(); }

    // IRegionCanvas
    void addRegion(const Region& r) override;
    void clearRegions() override;
    QVector<Region> regions() const override { return m_regions; }
    void setKind(const QString& kind) override { m_kind = kind; }
    QString kind() const override { return m_kind; }
    void setZoom(double z) override;
    double zoom() const override { return m_zoom; }
    void zoomIn() override  { setZoom(m_zoom * 1.25); }
    void zoomOut() override { setZoom(m_zoom / 1.25); }
    void fitToWidth(int viewportWidthPx) override;
    void setWordBoxes(const QVector<WordBox>& words) override;
    void clearWordBoxes() override { m_overlay.clear(); update(); }
    void showPage(int page) override { scrollToPage(page); }

    void scrollToPage(int page);
    void fitToPageHeight(int viewportHeightPx);   // 「适合页面」（整页可见）

    // 抽出：把当前页（视口中心页）的渲染诊断信息回一句给日志用。
    QString lastDiagnostics() const { return m_lastDiag; }

    // ---- 自检用的只读访问器（无头验证版面与命中判定）----
    int    pageCount() const { return m_slots.size(); }
    QRectF pageRect(int page) const;                 // 该页在控件坐标中的矩形
    int    pageAt(const QPointF& widgetPos) const { return slotAt(widgetPos); }
    bool   pageHasImage(int page) const;
    QSizeF pageSizePt(int page) const;
    // 纯版面自检（不碰内核、不依赖 PDF）：堆叠顺序/间距/居中/缩放/命中判定。
    // 返回空串表示全部通过，否则返回失败原因。
    QString runLayoutSelfTest();
    // 调度状态（自检/排障用）：队列深度、正在渲染的页、某页是否待渲染/失败。
    int  debugQueueDepth() const { return m_queue.size(); }
    int  debugInFlightPage() const { return m_inFlightPage; }
    bool debugPending(int page) const
    { return page >= 0 && page < m_slots.size() && m_slots[page].pending; }
    bool debugFailed(int page) const
    { return page >= 0 && page < m_slots.size() && m_slots[page].failed; }
    int  debugRenderedCount() const
    { int n = 0; for (const Slot& s : m_slots) if (!s.img.isNull()) ++n; return n; }
    // 调度轨迹（最近若干步）：用于把"某页为什么不渲染"这类问题一次定位到具体环节。
    QStringList debugTrace() const { return m_trace; }

signals:
    void regionSelected(int page, const QString& kind, const QRectF& rectPt);
    void textPreview(int page, const QString& text);
    void zoomChanged(double zoom);
    void currentPageChanged(int page);
    void statusMessage(const QString& msg);
    void layoutChanged(const QSize& contentSize);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    // 一页的版面与内容
    struct Slot
    {
        int    page = 0;
        QSizeF sizePt;          // 页面尺寸（pt）；未知时用文档默认尺寸占位
        QRectF rect;            // 在画布控件坐标中的位置
        QImage img;             // 渲染结果（m_bundleScale 下）
        double bundleScale = 0; // 该图的渲染比例（px per pt）；0 = 未渲染
        bool   pending = false;
        bool   failed  = false;
        bool   known   = false; // sizePt 是否来自真实渲染
    };

    void   relayout();
    void   updateVisiblePages();
    void   pumpQueue();
    void   startRender(int page);
    void   evictFarPages(int firstVisible, int lastVisible);
    // 把某页插到渲染队首（用户明确要看的页必须优先，哪怕视口还没更新）。
    void   ensurePageQueued(int page);
    void   ensureWordsAsync(int page);
    int    slotAt(const QPointF& widgetPos) const;
    QRectF pageRectWidget(int page) const;   // 该页在控件坐标里的矩形（不含外边距）
    QRectF contentVisibleRect() const;       // 当前可视区（控件坐标）
    void   setZoomAnchored(double newZoom, const QPointF& anchorWidget);
    double bundleScaleForZoom(double z) const;
    void   invalidateImages();
    int    slotIndex(int page) const { return page; }   // 槽位索引 == 页号（全页都在）

    QString m_pdfPath;
    int     m_pageCount = 0;
    int     m_focusPage = 0;                 // 用户明确关注的页（可视区退化时的锚点）
    QString m_cliDir;
    QString m_backend = QStringLiteral("auto");

    QSizeF  m_defaultPtSize{595.0, 842.0};   // 未渲染前的占位尺寸（首屏渲染后被校正）
    double  m_zoom = 1.0;
    double  m_bundleScale = 1.0;             // 当前渲染比例档位（1.0 / 2.0）
    QString m_kind = QStringLiteral("detail_table");
    QString m_lastDiag;

    QVector<Slot>   m_slots;
    QVector<Region> m_regions;
    QVector<WordBox> m_overlay;              // 抽取结果叠加层（按 page 区分）
    QHash<int, QVector<WordBox>> m_wordsByPage;
    QSet<int>       m_wordsPending;
    QVector<WordBox> m_previewWords;         // 当前橡皮筋内的词（实时识别）

    QList<int> m_queue;                      // 待渲染队列（按与视口的距离排序）
    bool       m_renderInFlight = false;
    int        m_inFlightPage = -1;          // 正在渲染的页（-1 = 无）
    QThreadPool* m_pool = nullptr;
    QScrollArea* m_scroll = nullptr;
    bool       m_relayoutGuard = false;

    bool    m_dragging = false;
    int     m_dragPage = -1;
    QPointF m_pressPt;
    QPointF m_curPt;

    QStringList m_trace;                     // 调度轨迹（上限 200 条）
    void trace(const QString& line)
    {
        m_trace.append(line);
        if (m_trace.size() > 200) m_trace.removeFirst();
    }
};
