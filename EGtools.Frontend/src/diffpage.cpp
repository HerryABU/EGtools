#include "diffpage.h"
#include "panels.h"

#include "continuouspdfview.h"
#include "kernelclient.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// 变化类型 → 中文标签 + 颜色（与内核 changeType 取值一一对应）。
struct ChangeStyle { const char* label; const char* color; };
ChangeStyle styleForChange(const QString& changeType)
{
    if (changeType == QLatin1String("added"))    return { "新增", "#D32F2F" };
    if (changeType == QLatin1String("removed"))  return { "删除", "#1976D2" };
    if (changeType == QLatin1String("changed"))  return { "变更", "#2E7D32" };
    if (changeType == QLatin1String("modified")) return { "变更", "#2E7D32" };
    return { "移动", "#6D4C41" };
}

QString trimmedOneLine(const QString& s, int maxLen = 90)
{
    QString t = s;
    t.replace(QLatin1Char('\n'), QLatin1Char(' '));
    t.replace(QLatin1Char('\r'), QLatin1Char(' '));
    t = t.simplified();
    if (t.size() > maxLen) t = t.left(maxLen - 1) + QStringLiteral("…");
    return t;
}

// 「标签 + 输入 + 选择按钮」一行（顶部工具区反复用到）。
QWidget* makePathRow(const QString& label, QLineEdit** outEdit, QWidget* owner,
                     void (DiffPage::*slot)(), const QString& placeholder)
{
    auto* host = new QWidget();
    auto* l = new QHBoxLayout(host);
    l->setContentsMargins(0, 0, 0, 0);
    auto* lb = new QLabel(label);
    lb->setMinimumWidth(64);
    auto* edit = new QLineEdit();
    edit->setPlaceholderText(placeholder);
    auto* btn = new QPushButton(QStringLiteral("选择…"));
    btn->setMaximumWidth(72);
    QObject::connect(btn, &QPushButton::clicked, owner, slot);
    l->addWidget(lb);
    l->addWidget(edit, 1);
    l->addWidget(btn);
    *outEdit = edit;
    return host;
}

} // namespace

