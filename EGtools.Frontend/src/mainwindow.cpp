#include "mainwindow.h"
#include "clirunner.h"
#include "manualdialog.h"
#include "homepage.h"
#include "schemes.h"
#include "schemepage.h"
#include "diffpage.h"
#include "excelcomparepage.h"
#include "charsegmentpanel.h"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QCheckBox>
#include <QStatusBar>
#include <QStringList>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QToolBar>
#include <QVBoxLayout>

MainWindow::MainWindow(const QString& cliDir, QWidget* parent) : QMainWindow(parent)
{
    if (!cliDir.isEmpty()) m_cliDir = cliDir;
    else                   m_cliDir = qEnvironmentVariable("EGTOOLS_CLI_DIR");
    buildUi();
    log(QStringLiteral("EGtools Qt 前端已就绪（CLI 目录：%1）")
            .arg(m_cliDir.isEmpty() ? QStringLiteral("<未设置>") : m_cliDir));
}

// ---------------------------------------------------------------- UI

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("EGtools — PDF 框选提取 / 图纸差异（Qt）"));
    resize(1280, 860);

    auto* openA = new QAction(QStringLiteral("打开 PDF"), this);
    auto* prevA = new QAction(QStringLiteral("上一页"), this);
    auto* nextA = new QAction(QStringLiteral("下一页"), this);
    auto* renderA = new QAction(QStringLiteral("重渲染"), this);
    auto* clearA = new QAction(QStringLiteral("清空框选"), this);
    auto* exportA = new QAction(QStringLiteral("导出 JSON"), this);
    auto* extractA = new QAction(QStringLiteral("提取 (内核)"), this);
    auto* diffA = new QAction(QStringLiteral("图纸差异"), this);

    auto* tb = addToolBar(QStringLiteral("主工具栏"));
    auto* homeA = new QAction(QStringLiteral("首页"), this);
    auto* schemesA = new QAction(QStringLiteral("框选方案"), this);
    auto* captureA = new QAction(QStringLiteral("存为方案"), this);
    tb->addAction(schemesA);
    tb->addAction(captureA);
    tb->addSeparator();
    tb->addAction(homeA);
    tb->addSeparator();
    tb->addAction(openA);
    tb->addSeparator();
    tb->addAction(prevA);
    tb->addWidget(new QLabel(QStringLiteral(" 页 ")));
    tb->addAction(nextA);
    tb->addAction(renderA);
    tb->addSeparator();
    tb->addAction(exportA);
    tb->addAction(extractA);
    tb->addAction(clearA);
    tb->addSeparator();
    tb->addAction(diffA);

    connect(openA,    &QAction::triggered, this, &MainWindow::onOpen);
    connect(homeA,    &QAction::triggered, this, &MainWindow::onHome);
    connect(schemesA, &QAction::triggered, this, &MainWindow::onSchemes);
    connect(captureA, &QAction::triggered, this, &MainWindow::onCaptureAsScheme);
    connect(prevA,    &QAction::triggered, this, &MainWindow::onPrevPage);
    connect(nextA,    &QAction::triggered, this, &MainWindow::onNextPage);
    connect(renderA,  &QAction::triggered, this, &MainWindow::onRenderPage);
    connect(clearA,   &QAction::triggered, this, &MainWindow::onClearRegions);
    connect(exportA,  &QAction::triggered, this, &MainWindow::onExportJson);
    connect(extractA, &QAction::triggered, this, &MainWindow::onExtract);
    connect(diffA,    &QAction::triggered, this, [this]{ navTo(PageDiff); });

    // Menu bar: 文件 / 操作 / 帮助 (帮助 holds the in-app manual).
    auto* mFile = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    mFile->addAction(openA);
    mFile->addSeparator();
    auto* mQuit = mFile->addAction(QStringLiteral("退出(&X)"));
    connect(mQuit, &QAction::triggered, this, &QWidget::close);

    auto* mOps = menuBar()->addMenu(QStringLiteral("操作(&O)"));
    mOps->addAction(prevA);
    mOps->addAction(nextA);
    mOps->addAction(renderA);
    mOps->addSeparator();
    mOps->addAction(exportA);
    mOps->addAction(extractA);
    mOps->addAction(clearA);
    mOps->addSeparator();
    mOps->addAction(diffA);

    auto* mHelp = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    auto* manualA = mHelp->addAction(QStringLiteral("使用说明书(&M)"));
    manualA->setShortcut(QKeySequence::HelpContents); // F1
    connect(manualA, &QAction::triggered, this, &MainWindow::onManual);
    mHelp->addSeparator();
    auto* aboutA = mHelp->addAction(QStringLiteral("关于(&A)"));
    connect(aboutA, &QAction::triggered, this, &MainWindow::onAbout);

    // Center: view (left) + results (right).
    m_view = new PdfView();
    m_view->setKind(QStringLiteral("detail_table"));
    connect(m_view, &PdfView::regionSelected, this, &MainWindow::onRegionSelected);
    connect(m_view, &PdfView::textPreview, this, &MainWindow::onTextPreview);

    auto* scroll = new QScrollArea();
    scroll->setWidget(m_view);
    scroll->setWidgetResizable(false);
    scroll->setAlignment(Qt::AlignCenter);

    m_modeCombo = new QComboBox();
    m_modeCombo->addItem(QStringLiteral("全域表 (global_table)"), QStringLiteral("global_table"));
    m_modeCombo->addItem(QStringLiteral("细表 (detail_table)"),     QStringLiteral("detail_table"));
    m_modeCombo->addItem(QStringLiteral("材料编号区 (material_no_region)"), QStringLiteral("material_no_region"));
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onModeChanged);

    m_pageLabel = new QLabel(QStringLiteral("-"));
    auto* topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel(QStringLiteral("框选模式:")));
    topRow->addWidget(m_modeCombo);
    topRow->addStretch();
    topRow->addWidget(new QLabel(QStringLiteral("页码:")));
    topRow->addWidget(m_pageLabel);

    // 字符段（导出列集）面板：查看 / 编辑 / 恢复默认。
    m_charPanel = new CharSegmentPanel();
    connect(m_charPanel, &CharSegmentPanel::statusMessage, this, &MainWindow::log);
    auto* charBox = new QGroupBox(QStringLiteral("字符段（导出列集）"));
    auto* charLay = new QVBoxLayout(charBox);
    charLay->addWidget(m_charPanel);
    charBox->setMaximumHeight(230);

    auto* leftPane = new QWidget();
    auto* leftLay = new QVBoxLayout(leftPane);
    leftLay->setContentsMargins(4, 4, 4, 4);
    leftLay->addLayout(topRow);
    m_recogLabel = new QLabel(QStringLiteral("框选识别：在图上拖拽框选，松手前实时显示范围内文字"));
    m_recogLabel->setWordWrap(true);
    m_recogLabel->setStyleSheet(QStringLiteral("QLabel{background:#fff7e0;border:1px solid #e0c060;padding:3px 6px;border-radius:4px;color:#5a4500;}"));
    leftLay->addWidget(m_recogLabel);

    // OCR enhancement toggle: when on, the kernel runs OCR alongside the vector
    // layer and tags each word's provenance (green = vector, orange = OCR fill).
    m_ocrAlwaysCb = new QCheckBox(QStringLiteral("OCR 增强：始终用 OCR 补位图像区（矢量优先）"));
    m_ocrAlwaysCb->setToolTip(QStringLiteral("勾选后，内核在矢量文本之外额外运行 OCR，自动补回被栅格化为图像的文字（橙色标注）；矢量文字（绿色）始终优先。"));
    connect(m_ocrAlwaysCb, &QCheckBox::toggled, this, &MainWindow::onOcrAlwaysToggled);
    auto* ocrLegend = new QLabel(QStringLiteral("图例：<span style='color:#149650;'>■ 矢量文字</span>　<span style='color:#e67800;'>■ OCR 补位</span>"));
    ocrLegend->setStyleSheet(QStringLiteral("QLabel{font-size:11px;color:#555;}"));
    auto* ocrBox = new QGroupBox(QStringLiteral("OCR 增强"));
    auto* ocrLay = new QVBoxLayout(ocrBox);
    ocrLay->addWidget(m_ocrAlwaysCb);
    ocrLay->addWidget(ocrLegend);
    leftLay->addWidget(ocrBox);

    leftLay->addWidget(scroll, 1);
    leftLay->addWidget(charBox);

    m_tabs = new QTabWidget();
    m_table = new QTableWidget();
    m_pipes = new QListWidget();
    m_jsonView = new QTextEdit();
    m_jsonView->setReadOnly(true);
    m_jsonView->setLineWrapMode(QTextEdit::NoWrap);
    m_logView = new QTextEdit();
    m_logView->setReadOnly(true);
    m_tabs->addTab(m_table,  QStringLiteral("表格结果"));
    m_tabs->addTab(m_pipes,  QStringLiteral("管线号"));
    m_tabs->addTab(m_jsonView, QStringLiteral("区域 JSON"));
    m_tabs->addTab(m_logView,  QStringLiteral("内核日志"));

    m_cliEdit = new QLineEdit(m_cliDir);
    m_cliEdit->setPlaceholderText(QStringLiteral("CLI 内核目录（含 EGpdf2excel.exe / EGpdf2df.exe）"));
    auto* cliRow = new QHBoxLayout();
    cliRow->addWidget(new QLabel(QStringLiteral("CLI 目录:")));
    cliRow->addWidget(m_cliEdit, 1);
    auto* cliBrowse = new QPushButton(QStringLiteral("…"));
    cliRow->addWidget(cliBrowse);
    connect(cliBrowse, &QPushButton::clicked, this, [this]{
        QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("选择内核目录"), m_cliDir);
        if (!d.isEmpty()) { setCliDir(d); }
    });
    connect(m_cliEdit, &QLineEdit::editingFinished, this, [this]{ setCliDir(m_cliEdit->text().trimmed()); });

    auto* rightPane = new QWidget();
    auto* rightLay = new QVBoxLayout(rightPane);
    rightLay->setContentsMargins(4, 4, 4, 4);
    rightLay->addLayout(cliRow);
    rightLay->addWidget(m_tabs, 1);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(leftPane);
    split->addWidget(rightPane);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    // ---- Multi-level sidebar navigation: every sub-function is its own entry ----
    m_nav = new QTreeWidget();
    m_nav->setFixedWidth(172);
    m_nav->setHeaderHidden(true);
    m_nav->setStyleSheet(QStringLiteral("QTreeWidget{font-size:13px;} QTreeWidget::item{padding:7px 6px;} QTreeWidget::item:selected{background:#2d7d46;color:#fff;} QTreeWidget::item:has-children{font-weight:bold;}"));
    auto mkNav = [&](const QString& label, int page, int ocrMode = 0) {
        auto* it = new QTreeWidgetItem(QStringList(label));
        it->setData(0, Qt::UserRole, page);
        it->setData(0, Qt::UserRole + 1, ocrMode);
        return it;
    };
    auto* itHome = mkNav(QStringLiteral("首页"), PageHome);
    auto* itPdf  = mkNav(QStringLiteral("PDF 工具"), PagePdf);
    itPdf->addChild(mkNav(QStringLiteral("渲染与框选提取"), PagePdf, 0));
    itPdf->addChild(mkNav(QStringLiteral("材料表 OCR 增强"), PagePdf, 1));
    auto* itDiff = mkNav(QStringLiteral("图纸比对"), PageDiff);
    auto* itExcel = mkNav(QStringLiteral("Excel 比对"), PageExcel);
    auto* itSchemes = mkNav(QStringLiteral("框选方案"), PageScheme);
    m_nav->addTopLevelItem(itHome);
    m_nav->addTopLevelItem(itPdf);
    m_nav->addTopLevelItem(itDiff);
    m_nav->addTopLevelItem(itExcel);
    m_nav->addTopLevelItem(itSchemes);
    itPdf->setExpanded(true);
    m_nav->setCurrentItem(itHome);
    connect(m_nav, &QTreeWidget::itemClicked, this, &MainWindow::onNavItemClicked);

    // Pages: 0 home, 1 PDF→Excel workspace, 2 PDF diff, 3 Excel compare, 4 schemes.
    m_home = new HomePage();
    m_schemeStore = new SchemeStore();
    m_schemes = new SchemePage(m_schemeStore);
    m_diffPage = new DiffPage();
    m_excelPage = new ExcelComparePage();
    connect(m_diffPage,  &DiffPage::statusMessage,  this, &MainWindow::log);
    connect(m_excelPage, &ExcelComparePage::statusMessage, this, &MainWindow::log);

    m_stack = new QStackedWidget();
    m_stack->addWidget(m_home);     // PageHome
    m_stack->addWidget(split);      // PagePdf  (the PDF→Excel workspace)
    m_stack->addWidget(m_diffPage); // PageDiff
    m_stack->addWidget(m_excelPage);// PageExcel
    m_stack->addWidget(m_schemes);  // PageScheme

    auto* navSplit = new QSplitter(Qt::Horizontal);
    navSplit->addWidget(m_nav);
    navSplit->addWidget(m_stack);
    navSplit->setStretchFactor(0, 0);
    navSplit->setStretchFactor(1, 1);
    setCentralWidget(navSplit);

    connect(m_schemes, &SchemePage::rewriteRequested, this, &MainWindow::onRewriteRegion);
    connect(m_schemes, &SchemePage::reuseRequested,   this, &MainWindow::onReuseScheme);
    connect(m_schemes, &SchemePage::statusMessage,    this, &MainWindow::onSchemeStatus);

    connect(m_home, &HomePage::startExtract, this, [this]{
        navTo(PagePdf); onOpen();
    });
    connect(m_home, &HomePage::openPdfRequested,     this, [this]{ navTo(PagePdf); onOpen(); });
    connect(m_home, &HomePage::wholePageExtract,     this, [this]{ navTo(PagePdf); onWholePageExtract(); });
    connect(m_home, &HomePage::diffRequested,        this, [this]{ navTo(PageDiff); });
    connect(m_home, &HomePage::excelCompareRequested,this, [this]{ navTo(PageExcel); });
    connect(m_home, &HomePage::schemesRequested,     this, [this]{ navTo(PageScheme); });
    connect(m_home, &HomePage::manualRequested,      this, &MainWindow::onManual);
    connect(m_home, &HomePage::aboutRequested,       this, &MainWindow::onAbout);

    statusBar()->showMessage(QStringLiteral("就绪 — 从首页开始，或左侧选择功能"));
    refreshKernelStatus();
}

