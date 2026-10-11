#include "materialextractpage.h"

#include "kernelclient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTableWidget>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// 表格列
enum Col { ColFile = 0, ColSize, ColStatus, ColResult, ColCount };

// 行的角色数据：完整路径
constexpr int RolePath = Qt::UserRole;

QString humanSize(qint64 bytes)
{
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024) return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

// 从内核 stderr 解析本次抽取的结果。
//
// 内核在普通提取模式下 **stdout 为空**，全部进度与产物信息都写在 stderr（-v 时更详细）：
//   Processing <pdf> ...
//     page 1/11
//     rows: 42
//   Saved CSV  <path> (42 rows)
//   Saved XLSX <path> (42 rows)
//   Done. 1 file(s), 42 total rows.
// 这里只认 "Saved <FMT> <path> (<n> rows)" 与 "rows: <n>"，其余原样作为日志返回。
void parseExtractLog(const QString& logText, int* rows, QStringList* outputs)
{
    static const QRegularExpression reSaved(
        QStringLiteral(R"(Saved\s+(CSV|XLSX)\s+(.+?)\s+\((\d+)\s+rows?\))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reRows(QStringLiteral(R"(^\s*rows:\s*(\d+)\s*$)"),
                                           QRegularExpression::MultilineOption);

    int bestRows = 0;
    QStringList found;
    auto it = reSaved.globalMatch(logText);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString path = m.captured(2).trimmed();
        const int n = m.captured(3).toInt();
        if (n > bestRows) bestRows = n;
        if (!path.isEmpty()) {
            // 输出名只给文件名，避免整行被超长路径撑爆。
            const QString base = QFileInfo(QDir::fromNativeSeparators(path)).fileName();
            if (!base.isEmpty() && !found.contains(base)) found << base;
        }
    }
    if (bestRows == 0) {
        const QRegularExpressionMatch rm = reRows.match(logText);
        if (rm.hasMatch()) bestRows = rm.captured(1).toInt();
    }
    if (rows) *rows = bestRows;
    if (outputs) *outputs = found;
}

} // namespace

// ===========================================================================
// MaterialExtractWorker
// ===========================================================================
MaterialExtractWorker::MaterialExtractWorker(const QStringList& files, const QString& outDir,
                                             const QString& format, const QString& tag,
                                             const QString& backend, bool verbose, QObject* parent)
    : QObject(parent), m_files(files), m_outDir(outDir), m_format(format),
      m_tag(tag), m_backend(backend), m_verbose(verbose)
{
}

void MaterialExtractWorker::run()
{
    int ok = 0, fail = 0;
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_stop.loadRelaxed()) {
            emit allFinished(ok, fail, 1);
            return;
        }

        emit fileStarted(i);

        QStringList args{ m_files.at(i) };
        if (!m_outDir.isEmpty()) args << QStringLiteral("-o") << m_outDir;
        if (!m_format.isEmpty()) args << QStringLiteral("-f") << m_format;
        if (!m_tag.isEmpty())    args << QStringLiteral("--tag") << m_tag;
        if (m_verbose)           args << QStringLiteral("-v");

        // 融合层：按用户选择的调用方式（内嵌调库 / 命令行），两者吃同一份内核。
        const KernelClient::Result r = KernelClient::run(
            QStringLiteral("EGpdf2excel"), args, QString(),
            KernelClient::backendFromString(m_backend));

        int rows = 0;
        QStringList outputs;
        // 内嵌后端把内核日志放在 err；命令行后端 stderr 同样落在 err。两条路一致。
        const QString logText = r.err.trimmed().isEmpty() ? r.out : r.err;
        parseExtractLog(logText, &rows, &outputs);

        if (r.rc == 0) ++ok; else ++fail;
        emit fileFinished(i, r.rc, rows, outputs, logText);
    }
    emit allFinished(ok, fail, 0);
}