DiffPage::DiffPage(QWidget* parent) : QWidget(parent)
{
    // ---------------------------------------------------------------- 顶部：只放"要什么"
    auto* baseRow = makePathRow(QStringLiteral("基准图"), &m_baseEdit, this,
                                &DiffPage::onPickBaseline,
                                QStringLiteral("旧版图纸 PDF（比对基准）"));
    auto* tgtRow  = makePathRow(QStringLiteral("目标图"), &m_tgtEdit, this,
                                &DiffPage::onPickTarget,
                                QStringLiteral("新版图纸 PDF"));

    // 输出目录：显式给出"结果落在哪"。旧实现把结果写进 onRun 的局部临时目录，
    // 函数一返回就被删除 —— 用户其实拿不到文件。
    auto* outRow = makePathRow(QStringLiteral("输出目录"), &m_outEdit, this,
                               &DiffPage::onPickOutDir,
                               QStringLiteral("留空 = 与目标图同目录，结果命名 <目标名>_Diff.pdf"));

    auto* csvHost = new QWidget();
    {
        auto* l = new QHBoxLayout(csvHost);
        l->setContentsMargins(0, 0, 0, 0);
        auto* lb = new QLabel(QStringLiteral("材料表"));
        lb->setMinimumWidth(64);
        m_csvEdit = new QLineEdit();
        m_csvEdit->setPlaceholderText(QStringLiteral("可选：人工修正材料表（CSV/TSV），用于管线号一一对应检查"));
        auto* btn = new QPushButton(QStringLiteral("选择…"));
        btn->setMaximumWidth(72);
        auto* clr = new QPushButton(QStringLiteral("清除"));
        clr->setMaximumWidth(56);
        connect(btn, &QPushButton::clicked, this, &DiffPage::onPickCsv);
        connect(clr, &QPushButton::clicked, this, &DiffPage::onClearCsv);
        l->addWidget(lb);
        l->addWidget(m_csvEdit, 1);
        l->addWidget(btn);
        l->addWidget(clr);
    }

    m_runBtn = new QPushButton(QStringLiteral("开始比对"));
    m_runBtn->setObjectName(QStringLiteral("primaryBtn"));
    m_runBtn->setMinimumHeight(30);
    auto* settingsBtn = new QPushButton(QStringLiteral("比对设置…"));
    settingsBtn->setMinimumHeight(30);
    settingsBtn->setToolTip(QStringLiteral("所有比对参数都在这里，并带预设的「比对类型」。"));

    m_openOutBtn = new QPushButton(QStringLiteral("打开输出目录"));
    m_openOutBtn->setEnabled(false);
    connect(m_openOutBtn, &QPushButton::clicked, this, &DiffPage::onOpenOutDir);
    connect(settingsBtn, &QPushButton::clicked, this, &DiffPage::onOpenSettings);
    connect(m_runBtn, &QPushButton::clicked, this, &DiffPage::onRun);

    m_settingsSummary = new QLabel(QStringLiteral(
        "比对设置：默认（推荐）"));
    m_settingsSummary->setWordWrap(true);
    m_settingsSummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_settingsSummary->setObjectName(QStringLiteral("diffSummary"));

    auto* actionRow = new QHBoxLayout();
    actionRow->addWidget(settingsBtn);
    actionRow->addWidget(m_runBtn);
    actionRow->addWidget(m_openOutBtn);
    actionRow->addWidget(m_settingsSummary, 1);

    m_status = new QLabel(QStringLiteral("内核状态：未探测"));
    m_status->setWordWrap(true);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* header = new QWidget();
    auto* headerLay = new QVBoxLayout(header);
    headerLay->setContentsMargins(8, 8, 8, 4);
    headerLay->setSpacing(4);
    headerLay->addWidget(baseRow);
    headerLay->addWidget(tgtRow);
    headerLay->addWidget(csvHost);
    headerLay->addWidget(outRow);
    headerLay->addLayout(actionRow);
    headerLay->addWidget(m_status);

    // ---------------------------------------------------------------- 右：真正的差异图查看器
    m_viewer = new ContinuousPdfView();
    m_viewer->setCliDir(m_cliDir);
    m_viewScroll = new QScrollArea();
    m_viewScroll->setWidget(m_viewer);
    m_viewScroll->setWidgetResizable(false);
    m_viewScroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_viewer->attachScrollArea(m_viewScroll);

    m_zoomLabel = new QLabel(QStringLiteral("适合宽度"));
    auto* zOut  = new QPushButton(QStringLiteral("−"));  zOut->setMaximumWidth(28);
    auto* zIn   = new QPushButton(QStringLiteral("+"));  zIn->setMaximumWidth(28);
    auto* z100  = new QPushButton(QStringLiteral("100%"));
    auto* zFit  = new QPushButton(QStringLiteral("适合宽度"));
    auto* zPage = new QPushButton(QStringLiteral("适合页面"));
    connect(zOut,  &QPushButton::clicked, this, [this]{ m_viewer->zoomOut(); });
    connect(zIn,   &QPushButton::clicked, this, [this]{ m_viewer->zoomIn(); });
    connect(z100,  &QPushButton::clicked, this, [this]{ m_viewer->setZoom(1.0); });
    connect(zFit,  &QPushButton::clicked, this, [this]{
        if (m_viewScroll) m_viewer->fitToWidth(m_viewScroll->viewport()->width());
    });
    connect(zPage, &QPushButton::clicked, this, [this]{
        if (m_viewScroll) m_viewer->fitToPageHeight(m_viewScroll->viewport()->height());
    });
    connect(m_viewer, &ContinuousPdfView::zoomChanged, this, [this](double z){
        if (m_zoomLabel) m_zoomLabel->setText(QStringLiteral("缩放 %1%").arg(qRound(z * 100)));
    });

    auto* viewBar = new QHBoxLayout();
    viewBar->addWidget(new QLabel(QStringLiteral("差异图：")));
    viewBar->addWidget(zOut);
    viewBar->addWidget(zIn);
    viewBar->addWidget(z100);
    viewBar->addWidget(zFit);
    viewBar->addWidget(zPage);
    viewBar->addWidget(m_zoomLabel);
    viewBar->addStretch();

    auto* viewPane = new QWidget();
    auto* viewLay = new QVBoxLayout(viewPane);
    viewLay->setContentsMargins(4, 4, 4, 4);
    viewLay->setSpacing(4);
    viewLay->addLayout(viewBar);
    viewLay->addWidget(m_viewScroll, 1);

    // ---------------------------------------------------------------- 左：差异清单（Markups List 等价物）
    m_filterCombo = new QComboBox();
    m_filterCombo->addItem(QStringLiteral("全部差异"), QStringLiteral("all"));
    m_filterCombo->addItem(QStringLiteral("仅图形"),   QStringLiteral("graphic"));
    m_filterCombo->addItem(QStringLiteral("仅文字"),   QStringLiteral("text"));
    m_filterCombo->addItem(QStringLiteral("仅新增"),   QStringLiteral("added"));
    m_filterCombo->addItem(QStringLiteral("仅删除"),   QStringLiteral("removed"));
    m_filterCombo->addItem(QStringLiteral("仅变更"),   QStringLiteral("changed"));
    connect(m_filterCombo, &QComboBox::currentIndexChanged, this, [this](int){ applyFilter(); });

    m_prevBtn = new QPushButton(QStringLiteral("▲ 上一条"));
    m_nextBtn = new QPushButton(QStringLiteral("▼ 下一条"));
    m_prevBtn->setToolTip(QStringLiteral("跳到上一条差异并在图上高亮"));
    m_nextBtn->setToolTip(QStringLiteral("跳到下一条差异并在图上高亮"));
    connect(m_prevBtn, &QPushButton::clicked, this, &DiffPage::onPrevDiff);
    connect(m_nextBtn, &QPushButton::clicked, this, &DiffPage::onNextDiff);

    m_diffCountLabel = new QLabel(QStringLiteral("尚无结果"));
    m_diffCountLabel->setWordWrap(true);
    m_diffCountLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* diffBar = new QHBoxLayout();
    diffBar->addWidget(new QLabel(QStringLiteral("筛选：")));
    diffBar->addWidget(m_filterCombo, 1);
    diffBar->addWidget(m_prevBtn);
    diffBar->addWidget(m_nextBtn);

    m_diffTable = new QTableWidget(0, 6);
    m_diffTable->setHorizontalHeaderLabels({
        QStringLiteral("#"), QStringLiteral("页"), QStringLiteral("类型"),
        QStringLiteral("变化"), QStringLiteral("内容 / 说明"), QStringLiteral("置信度") });
    m_diffTable->verticalHeader()->setVisible(false);
    m_diffTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_diffTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_diffTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diffTable->setAlternatingRowColors(true);
    m_diffTable->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    for (int c = 0; c < 5; ++c)
        m_diffTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_diffTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    connect(m_diffTable, &QTableWidget::cellDoubleClicked, this, &DiffPage::onDiffRowActivated);
    connect(m_diffTable, &QTableWidget::itemSelectionChanged, this, [this]{
        const int r = m_diffTable->currentRow();
        if (r >= 0 && r != m_curDiffRow) gotoDiff(r);
    });

    auto* listPane = new QWidget();
    auto* listLay = new QVBoxLayout(listPane);
    listLay->setContentsMargins(4, 4, 4, 4);
    listLay->setSpacing(4);
    listLay->addLayout(diffBar);
    listLay->addWidget(m_diffTable, 1);
    listLay->addWidget(m_diffCountLabel);

    // 明细（管线号 / JSON / 图例）降级到标签页：专业工具不会把原始 JSON 摆在主视区。
    m_jsonView = new QTextEdit();
    m_jsonView->setReadOnly(true);
    m_jsonView->setLineWrapMode(QTextEdit::NoWrap);
    m_jsonView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_jsonView->setPlainText(QStringLiteral("差异 JSON 摘要（运行后显示）"));

    m_pipeView = new QTextEdit();
    m_pipeView->setReadOnly(true);
    m_pipeView->setLineWrapMode(QTextEdit::NoWrap);
    m_pipeView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_pipeView->setPlainText(QStringLiteral("运行后显示：匹配 / 图中独有(材料表缺) / 材料表独有(图中未画)"));

    auto* legendView = new QTextBrowser();
    legendView->setOpenExternalLinks(false);
    legendView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    legendView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    legendView->setHtml(QStringLiteral(
        "<b>怎么读结果</b><br>"
        "● 图形标记：<b>红(实线)</b> = 新增　<b>红(虚线)</b> = 删除　"
        "<b>绿</b> = 变更（同位置形状改了）<br>"
        "● 文字是<b>直接改色</b>（不画圈）："
        "<font color=\"#FF0000\">新增 = 红</font>、<font color=\"#0000FF\">删除 = 蓝</font>"
        "，保留原始字体与版式<br>"
        "● <b>差异线</b>沿真实变更的那几条线描边（细线 + 半透明），上千条也不糊成一片<br>"
        "● <b>页面配对</b>先按图号（标题栏右下角、每页唯一的管段号）1:1 配对；"
        "图号只在一侧的页直接判新增/删除；无图号的页用线画指纹兜底<br>"
        "● 想少调参就用「比对设置 → 比对类型」的预设。"));

    m_detailTabs = new QTabWidget();
    m_detailTabs->addTab(m_pipeView, QStringLiteral("管线号 ↔ 材料表"));
    m_detailTabs->addTab(m_jsonView, QStringLiteral("差异 JSON"));
    m_detailTabs->addTab(legendView, QStringLiteral("图例说明"));

    // ---------------------------------------------------------------- 组装
    auto* leftCol = new QSplitter(Qt::Vertical);
    leftCol->addWidget(listPane);
    leftCol->addWidget(m_detailTabs);
    leftCol->setStretchFactor(0, 3);
    leftCol->setStretchFactor(1, 2);
    makeSplitterNonDraggable(leftCol);        // 不给拖：尺寸走"视图 → 面板尺寸"预设

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(leftCol);
    split->addWidget(viewPane);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    split->setChildrenCollapsible(false);
    makeSplitterNonDraggable(split);          // 不给拖
    split->setSizes({560, 760});

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(header);
    lay->addWidget(split, 1);

    refreshKernelStatus();
    applyFilter();
    updateSettingsSummary();
}

