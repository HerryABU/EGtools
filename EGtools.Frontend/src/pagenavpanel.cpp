#include "pagenavpanel.h"

#include "kernelclient.h"

#include <QDir>
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

    void cancel() { m_cancel.storeRelaxed(1); }

public slots:
    void run()
    {
        QTemporaryDir tmp;
        for (int p = 0; p < m_pages; ++p)
        {
            if (m_cancel.loadRelaxed()) break;
            const QString bmp = QDir(tmp.path()).filePath(QStringLiteral("t%1.bmp").arg(p));
            const KernelClient::Result r = KernelClient::run(
                QStringLiteral("EGpdf2excel"),
                {QStringLiteral("--render"), QStringLiteral("--page"), QString::number(p),
                 QStringLiteral("--scale"), QStringLiteral("0.12"),
                 QStringLiteral("--render-out"), bmp, m_pdf},
                QString(), KernelClient::backendFromString(m_backend));
            if (r.rc != 0) continue;      // 某页渲染失败就跳过，不中断整批
            QImage img = QImageReader(bmp).read();
            if (img.isNull()) continue;
            emit thumbReady(p, img.copy());
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
    m_hint->setStyleSheet(QStringLiteral("color:#5a6472;font-size:8.5pt;"));

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
    if (m_worker) QMetaObject::invokeMethod(m_worker, "cancel", Qt::DirectConnection);
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(8000);
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
