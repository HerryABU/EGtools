#include "mainwindow.h"
#include "clirunner.h"
#include "kernelclient.h"
#include "pdfcanvas.h"
#include "manualdialog.h"
#include "homepage.h"
#include "schemes.h"
#include "schemepage.h"
#include "diffpage.h"
#include "excelcomparepage.h"
#include "materialextractpage.h"
#include "pagenavpanel.h"
#include "continuouspdfview.h"
#include "theme.h"
#include "charsegmentpanel.h"

#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
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
#include <QSettings>
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
    // 内核调用方式：命令行 --backend > 环境变量 > QSettings > 默认自动。
    {
        QSettings st;
        QString mode = st.value(QStringLiteral("kernel/backend")).toString();
        const QByteArray env = qgetenv("EGTOOLS_KERNEL_BACKEND");
        if (!env.isEmpty()) mode = QString::fromLocal8Bit(env);
        KernelClient::setDefaultBackend(KernelClient::backendFromString(mode));
    }
    buildUi();
    // 预取用单线程池：只在空闲时占用一个后台线程渲染相邻页（并行/异步渲染），
    // 不把 CPU 吃满，也不与用户操作争抢内核（内核调用在融合层内部串行）。
    m_prefetchPool = std::make_shared<QThreadPool>();
    m_prefetchPool->setMaxThreadCount(1);
    log(QStringLiteral("EGtools Qt 前端已就绪（CLI 目录：%1）")
            .arg(m_cliDir.isEmpty() ? QStringLiteral("<未设置>") : m_cliDir));
    // 明确告诉用户/日志当前走的是 GPU 还是 CPU 光栅 —— 排查"画面不对"时第一条线索。
    log(QStringLiteral("渲染路径：%1").arg(gpuBackendDescription()));
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

    // 视图菜单：视图模式 + 缩放 + 主题（都是全局观感开关）。
    auto* mView = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    {
        // ---- 视图模式：单页精看 / 连续滚动（Adobe 式）----
        auto* viewGroup = new QActionGroup(this);
        viewGroup->setExclusive(true);
        auto* singleA = mView->addAction(QStringLiteral("单页视图(&S)"));
        singleA->setCheckable(true);
        singleA->setChecked(true);
        singleA->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
        auto* contA = mView->addAction(QStringLiteral("连续滚动视图(&C)"));
        contA->setCheckable(true);
        contA->setShortcut(QKeySequence(QStringLiteral("Ctrl+2")));
        viewGroup->addAction(singleA);
        viewGroup->addAction(contA);
        connect(singleA, &QAction::triggered, this, [this]{ setContinuousMode(false); });
        connect(contA,   &QAction::triggered, this, [this]{ setContinuousMode(true);  });

        mView->addSeparator();
        auto* zi = mView->addAction(QStringLiteral("放大"));
        zi->setShortcut(QKeySequence::ZoomIn);
        connect(zi, &QAction::triggered, this, [this]{ activeCanvas()->zoomIn(); });
        auto* zo = mView->addAction(QStringLiteral("缩小"));
        zo->setShortcut(QKeySequence::ZoomOut);
        connect(zo, &QAction::triggered, this, [this]{ activeCanvas()->zoomOut(); });
        auto* z1 = mView->addAction(QStringLiteral("实际大小 (100%)"));
        connect(z1, &QAction::triggered, this, [this]{ activeCanvas()->setZoom(1.0); });
        auto* zf = mView->addAction(QStringLiteral("适合宽度"));
        connect(zf, &QAction::triggered, this, [this]{
            QScrollArea* sa = activeScroll();
            if (sa) activeCanvas()->fitToWidth(sa->viewport()->width());
        });
        auto* zp = mView->addAction(QStringLiteral("适合页面"));
        connect(zp, &QAction::triggered, this, [this]{
            QScrollArea* sa = activeScroll();
            if (!sa) return;
            if (m_continuousMode) m_contView->fitToPageHeight(sa->viewport()->height());
            else                  activeCanvas()->setZoom(1.0);
        });

        mView->addSeparator();
        auto* themeGroup = new QActionGroup(this);
        themeGroup->setExclusive(true);
        const Theme::Mode cur = Theme::current();
        struct { const char* label; Theme::Mode mode; } themes[] = {
            { "浅色主题(&L)", Theme::Light },
            { "深色主题(&D)", Theme::Dark  },
        };
        for (const auto& t : themes) {
            auto* a = mView->addAction(QString::fromUtf8(t.label));
            a->setCheckable(true);
            a->setChecked(cur == t.mode);
            themeGroup->addAction(a);
            const Theme::Mode mode = t.mode;
            connect(a, &QAction::triggered, this, [this, mode]{
                Theme::setCurrent(mode);
                refreshTheme();          // 画布视口底色等 QSS 覆盖不到的地方
                log(QStringLiteral("主题已切换为%1")
                        .arg(mode == Theme::Dark ? QStringLiteral("深色") : QStringLiteral("浅色")));
            });
        }
    }

    auto* mHelp = menuBar()->addMenu(QStringLiteral("帮助(&H)"));    auto* manualA = mHelp->addAction(QStringLiteral("使用说明书(&M)"));
    manualA->setShortcut(QKeySequence::HelpContents); // F1
    connect(manualA, &QAction::triggered, this, &MainWindow::onManual);
    mHelp->addSeparator();
    auto* aboutA = mHelp->addAction(QStringLiteral("关于(&A)"));
    connect(aboutA, &QAction::triggered, this, &MainWindow::onAbout);

    // Center: view (left) + results (right).
    // 画布走工厂：探测到可用的硬件 OpenGL 就用 GPU 壳，否则用 CPU 光栅壳 —— GPU
    // 是"加分项"，探测失败绝不会变成黑屏。
    m_view = createPdfViewHost();
    activeCanvas()->setKind(QStringLiteral("detail_table"));
    connect(m_view.canvas, &PdfCanvas::regionSelected, this, &MainWindow::onRegionSelected);
    connect(m_view.canvas, &PdfCanvas::textPreview, this, &MainWindow::onTextPreview);
    connect(m_view.canvas, &PdfCanvas::zoomChanged, this, [this](double z){
        if (m_zoomLabel) m_zoomLabel->setText(QStringLiteral("缩放 %1%").arg(qRound(z * 100)));
    });

    auto* scroll = new QScrollArea();
    m_scroll = scroll;
    scroll->setWidget(m_view.widget);
    scroll->setWidgetResizable(false);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->viewport()->setStyleSheet(QStringLiteral("background:#3a3e44;"));

    // 连续滚动视图（Adobe 式）：一屏多页 + 懒渲染 + 任意页框选。
    m_contView = new ContinuousPdfView();
    m_contView->setCliDir(m_cliDir);
    m_contView->setKernelBackend(KernelClient::backendToString(KernelClient::defaultBackend()));
    connect(m_contView, &ContinuousPdfView::regionSelected, this, &MainWindow::onRegionSelected);
    connect(m_contView, &ContinuousPdfView::textPreview,    this, &MainWindow::onTextPreview);
    connect(m_contView, &ContinuousPdfView::statusMessage,  this, &MainWindow::log);
    connect(m_contView, &ContinuousPdfView::zoomChanged, this, [this](double z){
        if (m_zoomLabel) m_zoomLabel->setText(QStringLiteral("缩放 %1%").arg(qRound(z * 100)));
    });
    connect(m_contView, &ContinuousPdfView::currentPageChanged, this, [this](int page){
        if (page < 0) return;
        m_pageIndex = page;
        if (m_pageLabel) m_pageLabel->setText(QStringLiteral("%1 / %2").arg(page + 1).arg(m_pageCount));
        if (m_pageNav) m_pageNav->setCurrentPage(page);
    });

    m_contScroll = new QScrollArea();
    m_contScroll->setWidget(m_contView);
    m_contScroll->setWidgetResizable(false);
    m_contScroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_contScroll->viewport()->setStyleSheet(QStringLiteral("background:#16191d;"));
    m_contView->attachScrollArea(m_contScroll);

    // 两种视图叠在同一个位置，靠 QStackedWidget 切换 —— 其余界面（缩放/框选/结果面板）
    // 完全共用，所以"换视图"不影响任何既有功能。
    m_viewStack = new QStackedWidget();
    m_viewStack->addWidget(scroll);        // 0 = 单页精看
    m_viewStack->addWidget(m_contScroll);  // 1 = 连续滚动

    m_modeCombo = new QComboBox();
    m_modeCombo->addItem(QStringLiteral("全域表 (global_table)"), QStringLiteral("global_table"));
    m_modeCombo->addItem(QStringLiteral("细表 (detail_table)"),     QStringLiteral("detail_table"));
    m_modeCombo->addItem(QStringLiteral("材料编号区 (material_no_region)"), QStringLiteral("material_no_region"));
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onModeChanged);

    m_pageLabel = new QLabel(QStringLiteral("-"));
    m_zoomLabel = new QLabel(QStringLiteral("缩放 100%"));
    auto* zoomOutBtn = new QPushButton(QStringLiteral("−"));
    auto* zoomInBtn  = new QPushButton(QStringLiteral("+"));
    auto* zoomFitBtn = new QPushButton(QStringLiteral("适合宽度"));
    auto* zoom1Btn   = new QPushButton(QStringLiteral("100%"));
    zoomOutBtn->setMaximumWidth(28);
    zoomInBtn->setMaximumWidth(28);
    zoomOutBtn->setToolTip(QStringLiteral("缩小（也可 Ctrl+滚轮）"));
    zoomInBtn->setToolTip(QStringLiteral("放大（也可 Ctrl+滚轮）"));
    connect(zoomOutBtn, &QPushButton::clicked, this, [this]{ activeCanvas()->zoomOut(); });
    connect(zoomInBtn,  &QPushButton::clicked, this, [this]{ activeCanvas()->zoomIn(); });
    connect(zoom1Btn,   &QPushButton::clicked, this, [this]{ activeCanvas()->setZoom(1.0); });
    connect(zoomFitBtn, &QPushButton::clicked, this, [this]{
        QScrollArea* sa = activeScroll();
        if (sa) activeCanvas()->fitToWidth(sa->viewport()->width());
    });
    // 连续视图里再给一个"适合页面"（整页可见），这是浏览图纸集最常用的档位。
    auto* zoomPageBtn = new QPushButton(QStringLiteral("适合页面"));
    connect(zoomPageBtn, &QPushButton::clicked, this, [this]{
        if (!m_continuousMode) { activeCanvas()->setZoom(1.0); return; }
        QScrollArea* sa = activeScroll();
        if (sa) m_contView->fitToPageHeight(sa->viewport()->height());
    });
    // ---------------- 命令条（固定高度，控件不拉伸） ----------------
    // 分组：[框选模式] | [缩放] | [页码]。所有按钮 Fixed 尺寸、下拉 Preferred，
    // 中间放 stretch —— 这样无论窗口怎么拉，控件都不会被拉变形或挤成一团。
    auto mkSep = []{
        auto* f = new QFrame();
        f->setObjectName(QStringLiteral("cmdSep"));   // 颜色交给主题 QSS
        f->setFrameShape(QFrame::VLine);
        f->setFixedWidth(1);
        return f;
    };
    auto mkGroupLabel = [](const QString& t){
        auto* l = new QLabel(t);
        l->setObjectName(QStringLiteral("cmdGroup")); // 颜色交给主题 QSS
        return l;
    };
    for (QPushButton* b : { zoomOutBtn, zoomInBtn, zoom1Btn, zoomFitBtn, zoomPageBtn })
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    zoomOutBtn->setFixedWidth(30);
    zoomInBtn->setFixedWidth(30);
    m_modeCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_zoomLabel->setMinimumWidth(74);
    m_pageLabel->setMinimumWidth(56);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_pageLabel->setObjectName(QStringLiteral("pageBadge"));

    auto* cmdBarWidget = new QWidget();
    cmdBarWidget->setObjectName(QStringLiteral("cmdBar"));
    auto* topRow = new QHBoxLayout(cmdBarWidget);
    topRow->setContentsMargins(6, 4, 6, 4);
    topRow->setSpacing(6);
    topRow->addWidget(mkGroupLabel(QStringLiteral("框选模式")));
    topRow->addWidget(m_modeCombo);
    topRow->addSpacing(4);
    topRow->addWidget(mkSep());
    topRow->addSpacing(4);
    topRow->addWidget(mkGroupLabel(QStringLiteral("缩放")));
    topRow->addWidget(zoomOutBtn);
    topRow->addWidget(zoomInBtn);
    topRow->addWidget(zoom1Btn);
    topRow->addWidget(zoomFitBtn);
    topRow->addWidget(zoomPageBtn);
    topRow->addWidget(m_zoomLabel);
    topRow->addStretch(1);
    topRow->addWidget(mkGroupLabel(QStringLiteral("页码")));
    topRow->addWidget(m_pageLabel);

    // 字符段（导出列集）面板：查看 / 编辑 / 恢复默认。
    m_charPanel = new CharSegmentPanel();
    connect(m_charPanel, &CharSegmentPanel::statusMessage, this, &MainWindow::log);

    // ---------------- 左侧工作区：命令条 + 识别条 + 画布 ----------------
    // 画布是主角：给 stretch=1 吃掉全部剩余高度；其余按内容定高。
    // （旧版把「字符段」「OCR」两个固定高度面板也塞在这一列，画布被压扁，
    //   窗口一拉长就"排版崩溃"——现在它们移到右侧标签页，画布不再被挤。）
    auto* leftPane = new QWidget();
    auto* leftLay = new QVBoxLayout(leftPane);
    leftLay->setContentsMargins(6, 6, 4, 6);
    leftLay->setSpacing(6);
    leftLay->addWidget(cmdBarWidget);

    m_recogLabel = new QLabel(QStringLiteral("框选识别：在图上拖拽框选，松手前实时显示范围内文字"));
    m_recogLabel->setObjectName(QStringLiteral("recogBar"));
    m_recogLabel->setWordWrap(true);
    leftLay->addWidget(m_recogLabel);

    leftLay->addWidget(m_viewStack, 1);

    m_tabs = new QTabWidget();
    m_tabs->setMinimumWidth(300);
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

    // OCR 增强与「导出列集」放在右侧标签里：它们都是"设置/参考"性质，
    // 不该占用画布的高度（旧版把画布压扁的根因）。
    {
        auto* ocrPage = new QWidget();
        auto* ocrLay = new QVBoxLayout(ocrPage);
        ocrLay->setContentsMargins(10, 10, 10, 10);
        ocrLay->setSpacing(8);
        m_ocrAlwaysCb = new QCheckBox(QStringLiteral("始终用 OCR 补位图像区（矢量优先）"));
        m_ocrAlwaysCb->setToolTip(QStringLiteral(
            "勾选后，内核在矢量文本之外额外运行 OCR，自动补回被栅格化为图像的文字；\n"
            "矢量文字始终优先（不会被 OCR 覆盖）。"));
        connect(m_ocrAlwaysCb, &QCheckBox::toggled, this, &MainWindow::onOcrAlwaysToggled);
        auto* ocrLegend = new QLabel(QStringLiteral(
            "图例：<span style='color:#149650;'>■ 矢量文字</span>　"
            "<span style='color:#e67800;'>■ OCR 补位</span>"));
        ocrLegend->setWordWrap(true);
        ocrLegend->setStyleSheet(QStringLiteral("QLabel{color:%1;}").arg(Theme::color(Theme::TextMuted)));
        auto* ocrHint = new QLabel(QStringLiteral(
            "说明：等轴测图纸通常含可提取的矢量文字，OCR 只在文字被栅格化时才需要。\n"
            "开启后会增加单页处理时间，但不会影响矢量结果的准确性。"));
        ocrHint->setWordWrap(true);
        ocrHint->setStyleSheet(QStringLiteral("QLabel{color:%1;}").arg(Theme::color(Theme::TextMuted)));
        ocrLay->addWidget(m_ocrAlwaysCb);
        ocrLay->addWidget(ocrLegend);
        ocrLay->addWidget(ocrHint);
        ocrLay->addStretch();
        m_tabs->addTab(ocrPage, QStringLiteral("OCR 增强"));

        auto* colPage = new QWidget();
        auto* colLay = new QVBoxLayout(colPage);
        colLay->setContentsMargins(0, 0, 0, 0);
        colLay->addWidget(m_charPanel);
        m_tabs->addTab(colPage, QStringLiteral("导出列集"));
    }

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

    // 内核调用方式：融合 CLI/内嵌两种方式，随时可切。
    m_backendCombo = new QComboBox();
    m_backendCombo->addItem(QStringLiteral("自动（优先内嵌调库，失败回退命令行）"), QStringLiteral("auto"));
    m_backendCombo->addItem(QStringLiteral("内嵌（进程内直接调内核库）"),       QStringLiteral("embedded"));
    m_backendCombo->addItem(QStringLiteral("命令行（QProcess 起内核 exe）"),     QStringLiteral("cli"));
    m_backendCombo->setToolTip(QStringLiteral(
        "内嵌：用 hostfxr 把内核 DLL 载入本进程直接调用 — 无子进程、无控制台窗口、启动更快，\n"
        "内核日志能实时回传（可在下方「内核日志」看到进度）。\n"
        "命令行：起独立的 {内核}.exe — 兼容性最好，内核崩溃不会带走界面。\n"
        "两条路吃的是同一份内核 DLL 与同一套 JSON 契约，结果一致。"));
    {
        const int idx = m_backendCombo->findData(KernelClient::backendToString(KernelClient::defaultBackend()));
        if (idx >= 0) m_backendCombo->setCurrentIndex(idx);
    }
    connect(m_backendCombo, &QComboBox::currentIndexChanged, this, [this](int){
        setKernelBackend(m_backendCombo->currentData().toString());
    });
    cliRow->addSpacing(12);
    cliRow->addWidget(new QLabel(QStringLiteral("内核调用:")));
    cliRow->addWidget(m_backendCombo);

    auto* rightPane = new QWidget();
    auto* rightLay = new QVBoxLayout(rightPane);
    rightLay->setContentsMargins(4, 6, 6, 6);
    rightLay->setSpacing(6);
    rightLay->addLayout(cliRow);
    rightLay->addWidget(m_tabs, 1);

    auto* split = new QSplitter(Qt::Horizontal);
    // 页面缩略图栏放在最左，学 Adobe 的版式：缩略图 | 画布 | 结果。
    m_pageNav = new PageNavPanel();
    m_pageNav->setCliDir(m_cliDir);
    connect(m_pageNav, &PageNavPanel::pageRequested, this, [this](int p){
        if (p < 0 || p >= m_pageCount || p == m_pageIndex) return;
        m_pageIndex = p;
        onRenderPage();
    });
    connect(m_pageNav, &PageNavPanel::statusMessage, this, &MainWindow::log);
    split->addWidget(m_pageNav);
    split->addWidget(leftPane);
    split->addWidget(rightPane);
    // 让三栏都能拖动改变宽度（旧版把宽度写死，用户反馈"侧栏不能拖动调整"）。
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 2);
    split->setChildrenCollapsible(false);
    m_pageNav->setMinimumWidth(150);
    m_pageNav->setMaximumWidth(320);
    leftPane->setMinimumWidth(320);
    rightPane->setMinimumWidth(300);
    split->setSizes({190, 700, 420});

    // ---- Multi-level sidebar navigation: every sub-function is its own entry ----
    m_nav = new QTreeWidget();
    // 可拖动调整宽度（旧版 setFixedWidth 让侧栏完全不能改宽度）。
    m_nav->setMinimumWidth(140);
    m_nav->setMaximumWidth(400);
    m_nav->setHeaderHidden(true);
    m_nav->setStyleSheet(QStringLiteral("QTreeWidget{font-size:13px;} QTreeWidget::item{padding:7px 6px;} QTreeWidget::item:has-children{font-weight:bold;}"));
    auto mkNav = [&](const QString& label, int page, int ocrMode = 0) {
        auto* it = new QTreeWidgetItem(QStringList(label));
        it->setData(0, Qt::UserRole, page);
        it->setData(0, Qt::UserRole + 1, ocrMode);
        return it;
    };
    auto* itHome = mkNav(QStringLiteral("首页"), PageHome);
    auto* itMat  = mkNav(QStringLiteral("材料表提取"), PageMaterials);
    auto* itPdf  = mkNav(QStringLiteral("PDF 工具"), PagePdf);
    itPdf->addChild(mkNav(QStringLiteral("渲染与框选提取"), PagePdf, 0));
    itPdf->addChild(mkNav(QStringLiteral("材料表 OCR 增强"), PagePdf, 1));
    auto* itDiff = mkNav(QStringLiteral("图纸比对"), PageDiff);
    auto* itExcel = mkNav(QStringLiteral("Excel 比对"), PageExcel);
    auto* itSchemes = mkNav(QStringLiteral("框选方案"), PageScheme);
    m_nav->addTopLevelItem(itHome);
    m_nav->addTopLevelItem(itMat);
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
    m_materialPage = new MaterialExtractPage();
    connect(m_diffPage,  &DiffPage::statusMessage,  this, &MainWindow::log);
    connect(m_excelPage, &ExcelComparePage::statusMessage, this, &MainWindow::log);
    connect(m_materialPage, &MaterialExtractPage::statusMessage, this, &MainWindow::log);
    m_materialPage->setCliDir(m_cliDir);

    m_stack = new QStackedWidget();
    m_stack->addWidget(m_home);     // PageHome
    m_stack->addWidget(split);      // PagePdf  (the PDF→Excel workspace)
    m_stack->addWidget(m_diffPage); // PageDiff
    m_stack->addWidget(m_excelPage);// PageExcel
    m_stack->addWidget(m_schemes);  // PageScheme
    m_stack->addWidget(m_materialPage); // PageMaterials (batch PDF → material tables)

    auto* navSplit = new QSplitter(Qt::Horizontal);
    navSplit->addWidget(m_nav);
    navSplit->addWidget(m_stack);
    navSplit->setStretchFactor(0, 0);
    navSplit->setStretchFactor(1, 1);
    navSplit->setChildrenCollapsible(false);
    navSplit->setSizes({190, 1100});     // 侧栏初始 190，之后用户可随意拖
    setCentralWidget(navSplit);

    connect(m_schemes, &SchemePage::rewriteRequested, this, &MainWindow::onRewriteRegion);
    connect(m_schemes, &SchemePage::reuseRequested,   this, &MainWindow::onReuseScheme);
    connect(m_schemes, &SchemePage::statusMessage,    this, &MainWindow::onSchemeStatus);

    connect(m_home, &HomePage::startExtract, this, [this]{
        navTo(PagePdf); onOpen();
    });
    connect(m_home, &HomePage::openPdfRequested,     this, [this]{ navTo(PagePdf); onOpen(); });
    connect(m_home, &HomePage::wholePageExtract,     this, [this]{ navTo(PageMaterials); });
    connect(m_home, &HomePage::materialExtractRequested, this, [this]{ navTo(PageMaterials); });
    connect(m_home, &HomePage::diffRequested,        this, [this]{ navTo(PageDiff); });
    connect(m_home, &HomePage::excelCompareRequested,this, [this]{ navTo(PageExcel); });
    connect(m_home, &HomePage::schemesRequested,     this, [this]{ navTo(PageScheme); });
    connect(m_home, &HomePage::manualRequested,      this, &MainWindow::onManual);
    connect(m_home, &HomePage::aboutRequested,       this, &MainWindow::onAbout);

    refreshTheme();      // 视口底色按当前主题初始化
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
    // 每次刷新内核状态都把当前后端一并下发，避免页面用的还是旧设置。
    setKernelBackend(KernelClient::backendToString(KernelClient::defaultBackend()));
}

