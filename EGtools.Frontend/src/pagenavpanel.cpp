#include "pagenavpanel.h"

#include "kernelclient.h"
#include "pdfrender.h"

#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>
#include <QImage>
#include <QPainter>

namespace {

constexpr int kThumbW = 150;          // 缩略图显示宽度（px）
constexpr int kRolePage = Qt::UserRole;

// ---------------------------------------------------------------------------
// 后台缩略图渲染器
//
// 顺序逐页渲染（内嵌后端本来就全局串行；顺序做也让进度可预期）。每渲染完一页就
// 发信号给界面，界面立刻更新那一格 —— 用户无需等待全部渲染完。
// 渲染比例取 0.12：A0/A1 图纸（约 2400pt 宽）⇒ 约 290px，足够看清版式，
// 再由界面缩放成缩略图尺寸，避免把大图整幅送进内存。
// ---------------------------------------------------------------------------
class ThumbRenderWorker : public QObject
{
    Q_OBJECT
public:
    ThumbRenderWorker(QString pdfPath, int pageCount, QString backend)
        : m_pdf(pdfPath), m_pages(pageCount), m_backend(backend) {}

    // ⚠️ 必须 Q_INVOKABLE：stopWorker() 用 QMetaObject::invokeMethod(worker, "cancel") 调用它，
    // 而"元对象系统能找到的方法"只有 signals / slots / Q_INVOKABLE —— 普通成员函数不在里面。
    // 之前漏了这个宏 ⇒ invokeMethod 静默失败（只打一行 "No such method QObject::cancel()"），
    // 结果**取消标志永远没被置位**：面板销毁后缩略图线程还会把整本图纸渲染完（白烧 CPU，
    // 且继续往已销毁的界面发信号）。这类"静默失败"必须靠看日志发现，故保留注释警示。
    Q_INVOKABLE void cancel() { m_cancel.storeRelaxed(1); }

public slots:
    void run()
    {
        // 缩略图改由 **PDFium** 在进程内渲染：不再"每页起一次内核进程"（旧做法在
        // 上百页的图纸集上要起上百个进程，慢且容易被环境问题打断）。
        // 仍然顺序逐页、渲染完一页就发一页（进度可预期，用户不用等全本）。
        auto* svc = PdfRenderService::instance();

        // ⚠️ 本函数跑在后台线程，而 pagePointSize() 读的是服务在 GUI 线程维护的数据。
        //    所以在**开始渲染之前**一次性取一份本地副本，之后只用副本 —— 避免跨线程读。
        QVector<QSizeF> sizes(m_pages);
        for (int p = 0; p < m_pages; ++p) sizes[p] = svc->pagePointSize(p);

        // 目标：长边约 320px 的缩略图（够看清版式，又不必把大图送进内存）。
        const double fullW = (sizes.value(0).width() > 1.0) ? sizes.value(0).width() : 842.0;
        const double scale = qBound(0.03, 320.0 / fullW, 0.5);

        auto devRectFor = [&sizes, scale](int p) {
            const QSizeF pt = sizes.value(p);
            if (pt.width() < 1.0) return QRect();
            return QRect(0, 0, qMax(1, qRound(pt.width() * scale)),
                               qMax(1, qRound(pt.height() * scale)));
        };

        for (int p = 0; p < m_pages; ++p) {
            if (m_cancel.loadRelaxed()) break;
            const QRect dev = devRectFor(p);
            if (dev.isEmpty()) continue;
            svc->requestRegion(p, scale, dev);      // 命中缓存时请求是空操作
            // 等这一页渲染好（或超时）：保持"逐页推进、可取消"的既有语义。
            QElapsedTimer t; t.start();
            while (!svc->hasRegionCovering(p, scale, dev)
                   && !m_cancel.loadRelaxed() && t.elapsed() < 20000) {
                QThread::msleep(15);
            }
            if (m_cancel.loadRelaxed()) break;
            const QImage img = svc->cachedImageForTest(p, scale, dev);
            if (!img.isNull()) emit thumbReady(p, img);
        }
        emit finished();
    }

signals:
    void thumbReady(int page, const QImage& img);
    void finished();

private:
    QString     m_pdf;
    int         m_pages = 0;
    QString     m_backend;
    QAtomicInt  m_cancel{0};
};

} // namespace

PageNavPanel::PageNavPanel(QWidget* parent) : QWidget(parent)
{
    auto* title = new QLabel(QStringLiteral("页面"));
    title->setStyleSheet(QStringLiteral("font-weight:600;"));

    m_list = new QListWidget();
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(kThumbW, kThumbW * 3 / 4));
    m_list->setGridSize(QSize(kThumbW + 18, kThumbW * 3 / 4 + 34));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setUniformItemSizes(true);
    m_list->setSpacing(3);
    m_list->setWordWrap(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);

    m_spin = new QSpinBox();
    m_spin->setMinimum(1);
    m_spin->setMaximum(1);
    m_spin->setFixedWidth(74);
    auto* goBtn    = new QPushButton(QStringLiteral("跳转"));
    auto* firstBtn = new QPushButton(QStringLiteral("⏮"));
    auto* lastBtn  = new QPushButton(QStringLiteral("⏭"));
    firstBtn->setFixedWidth(30);
    lastBtn->setFixedWidth(30);
    firstBtn->setToolTip(QStringLiteral("第一页"));
    lastBtn->setToolTip(QStringLiteral("最后一页"));

    auto* navRow = new QHBoxLayout();
    navRow->addWidget(firstBtn);
    navRow->addWidget(m_spin);
    navRow->addWidget(goBtn);
    navRow->addWidget(lastBtn);
    navRow->addStretch();

    m_hint = new QLabel(QStringLiteral("未打开 PDF"));
    m_hint->setWordWrap(true);
    m_hint->setObjectName(QStringLiteral("hintText"));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);
    lay->addWidget(title);
    lay->addWidget(m_list, 1);
    lay->addLayout(navRow);
    lay->addWidget(m_hint);

    connect(m_list, &QListWidget::itemClicked, this, &PageNavPanel::onItemClicked);
    connect(goBtn, &QPushButton::clicked, this, [this]{
        emit pageRequested(m_spin->value() - 1);
    });
    connect(m_spin, &QSpinBox::editingFinished, this, [this]{
        emit pageRequested(m_spin->value() - 1);
    });
    connect(firstBtn, &QPushButton::clicked, this, [this]{
        if (m_pageCount > 0) emit pageRequested(0);
    });
    connect(lastBtn, &QPushButton::clicked, this, [this]{
        if (m_pageCount > 0) emit pageRequested(m_pageCount - 1);
    });
}

