#include "continuouspdfview.h"

#include "kernelclient.h"
#include "pdfrender.h"
#include "pdfview.h"   // imageLuminanceStats
#include "pagerender.h"
#include "theme.h"

#include <QApplication>
#include <QFutureWatcher>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QThreadPool>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace {

constexpr int kMarginX   = 16;   // 页面左右外边距
constexpr int kMarginY   = 14;   // 顶部/底部外边距
constexpr int kGapY      = 18;   // 页间距
constexpr int kLabelH    = 18;   // 页码标签高度
constexpr double kMinZoom = 0.08;
constexpr double kMaxZoom = 3.0;
constexpr int kKeepPages = 2;    // 视口外保留 ±N 页的图像，再远则释放

} // namespace

ContinuousPdfView::ContinuousPdfView(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAutoFillBackground(false);          // 由 paintEvent 负责底色

    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(1);          // 词盒提取仍走内核（全局串行），排队顺序更可预期

    // 页面图像改由 **Qt PDF（PDFium）** 渲染：在它自己的线程里按目标分辨率重绘，
    // 不再"每页起一次内核进程"——那是连续视图不稳定、翻页慢的主因。
    auto* svc = PdfRenderService::instance();
    connect(svc, &PdfRenderService::regionReady, this, &ContinuousPdfView::onRegionReady);
    connect(svc, &PdfRenderService::documentReady, this,
            [this](const QString& path, int count) {
        if (path != m_pdfPath) return;
        adoptDocumentSizes(count);
        relayout();
        ensurePageQueued(0);
        updateVisiblePages();
        update();
    });
}

ContinuousPdfView::~ContinuousPdfView()
{
    if (m_pool) { m_pool->waitForDone(3000); }
}

void ContinuousPdfView::attachScrollArea(QScrollArea* area)
{
    m_scroll = area;
    if (!m_scroll) return;
    m_scroll->viewport()->installEventFilter(this);
    connect(m_scroll->verticalScrollBar(),   &QScrollBar::valueChanged, this, [this](int){
        m_focusPage = currentPage();   // 真实滚动后，关注页跟着视口走
        updateVisiblePages();
    });
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int){ updateVisiblePages(); });
}

bool ContinuousPdfView::eventFilter(QObject* obj, QEvent* ev)
{
    if (m_scroll && obj == m_scroll->viewport() && ev->type() == QEvent::Resize) {
        relayout();          // 视口变宽/变窄 ⇒ 重新居中并更新可视区
    }
    return QWidget::eventFilter(obj, ev);
}

void ContinuousPdfView::setCliDir(const QString& dir)
{
    m_cliDir = dir;
    KernelClient::setCliDir(dir);
    // 内核目录变了 ⇒ 之前的渲染结果可能来自另一个内核，重新按可视区取一遍。
    if (!m_slots.isEmpty()) { invalidateImages(); relayout(); }
}

// ---------------------------------------------------------------------------
// 文档
// ---------------------------------------------------------------------------
// 各页 pt 尺寸直接从渲染服务（PDFium）取 —— 顺带解决旧实现的"探测式尺寸"问题：
// 旧版要等第 0 页渲染完才知道真实尺寸，期间版面用的是默认占位尺寸。
void ContinuousPdfView::adoptDocumentSizes(int pageCount)
{
    auto* svc = PdfRenderService::instance();
    if (!svc->ready() || svc->path() != m_pdfPath) return;
    m_pageCount = qMax(1, qMin(pageCount, svc->pageCount()));
    if (m_slots.size() != m_pageCount) {
        m_slots.resize(m_pageCount);
        for (int i = 0; i < m_pageCount; ++i) m_slots[i].page = i;
        m_bundleScale = bundleScaleForZoom(m_zoom);
    }
    for (int i = 0; i < m_pageCount; ++i) {
        const QSizeF sz = svc->pagePointSize(i);
        if (sz.width() > 1.0 && sz.height() > 1.0) {
            m_slots[i].sizePt = sz;
            m_slots[i].known  = true;
        }
    }
    const QSizeF p0 = svc->pagePointSize(0);
    if (p0.width() > 1.0) m_defaultPtSize = p0;
}

void ContinuousPdfView::setDocument(const QString& pdfPath, int pageCount)
{
    clearDocument();
    if (pdfPath.isEmpty() || pageCount <= 0) return;

    m_pdfPath   = pdfPath;
    m_pageCount = pageCount;

    // 文档由渲染服务持有（PDFium）。这里主动确保它已打开 —— 否则"直接调 setDocument"
    // 的调用方（自检、或任何不走主窗口流程的用法）会因为服务没加载文档而永远渲染不出来。
    auto* svc0 = PdfRenderService::instance();
    if (svc0->path() != pdfPath || !svc0->ready()) svc0->openDocument(pdfPath);

    m_slots.resize(pageCount);
    for (int i = 0; i < pageCount; ++i) {
        Slot& s = m_slots[i];
        s.page    = i;
        s.sizePt  = m_defaultPtSize;
        s.bundleScale = 0;
    }
    adoptDocumentSizes(pageCount);      // 各页 pt 尺寸（PDFium 直接给，无需逐页渲染探测）
    m_bundleScale = bundleScaleForZoom(m_zoom);
    relayout();
    // 文档就绪后再入队（未就绪时页尺寸还是占位值，见 startRender 的说明）。
    if (svc0->ready() && svc0->path() == pdfPath) ensurePageQueued(0);
    updateVisiblePages();
    scrollToPage(0);
    emit currentPageChanged(0);
    emit statusMessage(QStringLiteral("连续视图：共 %1 页，滚动即按需渲染").arg(pageCount));
}

