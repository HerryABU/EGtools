#pragma once

#include <QAtomicInt>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QThread;

// ---------------------------------------------------------------------------
// 批量抽取工作对象（跑在独立线程里）
//
// 为什么逐文件调用而不是"一次调用把全部文件交给内核"：内核其实支持多输入
// （文件/目录/通配符），但那样只有一个总进度、一个总成败。逐文件调用换来：
//   ① 精确的文件级进度（进度条 = 已完成文件数 / 总数）；
//   ② 失败隔离 —— 某个坏 PDF 不会让整批停下；
//   ③ 每行都能显示自己的输出文件与行数（从内核 stderr 的 Saved ... 行解析）。
// 开销可忽略：hostfxr 运行时只在进程内初始化一次，每次调用只多几次宿主边界往返，
// 相对"解析一个 PDF 要 1~3 秒"根本看不出来。
// ---------------------------------------------------------------------------
class MaterialExtractWorker : public QObject
{
    Q_OBJECT
public:
    MaterialExtractWorker(const QStringList& files, const QString& outDir,
                          const QString& format, const QString& tag,
                          const QString& backend, bool verbose,
                          QObject* parent = nullptr);

    // 线程安全：置停止标志。当前文件跑完后再退出（内核调用本身不可中断）。
    void requestStop() { m_stop.storeRelaxed(1); }

signals:
    void fileStarted(int index);
    void fileFinished(int index, int rc, int rows, const QStringList& outputs, const QString& logText);
    void allFinished(int okCount, int failCount, int stopped);

public slots:
    void run();

private:
    QStringList m_files;
    QString     m_outDir;
    QString     m_format;
    QString     m_tag;
    QString     m_backend;
    bool        m_verbose = true;
    QAtomicInt  m_stop{0};
};

// ---------------------------------------------------------------------------
// 「材料表提取」页 —— PDF → 材料表（批量）
//
// 这是 EGpdf2excel 内核的完整前端：批量导入 PDF（文件多选 / 整目录 / 拖拽），
// 设置输出目录与格式，一键提取，并逐文件显示进度、行数、输出文件与失败原因。
// 所有解析都在内核完成，本页只做编排与展示（与项目"Qt 只做壳"的约定一致）。
// ---------------------------------------------------------------------------
class MaterialExtractPage : public QWidget
{
    Q_OBJECT
public:
    explicit MaterialExtractPage(QWidget* parent = nullptr);
    ~MaterialExtractPage() override;

    void setCliDir(const QString& dir);
    // 内核调用方式（auto / embedded / cli），由 MainWindow 下发。
    void setKernelBackend(const QString& mode) { m_kernelBackend = mode; }

    // 外部批量导入入口（例如首页直接带入文件）。
    void addFiles(const QStringList& paths);

signals:
    void statusMessage(const QString& msg);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private slots:
    void onAddFiles();
    void onAddDir();
    void onRemoveSelected();
    void onClearAll();
    void onPickOutDir();
    void onStart();
    void onStop();
    void onOpenOutputDir();
    void onFileStarted(int index);
    void onFileFinished(int index, int rc, int rows, const QStringList& outputs, const QString& logText);
    void onAllFinished(int okCount, int failCount, int stopped);

private:
    // 收集单个条目（文件或目录）下的 PDF；目录递归。
    static QStringList collectPdfs(const QStringList& paths);
    void    appendPaths(const QStringList& paths);
    void    refreshRowAppearance(int row);
    void    setRunning(bool running);
    void    appendLog(const QString& s);
    QString effectiveOutDir() const;
    QStringList currentFiles() const;

    QString m_cliDir;
    QString m_kernelBackend = QStringLiteral("auto");

    QTableWidget*   m_table     = nullptr;
    QPushButton*    m_addBtn    = nullptr;
    QPushButton*    m_addDirBtn = nullptr;
    QPushButton*    m_delBtn    = nullptr;
    QPushButton*    m_clearBtn  = nullptr;
    QLineEdit*      m_outEdit   = nullptr;
    QPushButton*    m_outBtn    = nullptr;
    QCheckBox*      m_sameDirCb = nullptr;
    QComboBox*      m_fmtCombo  = nullptr;
    QLineEdit*      m_tagEdit   = nullptr;
    QCheckBox*      m_verboseCb = nullptr;
    QPushButton*    m_startBtn  = nullptr;
    QPushButton*    m_stopBtn   = nullptr;
    QPushButton*    m_openBtn   = nullptr;
    QProgressBar*   m_progress  = nullptr;
    QLabel*         m_summary   = nullptr;
    QLabel*         m_backendLbl= nullptr;
    QPlainTextEdit* m_log       = nullptr;

    QThread*               m_thread = nullptr;
    MaterialExtractWorker* m_worker = nullptr;
    bool m_running = false;
};
