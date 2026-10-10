#pragma once

#include <QWidget>
#include <QString>

class QLabel;
class QLineEdit;
class QTextEdit;
class QPushButton;
class QSpinBox;
class QCheckBox;
class QGroupBox;

// Dedicated "PDF 比对 / 图纸差异" page (drives EGpdf2df).
// 红蓝差异 + 圈画文字 + 浅蓝斜体显现消失文字 + 管线号↔材料表一一对应。
// 持有 no PDF 逻辑——所有实质工作都在 CLI 内核。
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
    void onPickCsv();
    void onClearCsv();
    void onRun();
    void onBrowsePreview();

private:
    QString cliExe(const QString& name) const;
    void refreshKernelStatus();
    void showPipeMatch(const QJsonObject& root);

    QString m_cliDir;
    QLineEdit* m_baseEdit = nullptr;
    QLineEdit* m_tgtEdit  = nullptr;
    QLineEdit* m_csvEdit  = nullptr;
    QSpinBox*  m_dpiSpin = nullptr;
    QCheckBox* m_pipeChk = nullptr;
    QTextEdit* m_jsonView = nullptr;
    QTextEdit* m_pipeView = nullptr;
    QLabel*   m_preview   = nullptr;
    QLabel*   m_status    = nullptr;
    QPushButton* m_runBtn  = nullptr;
};
