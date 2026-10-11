#pragma once

#include <QMainWindow>
#include <QString>
#include <QImage>
#include <QVector>
#include "pdfview.h"
#include "schemes.h"

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
class HomePage;
class SchemePage;
class SchemeStore;
class DiffPage;
class ExcelComparePage;
class CharSegmentPanel;

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
    enum PageIdx { PageHome = 0, PagePdf = 1, PageDiff = 2, PageExcel = 3, PageScheme = 4 };
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

    PdfView*     m_view = nullptr;
    QStackedWidget* m_stack = nullptr;
    HomePage*    m_home = nullptr;
    SchemePage*  m_schemes = nullptr;
    SchemeStore* m_schemeStore = nullptr;
    DiffPage*    m_diffPage = nullptr;
    ExcelComparePage* m_excelPage = nullptr;
    CharSegmentPanel* m_charPanel = nullptr;
    QTreeWidget* m_nav = nullptr;
    QCheckBox*   m_ocrAlwaysCb = nullptr;
    QString      m_rewriteSchemeId;   // non-empty => a rewrite is in progress
    int          m_rewriteRow = -1;
    QComboBox*   m_modeCombo = nullptr;
    QLabel*      m_pageLabel = nullptr;
    QLabel*      m_recogLabel = nullptr;  // live in-canvas text recognition readout
    QTabWidget*  m_tabs = nullptr;
    QTableWidget* m_table = nullptr;
    QListWidget* m_pipes = nullptr;
    QTextEdit*   m_jsonView = nullptr;
    QTextEdit*   m_logView = nullptr;
    QLineEdit*   m_cliEdit = nullptr;
};