// ===========================================================================
// MaterialExtractPage
// ===========================================================================
MaterialExtractPage::MaterialExtractPage(QWidget* parent) : QWidget(parent)
{
    setAcceptDrops(true);

    // ---------------- 顶部说明 ----------------
    auto* intro = new QLabel(QStringLiteral(
        "把一批等轴测图纸 PDF 一次性抽成材料表（CSV / XLSX）。"
        "可直接把 PDF 或整个文件夹拖进来；逐文件独立处理，某个文件出错不影响其余。"));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral(
        "QLabel{background:#eef3fa;border:1px solid #c6d8ef;border-radius:6px;"
        "padding:8px 10px;color:#24405e;}"));

    // ---------------- PDF 输入区 ----------------
    m_addBtn    = new QPushButton(QStringLiteral("添加 PDF…"));
    m_addDirBtn = new QPushButton(QStringLiteral("添加文件夹…"));
    m_delBtn    = new QPushButton(QStringLiteral("移除选中"));
    m_clearBtn  = new QPushButton(QStringLiteral("清空列表"));

    auto* inBtnRow = new QHBoxLayout();
    inBtnRow->addWidget(m_addBtn);
    inBtnRow->addWidget(m_addDirBtn);
    inBtnRow->addWidget(m_delBtn);
    inBtnRow->addWidget(m_clearBtn);
    inBtnRow->addStretch();

    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({ QStringLiteral("PDF 文件"), QStringLiteral("大小"),
                                         QStringLiteral("状态"), QStringLiteral("结果") });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(ColFile, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColSize, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColStatus, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColResult, QHeaderView::Stretch);
    m_table->setMinimumHeight(160);

    auto* inputBox = new QGroupBox(QStringLiteral("输入 PDF（批量：多选文件 / 整个文件夹 / 拖拽到此处）"));
    auto* inputLay = new QVBoxLayout(inputBox);
    inputLay->addLayout(inBtnRow);
    inputLay->addWidget(m_table, 1);

    // ---------------- 输出与选项 ----------------
    m_outEdit = new QLineEdit();
    m_outEdit->setPlaceholderText(QStringLiteral("留空 = 与各 PDF 同目录"));
    m_outBtn  = new QPushButton(QStringLiteral("…"));
    auto* outRow = new QHBoxLayout();
    outRow->addWidget(new QLabel(QStringLiteral("输出目录:")));
    outRow->addWidget(m_outEdit, 1);
    outRow->addWidget(m_outBtn);

    m_sameDirCb = new QCheckBox(QStringLiteral("与源 PDF 同目录（忽略上面的输出目录）"));
    m_sameDirCb->setChecked(true);

    m_fmtCombo = new QComboBox();
    m_fmtCombo->addItem(QStringLiteral("CSV + XLSX（两者）"), QStringLiteral("both"));
    m_fmtCombo->addItem(QStringLiteral("仅 CSV"),             QStringLiteral("csv"));
    m_fmtCombo->addItem(QStringLiteral("仅 XLSX"),            QStringLiteral("xlsx"));

    m_tagEdit = new QLineEdit(QStringLiteral("V3"));
    m_tagEdit->setMaximumWidth(90);
    m_tagEdit->setToolTip(QStringLiteral("文件名标签：输出为 <PDF名>_<标签>.csv/.xlsx"));

    auto* fmtRow = new QHBoxLayout();
    fmtRow->addWidget(new QLabel(QStringLiteral("输出格式:")));
    fmtRow->addWidget(m_fmtCombo);
    fmtRow->addSpacing(12);
    fmtRow->addWidget(new QLabel(QStringLiteral("文件标签:")));
    fmtRow->addWidget(m_tagEdit);
    fmtRow->addStretch();

    m_verboseCb = new QCheckBox(QStringLiteral("详细日志（逐页进度，便于排查）"));
    m_verboseCb->setChecked(true);

    auto* optBox = new QGroupBox(QStringLiteral("输出与选项"));
    auto* optLay = new QVBoxLayout(optBox);
    optLay->addLayout(outRow);
    optLay->addWidget(m_sameDirCb);
    optLay->addLayout(fmtRow);
    optLay->addWidget(m_verboseCb);
    optLay->addStretch();

    // ---------------- 运行条 ----------------
    m_startBtn = new QPushButton(QStringLiteral("开始批量提取"));
    m_startBtn->setMinimumHeight(34);
    m_startBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:#2d7d46;color:#fff;border:none;border-radius:6px;"
        "padding:0 18px;font-weight:600;}"
        "QPushButton:hover{background:#25693a;}"
        "QPushButton:disabled{background:#9bb7a4;}"));
    m_stopBtn = new QPushButton(QStringLiteral("停止"));
    m_stopBtn->setEnabled(false);
    m_openBtn = new QPushButton(QStringLiteral("打开输出目录"));

    m_progress = new QProgressBar();
    m_progress->setFormat(QStringLiteral("%v / %m"));
    m_progress->setValue(0);
    m_progress->setMaximum(1);

    m_summary = new QLabel(QStringLiteral("待处理"));
    m_summary->setWordWrap(true);

    m_backendLbl = new QLabel();
    m_backendLbl->setStyleSheet(QStringLiteral("QLabel{color:#5a6472;}"));

    auto* runRow = new QHBoxLayout();
    runRow->addWidget(m_startBtn);
    runRow->addWidget(m_stopBtn);
    runRow->addWidget(m_openBtn);
    runRow->addSpacing(12);
    runRow->addWidget(m_progress, 1);

    auto* statusRow = new QHBoxLayout();
    statusRow->addWidget(m_summary, 1);
    statusRow->addWidget(m_backendLbl);

    // ---------------- 日志 ----------------
    m_log = new QPlainTextEdit();
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    m_log->setPlaceholderText(QStringLiteral("运行日志（内核 stderr）会显示在这里"));

    auto* logBox = new QGroupBox(QStringLiteral("运行日志"));
    auto* logLay = new QVBoxLayout(logBox);
    logLay->addWidget(m_log);
    logBox->setMaximumHeight(190);

    // ---------------- 组装 ----------------
    auto* top = new QWidget();
    auto* topLay = new QVBoxLayout(top);
    topLay->setContentsMargins(10, 10, 10, 6);
    topLay->setSpacing(10);
    topLay->addWidget(intro);
    topLay->addWidget(inputBox, 1);

    auto* side = new QWidget();
    auto* sideLay = new QVBoxLayout(side);
    sideLay->setContentsMargins(0, 0, 0, 0);
    sideLay->setSpacing(10);
    sideLay->addWidget(optBox);
    sideLay->addLayout(runRow);
    sideLay->addLayout(statusRow);
    sideLay->addWidget(logBox);
    sideLay->addStretch();

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(top);
    split->addWidget(side);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(split);

    // ---------------- 连接 ----------------
    connect(m_addBtn,    &QPushButton::clicked, this, &MaterialExtractPage::onAddFiles);
    connect(m_addDirBtn, &QPushButton::clicked, this, &MaterialExtractPage::onAddDir);
    connect(m_delBtn,    &QPushButton::clicked, this, &MaterialExtractPage::onRemoveSelected);
    connect(m_clearBtn,  &QPushButton::clicked, this, &MaterialExtractPage::onClearAll);
    connect(m_outBtn,    &QPushButton::clicked, this, &MaterialExtractPage::onPickOutDir);
    connect(m_startBtn,  &QPushButton::clicked, this, &MaterialExtractPage::onStart);
    connect(m_stopBtn,   &QPushButton::clicked, this, &MaterialExtractPage::onStop);
    connect(m_openBtn,   &QPushButton::clicked, this, &MaterialExtractPage::onOpenOutputDir);

    // 「与源目录同目录」勾选时，目录输入行整体置灰（避免用户以为设置了却不生效）。
    connect(m_sameDirCb, &QCheckBox::toggled, this, [this](bool on){
        m_outEdit->setEnabled(!on);
        m_outBtn->setEnabled(!on);
    });
    m_outEdit->setEnabled(false);
    m_outBtn->setEnabled(false);

    setCliDir(m_cliDir);
}