void MainWindow::navTo(int index)
{
    if (!m_stack) return;
    m_stack->setCurrentIndex(index);
    if (index == PageHome) refreshKernelStatus();
}

void MainWindow::showPage(int index)
{
    if (m_nav) {
        // Pick the navigation entry whose page matches (prefer the plain PDF
        // entry over the OCR variant for PagePdf).
        QTreeWidgetItem* pick = nullptr;
        for (int i = 0; i < m_nav->topLevelItemCount() && !pick; ++i) {
            QTreeWidgetItem* top = m_nav->topLevelItem(i);
            if (top->data(0, Qt::UserRole).toInt() == index && top->childCount() == 0) { pick = top; }
            for (int j = 0; j < top->childCount(); ++j)
                if (top->child(j)->data(0, Qt::UserRole).toInt() == index) { pick = top->child(j); break; }
            if (pick) break;
        }
        if (pick) m_nav->setCurrentItem(pick);
    }
    if (m_stack) m_stack->setCurrentIndex(index);
}

void MainWindow::onNavItemClicked(QTreeWidgetItem* item, int /*col*/)
{
    if (!item) return;
    int page = item->data(0, Qt::UserRole).toInt();
    int ocrMode = item->data(0, Qt::UserRole + 1).toInt();
    navTo(page);
    if (m_ocrAlwaysCb) m_ocrAlwaysCb->setChecked(ocrMode == 1);
}