void ContinuousPdfView::clearDocument()
{
    for (Slot& s : m_slots) s.img = QImage();     // 尽快释放大图
    m_slots.clear();
    m_queue.clear();
    m_pdfPath.clear();
    m_pageCount = 0;
    m_regions.clear();
    m_overlay.clear();
    m_wordsByPage.clear();
    m_wordsPending.clear();
    m_previewWords.clear();
    m_dragging = false;
    m_dragPage = -1;
    m_lastDiag.clear();
    setFixedSize(1, 1);
}

// ---------------------------------------------------------------------------
// 版面
// ---------------------------------------------------------------------------
double ContinuousPdfView::bundleScaleForZoom(double z) const
{
    // 渲染比例档位：避免每个缩放级别都重渲一遍图，但又保证放大后不至于糊掉。
    // 上限 2.0 —— 再高对大图纸就是几百 MB 的单页位图，得不偿失。
    return z <= 1.25 ? 1.0 : 2.0;
}

void ContinuousPdfView::relayout()
{
    if (m_slots.isEmpty()) { setFixedSize(1, 1); return; }
    m_relayoutGuard = true;

    const int viewportW = (m_scroll && m_scroll->viewport()) ? m_scroll->viewport()->width() : width();

    double maxPageW = 0;
    for (const Slot& s : m_slots)
        maxPageW = std::max(maxPageW, s.sizePt.width() * m_zoom);

    const int contentW = std::max<double>(viewportW, maxPageW + 2.0 * kMarginX);

    int y = kMarginY;
    for (Slot& s : m_slots) {
        const double w = s.sizePt.width()  * m_zoom;
        const double h = s.sizePt.height() * m_zoom;
        const double x = (contentW - w) / 2.0;      // 页窄于视口时居中（阅读器惯例）
        s.rect = QRectF(x, y, w, h);
        y += static_cast<int>(h + kLabelH + kGapY);
    }
    setFixedSize(contentW, y - kGapY + kMarginY);

    m_relayoutGuard = false;
    emit layoutChanged(size());
    updateVisiblePages();
    update();
}

QRectF ContinuousPdfView::contentVisibleRect() const
{
    if (m_scroll) {
        const int x = m_scroll->horizontalScrollBar()->value();
        const int y = m_scroll->verticalScrollBar()->value();
        return QRectF(x, y, m_scroll->viewport()->width(), m_scroll->viewport()->height());
    }
    return QRectF(0, 0, width(), height());
}

QRectF ContinuousPdfView::pageRectWidget(int page) const
{
    if (page < 0 || page >= m_slots.size()) return QRectF();
    return m_slots[page].rect;
}

int ContinuousPdfView::slotAt(const QPointF& widgetPos) const
{
    for (int i = 0; i < m_slots.size(); ++i) {
        // 把页码标签也算进命中区，这样点页脚也能落在该页上。
        QRectF r = m_slots[i].rect;
        r.setHeight(r.height() + kLabelH);
        if (r.contains(widgetPos)) return i;
    }
    return -1;
}

