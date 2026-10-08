#pragma once

#include <QWidget>
#include "schemes.h"

class QListWidget;
class QTableWidget;
class QLineEdit;
class QTextEdit;
class QLabel;
class SchemeStore;

// Second-level page: manage reusable box-selection "schemes".
//
// Editing is deliberately split the way the user asked:
//   * 修改字段  — done HERE, in a table (kind / page / x / y / w / h are all
//                 editable in place). No JSON editor is exposed.
//   * 重写框选  — asks the main window to re-draw a specific box on the canvas.
//   * 复用      — push the scheme's boxes back onto the canvas.
//   * 存储/导出  — persisted automatically to schemes.json; export writes a
//                 RegionRequest JSON the CLI can consume directly.
class SchemePage : public QWidget
{
    Q_OBJECT
public:
    explicit SchemePage(SchemeStore* store, QWidget* parent = nullptr);

    // Re-reads the store into the list (called after external changes).
    void reload();

    // Select a scheme by id (no-op if unknown).
    void selectScheme(const QString& id);

    // Capture the canvas boxes as a NEW scheme (from MainWindow).
    void captureFromCanvas(const QVector<RegionSpec>& regions);

signals:
    // Ask the main window to re-draw box `regionIndex` of `schemeId`.
    void rewriteRequested(const QString& schemeId, int regionIndex);
    // Ask the main window to load this scheme's boxes onto the canvas.
    void reuseRequested(const QString& schemeId);
    void statusMessage(const QString& msg);

private slots:
    void onNewScheme();
    void onDeleteScheme();
    void onListSelectionChanged();
    void onCellChanged(int row, int col);
    void onAddRegion();
    void onRemoveRegion();
    void onRewriteRegion();
    void onReuse();
    void onExport();
    void onImport();
    void onCaptureCanvas();

private:
    Scheme* current();
    void rebuildList(const QString& selectId = QString());
    void loadIntoEditor();
    void storeIntoTable(const Scheme& s);
    void pushEditToScheme();
    QVector<RegionSpec> tableToRegions() const;
    void appendStatusRow(const QString& text);

    SchemeStore* m_store = nullptr;
    QListWidget* m_list = nullptr;
    QTableWidget* m_table = nullptr;
    QLineEdit*   m_name = nullptr;
    QTextEdit*   m_note = nullptr;
    QLabel*      m_current = nullptr;
    QString      m_currentId;
    bool         m_loading = false;   // suppress cellChanged while populating
};