void MainWindow::onOcrAlwaysToggled(bool on)
{
    log(on ? QStringLiteral("OCR 增强已开启：内核将矢量优先，并对图像区用 OCR 补位")
           : QStringLiteral("OCR 增强已关闭：仅使用矢量文本"));
    refreshJsonPreview();
}

QStringList MainWindow::materialHeaders()
{
    return { QStringLiteral("pipe no"), QStringLiteral("TABLE"), QStringLiteral("NO"), QStringLiteral("type"),
             QStringLiteral("DN(mm)"), QStringLiteral("项目/DESCRIPTION"), QStringLiteral("QTY"),
             QStringLiteral("WEIGHT(kg)"), QStringLiteral("PAGE") };
}

void MainWindow::refreshKernelStatus()
{
    if (!m_home) return;
    QStringList found, missing;
    for (const QString& n : {QStringLiteral("EGpdf2excel"), QStringLiteral("EGpdf2df"), QStringLiteral("EGexcel2df")})
        (QFileInfo::exists(cliExe(n)) ? found : missing).append(n);
    if (missing.isEmpty()) {
        m_home->setKernelStatus(true,
            QStringLiteral("内核状态：已就绪（%1）").arg(found.join(QStringLiteral(" / "))));
    } else {
        m_home->setKernelStatus(false,
            QStringLiteral("内核状态：缺少 %1 — 请在下方「CLI 目录」指定内核目录").arg(missing.join(QStringLiteral(" / "))));
    }
    if (m_diffPage)  m_diffPage->setCliDir(m_cliDir);
    if (m_excelPage) m_excelPage->setCliDir(m_cliDir);
    if (m_charPanel) m_charPanel->setCliDir(m_cliDir);
}

