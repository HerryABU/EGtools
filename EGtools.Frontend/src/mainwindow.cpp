#include "mainwindow.h"
#include "clirunner.h"

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
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSplitter>
#include <QStatusBar>
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
    connect(prevA,    &QAction::triggered, this, &MainWindow::onPrevPage);
    connect(nextA,    &QAction::triggered, this, &MainWindow::onNextPage);
    connect(renderA,  &QAction::triggered, this, &MainWindow::onRenderPage);
    connect(clearA,   &QAction::triggered, this, &MainWindow::onClearRegions);
    connect(exportA,  &QAction::triggered, this, &MainWindow::onExportJson);
    connect(extractA, &QAction::triggered, this, &MainWindow::onExtract);
    connect(diffA,    &QAction::triggered, this, &MainWindow::onDiff);

    // Center: view (left) + results (right).
    m_view = new PdfView();
    m_view->setKind(QStringLiteral("detail_table"));
    connect(m_view, &PdfView::regionSelected, this, &MainWindow::onRegionSelected);

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

    auto* leftPane = new QWidget();
    auto* leftLay = new QVBoxLayout(leftPane);
    leftLay->setContentsMargins(4, 4, 4, 4);
    leftLay->addLayout(topRow);
    leftLay->addWidget(scroll, 1);

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
    setCentralWidget(split);

    statusBar()->showMessage(QStringLiteral("就绪 — 打开一个 PDF 开始"));
}

void MainWindow::setCliDir(const QString& d)
{
    m_cliDir = d;
    if (m_cliEdit) m_cliEdit->setText(d);
    log(QStringLiteral("CLI 目录 = %1").arg(d.isEmpty() ? QStringLiteral("<空>") : d));
}

void MainWindow::log(const QString& s)
{
    if (m_logView) m_logView->append(s);
    if (statusBar()) statusBar()->showMessage(s);
}

QString MainWindow::cliExe(const QString& name) const
{
#ifdef EGTOOLS_WIN32
    return QDir(m_cliDir).filePath(name + QStringLiteral(".exe"));
#else
    return QDir(m_cliDir).filePath(name);
#endif
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

    // Discover page count by probing (render page N until failure). Cheap for
    // typical drawings; avoids adding a separate "page count" kernel mode.
    m_pageCount = 1;
    for (int probe = 1; probe < 2000; ++probe) {
        QString out, err;
        QTemporaryDir tmp;
        QString bmp = QDir(tmp.path()).filePath(QStringLiteral("p.bmp"));
        int rc = CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
            {QStringLiteral("--render"), QStringLiteral("--page"), QString::number(probe),
             QStringLiteral("--scale"), QStringLiteral("0.05"),
             QStringLiteral("--render-out"), bmp, m_pdfPath}, out, err);
        if (rc != 0) break;
        m_pageCount = probe + 1;
    }

    m_view->setPage(img, scale, 0);
    m_view->setPageIndex(0);
    m_pwPt = pw; m_phPt = ph; m_scale = scale;
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
    if (!p.isEmpty()) openPdf(p);
}

void MainWindow::onRenderPage()
{
    if (m_pdfPath.isEmpty()) return;
    QImage img; double scale = 1, pw = 0, ph = 0;
    if (renderPage(m_pageIndex, img, scale, pw, ph)) {
        m_view->setPage(img, scale, m_pageIndex);
        m_scale = scale; m_pwPt = pw; m_phPt = ph;
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
            m_table->resizeColumnsToContents();
        }
    }
    m_view->setWordBoxes(overlay);
    log(QStringLiteral("提取完成：%1 个区域，字框叠加 %2 个").arg(regions.size()).arg(overlay.size()));
}

void MainWindow::onDiff()
{
    QString a = QFileDialog::getOpenFileName(this, QStringLiteral("基准图 (baseline)"), QString(), QStringLiteral("PDF (*.pdf)"));
    if (a.isEmpty()) return;
    QString b = QFileDialog::getOpenFileName(this, QStringLiteral("目标图 (target)"), QString(), QStringLiteral("PDF (*.pdf)"));
    if (b.isEmpty()) return;

    QTemporaryDir tmp;
    QString outPdf = QDir(tmp.path()).filePath(QStringLiteral("diff.pdf"));
    QString out, err;
    int rc = CliRunner::run(cliExe(QStringLiteral("EGpdf2df")),
        {a, b, QStringLiteral("-o"), outPdf}, out, err);
    if (!err.trimmed().isEmpty()) log(QStringLiteral("[EGpdf2df] ") + err.trimmed());
    if (rc != 0) { log(QStringLiteral("图纸差异失败，退出码 %1").arg(rc)); return; }

    m_jsonView->setPlainText(out);
    m_tabs->setCurrentWidget(m_jsonView);
    // Render the produced diff PDF into the view for a quick look.
    QTemporaryDir tmp2;
    QString bmp = QDir(tmp2.path()).filePath(QStringLiteral("diff.bmp"));
    QString out2, err2;
    if (CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
            {QStringLiteral("--render"), QStringLiteral("--page"), QStringLiteral("0"),
             QStringLiteral("--scale"), QStringLiteral("1"), QStringLiteral("--render-out"), bmp, outPdf},
            out2, err2) == 0) {
        QImage img = QImageReader(bmp).read();
        if (!img.isNull()) {
            m_view->setPage(img, 1.0, 0);
            log(QStringLiteral("已显示差异图（红框标注）"));
            return;
        }
    }
    log(QStringLiteral("差异 PDF 已生成: %1（未能内嵌预览）").arg(outPdf));
}