// ---------------------------------------------------------------- 内核定位

QString DiffPage::cliExe(const QString& name) const
{
    const QString exe = name + QStringLiteral(".exe");
    QStringList cands;
    auto add = [&](const QString& d){ if (!d.isEmpty()) cands << QDir(d).filePath(exe); };
    add(m_cliDir);
    const QString appDir = QCoreApplication::applicationDirPath();
    add(appDir);
    add(QDir(appDir).filePath(QStringLiteral("cli")));
    add(QDir(appDir).filePath(QStringLiteral("..")));
    add(QDir(appDir).filePath(QStringLiteral("../cli")));
    for (const QString& c : cands) if (QFileInfo::exists(c)) return c;
    return cands.isEmpty() ? exe : cands.first();
}

void DiffPage::refreshKernelStatus()
{
    const bool ok = QFileInfo::exists(cliExe(QStringLiteral("EGpdf2df")));
    m_status->setText(ok ? QStringLiteral("内核状态：已就绪（EGpdf2df）")
                         : QStringLiteral("内核状态：未找到 EGpdf2df.exe — 请在首页设置 CLI 目录"));
    m_runBtn->setEnabled(ok);
}

// ---------------------------------------------------------------- 文件选择

void DiffPage::onPickBaseline()
{
    const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择基准图 PDF"),
        QString(), QStringLiteral("PDF (*.pdf)"));
    if (!p.isEmpty()) m_baseEdit->setText(p);
}

