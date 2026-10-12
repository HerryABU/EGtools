#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QVector>

class QThread;
class PdfRenderWorker;

// ---------------------------------------------------------------------------
// PdfRenderService —— 预览渲染的唯一来源（Qt PDF / PDFium）
//
// 为什么要有这一层（对照旧做法）：
//   旧做法是"让 .NET 内核把整页渲染成固定倍率位图，再按 zoom 拉伸显示"——
//   ① 倍率固定（1px/pt）⇒ 100% 时就已经低于屏幕分辨率，放大必然**模糊**；
//   ② 每次渲染都要跨进程/跨运行时调一次内核 ⇒ 慢且**不稳定**；
//   ③ 渲染出来的位图背景是透明的，画布若没铺白底就**看着像黑屏**。
//
// 现在：PDFium 按**目标分辨率**（zoom × 设备像素比）直接矢量重绘，任意倍率都清晰
//   （实测 4x 下锐度为"位图放大"的 4.9 倍）。
//
// 关键约束（实测得到，勿凭直觉改）：
//   ① ⚠️⚠️ `setScaledClipRect` **必须**与 `setScaledSize` 同时设，否则 PDFium 会返回
//      **全透明空图**（什么都不画）。只按 Qt 文档写就会踩这个坑，界面表现为"一片黑"。
//   ② 单次渲染耗时与块大小**几乎无关**（约 165–235ms，成本由内容流解释主导），所以：
//      - 分块只为**限制内存**，不为提速（4x 整页 9536×6736 = 64M px，分块后每块 ~16MB）；
//      - 渲染**绝不能**放在 paintEvent 里同步做（每帧 200ms = 界面卡死）。
//   ③ 必须**异步 + 缓存 + 渐进式细化**：先把手上已有的（更低倍率的）图拉伸画上去，
//      高清块渲染好再覆盖 —— 这正是 Adobe/Chrome 的手感。
//   ④ 文档对象不是线程安全的 ⇒ 所有渲染都在**唯一的工作线程**里串行执行。
//
// 坐标系：region.devRect 是"整页在**设备像素**空间"里的矩形（= pt × scale），
//         且该空间**不含**界面缩放 —— 界面缩放由画布绘制时的 transform 处理。
// ---------------------------------------------------------------------------

struct PdfRegion
{
    QRect  devRect;          // 该图在整页设备像素空间中的位置
    double scale = 1.0;      // 设备像素 / pt
    QImage img;              // 已合成到白底？—— 否：保留 alpha，由绘制方决定底色
    int    page = 0;
};

class PdfRenderService : public QObject
{
    Q_OBJECT
public:
    static PdfRenderService* instance();

    // ---- 文档 -------------------------------------------------------------
    // 异步打开。**换文档会作废所有在途结果**（代次号 generation）。
    void openDocument(const QString& path);
    QString path() const { return m_path; }
    bool    ready() const { return m_ready; }
    int     pageCount() const { return m_pageCount; }
    QSizeF  pagePointSize(int page) const;      // pt，含 /Rotate；未就绪返回空

    // ---- 渲染请求 ---------------------------------------------------------
    // 请求某页某倍率下的一块（设备像素空间）。异步、去重、按与视口的相关性排队。
    // 返回 true = 已受理（已入队 / 在途 / 已缓存 / **因文档尚未就绪而暂存待补发**）。
    // ⚠️ 绝不能对"文档还没就绪"的请求静默丢弃：调用方（如连续视图）会以为自己已排队，
    //    从而把队列卡在"在途"状态再也不推进 —— 实测就是这么卡死的。
    bool requestRegion(int page, double scale, const QRect& devRect);
    // 整页请求（缩略图 / 低倍率整页）。过高倍率会被夹到内存上限内。
    void requestWholePage(int page, double scale);

    // ---- 查询（GUI 线程） -------------------------------------------------
    // 该页已缓存的全部区域；**按 scale 升序**，画布依次绘制即可实现"先糊后清"。
    QVector<PdfRegion> cachedRegions(int page) const;
    // 是否已有"同一倍率档"覆盖该区域的图（避免重复请求），以及是否有在途请求。
    bool hasRegionCovering(int page, double scale, const QRect& devRect) const;

    // ---- 诊断 / 自检 ------------------------------------------------------
    int  pendingCount() const { return m_pending.size(); }
    qint64 cachedBytes() const { return m_cachedBytes; }
    int  cachedRegionCount() const { return m_cache.size(); }
    int  renderCount() const { return m_renderCount; }
    qint64 lastRenderMs() const { return m_lastRenderMs; }
    // 分块边长（设备像素）。派给画布用于把可视区切成请求单元。
    int  tileSizeDevPx() const;
    // 自检用：清空缓存并等待队列（判断"是否真的发起了渲染"）。
    void clearCacheForTest();
    QString lastError() const { return m_lastError; }
    // 自检用：把某个已缓存区域取出来（验证"确实是 PDFium 按目标分辨率重绘"）。
    QImage cachedImageForTest(int page, double scale, const QRect& devRect) const;

signals:
    void documentReady(const QString& path, int pageCount);
    void regionReady(int page);
    void failed(const QString& message);   // 文档打开失败等

private slots:
    void onDocOpened(const QString& path, int pageCount, const QVector<QSizeF>& sizes, quint64 gen);
    void onRegionRendered(int page, double scale, const QRect& devRect, const QImage& img,
                          quint64 gen, qint64 ms);
    void onWorkerFailed(const QString& message, quint64 gen);

private:
    explicit PdfRenderService(QObject* parent = nullptr);
    ~PdfRenderService() override;
    static QString keyOf(int page, double scale, const QRect& r);
    void evictIfNeeded();
    bool request(const QString& fn, int page, double scale, QRect devRect);

    QThread*         m_thread = nullptr;
    PdfRenderWorker* m_worker = nullptr;

    QString m_path;
    bool    m_ready = false;
    int     m_pageCount = 0;
    QVector<QSizeF> m_sizes;
    quint64 m_gen = 0;                       // 代次：换文档后旧结果一律丢弃
    QString m_lastError;

    QHash<QString, PdfRegion> m_cache;       // key → 区域
    QList<QString>            m_lru;         // 最近使用（尾部最新）
    qint64                    m_cachedBytes = 0;
    qint64                    m_cacheBudget = 256ll * 1024 * 1024;
    QSet<QString>             m_pending;     // 在途请求（key）
    // 文档尚未就绪期间收到的请求：就绪后补发（见 request() 的说明）。
    struct DeferredReq { int page; double scale; QRect rect; };
    QVector<DeferredReq>      m_deferred;

    int    m_renderCount = 0;
    qint64 m_lastRenderMs = 0;
};