void MainWindow::setCliDir(const QString& d)
{
    m_cliDir = d;
    if (m_cliEdit) m_cliEdit->setText(d);
    refreshKernelStatus();
    log(QStringLiteral("CLI 目录 = %1").arg(d.isEmpty() ? QStringLiteral("<自动探测>") : d));
}

void MainWindow::log(const QString& s)
{
    if (m_logView) m_logView->append(s);
    if (statusBar()) statusBar()->showMessage(s);
}

QString MainWindow::cliExe(const QString& name) const
{
    const QString exe = name + QStringLiteral(".exe");
    QStringList candidates;
    auto add = [&](const QString& dir){ if (!dir.isEmpty()) candidates << QDir(dir).filePath(exe); };

    // 1) Explicit override wins (UI field / --cli-dir / EGTOOLS_CLI_DIR).
    add(m_cliDir);
    // 2) Auto-discover relative to the running executable so an installed /
    //    published layout "just works" without configuring anything.
    const QString appDir = QCoreApplication::applicationDirPath();
    add(appDir);                              // kernels flat beside the exe
    add(QDir(appDir).filePath(name));         // <app>/<name>/<name>.exe  (DIST layout)
    add(QDir(appDir).filePath(QStringLiteral("cli")));        // {app}/cli/<name>.exe
    add(QDir(appDir).filePath(QStringLiteral("../cli")));     // dev: ../cli
    add(QDir(appDir).filePath(QStringLiteral("..")));         // kernels flat in parent
    add(QDir(appDir).filePath(QStringLiteral("../") + name)); // {app}/<name>/<name>.exe (siblings)

    for (const QString& c : candidates)
        if (QFileInfo::exists(c)) return c;
    return candidates.isEmpty() ? exe : candidates.first();
}

// ---------------------------------------------------------------- open / render

bool MainWindow::openPdf(const QString& path)
{
    m_pdfPath = path;
    m_pageIndex = 0;
    m_view->clearRegions();

    // Page count: render page 0 to learn geometry, then probe via kernel render
    // loop is overkill; instead ask the kernel by rendering page 0 and reading
    // its summary. For page count we probe upward until the kernel errors.
    QImage img; double scale = 1, pw = 0, ph = 0;
    if (!renderPage(0, img, scale, pw, ph)) {
        log(QStringLiteral("打开失败：无法渲染第 1 页"));
        return false;
    }

    // Discover page count with a single cheap kernel call (--page-count) instead
    // of probing each page. Robust and fast even for hundreds-of-page documents.
    QString pcOut, pcErr;
    int rcPc = CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
        {QStringLiteral("--page-count"), m_pdfPath}, pcOut, pcErr);
    m_pageCount = 1;
    if (rcPc == 0) {
        QJsonParseError pe{};
        QJsonDocument doc = QJsonDocument::fromJson(pcOut.toUtf8(), &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject())
            m_pageCount = qMax(1, doc.object().value(QStringLiteral("pageCount")).toInt());
    } else {
        log(QStringLiteral("页码探测失败，默认按 1 页处理: %1").arg(pcErr.trimmed()));
    }

    m_view->setPage(img, scale, 0);
    m_view->setPageIndex(0);
    m_pwPt = pw; m_phPt = ph; m_scale = scale;
    loadPageWords(0);
    m_pageLabel->setText(QStringLiteral("%1 / %2").arg(1).arg(m_pageCount));
    log(QStringLiteral("已打开 %1（%2 页，%3 x %4 pt）")
            .arg(QFileInfo(path).fileName()).arg(m_pageCount).arg(pw).arg(ph));
    return true;
}

