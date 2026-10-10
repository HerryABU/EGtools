#pragma once

#include <QWidget>
#include <QString>

class QLabel;
class QLineEdit;
class QTextEdit;
class QPushButton;

// Dedicated "PDF 比对 / 图纸差异" page (drives EGpdf2df).
// Independent from the PDF->Excel workspace: pick a baseline + target PDF,
// run the kernel, then show the diff JSON and a red/blue annotated preview.
// Holds no PDF logic of its own — everything real happens in the CLI kernel.
class DiffPage : public QWidget
{
    Q_OBJECT
public:
    explicit DiffPage(QWidget* parent = nullptr);

    // CLI kernel directory (set by MainWindow whenever it changes).
    void setCliDir(const QString& dir) { m_cliDir = dir; refreshKernelStatus(); }

signals:
    void statusMessage(const QString& msg);

private slots:
    void onPickBaseline();
    void onPickTarget();
    void onRun();
    void onBrowsePreview();

private:
    QString cliExe(const QString& name) const;
    void refreshKernelStatus();

    QString m_cliDir;
    QLineEdit* m_baseEdit = nullptr;
    QLineEdit* m_tgtEdit  = nullptr;
    QTextEdit* m_jsonView = nullptr;
    QLabel*   m_preview   = nullptr;
    QLabel*   m_status    = nullptr;
    QPushButton* m_runBtn  = nullptr;
};