PageNavPanel::~PageNavPanel() { stopWorker(); }

void PageNavPanel::setCliDir(const QString& dir)
{
    m_cliDir = dir;
    KernelClient::setCliDir(dir);
}

void PageNavPanel::stopWorker()
{
    // 先置取消标志（Q_INVOKABLE 才能被 invokeMethod 找到），让 worker 在"当前这页渲染完"
    // 之后立刻退出循环 —— 内核调用本身不可中断，所以最多再等多一页。
    if (m_worker) QMetaObject::invokeMethod(m_worker, "cancel", Qt::DirectConnection);

    if (m_thread) {
        m_thread->quit();
        if (!m_thread->wait(8000)) {
            // 内核调用极端卡住时不让界面跟着卡死：只记一条日志，线程交给 Qt 收尾。
            qWarning("thumbnail worker did not stop within 8s; detaching");
        }
        m_thread = nullptr;
    }
    m_worker = nullptr;   // 由 QThread::finished 的 deleteLater 释放
}

void PageNavPanel::clearDocument()
{
    stopWorker();
    m_list->clear();
    m_pdfPath.clear();
    m_pageCount = 0;
    m_current = 0;
    m_spin->setMaximum(1);
    m_hint->setText(QStringLiteral("未打开 PDF"));
}

void PageNavPanel::setDocument(const QString& pdfPath, int pageCount)
{
    clearDocument();
    if (pdfPath.isEmpty() || pageCount <= 0) return;

    m_pdfPath   = pdfPath;
    m_pageCount = pageCount;
    m_spin->setMaximum(pageCount);

    // 先占位，保证页码/跳转立即可用；缩略图随后逐页补上。
    for (int p = 0; p < pageCount; ++p) {
        auto* it = new QListWidgetItem(QString::number(p + 1));
        it->setData(kRolePage, p);
        it->setTextAlignment(Qt::AlignHCenter);
        it->setSizeHint(QSize(kThumbW + 18, kThumbW * 3 / 4 + 34));
        m_list->addItem(it);
    }
    m_hint->setText(QStringLiteral("正在后台生成 %1 页缩略图…").arg(pageCount));

    // 后台线程：顺序渲染，边渲染边回填。
    auto* w = new ThumbRenderWorker(pdfPath, pageCount, m_backend);
    m_thread = new QThread(this);
    m_worker = w;
    w->moveToThread(m_thread);
    connect(m_thread, &QThread::started,  w, &ThumbRenderWorker::run);
    connect(w, &ThumbRenderWorker::thumbReady, this, &PageNavPanel::onThumbReady);
    connect(w, &ThumbRenderWorker::finished,  this, &PageNavPanel::onRenderFinished);
    connect(w, &ThumbRenderWorker::finished,  m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, w, &QObject::deleteLater);
    m_thread->start();
}

void PageNavPanel::onThumbReady(int page, const QImage& img)
{
    if (page < 0 || page >= m_list->count()) return;
    QImage thumb = img.scaled(kThumbW, kThumbW * 3 / 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // 给缩略图加一圈浅边框，和专业阅读器一致，白底图纸在浅色主题下也能看出边界。
    QImage framed(thumb.width() + 2, thumb.height() + 2, QImage::Format_ARGB32_Premultiplied);
    framed.fill(QColor(0x88, 0x8f, 0x99));
    QPainter g(&framed);
    g.drawImage(1, 1, thumb);
    g.end();
    m_list->item(page)->setIcon(QIcon(QPixmap::fromImage(framed)));
}

void PageNavPanel::onRenderFinished()
{
    if (m_pageCount > 0)
        m_hint->setText(QStringLiteral("共 %1 页").arg(m_pageCount));
}

void PageNavPanel::onItemClicked(QListWidgetItem* item)
{
    if (!item) return;
    emit pageRequested(item->data(kRolePage).toInt());
}

void PageNavPanel::setCurrentPage(int page)
{
    if (page < 0 || page >= m_list->count()) return;
    m_current = page;
    m_updating = true;
    m_list->setCurrentRow(page);
    m_list->scrollToItem(m_list->item(page), QAbstractItemView::EnsureVisible);
    m_spin->setValue(page + 1);
    m_updating = false;
}

int PageNavPanel::currentPage() const { return m_current; }

#include "pagenavpanel.moc"
