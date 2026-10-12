#include "pdfrender.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QThread>
#include <QtMath>

#include <algorithm>
#include <memory>

// 分块边长（设备像素）。取 2048：单块约 16MB，且"单次渲染耗时与块大小几乎无关"
// （见文件头注释），所以块不必太小 —— 太小只会把同一次内容解释拆成多次。
static constexpr int kTileDevPx = 2048;

// 整页请求的内存上限（像素）：超过就按比例降倍率，避免一次要几百 MB。
static constexpr qint64 kMaxWholePagePx = 16ll * 1024 * 1024;

// ---------------------------------------------------------------------------
// 工作线程：唯一持有 QPdfDocument 的地方（PDFium 文档对象不是线程安全的）。
// ---------------------------------------------------------------------------
class PdfRenderWorker : public QObject
{
    Q_OBJECT
public:
    ~PdfRenderWorker() override { m_doc.reset(); }

public slots:
    void openDoc(const QString& path, quint64 gen)
    {
        m_doc.reset();
        auto doc = std::make_unique<QPdfDocument>();
        const QPdfDocument::Error e = doc->load(path);
        if (e != QPdfDocument::Error::None) {
            emit failed(QStringLiteral("PDFium 加载失败（错误码 %1）：%2").arg(int(e)).arg(path), gen);
            return;
        }
        m_doc = std::move(doc);
        QVector<QSizeF> sizes;
        sizes.reserve(m_doc->pageCount());
        for (int i = 0; i < m_doc->pageCount(); ++i) sizes.append(m_doc->pagePointSize(i));
        emit docOpened(path, m_doc->pageCount(), sizes, gen);
    }

    void renderRegion(int page, double scale, QRect devRect, quint64 gen)
    {
        QElapsedTimer t; t.start();
        if (!m_doc || page < 0 || page >= m_doc->pageCount() || scale <= 0.0) {
            emit regionRendered(page, scale, devRect, QImage(), gen, 0);   // 回空图清掉在途标记
            return;
        }

        const QSizeF pt = m_doc->pagePointSize(page);
        const QSize fullScaled(qMax(1, qRound(pt.width() * scale)),
                               qMax(1, qRound(pt.height() * scale)));

        // 夹到页面范围内，并保证非空。
        devRect = devRect.intersected(QRect(0, 0, fullScaled.width(), fullScaled.height()));
        if (devRect.width() < 1 || devRect.height() < 1) {
            emit regionRendered(page, scale, devRect, QImage(), gen, 0);
            return;
        }

        QPdfDocumentRenderOptions opt;
        // ⚠️⚠️ 这两行必须成对出现。只设 scaledClipRect 时 PDFium 会返回**全透明空图**
        //     （实测：不透明像素 0 / maxAlpha 0）—— 界面表现就是"一片黑/一片深色"。
        opt.setScaledSize(fullScaled);
        opt.setScaledClipRect(devRect);

        QImage img = m_doc->render(page, devRect.size(), opt);
        if (img.isNull()) { emit regionRendered(page, scale, devRect, QImage(), gen, 0); return; }

        // 合成到白底并转成不透明格式：
        //   PDFium 渲染出的页面背景是**透明**的（不是白也不是黑）。若直接交给画布绘制，
        //   深色主题下就会看到"黑画布"；测量亮度也会恒为 0，从而掩盖真实问题。
        //   这里一次性合成白底（＝读者看到的纸面），此后的"是不是黑的"判定才有意义。
        QImage opaque(devRect.size(), QImage::Format_RGB32);
        opaque.fill(Qt::white);
        {
            QPainter p(&opaque);
            p.drawImage(0, 0, img);
        }
        emit regionRendered(page, scale, devRect, opaque, gen, t.elapsed());
    }

signals:
    void docOpened(const QString& path, int pageCount, const QVector<QSizeF>& sizes, quint64 gen);
    void regionRendered(int page, double scale, const QRect& devRect, const QImage& img,
                        quint64 gen, qint64 ms);
    void failed(const QString& message, quint64 gen);
    void stale(quint64 gen);      // 结果作废（换文档/参数无效），不报错

private:
    std::unique_ptr<QPdfDocument> m_doc;
};