MaterialExtractPage::~MaterialExtractPage()
{
    if (m_thread && m_thread->isRunning()) {
        if (m_worker) m_worker->requestStop();
        m_thread->quit();
        m_thread->wait(5000);
    }
}

void MaterialExtractPage::setCliDir(const QString& dir)
{
    m_cliDir = dir;
    KernelClient::setCliDir(dir);

    const bool have = KernelClient::embeddedAvailable(QStringLiteral("EGpdf2excel"))
                      || QFileInfo::exists(KernelClient::findKernelFile(
                             QStringLiteral("EGpdf2excel"), QStringLiteral(".exe")));
    m_startBtn->setEnabled(have && !m_running);
    m_backendLbl->setText(have
        ? KernelClient::describe(QStringLiteral("EGpdf2excel"))
        : QStringLiteral("未找到 EGpdf2excel 内核 — 请在顶部设置 CLI 目录"));
    m_backendLbl->setStyleSheet(have ? QStringLiteral("QLabel{color:#5a6472;}")
                                     : QStringLiteral("QLabel{color:#c0392b;}"));
}

// ---------------------------------------------------------------------------
// 批量导入
// ---------------------------------------------------------------------------
QStringList MaterialExtractPage::collectPdfs(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        if (fi.isDir()) {
            // 目录递归收集，排序保证结果稳定可复现。
            QDirIterator it(fi.absoluteFilePath(), { QStringLiteral("*.pdf") },
                            QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
            QStringList found;
            while (it.hasNext()) found << QDir::toNativeSeparators(it.next());
            found.sort(Qt::CaseInsensitive);
            out += found;
        } else if (fi.isFile() && fi.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) {
            out << QDir::toNativeSeparators(fi.absoluteFilePath());
        }
    }
    return out;
}

