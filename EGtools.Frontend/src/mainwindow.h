#pragma once

#include <QMainWindow>
#include <QString>
#include <QImage>
#include <QVector>
#include <QThreadPool>
#include <memory>
#include "pdfview.h"
#include "schemes.h"
#include "types.h"

class QComboBox;
class QLabel;
class QTableWidget;
class QTextEdit;
class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QTabWidget;
class QLineEdit;
class QCheckBox;
class QStackedWidget;
class QScrollArea;
class QActionGroup;
class QTabBar;
class QGroupBox;
class QSplitter;
class HomePage;
class SchemePage;
class SchemeStore;
class DiffPage;
class ExcelComparePage;
class CharSegmentPanel;
class MaterialExtractPage;
class PageNavPanel;
class ContinuousPdfView;
class IRegionCanvas;

// The Qt shell. It owns NO PDF processing logic: it displays pages rendered by
// the CLI, captures rubber-band selections, exports the region JSON contract for
// the kernel, invokes the kernels via QProcess, and renders their JSON results.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QString& cliDir = QString(), QWidget* parent = nullptr);

    // Loads a PDF and renders page 0. Returns false (and logs) on failure.
    bool openPdf(const QString& path);

    // ---- 自检钩子（只给无头/自动化测试用，不参与任何业务逻辑）----
    // 端到端画布绘制自检必须能拿到"真实主窗口里那个画布控件"：只有这样才能复现
    // "渲染正常但界面全黑"这类故障（画布控件在 QScrollArea/QStackedWidget 里的
    // 真实处境，与单独 new 一个壳测试并不等价）。
    QWidget* canvasWidgetForTest() const { return m_view.widget; }
    PdfCanvas* canvasForTest() const { return m_view.canvas; }
    bool     canvasIsGpuForTest() const { return m_view.gpu; }
    int      pageCountForTest() const { return m_pageCount; }
    QString  continuousBackendForTest() const { return gpuBackendDescription(); }
    // 切到「PDF 工具」页 —— `onOpen()` 会做这一步（showPage(1)），
    // 自检若只调 openPdf() 而不导航，画布其实处于隐藏页里，抓图只会得到底色。
    void     showPdfPageForTest() { showPage(PagePdf); }

public slots:
    void onOpen();
    void onPrevPage();
    void onNextPage();
    void onRenderPage();
    void onClearRegions();
    void onExportJson();
    void onExtract();
    void onManual();
    void onAbout();
    void onHome();
    void onWholePageExtract();
    void onSchemes();
    void onCaptureAsScheme();
    void onRewriteRegion(const QString& schemeId, int regionIndex);
    void onReuseScheme(const QString& schemeId);
    void onSchemeStatus(const QString& msg);
    void onModeChanged(int index);
    void onRegionSelected(int page, const QString& kind, const QRectF& rectPt);
    void onTextPreview(int page, const QString& text);
    void onNavItemClicked(QTreeWidgetItem* item, int col);
    void onOcrAlwaysToggled(bool on);