bool MainWindow::renderPage(int page, QImage& img, double& scale, double& pwPt, double& phPt)
{
    if (m_pdfPath.isEmpty()) { log(QStringLiteral("未打开 PDF")); return false; }
    QTemporaryDir tmp;
    QString bmp = QDir(tmp.path()).filePath(QStringLiteral("page.bmp"));
    QString out, err;
    int rc = CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
        {QStringLiteral("--render"), QStringLiteral("--page"), QString::number(page),
         QStringLiteral("--scale"), QStringLiteral("1"),
         QStringLiteral("--render-out"), bmp, m_pdfPath}, out, err);
    if (rc != 0) { log(QStringLiteral("渲染第 %1 页失败: %2").arg(page).arg(err.trimmed())); return false; }

    QImageReader reader(bmp);
    img = reader.read();
    if (img.isNull()) { log(QStringLiteral("读取渲染图失败: %1").arg(reader.errorString())); return false; }

    // Kernel prints {page,pageWidthPt,pageHeightPt,imageWidthPx,imageHeightPx,scale}
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8(), &pe);
    if (pe.error == QJsonParseError::NoError && doc.isObject()) {
        QJsonObject o = doc.object();
        pwPt = o.value(QStringLiteral("pageWidthPt")).toDouble();
        phPt = o.value(QStringLiteral("pageHeightPt")).toDouble();
        scale = o.value(QStringLiteral("scale")).toDouble(1.0);
    } else {
        // Fall back to a 1:1 assumption (still derived from the image, never hard-coded to a drawing).
        scale = 1.0; pwPt = img.width(); phPt = img.height();
    }
    return true;
}

void MainWindow::onOpen()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择 PDF"), QString(),
                                             QStringLiteral("PDF (*.pdf)"));
    if (!p.isEmpty()) { showPage(1); openPdf(p); }
}

void MainWindow::onHome()
{
    navTo(PageHome);
}

// ---- Full-page material table extraction (drives EGpdf2excel directly) ----
void MainWindow::onWholePageExtract()
{
    QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("选择 PDF（可多选）"),
        QString(), QStringLiteral("PDF (*.pdf)"));
    if (files.isEmpty()) return;
    QString outDir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出目录"));
    if (outDir.isEmpty()) return;

    showPage(1);
    QString out, err;
    QStringList args{ files.first(), QStringLiteral("-o"), outDir, QStringLiteral("-f"),
                      QStringLiteral("both") };
    for (int i = 1; i < files.size(); ++i) args << files[i];
    int rc = CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")), args, out, err);
    log(QStringLiteral("[整页抽取] rc=%1").arg(rc));
    if (!err.trimmed().isEmpty()) log(err.trimmed());
    log(rc == 0 ? QStringLiteral("整页材料表抽取完成 → %1").arg(outDir)
                : QStringLiteral("整页材料表抽取失败，退出码 %1").arg(rc));
    if (rc == 0) {
        m_tabs->setCurrentWidget(m_logView);
        QMessageBox::information(this, QStringLiteral("整页抽取"),
            QStringLiteral("已在内核中完成整页材料表抽取，输出目录：\n%1\n\n详细日志见「内核日志」页。").arg(outDir));
    }
}

// ---- Excel drawing comparison (drives EGexcel2df directly) ----
void MainWindow::onRenderPage()
{
    if (m_pdfPath.isEmpty()) return;
    QImage img; double scale = 1, pw = 0, ph = 0;
    if (renderPage(m_pageIndex, img, scale, pw, ph)) {
        m_view->setPage(img, scale, m_pageIndex);
        m_scale = scale; m_pwPt = pw; m_phPt = ph;
        loadPageWords(m_pageIndex);
        m_pageLabel->setText(QStringLiteral("%1 / %2").arg(m_pageIndex + 1).arg(m_pageCount));
        log(QStringLiteral("渲染第 %1 页 (%2 x %3 px)").arg(m_pageIndex + 1).arg(img.width()).arg(img.height()));
    }
}

void MainWindow::onPrevPage() { if (m_pageIndex > 0) { --m_pageIndex; onRenderPage(); } }
void MainWindow::onNextPage() { if (m_pageIndex + 1 < m_pageCount) { ++m_pageIndex; onRenderPage(); } }

void MainWindow::onModeChanged(int)
{
    m_view->setKind(m_modeCombo->currentData().toString());
}