void MaterialExtractPage::appendPaths(const QStringList& paths)
{
    const QStringList pdfs = collectPdfs(paths);
    if (pdfs.isEmpty()) {
        emit statusMessage(QStringLiteral("没有可添加的 PDF"));
        return;
    }

    // 已存在的路径去重（按规范化路径比较）。
    QSet<QString> have;
    for (int r = 0; r < m_table->rowCount(); ++r)
        have.insert(m_table->item(r, ColFile)->data(RolePath).toString());

    int added = 0;
    for (const QString& p : pdfs) {
        const QString canon = QFileInfo(p).absoluteFilePath();
        if (have.contains(canon)) continue;
        have.insert(canon);

        const int r = m_table->rowCount();
        m_table->insertRow(r);

        auto* nameItem = new QTableWidgetItem(QFileInfo(canon).fileName());
        nameItem->setData(RolePath, canon);
        nameItem->setToolTip(canon);
        m_table->setItem(r, ColFile, nameItem);
        m_table->setItem(r, ColSize, new QTableWidgetItem(humanSize(QFileInfo(canon).size())));
        m_table->setItem(r, ColStatus, new QTableWidgetItem(QStringLiteral("待处理")));
        m_table->setItem(r, ColResult, new QTableWidgetItem(QString()));
        ++added;
    }

    m_progress->setMaximum(qMax(1, m_table->rowCount()));
    m_progress->setValue(0);
    m_summary->setText(QStringLiteral("已导入 %1 个 PDF%2")
                           .arg(m_table->rowCount())
                           .arg(added < pdfs.size()
                                    ? QStringLiteral("（跳过 %1 个重复项）").arg(pdfs.size() - added)
                                    : QString()));
    appendLog(QStringLiteral("导入 %1 个 PDF（新增 %2）").arg(pdfs.size()).arg(added));
    emit statusMessage(QStringLiteral("已导入 %1 个 PDF（新增 %2）").arg(pdfs.size()).arg(added));
}

void MaterialExtractPage::addFiles(const QStringList& paths) { appendPaths(paths); }

void MaterialExtractPage::onAddFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择 PDF（可多选）"), QString(), QStringLiteral("PDF (*.pdf)"));
    if (!files.isEmpty()) appendPaths(files);
}

void MaterialExtractPage::onAddDir()
{
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("选择包含 PDF 的文件夹"));
    if (!d.isEmpty()) appendPaths({ d });
}

void MaterialExtractPage::onRemoveSelected()
{
    if (m_running) { emit statusMessage(QStringLiteral("提取进行中，暂不能修改列表")); return; }
    QList<int> rows;
    for (const QModelIndex& idx : m_table->selectionModel()->selectedRows()) rows << idx.row();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int r : rows) m_table->removeRow(r);
    m_progress->setMaximum(qMax(1, m_table->rowCount()));
    m_progress->setValue(0);
    m_summary->setText(m_table->rowCount() ? QStringLiteral("已导入 %1 个 PDF").arg(m_table->rowCount())
                                           : QStringLiteral("待处理"));
}

void MaterialExtractPage::onClearAll()
{
    if (m_running) { emit statusMessage(QStringLiteral("提取进行中，暂不能修改列表")); return; }
    m_table->setRowCount(0);
    m_progress->setMaximum(1);
    m_progress->setValue(0);
    m_summary->setText(QStringLiteral("待处理"));
    m_log->clear();
}

void MaterialExtractPage::onPickOutDir()
{
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出目录"), m_outEdit->text());
    if (!d.isEmpty()) { m_outEdit->setText(QDir::toNativeSeparators(d)); m_sameDirCb->setChecked(false); }
}