// ---------------------------------------------------------------------------
// 服务（GUI 线程）
// ---------------------------------------------------------------------------
PdfRenderService* PdfRenderService::instance()
{
    static PdfRenderService* s = new PdfRenderService();
    return s;
}

PdfRenderService::PdfRenderService(QObject* parent) : QObject(parent)
{
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("pdfium-render"));
    m_worker = new PdfRenderWorker();          // 无父对象，moveToThread 后归线程管
    m_worker->moveToThread(m_thread);

    connect(m_worker, &PdfRenderWorker::docOpened,      this, &PdfRenderService::onDocOpened);
    connect(m_worker, &PdfRenderWorker::regionRendered, this, &PdfRenderService::onRegionRendered);
    connect(m_worker, &PdfRenderWorker::failed,         this, &PdfRenderService::onWorkerFailed);
    connect(m_worker, &PdfRenderWorker::stale,          this, [this](quint64){ /* 本就在途清账 */ });
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
}

PdfRenderService::~PdfRenderService()
{
    if (m_thread) { m_thread->quit(); m_thread->wait(5000); }
}

void PdfRenderService::openDocument(const QString& path)
{
    ++m_gen;                       // 作废所有在途结果
    m_path = path;
    m_ready = false;
    m_pageCount = 0;
    m_sizes.clear();
    m_cache.clear(); m_lru.clear(); m_cachedBytes = 0; m_pending.clear();
    m_deferred.clear();                    // 换文档 ⇒ 旧的待补发请求作废
    QMetaObject::invokeMethod(m_worker, "openDoc", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(quint64, m_gen));
}

QSizeF PdfRenderService::pagePointSize(int page) const
{
    if (page < 0 || page >= m_sizes.size()) return QSizeF();
    return m_sizes[page];
}

void PdfRenderService::onDocOpened(const QString& path, int pageCount,
                                   const QVector<QSizeF>& sizes, quint64 gen)
{
    if (gen != m_gen) return;      // 已经换过文档
    m_path = path;
    m_pageCount = pageCount;
    m_sizes = sizes;
    m_ready = true;
    emit documentReady(path, pageCount);   // 先让调用方按新文档重新排队……

    // ……再补发"就绪之前"收到的请求（否则它们会被永远忘掉）。
    const QVector<DeferredReq> backlog = m_deferred;
    m_deferred.clear();
    for (const DeferredReq& d : backlog) request(QStringLiteral("renderRegion"), d.page, d.scale, d.rect);
}

void PdfRenderService::onWorkerFailed(const QString& message, quint64 gen)
{
    if (gen != m_gen) return;
    m_ready = false;
    m_lastError = message;
    emit failed(message);
}

void PdfRenderService::onRegionRendered(int page, double scale, const QRect& devRect,
                                        const QImage& img, quint64 gen, qint64 ms)
{
    const QString k = keyOf(page, scale, devRect);
    m_pending.remove(k);
    if (gen != m_gen) return;      // 旧文档的结果，丢弃
    if (img.isNull()) return;

    ++m_renderCount;
    m_lastRenderMs = ms;

    PdfRegion r;
    r.page = page; r.scale = scale; r.devRect = devRect; r.img = img;
    const qint64 bytes = qint64(img.sizeInBytes());
    if (m_cache.contains(k)) m_cachedBytes -= qint64(m_cache.value(k).img.sizeInBytes());
    m_cache.insert(k, r);
    m_cachedBytes += bytes;
    m_lru.removeAll(k);
    m_lru.append(k);
    evictIfNeeded();

    emit regionReady(page);
}