void MainWindow::onRegionSelected(int page, const QString& kind, const QRectF& rectPt)
{
    // If a scheme row is being re-drawn, this box REPLACES that row instead of
    // adding a new region, then we leave rewrite mode.
    if (!m_rewriteSchemeId.isEmpty() && m_schemeStore) {
        Scheme* s = m_schemeStore->findMutable(m_rewriteSchemeId);
        if (s && m_rewriteRow >= 0 && m_rewriteRow < s->regions.size()) {
            RegionSpec& g = s->regions[m_rewriteRow];
            g.page = page;
            g.kind = kind;
            g.x = rectPt.x(); g.y = rectPt.y();
            g.w = rectPt.width(); g.h = rectPt.height();
            m_schemeStore->update(*s);
            m_schemeStore->save();
            log(QStringLiteral("已重写方案「%1」的第 %2 个区域并保存").arg(s->name).arg(m_rewriteRow + 1));
            m_rewriteSchemeId.clear();
            m_rewriteRow = -1;
            if (m_schemes) { m_schemes->reload(); m_schemes->selectScheme(s->id); }
            showPage(PageScheme);
            return;
        }
    }

    Region r; r.page = page; r.kind = kind; r.rectPt = rectPt;
    m_view->addRegion(r);
    refreshJsonPreview();
    log(QStringLiteral("框选: 页%1 %2 [x=%3 y=%4 w=%5 h=%6]")
            .arg(page + 1).arg(kind).arg(rectPt.x(), 0, 'f', 1).arg(rectPt.y(), 0, 'f', 1)
            .arg(rectPt.width(), 0, 'f', 1).arg(rectPt.height(), 0, 'f', 1));
}

void MainWindow::onClearRegions()
{
    m_view->clearRegions();
    m_table->clear(); m_table->setRowCount(0); m_table->setColumnCount(0);
    m_pipes->clear();
    m_jsonView->clear();
    log(QStringLiteral("已清空框选"));
}

// Live in-canvas text recognition readout: while the user drags a rubber band
// this shows the vector text inside the box (no raster OCR — the words are the
// kernel's own structured-text output, re-projected through the same scale).
void MainWindow::onTextPreview(int /*page*/, const QString& text)
{
    if (!m_recogLabel) return;
    if (text.isEmpty())
        m_recogLabel->setText(QStringLiteral("框选识别：在图上拖拽框选，松手前实时显示范围内文字"));
    else
        m_recogLabel->setText(QStringLiteral("识别到文字（%1 词）：%2")
            .arg(text.split(QStringLiteral(" "), Qt::SkipEmptyParts).size()).arg(text));
}

// Load every vector word of the just-rendered page so the rubber band can
// recognise text live. Runs the kernel --words mode and re-projects the boxes
// into the canvas pt coordinate space.
void MainWindow::loadPageWords(int page)
{
    if (m_pdfPath.isEmpty() || !m_view) return;
    QString out, err;
    int rc = CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
        {QStringLiteral("--words"), QString::number(page), m_pdfPath}, out, err);
    if (rc != 0) {
        log(QStringLiteral("[words] 提取页文字失败 rc=%1").arg(rc));
        return;
    }
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        log(QStringLiteral("[words] 解析失败: %1").arg(pe.errorString()));
        return;
    }
    QVector<WordBox> words;
    for (const QJsonValue& wv : doc.object().value(QStringLiteral("words")).toArray()) {
        QJsonObject w = wv.toObject();
        WordBox wb;
        wb.rectPt = QRectF(w.value(QStringLiteral("x0")).toDouble(), w.value(QStringLiteral("y0")).toDouble(),
                           w.value(QStringLiteral("x1")).toDouble() - w.value(QStringLiteral("x0")).toDouble(),
                           w.value(QStringLiteral("y1")).toDouble() - w.value(QStringLiteral("y0")).toDouble());
        wb.text = w.value(QStringLiteral("text")).toString();
        words.append(wb);
    }
    m_view->setPageWords(words);
    log(QStringLiteral("已加载本页矢量文字 %1 词（实时识别就绪）").arg(words.size()));
}

// ---------------------------------------------------------------- JSON contract

QJsonObject MainWindow::buildRequest() const
{
    QJsonArray regions;
    for (const Region& r : m_view->regions()) {
        QJsonObject bbox;
        bbox.insert(QStringLiteral("x"),      r.rectPt.x());
        bbox.insert(QStringLiteral("y"),      r.rectPt.y());
        bbox.insert(QStringLiteral("width"),  r.rectPt.width());
        bbox.insert(QStringLiteral("height"), r.rectPt.height());
        QJsonObject ro;
        ro.insert(QStringLiteral("page"), r.page);
        ro.insert(QStringLiteral("kind"), r.kind);
        ro.insert(QStringLiteral("bbox"), bbox);
        regions.append(ro);
    }
    QJsonObject req;
    req.insert(QStringLiteral("pdf"),  m_pdfPath);
    if (!m_pdfPath.isEmpty()) {
        QJsonArray pages;
        // Only the pages that actually carry a selection.
        QSet<int> used; for (const Region& r : m_view->regions()) used.insert(r.page);
        QList<int> sorted(used.begin(), used.end()); std::sort(sorted.begin(), sorted.end());
        for (int p : sorted) pages.append(p);
        req.insert(QStringLiteral("pages"), pages);
    }
    req.insert(QStringLiteral("regions"), regions);
    QJsonObject opts;
    opts.insert(QStringLiteral("ocrFallback"), true);
    opts.insert(QStringLiteral("mergeOverlapping"), true);
    if (m_ocrAlwaysCb && m_ocrAlwaysCb->isChecked())
        opts.insert(QStringLiteral("ocrAlways"), true);
    req.insert(QStringLiteral("options"), opts);
    return req;
}