void MainWindow::setCliDir(const QString& d)
{
    m_cliDir = d;
    if (m_cliEdit) m_cliEdit->setText(d);
    KernelClient::setCliDir(d);          // 融合层也要知道内核目录
    if (m_materialPage) m_materialPage->setCliDir(d);
    if (m_pageNav)      m_pageNav->setCliDir(d);
    if (m_contView)     m_contView->setCliDir(d);
    refreshKernelStatus();
    log(QStringLiteral("CLI 目录 = %1").arg(d.isEmpty() ? QStringLiteral("<自动探测>") : d));
}

void MainWindow::setKernelBackend(const QString& mode)
{
    const KernelClient::Backend b = KernelClient::backendFromString(mode);
    KernelClient::setDefaultBackend(b);
    QSettings().setValue(QStringLiteral("kernel/backend"), KernelClient::backendToString(b));
    if (m_backendCombo)
    {
        const int idx = m_backendCombo->findData(KernelClient::backendToString(b));
        if (idx >= 0 && idx != m_backendCombo->currentIndex())
        {
            QSignalBlocker blk(m_backendCombo);
            m_backendCombo->setCurrentIndex(idx);
        }
    }
    if (m_diffPage)  m_diffPage->setKernelBackend(KernelClient::backendToString(b));
    if (m_excelPage) m_excelPage->setKernelBackend(KernelClient::backendToString(b));
    if (m_charPanel) m_charPanel->setKernelBackend(KernelClient::backendToString(b));
    if (m_materialPage) m_materialPage->setKernelBackend(KernelClient::backendToString(b));
    if (m_contView)     m_contView->setKernelBackend(KernelClient::backendToString(b));
    log(QStringLiteral("内核调用方式 = %1（%2）")
            .arg(KernelClient::backendToString(b), KernelClient::describe()));
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

namespace {

// 一次页面渲染的结果（含几何）。用于后台预取缓存。
struct RenderedPage
{
    bool   ok = false;
    QImage img;
    double scale = 1.0, pwPt = 0, phPt = 0;
    int    meanLum = -1, maxLum = -1;   // 亮度诊断（见下）
};

// 纯函数式的渲染（不含任何 UI 状态），因此可以安全地在后台线程调用。
// 内核调用在 KernelClient 内部有全局串行保护（内嵌运行时是进程级单例），
// 所以后台预取与用户操作并发时不会互相踩踏，只是排队执行。
RenderedPage renderPageViaKernel(const QString& pdfPath, int page)
{
    RenderedPage r;
    if (pdfPath.isEmpty()) return r;
    QTemporaryDir tmp;
    const QString bmp = QDir(tmp.path()).filePath(QStringLiteral("page.bmp"));
    const KernelClient::Result kr = KernelClient::run(
        QStringLiteral("EGpdf2excel"),
        {QStringLiteral("--render"), QStringLiteral("--page"), QString::number(page),
         QStringLiteral("--scale"), QStringLiteral("1"),
         QStringLiteral("--render-out"), bmp, pdfPath},
        QString(), KernelClient::defaultBackend());
    if (kr.rc != 0) return r;

    QImage img = QImageReader(bmp).read();
    if (img.isNull()) return r;
    r.img = img.copy();                    // 脱离临时目录（否则可能延迟解码失效）
    r.ok = true;

    const QJsonObject o = QJsonDocument::fromJson(kr.out.toUtf8()).object();
    r.pwPt = o.value(QStringLiteral("pageWidthPt")).toDouble();
    r.phPt = o.value(QStringLiteral("pageHeightPt")).toDouble();
    r.scale = o.value(QStringLiteral("scale")).toDouble(1.0);
    if (r.scale <= 0) r.scale = 1.0;
    if (r.pwPt <= 0) r.pwPt = r.img.width();
    if (r.phPt <= 0) r.phPt = r.img.height();

    // 亮度诊断：把"黑屏"变成可判定的事实 —— 若整页近乎全黑，是内核渲染出的图本身
    // 就是黑的（PDF 内容/背景问题）；若亮度正常却看不到图，则是显示侧的问题。
    {
        const QImage probe = img.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        quint64 lum = 0; int maxL = 0;
        for (int y = 0; y < probe.height(); ++y)
            for (int x = 0; x < probe.width(); ++x) {
                const QRgb c = probe.pixel(x, y);
                const int l = (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000;
                lum += static_cast<quint64>(l);
                maxL = qMax(maxL, l);
            }
        r.meanLum = static_cast<int>(lum / static_cast<quint64>(probe.width() * probe.height()));
        r.maxLum  = maxL;
    }
    return r;
}

} // namespace

// 后台预取下一页：翻页时直接用缓存，做到"点下一页几乎瞬时"。
// 串行池（maxThreadCount=1）：预取只该在空闲时占一个后台线程，不应把 CPU 吃满。
void MainWindow::prefetchPage(int page)
{
    if (m_pdfPath.isEmpty() || page < 0 || page >= m_pageCount) return;
    if (m_cache.page == page && m_cache.ok) return;
    if (m_prefetchBusy) return;

    const QString pdf = m_pdfPath;
    m_prefetchBusy = true;
    auto* watcher = new QFutureWatcher<RenderedPage>(this);
    connect(watcher, &QFutureWatcher<RenderedPage>::finished, this, [this, watcher, page]{
        const RenderedPage r = watcher->result();
        watcher->deleteLater();
        m_prefetchBusy = false;
        if (r.ok) {
            m_cache.page = page;
            m_cache.ok = true;
            m_cache.img = r.img;
            m_cache.scale = r.scale;
            m_cache.pwPt = r.pwPt;
            m_cache.phPt = r.phPt;
        }
    });
    watcher->setFuture(QtConcurrent::run(m_prefetchPool.get(), [pdf, page]{
        return renderPageViaKernel(pdf, page);
    }));
}

bool MainWindow::openPdf(const QString& path)
{
    m_pdfPath = path;
    m_pageIndex = 0;
    m_cache = PageCache{};          // 换文档 ⇒ 预取缓存必须作废
    clearRegionsAll();

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
    // 同样走融合层，与渲染/抽取保持同一条通道。
    const KernelClient::Result pcRes = KernelClient::run(
        QStringLiteral("EGpdf2excel"),
        {QStringLiteral("--page-count"), m_pdfPath},
        QString(), KernelClient::defaultBackend());
    m_pageCount = 1;
    if (pcRes.rc == 0) {
        QJsonParseError pe{};
        QJsonDocument doc = QJsonDocument::fromJson(pcRes.out.toUtf8(), &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject())
            m_pageCount = qMax(1, doc.object().value(QStringLiteral("pageCount")).toInt());
    } else {
        log(QStringLiteral("页码探测失败，默认按 1 页处理: %1").arg(pcRes.err.trimmed()));
    }

    // 单页视图用这一次渲染出图；连续视图只需要页尺寸信息（它会自己懒渲染）。
    m_view.canvas->setPage(img, scale, 0);
    m_view.canvas->setPageIndex(0);
    m_pwPt = pw; m_phPt = ph; m_scale = scale;
    loadPageWords(0);
    // 连续视图：整本文档交给它，滚动到哪渲染哪。
    if (m_contView) m_contView->setDocument(path, m_pageCount);
    m_pageLabel->setText(QStringLiteral("%1 / %2").arg(1).arg(m_pageCount));
    // 缩略图栏：一次性告知文档与页数，后台逐页补缩略图。
    if (m_pageNav) { m_pageNav->setDocument(path, m_pageCount); m_pageNav->setCurrentPage(0); }
    log(QStringLiteral("已打开 %1（%2 页，%3 x %4 pt）")
            .arg(QFileInfo(path).fileName()).arg(m_pageCount).arg(pw).arg(ph));
    return true;
}

// ---------------------------------------------------------------- 视图分派
//
// 单页精看（PdfCanvas，GPU 或光栅壳）与连续滚动（ContinuousPdfView）都实现
// IRegionCanvas，所以业务代码只调 activeCanvas()，切换视图不用改任何逻辑。

IRegionCanvas* MainWindow::activeCanvas() const
{
    if (m_continuousMode && m_contView) return m_contView;
    return m_view.canvas;
}

QScrollArea* MainWindow::activeScroll() const
{
    if (m_continuousMode && m_contScroll) return m_contScroll;
    return m_scroll;
}

void MainWindow::setContinuousMode(bool continuous)
{
    if (m_continuousMode == continuous) return;
    m_continuousMode = continuous;
    if (m_viewStack) m_viewStack->setCurrentIndex(continuous ? 1 : 0);
    refreshTheme();

    // 已打开文档时，把"当前页"在两个视图之间搬过去，切换不丢位置。
    if (!m_pdfPath.isEmpty()) {
        if (continuous) {
            if (m_contView && !m_contView->hasDocument())
                m_contView->setDocument(m_pdfPath, m_pageCount);
            if (m_contView) m_contView->scrollToPage(m_pageIndex);
        } else {
            onRenderPage();
        }
    }
    // 缩放百分比标签与"适合页面"等按钮的语义随视图走，刷新一下显示。
    const double z = activeCanvas() ? activeCanvas()->zoom() : 1.0;
    if (m_zoomLabel) m_zoomLabel->setText(QStringLiteral("缩放 %1%").arg(qRound(z * 100)));
    log(continuous ? QStringLiteral("已切换到「连续滚动视图」（一屏多页，滚动即按需渲染）")
                   : QStringLiteral("已切换到「单页视图」"));
}

void MainWindow::refreshTheme()
{
    // 画布周围一圈底色是自绘/视口级样式，QSS 覆盖不到，只能显式刷。
    const QString canvasBg = Theme::color(Theme::CanvasBg);
    if (m_scroll)     m_scroll->viewport()->setStyleSheet(QStringLiteral("background:%1;").arg(canvasBg));
    if (m_contScroll) m_contScroll->viewport()->setStyleSheet(QStringLiteral("background:%1;").arg(canvasBg));
    if (m_view.widget) m_view.widget->update();
    if (m_contView)    m_contView->update();
}

void MainWindow::addRegionAll(const Region& r)
{
    if (m_view.canvas) m_view.canvas->addRegion(r);
    if (m_contView)    m_contView->addRegion(r);
}

void MainWindow::clearRegionsAll()
{
    if (m_view.canvas) m_view.canvas->clearRegions();
    if (m_contView)    m_contView->clearRegions();
}

bool MainWindow::renderPage(int page, QImage& img, double& scale, double& pwPt, double& phPt)
{
    if (m_pdfPath.isEmpty()) { log(QStringLiteral("未打开 PDF")); return false; }
    // 命中后台预取缓存 ⇒ 立即出图（翻页体感的关键）。
    if (m_cache.ok && m_cache.page == page) {
        img = m_cache.img;
        scale = m_cache.scale; pwPt = m_cache.pwPt; phPt = m_cache.phPt;
        log(QStringLiteral("第 %1 页来自预取缓存 (%2x%3 px)").arg(page + 1).arg(img.width()).arg(img.height()));
        return true;
    }

    const RenderedPage r = renderPageViaKernel(m_pdfPath, page);
    if (!r.ok) {
        log(QStringLiteral("渲染第 %1 页失败（详见内核日志）").arg(page + 1));
        return false;
    }
    img = r.img; scale = r.scale; pwPt = r.pwPt; phPt = r.phPt;
    log(QStringLiteral("渲染第 %1 页: %2x%3 px, 格式#%4, 平均亮度 %5 / max %6")
            .arg(page + 1).arg(img.width()).arg(img.height())
            .arg(static_cast<int>(img.format())).arg(r.meanLum).arg(r.maxLum));
    if (r.maxLum >= 0 && r.maxLum < 24)
        log(QStringLiteral("警告: 第 %1 页渲染结果几乎全黑（max 亮度 %2）——"
                           "该页可能确实无内容，或 PDF 背景被画成黑色。").arg(page + 1).arg(r.maxLum));
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
    // 旧的"弹两个对话框 + 跑一条命令 + 弹个消息框"流程，已被独立的「材料表提取」页
    // 取代：那里有真正的 PDF 输入区（多选 / 整目录 / 拖拽）、输出设置、逐文件进度与
    // 失败隔离。这里只做导航，保持工具栏/菜单调用点的语义不变。
    navTo(PageMaterials);
}

// ---- Excel drawing comparison (drives EGexcel2df directly) ----
void MainWindow::onRenderPage()
{
    if (m_pdfPath.isEmpty()) return;

    // 连续视图里"翻到某页"= 滚动到该页（它自己按可视区懒渲染），不需要预渲染单页图。
    if (m_continuousMode) {
        if (m_contView) m_contView->scrollToPage(m_pageIndex);
        m_pageLabel->setText(QStringLiteral("%1 / %2").arg(m_pageIndex + 1).arg(m_pageCount));
        if (m_pageNav) m_pageNav->setCurrentPage(m_pageIndex);
        return;
    }

    QImage img; double scale = 1, pw = 0, ph = 0;
    if (renderPage(m_pageIndex, img, scale, pw, ph)) {
        m_view.canvas->setPage(img, scale, m_pageIndex);
        m_scale = scale; m_pwPt = pw; m_phPt = ph;
        loadPageWords(m_pageIndex);
        m_pageLabel->setText(QStringLiteral("%1 / %2").arg(m_pageIndex + 1).arg(m_pageCount));
        if (m_pageNav) m_pageNav->setCurrentPage(m_pageIndex);
        log(QStringLiteral("渲染第 %1 页 (%2 x %3 px)").arg(m_pageIndex + 1).arg(img.width()).arg(img.height()));
        // 后台预取下一页（并行/异步）：等用户点"下一页"时直接命中缓存。
        prefetchPage(m_pageIndex + 1);
    } else if (m_view.canvas) {
        // 渲染失败时把原因推到画布上，界面给出可读提示而不是一块深灰（"黑屏"）。
        // setRenderFailed 是单页画布专有的（连续视图自己画"渲染失败"占位），故动态转换。
        if (auto* pc = dynamic_cast<PdfCanvas*>(m_view.canvas))
            pc->setRenderFailed(QStringLiteral("见「内核日志」页"));
    }
}

void MainWindow::onPrevPage() { if (m_pageIndex > 0) { --m_pageIndex; onRenderPage(); } }
void MainWindow::onNextPage() { if (m_pageIndex + 1 < m_pageCount) { ++m_pageIndex; onRenderPage(); } }

void MainWindow::onModeChanged(int)
{
    activeCanvas()->setKind(m_modeCombo->currentData().toString());
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
    addRegionAll(r);
    refreshJsonPreview();
    log(QStringLiteral("框选: 页%1 %2 [x=%3 y=%4 w=%5 h=%6]")
            .arg(page + 1).arg(kind).arg(rectPt.x(), 0, 'f', 1).arg(rectPt.y(), 0, 'f', 1)
            .arg(rectPt.width(), 0, 'f', 1).arg(rectPt.height(), 0, 'f', 1));
}

void MainWindow::onClearRegions()
{
    clearRegionsAll();
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
    if (m_pdfPath.isEmpty() || !m_view.canvas) return;
    // 同一条融合层通道（内嵌优先）：矢量词盒直接来自内核，不做任何本地重算。
    const KernelClient::Result wRes = KernelClient::run(
        QStringLiteral("EGpdf2excel"),
        {QStringLiteral("--words"), QString::number(page), m_pdfPath},
        QString(), KernelClient::defaultBackend());
    int rc = wRes.rc;
    const QString out = wRes.out, err = wRes.err;
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
        wb.page = page;
        words.append(wb);
    }
    m_view.canvas->setPageWords(words);
    log(QStringLiteral("已加载本页矢量文字 %1 词（实时识别就绪）").arg(words.size()));
}

// ---------------------------------------------------------------- JSON contract

QJsonObject MainWindow::buildRequest() const
{
    QJsonArray regions;
    for (const Region& r : activeCanvas()->regions()) {
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
        QSet<int> used; for (const Region& r : activeCanvas()->regions()) used.insert(r.page);
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
    if (activeCanvas()->regions().isEmpty()) { log(QStringLiteral("没有框选可导出")); return; }
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
    if (activeCanvas()->regions().isEmpty()) { log(QStringLiteral("请先框选区域")); return; }
    refreshJsonPreview();

    // 走融合层（可选内嵌调库 / 命令行）。JSON 契约直接交给内核，不落临时文件。
    const QString reqJson = QString::fromUtf8(QJsonDocument(buildRequest()).toJson(QJsonDocument::Compact));
    const KernelClient::Result kr = KernelClient::run(
        QStringLiteral("EGpdf2excel"),
        {QStringLiteral("--mode"), QStringLiteral("region")},
        reqJson, KernelClient::defaultBackend());
    QString out = kr.out, err = kr.err;
    int rc = kr.rc;
    if (!err.trimmed().isEmpty()) log(QStringLiteral("[内核 stderr/%1] ").arg(kr.backend) + err.trimmed().left(500));
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
    // 叠加层收录**所有页**的词盒（每条带 page）：单页视图只画当前页，连续视图按页画。
    // 表格/管线号等"结果面板"仍只取当前页，避免翻页时内容重复堆积。
    for (const QJsonValue& rv : regions) {
        QJsonObject r = rv.toObject();
        int page = r.value(QStringLiteral("page")).toInt();
        QJsonArray words = r.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wv : words) {
            QJsonObject w = wv.toObject();
            WordBox wb;
            wb.rectPt = QRectF(w.value(QStringLiteral("x0")).toDouble(), w.value(QStringLiteral("y0")).toDouble(),
                               w.value(QStringLiteral("x1")).toDouble() - w.value(QStringLiteral("x0")).toDouble(),
                               w.value(QStringLiteral("y1")).toDouble() - w.value(QStringLiteral("y0")).toDouble());
            wb.text = w.value(QStringLiteral("text")).toString();
            wb.source = w.value(QStringLiteral("source")).toString();
            wb.page = page;
            overlay.push_back(wb);
        }
        if (page != m_pageIndex) continue;
        if (r.value(QStringLiteral("ocrUsed")).toBool())
            log(QStringLiteral("注意: 该区域使用了 OCR 兜底"));

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
    activeCanvas()->setWordBoxes(overlay);
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
    if (!m_view.canvas) return out;
    for (const Region& r : activeCanvas()->regions()) {
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
    if (!m_view.canvas) return;
    clearRegionsAll();
    for (const RegionSpec& g : s.regions) {
        Region r;
        r.page = g.page;
        r.kind = g.kind;
        r.rectPt = QRectF(g.x, g.y, g.w, g.h);
        addRegionAll(r);
    }
    // Jump to the first page that carries a box so the user sees them.
    if (!s.regions.isEmpty()) {
        m_pageIndex = s.regions.first().page;
        onRenderPage();
        clearRegionsAll();
        for (const RegionSpec& g : s.regions) {
            Region r;
            r.page = g.page;
            r.kind = g.kind;
            r.rectPt = QRectF(g.x, g.y, g.w, g.h);
            addRegionAll(r);
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
    clearRegionsAll();
    Region r;
    r.page = s->regions[regionIndex].page;
    r.kind = s->regions[regionIndex].kind;
    r.rectPt = QRectF(s->regions[regionIndex].x, s->regions[regionIndex].y,
                      s->regions[regionIndex].w, s->regions[regionIndex].h);
    addRegionAll(r);

    log(QStringLiteral("重写模式：在画布上拖拽以重画该区域（完成后自动保存到方案「%1」）").arg(s->name));
    statusBar()->showMessage(QStringLiteral("重写模式 — 在画布上拖拽重画区域"));
}