QString PdfRenderService::keyOf(int page, double scale, const QRect& r)
{
    return QStringLiteral("%1|%2|%3,%4,%5,%6")
        .arg(page).arg(qRound(scale * 1000))
        .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

void PdfRenderService::evictIfNeeded()
{
    while (m_cachedBytes > m_cacheBudget && m_lru.size() > 1) {
        const QString victim = m_lru.takeFirst();
        auto it = m_cache.find(victim);
        if (it == m_cache.end()) continue;
        m_cachedBytes -= qint64(it->img.sizeInBytes());
        m_cache.erase(it);
    }
}

bool PdfRenderService::request(const QString& fn, int page, double scale, QRect devRect)
{
    if (page < 0 || scale <= 0.0) return false;
    if (devRect.width() < 1 || devRect.height() < 1) return false;

    // ⚠️ 文档还没就绪：**暂存并在就绪后补发**，绝不静默丢弃。
    //    静默丢弃的后果很隐蔽：调用方以为自己已排队（把队列标记为"在途"），
    //    于是再也不推进 —— 界面表现就是"这一页永远渲染不出来"。
    if (!m_ready) {
        m_deferred.append(DeferredReq{page, scale, devRect});
        if (m_deferred.size() > 256) m_deferred.removeFirst();
        return true;                       // 已受理（就绪后会补发）
    }
    if (page >= m_pageCount) return false;

    const QString k = keyOf(page, scale, devRect);
    if (m_cache.contains(k) || m_pending.contains(k)) return true;
    m_pending.insert(k);
    QMetaObject::invokeMethod(m_worker, fn.toLatin1().constData(), Qt::QueuedConnection,
                              Q_ARG(int, page), Q_ARG(double, scale),
                              Q_ARG(QRect, devRect), Q_ARG(quint64, m_gen));
    return true;
}

bool PdfRenderService::requestRegion(int page, double scale, const QRect& devRect)
{
    return request(QStringLiteral("renderRegion"), page, scale, devRect);
}

void PdfRenderService::requestWholePage(int page, double scale)
{
    const QSizeF pt = pagePointSize(page);
    if (pt.isEmpty() || scale <= 0.0) return;
    // 超高倍率的"整页"会一次要几百 MB —— 按内存上限把倍率夹下来。
    const double px = pt.width() * pt.height() * scale * scale;
    if (px > double(kMaxWholePagePx))
        scale *= qSqrt(double(kMaxWholePagePx) / px);
    const QRect r(0, 0, qMax(1, qRound(pt.width() * scale)), qMax(1, qRound(pt.height() * scale)));
    request(QStringLiteral("renderRegion"), page, scale, r);
}

bool PdfRenderService::hasRegionCovering(int page, double scale, const QRect& devRect) const
{
    // 精确倍率档（毫尺）命中 + 覆盖即可；差一点点倍率的图不能当"已覆盖"（那是糊的）。
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it) {
        const PdfRegion& r = it.value();
        if (r.page != page) continue;
        if (qAbs(r.scale - scale) > 1e-6) continue;
        if (r.devRect.contains(devRect)) return true;
    }
    return false;
}

QVector<PdfRegion> PdfRenderService::cachedRegions(int page) const
{
    QVector<PdfRegion> out;
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it)
        if (it.value().page == page) out.append(it.value());
    // 升序：低倍率（糊）先画，高倍率（清）后画 → 自然实现"先糊后清"的渐进细化。
    std::sort(out.begin(), out.end(),
              [](const PdfRegion& a, const PdfRegion& b){ return a.scale < b.scale; });
    return out;
}

QImage PdfRenderService::cachedImageForTest(int page, double scale, const QRect& devRect) const
{
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it) {
        const PdfRegion& r = it.value();
        if (r.page == page && qAbs(r.scale - scale) < 1e-6 && r.devRect.contains(devRect))
            return r.img;
    }
    return QImage();
}

void PdfRenderService::clearCacheForTest()
{
    m_cache.clear(); m_lru.clear(); m_pending.clear(); m_cachedBytes = 0;
    m_renderCount = 0;
}

int PdfRenderService::tileSizeDevPx() const { return kTileDevPx; }

#include "pdfrender.moc"