void DiffPage::onPickTarget()
{
    const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择目标图 PDF"),
        QString(), QStringLiteral("PDF (*.pdf)"));
    if (!p.isEmpty()) m_tgtEdit->setText(p);
}

void DiffPage::onPickCsv()
{
    const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择人工修正材料表"),
        QString(), QStringLiteral("CSV/TSV (*.csv *.tsv *.txt)"));
    if (!p.isEmpty()) m_csvEdit->setText(p);
}

void DiffPage::onClearCsv() { m_csvEdit->clear(); }

void DiffPage::onPickOutDir()
{
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出目录"),
                                                        m_outEdit->text());
    if (!d.isEmpty()) m_outEdit->setText(d);
}

void DiffPage::onOpenOutDir()
{
    if (m_outPdfPath.isEmpty()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_outPdfPath).absolutePath()));
}

// 结果目录：空 = 与目标图同目录；需要时创建（绝不默默把结果丢掉）。
bool DiffPage::outDirReady(bool createIfNeeded)
{
    QString dir = m_outEdit->text().trimmed();
    if (dir.isEmpty()) dir = QFileInfo(m_tgtEdit->text().trimmed()).absolutePath();
    if (dir.isEmpty()) return false;
    QDir d(dir);
    if (d.exists()) return true;
    if (!createIfNeeded) return false;
    if (!d.mkpath(QStringLiteral("."))) {
        emit statusMessage(QStringLiteral("无法创建输出目录：%1").arg(dir));
        return false;
    }
    return true;
}