// ---------------------------------------------------------------------------
// 拖拽导入
// ---------------------------------------------------------------------------
void MaterialExtractPage::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MaterialExtractPage::dropEvent(QDropEvent* e)
{
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls()) {
        if (u.isLocalFile()) paths << QDir::toNativeSeparators(u.toLocalFile());
    }
    if (paths.isEmpty()) return;
    appendPaths(paths);
    e->acceptProposedAction();
}

// ---------------------------------------------------------------------------
// 执行
// ---------------------------------------------------------------------------
QStringList MaterialExtractPage::currentFiles() const
{
    QStringList files;
    for (int r = 0; r < m_table->rowCount(); ++r)
        files << m_table->item(r, ColFile)->data(RolePath).toString();
    return files;
}

QString MaterialExtractPage::effectiveOutDir() const
{
    if (m_sameDirCb->isChecked()) return QString();
    return QDir::toNativeSeparators(m_outEdit->text().trimmed());
}

void MaterialExtractPage::setRunning(bool running)
{
    m_running = running;
    m_startBtn->setEnabled(!running);
    m_stopBtn->setEnabled(running);
    m_addBtn->setEnabled(!running);
    m_addDirBtn->setEnabled(!running);
    m_delBtn->setEnabled(!running);
    m_clearBtn->setEnabled(!running);
    m_fmtCombo->setEnabled(!running);
    m_tagEdit->setEnabled(!running);
    m_verboseCb->setEnabled(!running);
    m_sameDirCb->setEnabled(!running);
    if (!running) {
        const bool have = KernelClient::embeddedAvailable(QStringLiteral("EGpdf2excel"))
                          || QFileInfo::exists(KernelClient::findKernelFile(
                                 QStringLiteral("EGpdf2excel"), QStringLiteral(".exe")));
        m_startBtn->setEnabled(have);
    }
}

void MaterialExtractPage::appendLog(const QString& s)
{
    if (s.isEmpty()) return;
    m_log->appendPlainText(s);
}

void MaterialExtractPage::onStart()
{
    if (m_running) return;
    const QStringList files = currentFiles();
    if (files.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("材料表提取"),
                                 QStringLiteral("请先添加至少一个 PDF（可多选，也可拖拽整个文件夹进来）。"));
        return;
    }

    const QString outDir = effectiveOutDir();
    if (!outDir.isEmpty() && !QDir().mkpath(outDir)) {
        QMessageBox::warning(this, QStringLiteral("材料表提取"),
                             QStringLiteral("无法创建输出目录：\n%1").arg(outDir));
        return;
    }

    // 重置每一行的状态。
    for (int r = 0; r < m_table->rowCount(); ++r) {
        m_table->item(r, ColStatus)->setText(QStringLiteral("待处理"));
        m_table->item(r, ColResult)->setText(QString());
    }
    m_progress->setMaximum(files.size());
    m_progress->setValue(0);
    m_log->clear();
    setRunning(true);
    appendLog(QStringLiteral("开始批量提取：%1 个文件，输出目录：%2，格式：%3，后端：%4")
                  .arg(files.size())
                  .arg(outDir.isEmpty() ? QStringLiteral("（与源文件同目录）") : outDir)
                  .arg(m_fmtCombo->currentData().toString())
                  .arg(m_kernelBackend));
    emit statusMessage(QStringLiteral("开始批量提取 %1 个 PDF…").arg(files.size()));

    m_thread = new QThread(this);
    m_worker = new MaterialExtractWorker(files, outDir, m_fmtCombo->currentData().toString(),
                                         m_tagEdit->text().trimmed(), m_kernelBackend,
                                         m_verboseCb->isChecked());
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started,  m_worker, &MaterialExtractWorker::run);
    connect(m_worker, &MaterialExtractWorker::fileStarted,  this, &MaterialExtractPage::onFileStarted);
    connect(m_worker, &MaterialExtractWorker::fileFinished, this, &MaterialExtractPage::onFileFinished);
    connect(m_worker, &MaterialExtractWorker::allFinished,  this, &MaterialExtractPage::onAllFinished);
    // 全部结束后收线程（用 deleteLater，避免在信号回调里销毁发送者）。
    connect(m_worker, &MaterialExtractWorker::allFinished, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_thread->start();
}

