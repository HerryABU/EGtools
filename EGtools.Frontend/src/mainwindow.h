#pragma once

#include <QMainWindow>
#include <QString>
#include <QImage>
#include "pdfview.h"

class QComboBox;
class QLabel;
class QTableWidget;
class QTextEdit;
class QListWidget;
class QTabWidget;
class QLineEdit;
class QStackedWidget;
class HomePage;

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
    void onDiff();
    void onManual();
    void onAbout();
    void onHome();
    void onWholePageExtract();
    void onExcelCompare();
    void onModeChanged(int index);
    void onRegionSelected(int page, const QString& kind, const QRectF& rectPt);

private:
    void buildUi();
    QString cliExe(const QString& name) const;
    void log(const QString& s);
    void setCliDir(const QString& d);
    void refreshKernelStatus();
    void showPage(int index);

    // Render one page via the CLI; returns the loaded image + geometry.
    bool renderPage(int page, QImage& img, double& scale, double& pwPt, double& phPt);
    QJsonObject buildRequest() const;
    void refreshJsonPreview();
    void showRegionResponse(const QJsonObject& resp);
    void showRenderSummary(const QString& stdoutJson);

    QString m_cliDir;
    QString m_pdfPath;
    int     m_pageCount = 0;
    int     m_pageIndex = 0;
    double  m_scale = 1.0, m_pwPt = 0, m_phPt = 0;

    PdfView*     m_view = nullptr;
    QStackedWidget* m_stack = nullptr;
    HomePage*    m_home = nullptr;
    QComboBox*   m_modeCombo = nullptr;
    QLabel*      m_pageLabel = nullptr;
    QTabWidget*  m_tabs = nullptr;
    QTableWidget* m_table = nullptr;
    QListWidget* m_pipes = nullptr;
    QTextEdit*   m_jsonView = nullptr;
    QTextEdit*   m_logView = nullptr;
    QLineEdit*   m_cliEdit = nullptr;
};