void DiffPage::updateSettingsSummary()
{
    if (!m_settingsSummary) return;
    m_settingsSummary->setText(
        QStringLiteral("当前设置：检测=%1 · 位置容差=%2pt · 报告=%3 · 聚类=%4pt · 标记=%5")
            .arg(m_params.graphicMethod)
            .arg(m_params.vectorTolPt)
            .arg(m_params.reportMode)
            .arg(m_params.clusterRadiusPt)
            .arg(m_params.markStyle));
}

void DiffPage::onOpenSettings()
{
    DiffSettingsDialog dlg(m_params, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_params = dlg.params();
        emit statusMessage(QStringLiteral("比对设置已更新"));
    }
    updateSettingsSummary();
}

// ---------------------------------------------------------------- 运行

void DiffPage::onRun()
{
    const QString a = m_baseEdit->text().trimmed();
    const QString b = m_tgtEdit->text().trimmed();
    if (a.isEmpty() || b.isEmpty()) {
        emit statusMessage(QStringLiteral("请先选择基准图与目标图"));
        return;
    }
    if (!outDirReady(true)) return;

    // ⚠️ 结果必须落到**用户可见的目录**：旧实现写在 onRun 的局部 QTemporaryDir 里，
    // 函数一返回即被删除，用户其实拿不到差异 PDF。
    QString dir = m_outEdit->text().trimmed();
    if (dir.isEmpty()) dir = QFileInfo(b).absolutePath();
    const QString stem = QFileInfo(b).completeBaseName();
    QString outPdf = QDir(dir).filePath(stem + QStringLiteral("_Diff.pdf"));
    for (int i = 2; QFileInfo::exists(outPdf) && i < 1000; ++i)
        outPdf = QDir(dir).filePath(QStringLiteral("%1_Diff(%2).pdf").arg(stem).arg(i));
    m_outPdfPath = outPdf;

    emit statusMessage(QStringLiteral("开始比对…（结果将写入 %1）").arg(outPdf));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_runBtn->setEnabled(false);

    QJsonObject req;
    req[QStringLiteral("baseline")] = a;
    req[QStringLiteral("target")]   = b;
    req[QStringLiteral("output")]   = outPdf;
    req[QStringLiteral("options")]  = m_params.toOptions();
    const QString csv = m_csvEdit->text().trimmed();
    if (!csv.isEmpty()) req[QStringLiteral("correctedMaterialTable")] = csv;

    // 交给融合层：JSON 契约直接送进去（内嵌走 stdin / 零临时文件；命令行由融合层补 --json）。
    const QString reqJson = QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact));
    const KernelClient::Result kr = KernelClient::run(
        QStringLiteral("EGpdf2df"), {}, reqJson,
        KernelClient::backendFromString(m_kernelBackend));

    QApplication::restoreOverrideCursor();
    m_runBtn->setEnabled(true);

    m_jsonView->setPlainText(kr.out);
    emit statusMessage(QStringLiteral("[EGpdf2df/%1] rc=%2 %3")
                           .arg(kr.backend).arg(kr.rc).arg(kr.err.trimmed().left(300)));

    const QJsonObject root = QJsonDocument::fromJson(kr.out.toUtf8()).object();
    const int nDiff = fillDiffList(root);
    showPipeMatch(root);

    if (kr.rc != 0 || !QFileInfo::exists(outPdf)) {
        m_status->setText(QStringLiteral("比对失败（rc=%1）——详见「差异 JSON」标签页").arg(kr.rc));
        return;
    }

    m_openOutBtn->setEnabled(true);

    // 差异 PDF 交给**真正的查看器**：一屏多页、懒渲染、默认适合宽度。
    m_viewer->setDocument(outPdf, 0);
    if (m_viewScroll) m_viewer->fitToWidth(m_viewScroll->viewport()->width());

    m_status->setText(QStringLiteral("比对完成：%1 条差异 → %2").arg(nDiff).arg(outPdf));
    emit statusMessage(QStringLiteral("比对完成，差异图已打开：%1").arg(outPdf));

    // 有差异就直接定位第一条，用户一眼就看到东西。
    if (nDiff > 0) {
        m_diffTable->selectRow(0);
        gotoDiff(0);
    }
}