void MainWindow::refreshJsonPreview()
{
    QJsonDocument d(buildRequest());
    m_jsonView->setPlainText(QString::fromUtf8(d.toJson(QJsonDocument::Indented)));
}

void MainWindow::onExportJson()
{
    if (m_view->regions().isEmpty()) { log(QStringLiteral("没有框选可导出")); return; }
    QString p = QFileDialog::getSaveFileName(this, QStringLiteral("导出区域 JSON"),
                                             QStringLiteral("regions.json"),
                                             QStringLiteral("JSON (*.json)"));
    if (p.isEmpty()) return;
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { log(QStringLiteral("写入失败")); return; }
    f.write(QJsonDocument(buildRequest()).toJson(QJsonDocument::Indented));
    f.close();
    log(QStringLiteral("区域 JSON 已导出: %1").arg(p));
}

// ---------------------------------------------------------------- kernel calls

void MainWindow::onExtract()
{
    if (m_view->regions().isEmpty()) { log(QStringLiteral("请先框选区域")); return; }
    refreshJsonPreview();

    QTemporaryDir tmp;
    QString reqPath = QDir(tmp.path()).filePath(QStringLiteral("request.json"));
    QFile f(reqPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { log(QStringLiteral("无法写入临时请求")); return; }
    f.write(QJsonDocument(buildRequest()).toJson(QJsonDocument::Compact));
    f.close();

    QString out, err;
    int rc = CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
        {QStringLiteral("--mode"), QStringLiteral("region"),
         QStringLiteral("--json"), reqPath}, out, err);
    if (!err.trimmed().isEmpty()) log(QStringLiteral("[内核 stderr] ") + err.trimmed());
    if (rc != 0) { log(QStringLiteral("提取失败，退出码 %1").arg(rc)); return; }

    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        log(QStringLiteral("解析内核 JSON 失败: %1").arg(pe.errorString()));
        m_jsonView->setPlainText(out);
        return;
    }
    showRegionResponse(doc.object());
}

void MainWindow::showRegionResponse(const QJsonObject& resp)
{
    // Raw JSON to the JSON tab.
    m_jsonView->setPlainText(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));

    m_table->clear(); m_table->setRowCount(0); m_table->setColumnCount(0);
    m_pipes->clear();
    QVector<WordBox> overlay;

    QJsonArray regions = resp.value(QStringLiteral("regions")).toArray();
    // Build overlay of all word segments (in the current page's coordinate space).
    for (const QJsonValue& rv : regions) {
        QJsonObject r = rv.toObject();
        int page = r.value(QStringLiteral("page")).toInt();
        if (page != m_pageIndex) continue;
        if (r.value(QStringLiteral("ocrUsed")).toBool())
            log(QStringLiteral("注意: 该区域使用了 OCR 兜底"));

        QJsonArray words = r.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wv : words) {
            QJsonObject w = wv.toObject();
            WordBox wb;
            wb.rectPt = QRectF(w.value(QStringLiteral("x0")).toDouble(), w.value(QStringLiteral("y0")).toDouble(),
                               w.value(QStringLiteral("x1")).toDouble() - w.value(QStringLiteral("x0")).toDouble(),
                               w.value(QStringLiteral("y1")).toDouble() - w.value(QStringLiteral("y0")).toDouble());
            wb.text = w.value(QStringLiteral("text")).toString();
            wb.source = w.value(QStringLiteral("source")).toString();
            overlay.push_back(wb);
        }

        // Pipe numbers -> pipe tab.
        for (const QJsonValue& pv : r.value(QStringLiteral("pipeNumbers")).toArray())
            m_pipes->addItem(pv.toString());

        // First table region -> table tab (row/col sized dynamically).
        QJsonArray table = r.value(QStringLiteral("table")).toArray();
        if (!table.isEmpty() && m_table->rowCount() == 0) {
            int rows = table.size(), cols = 0;
            for (const QJsonValue& row : table) cols = qMax(cols, row.toArray().size());
            m_table->setRowCount(rows); m_table->setColumnCount(cols);
            for (int i = 0; i < rows; ++i) {
                QJsonArray row = table[i].toArray();
                for (int j = 0; j < row.size(); ++j)
                    m_table->setItem(i, j, new QTableWidgetItem(row[j].toString()));
            }
            // Label the columns with the material-table headers (config-driven);
            // the 7th (QTY) and 8th (WEIGHT(kg)) columns are preserved here as a
            // hint, and the full 9-column table is produced by the whole-page path.
            QStringList hdr = materialHeaders();
            for (int c = 0; c < cols && c < hdr.size(); ++c)
                m_table->setHorizontalHeaderItem(c, new QTableWidgetItem(hdr[c]));
            m_table->resizeColumnsToContents();
            QString present = (cols >= 7 ? QStringLiteral("；第7列 QTY / 第8列 WEIGHT(kg) 已含")
                                         : QStringLiteral("；如需完整 9 列(含 QTY/WEIGHT)请用「整页材料表抽取」"));
            log(QStringLiteral("材料表列：前 %1 列对应 %2%3").arg(cols).arg(hdr.mid(0, cols).join(QStringLiteral(" / "))).arg(present));
        }
    }
    m_view->setWordBoxes(overlay);
    log(QStringLiteral("提取完成：%1 个区域，字框叠加 %2 个").arg(regions.size()).arg(overlay.size()));
}