int ContinuousPdfView::currentPage() const
{
    const QRectF vis = contentVisibleRect();
    const QPointF center = vis.center();
    const int i = slotAt(center);
    if (i >= 0) return i;
    // 中心落在页间留白：取离中心最近的页。
    int best = 0; double bestD = 1e18;
    for (int k = 0; k < m_slots.size(); ++k) {
        const QPointF c = m_slots[k].rect.center();
        const double d = QLineF(c, center).length();
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// 懒渲染
// ---------------------------------------------------------------------------
void ContinuousPdfView::updateVisiblePages()
{
    if (m_slots.isEmpty() || m_relayoutGuard) return;

    QRectF vis = contentVisibleRect();

    // ⚠️ 滚动条可能还没反映真实布局（窗口刚打开、控件未显示、布局事件未跑完），
    // 此时 contentVisibleRect() 会退化成"内容左上角"。曾经因此出现两个后果：
    //   ① 用户点了第 N 页却不渲染（它被判成不可见）；
    //   ② 刚渲染好的第 N 页立刻被 evictFarPages 回收（白渲染一场）。
    // 所以把"用户明确关注的页"（scrollToPage/翻页/缩略图点击所设）也并入可视区，
    // 让它既能被请求渲染，也不会被回收。真实滚动时 m_focusPage 会跟着视口走。
    if (m_focusPage >= 0 && m_focusPage < m_slots.size()) {
        const QRectF fr = m_slots[m_focusPage].rect;
        if (fr.isValid()) vis = (vis.height() <= 1.0 || vis.width() <= 1.0) ? fr : vis.united(fr);
    }

    // 上下各多要一点（±1 页高），滚动时下一屏已就绪，避免"白块追着滚"。
    const QRectF lookahead = vis.adjusted(0, -vis.height() * 0.6, 0, vis.height() * 0.6);

    QList<int> want;
    for (int i = 0; i < m_slots.size(); ++i) {
        Slot& s = m_slots[i];
        if (!s.rect.intersects(lookahead)) continue;
        if (s.bundleScale != m_bundleScale) {         // 未渲染，或渲染档位与当前缩放不符
            if (!s.pending) { s.pending = true; want.append(i); }
        }
    }

    // 靠近视口中心的先渲染 —— 用户最可能马上看到它。
    const double cy = vis.center().y();
    std::sort(want.begin(), want.end(), [this, cy](int a, int b){
        return qAbs(m_slots[a].rect.center().y() - cy) < qAbs(m_slots[b].rect.center().y() - cy);
    });

    for (int p : want) if (!m_queue.contains(p)) m_queue.append(p);

    // 释放远处页面的位图（大图纸单页几十 MB，不回收会吃光内存）。
    int first = -1, last = -1;
    for (int i = 0; i < m_slots.size(); ++i)
        if (m_slots[i].rect.intersects(vis)) { if (first < 0) first = i; last = i; }
    if (first >= 0) evictFarPages(first, last);

    pumpQueue();
    emit currentPageChanged(currentPage());
}

void ContinuousPdfView::evictFarPages(int firstVisible, int lastVisible)
{
    // 回收窗口 = 可视页 ±kKeepPages，**并且必须包含关注页**（否则刚 scrollToPage
    // 请求来的、或刚渲染完成的页会在下一次 updateVisiblePages 里被立刻扔掉）。
    int lo = firstVisible - kKeepPages;
    int hi = lastVisible + kKeepPages;
    if (m_focusPage >= 0) {
        lo = qMin(lo, m_focusPage - kKeepPages);
        hi = qMax(hi, m_focusPage + kKeepPages);
    }
    for (int i = 0; i < m_slots.size(); ++i) {
        if (i >= lo && i <= hi) continue;
        if (i == m_inFlightPage) continue;      // 正在渲染的页别动
        Slot& s = m_slots[i];
        if (!s.img.isNull()) {
            s.img = QImage(); s.bundleScale = 0; s.failed = false;
            // ⚠️ 不能顺手清 pending：该页可能正躺在渲染队列里，清了它就会被"遗忘"。
            trace(QStringLiteral("evict p%1").arg(i));
        }
    }
}

void ContinuousPdfView::ensurePageQueued(int page)
{
    if (page < 0 || page >= m_slots.size() || m_pdfPath.isEmpty()) return;
    Slot& s = m_slots[page];
    if (!s.img.isNull() && s.bundleScale == m_bundleScale) return;   // 已有可用图
    if (page == m_inFlightPage) return;                              // 正在渲染，别重复排队
    m_queue.removeAll(page);
    m_queue.prepend(page);        // 用户明确要看的页 → 插队首
    s.pending = true;
    trace(QStringLiteral("ensure p%1 -> q=%2").arg(page).arg(m_queue.size()));
}

void ContinuousPdfView::pumpQueue()
{
    if (m_renderInFlight || m_queue.isEmpty() || m_pdfPath.isEmpty()) return;
    const int page = m_queue.takeFirst();
    trace(QStringLiteral("pump take p%1 q=%2").arg(page).arg(m_queue.size()));
    startRender(page);
}

void ContinuousPdfView::startRender(int page)
{
    if (page < 0 || page >= m_slots.size()) return;
    if (!m_slots[page].img.isNull() && m_slots[page].bundleScale == m_bundleScale) {
        m_slots[page].pending = false;
        pumpQueue();
        return;
    }

    auto* svc = PdfRenderService::instance();

    // ⚠️⚠️ 文档还没就绪时**绝不能**发请求：此时页尺寸还是占位值，算出的矩形与真实
    //   页面尺寸对不上；等真实尺寸到位后，结果回来会被"覆盖判定"拒收 ——
    //   表现就是这一页**永远渲染不出来**（实测踩过：调度轨迹停在 "start p0"）。
    //   正确做法是保持 pending，等 documentReady 里重排后再入队。
    if (!svc->ready() || svc->path() != m_pdfPath) {
        m_renderInFlight = false;
        m_inFlightPage = -1;
        trace(QStringLiteral("wait-doc p%1").arg(page));
        return;
    }

    const double bs = m_bundleScale;
    const QSizeF pt = m_slots[page].sizePt;
    const QRect devRect(0, 0, qMax(1, qRound(pt.width() * bs)),
                                qMax(1, qRound(pt.height() * bs)));

    // 命中渲染服务缓存 ⇒ 立刻可用（来回缩放时几乎瞬时）。
    if (svc->hasRegionCovering(page, bs, devRect)) {
        Slot& s = m_slots[page];
        const QImage img = svc->cachedImageForTest(page, bs, devRect);
        if (!img.isNull()) {
            s.img = img; s.bundleScale = bs; s.failed = false; s.pending = false;
            trace(QStringLiteral("cache hit p%1 bs=%2").arg(page).arg(bs));
            update();
            pumpQueue();
            return;
        }
    }

    if (!svc->requestRegion(page, bs, devRect)) {
        // 请求没被受理（页码越界等）：别把自己标记成"在途"，否则队列会永久卡住。
        m_slots[page].pending = false;
        trace(QStringLiteral("reject p%1").arg(page));
        pumpQueue();
        return;
    }
    m_renderInFlight = true;
    m_inFlightPage = page;
    trace(QStringLiteral("start p%1 bs=%2").arg(page).arg(bs));
    // 结果经 regionReady → onRegionReady 回来（服务在自己的线程里渲染）。
}

// 渲染服务某页有新块可用：只处理"在途那一页"，拿到整页图后继续队列。
void ContinuousPdfView::onRegionReady(int page)
{
    if (page != m_inFlightPage || page < 0 || page >= m_slots.size()) return;
    auto* svc = PdfRenderService::instance();
    const double bs = m_bundleScale;
    const QSizeF pt = m_slots[page].sizePt;
    const QRect devRect(0, 0, qMax(1, qRound(pt.width() * bs)),
                                qMax(1, qRound(pt.height() * bs)));
    const QImage img = svc->cachedImageForTest(page, bs, devRect);
    if (img.isNull()) {
        // 可能是"只到了一部分"（等着覆盖整页的那块）；但也可能是矩形对不上。
        // 无论哪种，**不能把在途标记一直挂着** —— 那会让整条渲染队列永久停住。
        // 这里只解除在途并继续队列；真正的重排在下次绘制/滚动时自然发生。
        m_renderInFlight = false;
        m_inFlightPage = -1;
        trace(QStringLiteral("miss p%1 （未取到覆盖整页的图，解除在途）").arg(page));
        pumpQueue();
        return;
    }

    m_renderInFlight = false;
    m_inFlightPage = -1;

    Slot& s = m_slots[page];
    s.pending = false;
    s.img = img;
    s.bundleScale = bs;
    s.failed = false;
    {
        const LumaStats st = imageLuminanceStats(img);
        m_lastDiag = QStringLiteral("第 %1 页 %2x%3 px 平均亮度 %4 / max %5（PDFium 矢量）")
                         .arg(page + 1).arg(img.width()).arg(img.height())
                         .arg(st.avg, 0, 'f', 1).arg(st.maxLum);
        if (st.maxLum < 24)
            emit statusMessage(QStringLiteral("第 %1 页几乎全黑（max 亮度 %2）——该页可能确实无内容")
                                   .arg(page + 1).arg(st.maxLum));
    }
    trace(QStringLiteral("done p%1 ok=1").arg(page));
    update();
    pumpQueue();
}

void ContinuousPdfView::invalidateImages()
{
    // 只丢**图像**：换渲染档位时页面尺寸（pt）没有任何变化，把它们重置回默认值
    // 会让版面在缩放时"跳一下"，也会让已知的真实页尺寸退化。
    m_queue.clear();
    for (Slot& s : m_slots) {
        s.img = QImage();
        s.bundleScale = 0;
        s.pending = false;
        s.failed = false;
    }
}

QRectF ContinuousPdfView::pageRect(int page) const { return pageRectWidget(page); }

bool ContinuousPdfView::pageHasImage(int page) const
{
    return page >= 0 && page < m_slots.size() && !m_slots[page].img.isNull();
}

QSizeF ContinuousPdfView::pageSizePt(int page) const
{
    if (page < 0 || page >= m_slots.size()) return QSizeF();
    return m_slots[page].sizePt;
}

// ---------------------------------------------------------------------------
// 纯版面自检
//
// 连续视图最容易出错的地方不是渲染，而是**版面与坐标**：页堆叠顺序、页间留白、
// 居中、缩放后重排、点→页命中。这些全都不依赖内核，可以确定性地断言，
// 所以放进自检里，避免"看起来滚动就能用"却把框选算到别的页上去。
// ---------------------------------------------------------------------------
QString ContinuousPdfView::runLayoutSelfTest()
{
    const QString savedPdf = m_pdfPath;
    const int     savedCount = m_pageCount;
    const double  savedZoom = m_zoom;

    // 用合成文档（无 pdfPath ⇒ pumpQueue 提前返回，不会触发任何内核调用）。
    m_pdfPath.clear();
    m_slots.clear();
    m_queue.clear();
    m_pageCount = 4;
    m_defaultPtSize = QSizeF(1000.0, 700.0);
    m_slots.resize(m_pageCount);
    for (int i = 0; i < m_pageCount; ++i) {
        Slot& s = m_slots[i];
        s.page = i;
        s.sizePt = m_defaultPtSize;
        s.rect = QRectF();
        s.bundleScale = 0;
    }
    m_zoom = 1.0;
    m_bundleScale = bundleScaleForZoom(m_zoom);
    relayout();

    auto fail = [&](const QString& why) {
        m_pdfPath = savedPdf; m_pageCount = savedCount; m_zoom = savedZoom;
        return why;
    };

    // 1) 页序：y 单调递增，且每页高度一致（同一尺寸）。
    for (int i = 0; i < m_pageCount; ++i) {
        const QRectF r = m_slots[i].rect;
        if (r.width() <= 0 || r.height() <= 0) return fail(QStringLiteral("第 %1 页尺寸为 0").arg(i + 1));
        if (qAbs(r.height() - 700.0) > 0.6)
            return fail(QStringLiteral("第 %1 页高度异常 %2").arg(i + 1).arg(r.height()));
        if (qAbs(r.width() - 1000.0) > 0.6)
            return fail(QStringLiteral("第 %1 页宽度异常 %2").arg(i + 1).arg(r.width()));
        if (i > 0) {
            const QRectF prev = m_slots[i - 1].rect;
            if (r.top() <= prev.top()) return fail(QStringLiteral("第 %1 页未排在第 %2 页下方").arg(i + 1).arg(i));
            if (r.top() - prev.bottom() < 8.0)
                return fail(QStringLiteral("第 %1/%2 页之间缺少留白(%3)").arg(i).arg(i + 1).arg(r.top() - prev.bottom()));
        }
    }

    // 2) 首末页与外边距
    if (qAbs(m_slots.first().rect.top() - kMarginY) > 0.6)
        return fail(QStringLiteral("首页顶边距异常 %1").arg(m_slots.first().rect.top()));
    if (height() < m_slots.last().rect.bottom())
        return fail(QStringLiteral("内容高度(%1) 未覆盖末页底部(%2)").arg(height()).arg(m_slots.last().rect.bottom()));

    // 3) 命中判定：每页中心点都应命中该页（这是"框选落到正确页"的基础）
    for (int i = 0; i < m_pageCount; ++i) {
        const int hit = slotAt(m_slots[i].rect.center());
        if (hit != i) return fail(QStringLiteral("第 %1 页中心命中了第 %2 页").arg(i + 1).arg(hit + 1));
    }
    // 页间留白处应落在相邻两页之一，而不是 -1（否则拖选会丢失）
    {
        const QPointF gap(m_slots[0].rect.center().x(),
                          m_slots[0].rect.bottom() + (m_slots[1].rect.top() - m_slots[0].rect.bottom()) / 2.0);
        const int hit = slotAt(gap);
        if (hit < 0) return fail(QStringLiteral("页间留白处命中为 -1（拖选会丢页）"));
    }

    // 4) 缩放：尺寸按比例变化，页序不变；缩放参与坐标换算（widgetPx = pt * zoom）
    {
        setZoom(2.0);
        const QRectF r0 = m_slots[0].rect;
        if (qAbs(r0.height() - 1400.0) > 1.0 || qAbs(r0.width() - 2000.0) > 1.0)
            return fail(QStringLiteral("缩放 200%% 后首页尺寸异常 %1x%2").arg(r0.width()).arg(r0.height()));
        for (int i = 1; i < m_pageCount; ++i)
            if (m_slots[i].rect.top() <= m_slots[i - 1].rect.top())
                return fail(QStringLiteral("缩放后第 %1 页顺序乱了").arg(i + 1));

        // 在首页里取一个"距页原点 (100pt, 50pt)"的点，换算回来应当仍是 (100, 50)。
        const QPointF widgetPt = m_slots[0].rect.topLeft() + QPointF(100.0 * m_zoom, 50.0 * m_zoom);
        const QPointF back = (widgetPt - m_slots[0].rect.topLeft()) / m_zoom;
        if (qAbs(back.x() - 100.0) > 0.01 || qAbs(back.y() - 50.0) > 0.01)
            return fail(QStringLiteral("缩放后坐标换算不闭合 (%1,%2)").arg(back.x()).arg(back.y()));
        if (slotAt(widgetPt) != 0) return fail(QStringLiteral("缩放后首页内点位未命中首页"));
        setZoom(1.0);
    }

    m_pdfPath = savedPdf;
    m_pageCount = savedCount;
    m_zoom = savedZoom;
    return QString();      // 通过
}

// ---------------------------------------------------------------------------
// 缩放（带视口锚点）
// ---------------------------------------------------------------------------
void ContinuousPdfView::setZoomAnchored(double newZoom, const QPointF& anchorWidget)
{
    const double nz = std::clamp(newZoom, kMinZoom, kMaxZoom);
    if (qFuzzyCompare(nz, m_zoom)) return;

    // 锚点在内容里的**相对位置**（0..1），缩放后按比例还原 ⇒ 视口不跳。
    const double oldW = qMax(1, width());
    const double oldH = qMax(1, height());
    const double rx = anchorWidget.x() / oldW;
    const double ry = anchorWidget.y() / oldH;

    m_zoom = nz;

    const double newBundle = bundleScaleForZoom(m_zoom);
    if (!qFuzzyCompare(newBundle, m_bundleScale)) {
        m_bundleScale = newBundle;
        invalidateImages();          // 换档 ⇒ 需要按新比例重渲
    }

    relayout();
    // relayout 已重设控件尺寸；把滚动条挪到"锚点仍在视口同一相对位置"。
    if (m_scroll) {
        const int vx = static_cast<int>(rx * width()  - m_scroll->viewport()->width()  / 2.0);
        const int vy = static_cast<int>(ry * height() - m_scroll->viewport()->height() / 2.0);
        m_scroll->horizontalScrollBar()->setValue(vx);
        m_scroll->verticalScrollBar()->setValue(vy);
    }
    emit zoomChanged(m_zoom);
    updateVisiblePages();
    update();
}

void ContinuousPdfView::setZoom(double z) { setZoomAnchored(z, contentVisibleRect().center()); }

void ContinuousPdfView::fitToWidth(int viewportWidthPx)
{
    if (m_slots.isEmpty() || viewportWidthPx <= 0) return;
    const double ptW = m_defaultPtSize.width() > 0 ? m_defaultPtSize.width() : 595.0;
    setZoomAnchored((viewportWidthPx - 2.0 * kMarginX - 4) / ptW, contentVisibleRect().center());
}

void ContinuousPdfView::fitToPageHeight(int viewportHeightPx)
{
    if (m_slots.isEmpty() || viewportHeightPx <= 0) return;
    const double ptH = m_defaultPtSize.height() > 0 ? m_defaultPtSize.height() : 842.0;
    setZoomAnchored((viewportHeightPx - 2.0 * kMarginY - 8) / ptH, contentVisibleRect().center());
}

void ContinuousPdfView::scrollToPage(int page)
{
    if (page < 0 || page >= m_slots.size()) return;
    m_focusPage = page;      // 用户明确要看这一页（可视区退化时以它为准）

    // ⚠️ 顺序很重要：先把目标页插进渲染队首，再动滚动条。
    // 原因：窗口刚打开 / 布局尚未跑完时，滚动条的 range 还是 0，setValue 会被钳到 0；
    // 只依赖"滚动条变化 → updateVisiblePages"的话，用户点的页永远不会开始渲染
    // （表现为"点了缩略图却没反应"）。这里用显式意图兜住这种情况。
    trace(QStringLiteral("scrollTo p%1 vis=%2,%3,%4x%5").arg(page)
              .arg(qRound(contentVisibleRect().x())).arg(qRound(contentVisibleRect().y()))
              .arg(qRound(contentVisibleRect().width())).arg(qRound(contentVisibleRect().height())));
    ensurePageQueued(page);

    if (m_scroll) {
        const QRectF r = m_slots[page].rect;
        m_scroll->verticalScrollBar()->setValue(static_cast<int>(r.top() - kMarginY));
    }
    updateVisiblePages();
    emit currentPageChanged(page);
}

// ---------------------------------------------------------------------------
// 框选 / 叠加层
// ---------------------------------------------------------------------------
void ContinuousPdfView::addRegion(const Region& r) { m_regions.push_back(r); update(); }

void ContinuousPdfView::clearRegions()
{
    m_regions.clear();
    m_overlay.clear();
    m_previewWords.clear();
    update();
}

void ContinuousPdfView::setWordBoxes(const QVector<WordBox>& words) { m_overlay = words; update(); }

void ContinuousPdfView::ensureWordsAsync(int page)
{
    if (page < 0 || page >= m_slots.size()) return;
    if (m_wordsByPage.contains(page) || m_wordsPending.contains(page)) return;
    m_wordsPending.insert(page);
    const QString pdf = m_pdfPath;
    auto* watcher = new QFutureWatcher<QVector<WordBox>>(this);
    connect(watcher, &QFutureWatcher<QVector<WordBox>>::finished, this, [this, watcher, page]{
        const QVector<WordBox> w = watcher->result();
        watcher->deleteLater();
        m_wordsPending.remove(page);
        m_wordsByPage.insert(page, w);
    });
    watcher->setFuture(QtConcurrent::run(m_pool, [pdf, page]{
        return loadWordsViaKernel(pdf, page);
    }));
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------
void ContinuousPdfView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    const int page = slotAt(e->position());
    if (page < 0) { QWidget::mousePressEvent(e); return; }

    m_dragging = true;
    m_dragPage = page;
    m_pressPt  = e->position();
    m_curPt    = m_pressPt;
    ensureWordsAsync(page);        // 实时识别：异步拉取该页词盒
    update();
}

void ContinuousPdfView::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging) { QWidget::mouseMoveEvent(e); return; }
    if (m_dragPage < 0 || m_dragPage >= m_slots.size()) return;
    // 限制在起始页内：跨页框选没有意义，越界即贴边。
    const QRectF pr = m_slots[m_dragPage].rect;
    QPointF p = e->position();
    p.setX(std::clamp(p.x(), pr.left(), pr.right()));
    p.setY(std::clamp(p.y(), pr.top(), pr.bottom()));
    m_curPt = p;

    // 实时识别：把框内词盒文字报给界面（词盒可能仍在异步加载中）。
    m_previewWords.clear();
    QStringList txt;
    const QVector<WordBox>& words = m_wordsByPage.value(m_dragPage);
    const QRectF band = QRectF(m_pressPt, m_curPt).normalized();
    for (const WordBox& w : words) {
        const QRectF wr = QRectF(pr.topLeft() + QPointF(w.rectPt.x() * m_zoom, w.rectPt.y() * m_zoom),
                                 QSizeF(w.rectPt.width() * m_zoom, w.rectPt.height() * m_zoom));
        if (band.contains(wr.center()) && !w.text.isEmpty()) {
            WordBox copy = w; copy.page = m_dragPage;
            m_previewWords.append(copy);
            txt.append(w.text);
        }
    }
    emit textPreview(m_dragPage, txt.join(QStringLiteral(" ")));
    update();
}