// ---------------------------------------------------------------- 差异清单

int DiffPage::fillDiffList(const QJsonObject& root)
{
    m_diffTable->setRowCount(0);
    m_curDiffRow = -1;
    const QJsonArray diffs = root.value(QStringLiteral("diffs")).toArray();

    int idx = 0;
    for (const QJsonValue& v : diffs) {
        const QJsonObject d = v.toObject();
        const QString type   = d.value(QStringLiteral("type")).toString();
        const QString change = d.value(QStringLiteral("changeType")).toString();
        const QJsonObject bbox = d.value(QStringLiteral("bbox")).toObject();
        const double x = bbox.value(QStringLiteral("x")).toDouble();
        const double y = bbox.value(QStringLiteral("y")).toDouble();
        const double w = bbox.value(QStringLiteral("w")).toDouble();
        const double h = bbox.value(QStringLiteral("h")).toDouble();
        const int    page = d.value(QStringLiteral("page")).toInt();
        const double conf = d.value(QStringLiteral("confidence")).toDouble();

        QString detail;
        if (type == QLatin1String("text")) {
            const QString oldT = d.value(QStringLiteral("oldText")).toString();
            const QString newT = d.value(QStringLiteral("newText")).toString();
            if (change == QLatin1String("added"))        detail = trimmedOneLine(newT);
            else if (change == QLatin1String("removed")) detail = trimmedOneLine(oldT);
            else detail = QStringLiteral("旧: %1  →  新: %2")
                              .arg(trimmedOneLine(oldT, 40), trimmedOneLine(newT, 40));
        } else {
            detail = QStringLiteral("图形 · 位置 (%1, %2) 尺寸 %3×%4")
                         .arg(x, 0, 'f', 1).arg(y, 0, 'f', 1).arg(w, 0, 'f', 1).arg(h, 0, 'f', 1);
        }

        const int r = m_diffTable->rowCount();
        m_diffTable->insertRow(r);
        auto setCell = [&](int col, const QString& text, const QString& color = QString()) {
            auto* it = new QTableWidgetItem(text);
            if (!color.isEmpty()) it->setForeground(QColor(color));
            m_diffTable->setItem(r, col, it);
            return it;
        };
        setCell(0, QString::number(++idx));
        setCell(1, QString::number(page + 1));
        setCell(2, type == QLatin1String("text") ? QStringLiteral("文字") : QStringLiteral("图形"));
        const ChangeStyle cs = styleForChange(change);
        setCell(3, QString::fromUtf8(cs.label), QString::fromLatin1(cs.color));
        setCell(4, detail);
        setCell(5, QString::number(conf, 'f', 2));

        // 定位所需的几何挂在第 0 列（列本身仍显示序号）：
        // 0=页 1=x 2=y 3=w 4=h 5=变化 6=类型
        auto* it0 = m_diffTable->item(r, 0);
        it0->setData(Qt::UserRole,     page);
        it0->setData(Qt::UserRole + 1, x);
        it0->setData(Qt::UserRole + 2, y);
        it0->setData(Qt::UserRole + 3, w);
        it0->setData(Qt::UserRole + 4, h);
        it0->setData(Qt::UserRole + 5, change);
        it0->setData(Qt::UserRole + 6, type);
    }

    applyFilter();
    return diffs.size();
}

