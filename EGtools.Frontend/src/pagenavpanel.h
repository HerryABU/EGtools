#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QSpinBox;
class QThread;

// ---------------------------------------------------------------------------
// 页面导航面板（Adobe 风格的“页面缩略图”栏）
//
// 职责：把文档的**全部页面**以小图列出，点击跳页，当前页高亮；并提供页码跳转与
// 首/末页按钮。缩略图在**后台线程**逐页渲染（内核调用不阻塞界面；内嵌后端在
// KernelClient 内部全局串行，见那里的说明），渲染好一页就刷新一格 —— 所以打开
// 上百页的图纸时，界面立刻可用，缩略图陆续补上。
// ---------------------------------------------------------------------------
class PageNavPanel : public QWidget
{
    Q_OBJECT
public:
    explicit PageNavPanel(QWidget* parent = nullptr);
    ~PageNavPanel() override;

    void setCliDir(const QString& dir);
    void setKernelBackend(const QString& mode) { m_backend = mode; }

    // 打开新文档：清空并开始后台生成缩略图。
    void setDocument(const QString& pdfPath, int pageCount);
    void clearDocument();
    void setCurrentPage(int page);      // 只负责高亮，不发信号（避免回环）
    int  currentPage() const;

signals:
    void pageRequested(int page);       // 用户点击某页/跳转
    void statusMessage(const QString& msg);

private slots:
    void onItemClicked(QListWidgetItem* item);
    void onThumbReady(int page, const QImage& img);
    void onRenderFinished();

private:
    void stopWorker();

    QString m_pdfPath;
    int     m_pageCount = 0;
    int     m_current   = 0;
    QString m_cliDir;
    QString m_backend = QStringLiteral("auto");
    bool    m_updating = false;

    QListWidget* m_list  = nullptr;
    QSpinBox*    m_spin  = nullptr;
    QLabel*      m_hint  = nullptr;
    QThread*     m_thread = nullptr;
    // ⚠️ 必须是 QPointer，不能是裸 QObject*：
    //    worker 跑完 → 线程结束 → `QThread::finished → deleteLater` **把它释放掉**
    //    （对象在"事件循环已停"的线程里，Qt 会在线程结束时销毁它）。
    //    裸指针在那一刻起就是悬垂的，而析构里 stopWorker() 只判 `if (m_worker)` ⇒
    //    会对已释放内存做 `QMetaObject::invokeMethod` ⇒ **跳到 0x0 段错误**。
    //    实测：`--screenshot`（会渲染缩略图 ⇒ worker 会正常跑完）退出即 SIGSEGV；
    //    而不开文档的 `--test-panels/--test-theme` 不崩 —— 差别正在于此。
    //    QPointer 在对象被析构时自动置空，从根上消除这一类 use-after-free。
    QPointer<QObject> m_worker;   // ThumbRenderWorker（定义在 .cpp）
};