void ContinuousPdfView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !m_dragging) { QWidget::mouseReleaseEvent(e); return; }
    m_dragging = false;
    const int page = m_dragPage;
    m_dragPage = -1;
    const QRectF band = QRectF(m_pressPt, m_curPt).normalized();
    m_previewWords.clear();
    emit textPreview(page, QString());
    update();
    if (page >= 0 && band.width() >= 3 && band.height() >= 3 && page < m_slots.size()) {
        // 控件坐标 → 页内 pt：唯一映射 widgetPx = pt * zoom。
        const QPointF tl = band.topLeft() - m_slots[page].rect.topLeft();
        emit regionSelected(page, m_kind,
                            QRectF(tl.x() / m_zoom, tl.y() / m_zoom,
                                   band.width() / m_zoom, band.height() / m_zoom));
    }
}

void ContinuousPdfView::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
        // Ctrl+滚轮：以光标为锚缩放（阅读器标准手感）。
        const double factor = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        setZoomAnchored(m_zoom * factor, e->position());
        e->accept();
        return;
    }
    QWidget::wheelEvent(e);   // 普通滚轮交给 QScrollArea
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void ContinuousPdfView::paintEvent(QPaintEvent* ev)
{
    QPainter g(this);

    const QString canvasBg = Theme::color(Theme::CanvasBg);
    const QString textCol  = Theme::color(Theme::Text);
    const QString mutedCol = Theme::color(Theme::TextMuted);

    // 只画需要重绘的区域，但底色要铺满（否则滚动时出现残影）。
    g.fillRect(ev->rect(), QColor(canvasBg));

    for (int i = 0; i < m_slots.size(); ++i)
    {
        const Slot& s = m_slots[i];
        const QRectF pageRect = s.rect;
        // 只处理与本次重绘区相交的页（懒绘制的另一半价值：绘制量也只与可视区成正比）。
        QRectF dirtyUnion = QRectF(pageRect.topLeft(), QSizeF(pageRect.width(), pageRect.height() + kLabelH));
        if (!dirtyUnion.intersects(ev->rect())) continue;

        // 纸张（图纸本色是白的，深浅主题下都保持白，避免影响判读）。
        g.fillRect(pageRect, QColor(0xFF, 0xFF, 0xFF));

        if (!s.img.isNull()) {
            g.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom != 1.0);
            g.drawImage(pageRect, s.img, QRectF(0, 0, s.img.width(), s.img.height()));
        } else {
            // 占位：明确告诉用户这一页的状态，而不是留一块白。
            g.setPen(QColor(mutedCol));
            QFont f = g.font();
            g.setFont(f);
            const QString msg = s.failed ? QStringLiteral("第 %1 页渲染失败")
                                         : QStringLiteral("第 %1 页渲染中…");
            g.drawText(pageRect, Qt::AlignCenter, msg.arg(i + 1));
        }

        // 页边框
        g.setPen(QPen(QColor(0x9A, 0xA2, 0xAC), 1));
        g.setBrush(Qt::NoBrush);
        g.drawRect(pageRect);

        // 页码标签（页下方居中）
        g.setPen(QColor(textCol));
        QRectF label(pageRect.left(), pageRect.bottom() + 1, pageRect.width(), kLabelH);
        g.drawText(label, Qt::AlignHCenter | Qt::AlignTop,
                   QStringLiteral("第 %1 / %2 页").arg(i + 1).arg(m_pageCount));

        // ---- 叠加层：框选 ----
        for (const Region& r : m_regions) {
            if (r.page != i) continue;
            const QColor cc = (r.kind == QLatin1String("global_table"))       ? QColor(30, 120, 220)
                            : (r.kind == QLatin1String("material_no_region")) ? QColor(220, 120, 20)
                                                                             : QColor(30, 160, 90);
            const QRectF rr(pageRect.topLeft() + QPointF(r.rectPt.x() * m_zoom, r.rectPt.y() * m_zoom),
                            QSizeF(r.rectPt.width() * m_zoom, r.rectPt.height() * m_zoom));
            g.setPen(QPen(cc, 2));
            g.setBrush(QColor(cc.red(), cc.green(), cc.blue(), 40));
            g.drawRect(rr);
        }

        // ---- 叠加层：内核返回的词盒（绿=矢量，橙=OCR 补位）----
        g.setBrush(Qt::NoBrush);
        for (const WordBox& w : m_overlay) {
            if (w.page != i) continue;
            const QRectF rr(pageRect.topLeft() + QPointF(w.rectPt.x() * m_zoom, w.rectPt.y() * m_zoom),
                            QSizeF(w.rectPt.width() * m_zoom, w.rectPt.height() * m_zoom));
            g.setPen(QPen(w.source == QLatin1String("ocr") ? QColor(230, 120, 0)
                                                           : QColor(20, 150, 80), 1));
            g.drawRect(rr);
        }

        // ---- 实时识别高亮 ----
        if (m_dragging && m_dragPage == i && !m_previewWords.isEmpty()) {
            g.setPen(QPen(QColor(255, 170, 0), 2));
            g.setBrush(QColor(255, 200, 0, 60));
            for (const WordBox& w : m_previewWords) {
                const QRectF rr(pageRect.topLeft() + QPointF(w.rectPt.x() * m_zoom, w.rectPt.y() * m_zoom),
                                QSizeF(w.rectPt.width() * m_zoom, w.rectPt.height() * m_zoom));
                g.drawRect(rr);
            }
            g.setBrush(Qt::NoBrush);
        }

        // ---- 差异定位高亮（跳到某条差异时用，瞬态、不参与导出）----
        if (m_focusDiffPage == i && !m_focusDiffRectPt.isNull()) {
            const QRectF rr(pageRect.topLeft() + QPointF(m_focusDiffRectPt.x() * m_zoom,
                                                         m_focusDiffRectPt.y() * m_zoom),
                            QSizeF(m_focusDiffRectPt.width()  * m_zoom,
                                   m_focusDiffRectPt.height() * m_zoom));
            // 内外两层：浅色光晕 + 醒目的橙色虚线，缩放到任何倍率都能一眼看到。
            g.setBrush(Qt::NoBrush);
            g.setPen(QPen(QColor(255, 140, 0, 90), 6));
            g.drawRect(rr.adjusted(-3, -3, 3, 3));
            g.setPen(QPen(QColor(255, 90, 0), 2, Qt::DashLine));
            g.drawRect(rr);
        }

        // ---- 进行中的橡皮筋 ----
        if (m_dragging && m_dragPage == i) {
            const QRectF band = QRectF(m_pressPt, m_curPt).normalized();
            g.setPen(QPen(QColor(255, 0, 0), 2, Qt::DashLine));
            g.setBrush(QColor(255, 0, 0, 30));
            g.drawRect(band);
            g.setBrush(Qt::NoBrush);
        }
    }
}