// 筛选 —— 清单的第一件事就是"能筛"（专业工具的 Markups List 都是这么用的）。
void DiffPage::applyFilter()
{
    const QString f = m_filterCombo ? m_filterCombo->currentData().toString() : QStringLiteral("all");
    int visible = 0;
    for (int r = 0; r < m_diffTable->rowCount(); ++r) {
        const QTableWidgetItem* it0 = m_diffTable->item(r, 0);
        if (!it0) continue;
        const QString change = it0->data(Qt::UserRole + 5).toString();
        const QString type   = it0->data(Qt::UserRole + 6).toString();
        bool show = true;
        if (f == QLatin1String("graphic"))      show = (type == QLatin1String("graphic"));
        else if (f == QLatin1String("text"))    show = (type == QLatin1String("text"));
        else if (f == QLatin1String("added"))   show = (change == QLatin1String("added"));
        else if (f == QLatin1String("removed")) show = (change == QLatin1String("removed"));
        else if (f == QLatin1String("changed"))
            show = (change == QLatin1String("changed") || change == QLatin1String("modified"));
        m_diffTable->setRowHidden(r, !show);
        if (show) ++visible;
    }
    if (m_diffCountLabel) {
        m_diffCountLabel->setText(m_diffTable->rowCount() == 0
            ? QStringLiteral("尚无结果")
            : QStringLiteral("共 %1 条差异，当前显示 %2 条").arg(m_diffTable->rowCount()).arg(visible));
    }
    updateDiffNav();
}

// 跳到某条差异：查看器滚到该页并高亮该区域（这就是"一眼看到改动在哪"）。
void DiffPage::gotoDiff(int listRow)
{
    if (listRow < 0 || listRow >= m_diffTable->rowCount()) return;
    const QTableWidgetItem* it0 = m_diffTable->item(listRow, 0);
    if (!it0) return;
    const int page = it0->data(Qt::UserRole).toInt();
    const QRectF rect(it0->data(Qt::UserRole + 1).toDouble(),
                      it0->data(Qt::UserRole + 2).toDouble(),
                      it0->data(Qt::UserRole + 3).toDouble(),
                      it0->data(Qt::UserRole + 4).toDouble());
    m_curDiffRow = listRow;
    m_lastGotoPage = page;
    m_lastGotoRect = rect;
    m_viewer->focusRegion(page, rect);
    updateDiffNav();
}

void DiffPage::updateDiffNav()
{
    const int n = m_diffTable ? m_diffTable->rowCount() : 0;
    if (m_prevBtn) m_prevBtn->setEnabled(n > 0 && m_curDiffRow > 0);
    if (m_nextBtn) m_nextBtn->setEnabled(n > 0 && m_curDiffRow + 1 < n);
}

// 上/下一条要**跳过被筛选隐藏的行**，否则会出现"点了没反应"的错觉。
void DiffPage::onNextDiff()
{
    const int n = m_diffTable->rowCount();
    for (int r = m_curDiffRow + 1; r < n; ++r) {
        if (!m_diffTable->isRowHidden(r)) { m_diffTable->selectRow(r); gotoDiff(r); return; }
    }
}

void DiffPage::onPrevDiff()
{
    for (int r = m_curDiffRow - 1; r >= 0; --r) {
        if (!m_diffTable->isRowHidden(r)) { m_diffTable->selectRow(r); gotoDiff(r); return; }
    }
}