void MaterialExtractPage::onStop()
{
    if (!m_running || !m_worker) return;
    m_worker->requestStop();
    m_stopBtn->setEnabled(false);
    appendLog(QStringLiteral("已请求停止 —— 当前文件处理完后结束（内核调用本身不可中断）。"));
    emit statusMessage(QStringLiteral("已请求停止…"));
}

void MaterialExtractPage::onFileStarted(int index)
{
    if (index < 0 || index >= m_table->rowCount()) return;
    m_table->item(index, ColStatus)->setText(QStringLiteral("处理中…"));
    m_table->scrollToItem(m_table->item(index, ColFile));
}

void MaterialExtractPage::onFileFinished(int index, int rc, int rows,
                                         const QStringList& outputs, const QString& logText)
{
    if (index >= 0 && index < m_table->rowCount()) {
        if (rc == 0) {
            m_table->item(index, ColStatus)->setText(QStringLiteral("完成"));
            QString res = QStringLiteral("%1 行").arg(rows);
            if (!outputs.isEmpty()) res += QStringLiteral(" → ") + outputs.join(QStringLiteral(" / "));
            m_table->item(index, ColResult)->setText(res);
        } else {
            m_table->item(index, ColStatus)->setText(QStringLiteral("失败 (rc=%1)").arg(rc));
            // 失败时把最后一条有信息量的日志塞进结果列，便于一眼看出原因。
            const QStringList lines = logText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            m_table->item(index, ColResult)->setText(lines.isEmpty() ? QString()
                                                                     : lines.last().trimmed());
        }
        refreshRowAppearance(index);
    }

    m_progress->setValue(m_progress->value() + 1);
    if (!logText.isEmpty()) {
        appendLog(QStringLiteral("--- [%1] rc=%2 ---").arg(index + 1).arg(rc));
        appendLog(logText);
    }
    m_summary->setText(QStringLiteral("进行中：%1 / %2").arg(m_progress->value()).arg(m_progress->maximum()));
}

void MaterialExtractPage::onAllFinished(int okCount, int failCount, int stopped)
{
    setRunning(false);
    if (m_worker) { m_worker = nullptr; }   // 由 QThread::finished 触发的 deleteLater 接管
    if (m_thread) { m_thread = nullptr; }   // QThread 以本页为 parent，随页面销毁

    const QString head = stopped ? QStringLiteral("已停止") : QStringLiteral("提取完成");
    m_summary->setText(QStringLiteral("%1：成功 %2 个，失败 %3 个%4")
                           .arg(head).arg(okCount).arg(failCount)
                           .arg(stopped ? QStringLiteral("（未处理完）") : QString()));
    appendLog(m_summary->text());
    emit statusMessage(m_summary->text());

    if (!stopped && failCount == 0 && okCount > 0) {
        QMessageBox::information(this, QStringLiteral("材料表提取"),
            QStringLiteral("已成功抽取 %1 个 PDF 的材料表。\n\n可点「打开输出目录」查看结果。").arg(okCount));
    }
}

void MaterialExtractPage::onOpenOutputDir()
{
    QString dir = effectiveOutDir();
    if (dir.isEmpty()) {
        // "与源文件同目录"时，打开列表里第一个文件所在目录。
        if (m_table->rowCount() > 0)
            dir = QFileInfo(m_table->item(0, ColFile)->data(RolePath).toString()).absolutePath();
    }
    if (dir.isEmpty() || !QDir(dir).exists()) {
        emit statusMessage(QStringLiteral("输出目录不存在（还没开始提取？）"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MaterialExtractPage::refreshRowAppearance(int row)
{
    if (row < 0 || row >= m_table->rowCount()) return;
    QTableWidgetItem* st = m_table->item(row, ColStatus);
    if (!st) return;
    const QString t = st->text();
    QColor bg, fg;
    if (t == QStringLiteral("完成")) { bg = QColor(0xE8, 0xF6, 0xEC); fg = QColor(0x1B, 0x7F, 0x3B); }
    else if (t.startsWith(QStringLiteral("失败"))) { bg = QColor(0xFD, 0xEC, 0xEA); fg = QColor(0xC0, 0x39, 0x2B); }
    else if (t.startsWith(QStringLiteral("处理中"))) { bg = QColor(0xFF, 0xF7, 0xE0); fg = QColor(0x8A, 0x6D, 0x00); }
    else { return; }
    st->setBackground(bg);
    st->setForeground(fg);
}