// ---------------------------------------------------------------------------
// 差异定位：把某条差异滚到视口中央并高亮（"跳到下一个差异"的实现基础）
// ---------------------------------------------------------------------------
void ContinuousPdfView::focusRegion(int page, const QRectF& rectPt)
{
    if (page < 0 || page >= m_slots.size()) return;
    m_focusDiffPage = page;
    m_focusDiffRectPt = rectPt;

    // 先把目标页放进渲染队首（与 scrollToPage 同理：滚动条 range 可能还没算出来）。
    m_focusPage = page;
    ensurePageQueued(page);

    if (m_scroll) {
        const QRectF slot = m_slots[page].rect;
        // 有区域时把"区域中心"对准视口中心；否则把页顶对齐视口顶。
        int target;
        if (!rectPt.isNull()) {
            const double cy = slot.top() + (rectPt.y() + rectPt.height() / 2.0) * m_zoom;
            target = qRound(cy - m_scroll->viewport()->height() / 2.0);
        } else {
            target = qRound(slot.top() - kMarginY);
        }
        m_scroll->verticalScrollBar()->setValue(target);
    }
    updateVisiblePages();
    update();
    emit currentPageChanged(page);
}

void ContinuousPdfView::clearFocusRegion()
{
    if (m_focusDiffPage < 0) return;
    m_focusDiffPage = -1;
    m_focusDiffRectPt = QRectF();
    update();
}