void DiffPage::onDiffRowActivated(int row, int) { gotoDiff(row); }

// ---------------------------------------------------------------- 管线号 ↔ 材料表

void DiffPage::showPipeMatch(const QJsonObject& root)
{
    m_pipeView->clear();
    if (root.isEmpty()) { m_pipeView->setPlainText(QStringLiteral("无 JSON 输出")); return; }

    QStringList lines;
    const QJsonArray aligns = root.value(QStringLiteral("alignments")).toArray();
    if (!aligns.isEmpty()) {
        lines << QStringLiteral("【对齐】先按图纸号配对，再估算平移后比对：");
        for (const QJsonValue& v : aligns) {
            const QJsonObject o = v.toObject();
            lines << QStringLiteral("  页%1  缩放=%2  平移 dx=%3pt dy=%4pt")
                        .arg(o.value(QStringLiteral("page")).toInt() + 1)
                        .arg(o.value(QStringLiteral("scale")).toDouble(), 0, 'f', 4)
                        .arg(o.value(QStringLiteral("dxPt")).toDouble(), 0, 'f', 1)
                        .arg(o.value(QStringLiteral("dyPt")).toDouble(), 0, 'f', 1);
        }
    }

    const QJsonObject pm = root.value(QStringLiteral("pipeMatch")).toObject();
    if (!pm.isEmpty()) {
        const QString src = pm.value(QStringLiteral("tableSource")).toString();
        const QJsonArray matched    = pm.value(QStringLiteral("matched")).toArray();
        const QJsonArray figureOnly = pm.value(QStringLiteral("figureOnly")).toArray();
        const QJsonArray tableOnly  = pm.value(QStringLiteral("tableOnly")).toArray();
        lines << QString();
        lines << QStringLiteral("【管线号↔材料表 一一对应】  材料表来源：%1").arg(src);
        lines << QStringLiteral("  匹配(两侧都有)：%1 项").arg(matched.size());
        lines << QStringLiteral("  图中独有 / 材料表缺(风险)：%1 项").arg(figureOnly.size());
        lines << QStringLiteral("  材料表独有 / 图中未画(风险)：%1 项").arg(tableOnly.size());
        auto dump = [&](const QString& title, const QJsonArray& arr) {
            if (arr.isEmpty()) return;
            QStringList items;
            for (const QJsonValue& v : arr) items << v.toString();
            lines << QStringLiteral("  %1：").arg(title);
            lines << QStringLiteral("    ") + items.join(QStringLiteral(", "));
        };
        dump(QStringLiteral("匹配"), matched);
        dump(QStringLiteral("图中独有"), figureOnly);
        dump(QStringLiteral("材料表独有"), tableOnly);
    } else {
        lines << QString();
        lines << QStringLiteral("【管线号↔材料表】未启用或未输出");
    }
    m_pipeView->setPlainText(lines.join(QStringLiteral("\n")));
}

// ---------------------------------------------------------------- 自检钩子

void DiffPage::feedDiffResultForTest(const QJsonObject& root)
{
    fillDiffList(root);
    showPipeMatch(root);
}

int DiffPage::diffRowCountForTest() const
{
    return m_diffTable ? m_diffTable->rowCount() : 0;
}

int DiffPage::diffVisibleCountForTest() const
{
    if (!m_diffTable) return 0;
    int n = 0;
    for (int r = 0; r < m_diffTable->rowCount(); ++r)
        if (!m_diffTable->isRowHidden(r)) ++n;
    return n;
}

void DiffPage::setFilterForTest(const QString& id)
{
    if (!m_filterCombo) return;
    const int idx = m_filterCombo->findData(id);
    if (idx >= 0) m_filterCombo->setCurrentIndex(idx);   // 触发 applyFilter()
}

void DiffPage::selectDiffRowForTest(int row)
{
    if (!m_diffTable || row < 0 || row >= m_diffTable->rowCount()) return;
    m_diffTable->selectRow(row);
    gotoDiff(row);
}