private:
    void buildUi();
    QString cliExe(const QString& name) const;
    void log(const QString& s);
    void setCliDir(const QString& d);
    // 内核调用方式（自动 / 内嵌 / 命令行）—— 融合层的选择，持久化到 QSettings。
    void setKernelBackend(const QString& mode);
    void refreshKernelStatus();
    void showPage(int index);

    // Render one page via the CLI; returns the loaded image + geometry.
    bool renderPage(int page, QImage& img, double& scale, double& pwPt, double& phPt);
    // 后台预取相邻页（并行/异步渲染），使用户翻页近乎瞬时。
    void prefetchPage(int page);
    // 当前生效的画布（单页精看 或 连续滚动）—— 两者都实现 IRegionCanvas，
    // 于是框选/语义/缩放/叠加层这些操作对视图类型无感。
    IRegionCanvas* activeCanvas() const;
    QScrollArea*   activeScroll() const;
    // 把画布信号接到本窗口（构造与"热换壳"两处都要用，故抽成函数）。
    void wirePdfCanvas();
    // 换画布壳（携带当前页图/缩放/框选/词盒，用户无感）。
    //   wantGpu=false → 一定用光栅；true → 机器可用才用 GPU，否则回落并记日志。
    // 两种触发：GPU 壳首帧画不出内容时自愈（useRasterBackend），
    // 以及用户在「视图 → 渲染后端」里手动切换（setRenderBackend）。
    void rebuildCanvasShell(bool wantGpu, const QString& why);
    void useRasterBackend(const QString& reason);
    // 渲染后端偏好（auto/gpu/raster）：持久化 + 立即重建画布。
    void setRenderBackend(const QString& mode);
    // 切换视图模式：true = 连续滚动（Adobe 式），false = 单页精看。
    void setContinuousMode(bool continuous);
    // 布局模式：false = 侧栏式（导航树在左列）；true = 竖向堆叠式
    // （导航收纳为顶部标签条，缩略图栏下放到底部标签页，画布独占整宽）。
    void setStackedLayout(bool stacked);
    void applyLayoutMode();
    bool stackedLayout() const { return m_stackedLayout; }
    // 主题切换后需要手动刷新的少数几处（画布视口底色等自绘/局部样式）。
    void refreshTheme();
    // 框选是"文档级"的：两个视图各自保存一份，变更时镜像过去 —— 这样切换视图
    // 不会丢框选，导出 JSON / 提取用的也始终是同一份内容。
    void addRegionAll(const Region& r);
    void clearRegionsAll();
    QJsonObject buildRequest() const;
    void refreshJsonPreview();
    void showRegionResponse(const QJsonObject& resp);
    void showRenderSummary(const QString& stdoutJson);

    QVector<RegionSpec> canvasRegions() const;
    void applySchemeToCanvas(const Scheme& s);
    void loadSchemeIntoStore(const Scheme& s);
    // The 9 material-table headers (config-driven); columns 7 & 8 are QTY / WEIGHT.
    static QStringList materialHeaders();

    // Stacked page indices (the left sidebar switches between these).
    enum PageIdx { PageHome = 0, PagePdf = 1, PageDiff = 2, PageExcel = 3, PageScheme = 4,
                   PageMaterials = 5 };
    void navTo(int index);
    // Load every vector word of a page (kernel --words) into the canvas so the
    // rubber band can recognise text live as the user drags.
    void loadPageWords(int page);

    QString m_cliDir;
    QComboBox* m_backendCombo = nullptr;   // 内核调用方式：自动/内嵌(调库)/命令行
    QString m_pdfPath;
    int     m_pageCount = 0;
    int     m_pageIndex = 0;
    double  m_scale = 1.0, m_pwPt = 0, m_phPt = 0;

    PdfViewHost  m_view;      // 画布宿主（GPU 或 CPU 光栅，取决于探测结果）
    QStackedWidget* m_stack = nullptr;
    HomePage*    m_home = nullptr;
    SchemePage*  m_schemes = nullptr;
    SchemeStore* m_schemeStore = nullptr;
    DiffPage*    m_diffPage = nullptr;
    ExcelComparePage* m_excelPage = nullptr;
    MaterialExtractPage* m_materialPage = nullptr;
    PageNavPanel* m_pageNav = nullptr;    // Adobe 风格页面缩略图导航栏
    // 视图：0 = 单页精看（m_view），1 = 连续滚动（m_contView）。
    QStackedWidget*    m_viewStack = nullptr;
    ContinuousPdfView* m_contView  = nullptr;
    QScrollArea*       m_contScroll = nullptr;
    bool               m_continuousMode = false;

    // ---- 布局模式（侧栏式 / 竖向堆叠式）----
    bool       m_stackedLayout = false;
    QTabBar*   m_topNav    = nullptr;   // 堆叠模式下的顶部导航条（取代左侧导航树）
    QWidget*   m_sidePane  = nullptr;   // 侧栏容器（导航树 + 内核设置）
    QSplitter* m_canvasSplit = nullptr; // 缩略图 | 画布
    QSplitter* m_navSplit    = nullptr; // 侧栏 | 内容页
    QWidget*   m_kernelBox   = nullptr; // 内核设置（滚动容器；侧栏式在侧栏、堆叠式在底部标签）
    void syncTopNav(int page);
    CharSegmentPanel* m_charPanel = nullptr;
    QTreeWidget* m_nav = nullptr;
    QCheckBox*   m_ocrAlwaysCb = nullptr;
    QString      m_rewriteSchemeId;   // non-empty => a rewrite is in progress
    int          m_rewriteRow = -1;
    QComboBox*   m_modeCombo = nullptr;
    QLabel*      m_pageLabel = nullptr;
    QLabel*      m_zoomLabel = nullptr;   // 当前缩放百分比（Ctrl+滚轮 / 适合宽度）
    QScrollArea* m_scroll    = nullptr;   // 画布滚动容器（"适合宽度"需要视口宽度）
    // 预取缓存与后台池（maxThreadCount=1：只在空闲时占一个线程，不抢 CPU）。
    struct PageCache { int page = -1; bool ok = false; QImage img;
                       double scale = 1.0, pwPt = 0, phPt = 0; };
    PageCache m_cache;
    bool m_prefetchBusy = false;
    std::shared_ptr<QThreadPool> m_prefetchPool;
    QLabel*      m_recogLabel = nullptr;  // live in-canvas text recognition readout
    QTabWidget*  m_tabs = nullptr;
    QTableWidget* m_table = nullptr;
    QListWidget* m_pipes = nullptr;
    QTextEdit*   m_jsonView = nullptr;
    QTextEdit*   m_logView = nullptr;
    QLineEdit*   m_cliEdit = nullptr;
};