void MainWindow::onManual()
{
    ManualDialog dlg(this);
    dlg.exec();
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, QStringLiteral("关于 EGtools"),
        QStringLiteral("<h3>EGtools</h3>"
                       "<p><b>工程图纸材料表提取与对比工具集</b></p>"
                       "<p>Qt 6 图形前端 + .NET 命令行内核（三层架构）。<br>"
                       "前端只做交互与编排，PDF / Excel 处理全部由 CLI 内核完成。</p>"
                       "<p>版本 1.0.0<br>许可证 AGPL-3.0<br>版权 © 2026 HerryABU</p>"
                       "<p>点击菜单「帮助 → 使用说明书」查看完整说明。</p>"));
}

// ---------------------------------------------------------------- schemes

void MainWindow::onSchemes()
{
    if (!m_schemes) return;
    m_schemes->reload();
    navTo(PageScheme);
}

void MainWindow::onSchemeStatus(const QString& msg) { log(msg); }

QVector<RegionSpec> MainWindow::canvasRegions() const
{
    QVector<RegionSpec> out;
    if (!m_view) return out;
    for (const Region& r : m_view->regions()) {
        RegionSpec g;
        g.page = r.page;
        g.kind = r.kind;
        g.x = r.rectPt.x();
        g.y = r.rectPt.y();
        g.w = r.rectPt.width();
        g.h = r.rectPt.height();
        out.append(g);
    }
    return out;
}

void MainWindow::onCaptureAsScheme()
{
    if (!m_schemes) return;
    const QVector<RegionSpec> regs = canvasRegions();
    if (regs.isEmpty()) {
        log(QStringLiteral("画布上还没有框选，无法存为方案"));
        return;
    }
    m_schemes->captureFromCanvas(regs);
    showPage(PageScheme);
}

// Switch to the workspace and put the scheme's boxes back on the canvas.
void MainWindow::onReuseScheme(const QString& schemeId)
{
    if (!m_schemeStore) return;
    Scheme* s = m_schemeStore->findMutable(schemeId);
    if (!s) return;
    applySchemeToCanvas(*s);
    showPage(1);
    log(QStringLiteral("已把方案「%1」的 %2 个框选复用到画布，可直接点「提取 (内核)」")
            .arg(s->name).arg(s->regions.size()));
}

void MainWindow::applySchemeToCanvas(const Scheme& s)
{
    if (!m_view) return;
    m_view->clearRegions();
    for (const RegionSpec& g : s.regions) {
        Region r;
        r.page = g.page;
        r.kind = g.kind;
        r.rectPt = QRectF(g.x, g.y, g.w, g.h);
        m_view->addRegion(r);
    }
    // Jump to the first page that carries a box so the user sees them.
    if (!s.regions.isEmpty()) {
        m_pageIndex = s.regions.first().page;
        onRenderPage();
        m_view->clearRegions();
        for (const RegionSpec& g : s.regions) {
            Region r;
            r.page = g.page;
            r.kind = g.kind;
            r.rectPt = QRectF(g.x, g.y, g.w, g.h);
            m_view->addRegion(r);
        }
    }
    refreshJsonPreview();
}

// Enter "rewrite" mode: the next box drawn on the canvas replaces the given row.
void MainWindow::onRewriteRegion(const QString& schemeId, int regionIndex)
{
    if (!m_schemeStore) return;
    Scheme* s = m_schemeStore->findMutable(schemeId);
    if (!s || regionIndex < 0 || regionIndex >= s->regions.size()) return;

    m_rewriteSchemeId = schemeId;
    m_rewriteRow = regionIndex;
    m_pageIndex = s->regions[regionIndex].page;
    showPage(1);
    onRenderPage();
    m_view->clearRegions();
    Region r;
    r.page = s->regions[regionIndex].page;
    r.kind = s->regions[regionIndex].kind;
    r.rectPt = QRectF(s->regions[regionIndex].x, s->regions[regionIndex].y,
                      s->regions[regionIndex].w, s->regions[regionIndex].h);
    m_view->addRegion(r);

    log(QStringLiteral("重写模式：在画布上拖拽以重画该区域（完成后自动保存到方案「%1」）").arg(s->name));
    statusBar()->showMessage(QStringLiteral("重写模式 — 在画布上拖拽重画区域"));
}