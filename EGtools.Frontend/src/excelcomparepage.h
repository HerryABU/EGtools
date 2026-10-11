#pragma once

#include <QWidget>
#include <QString>

class QLabel;
class QLineEdit;
class QTextEdit;
class QPushButton;

// Dedicated "Excel 比对" page (drives EGexcel2df): compare an old vs new
// drawing Excel and produce a change list. Independent page — no PDF logic.
class ExcelComparePage : public QWidget
{
    Q_OBJECT
public:
    explicit ExcelComparePage(QWidget* parent = nullptr);
    void setCliDir(const QString& dir) { m_cliDir = dir; refreshKernelStatus(); }
    void setKernelBackend(const QString& mode) { m_kernelBackend = mode; }

signals:
    void statusMessage(const QString& msg);

private slots:
    void onPickOld();
    void onPickNew();
    void onRun();

private:
    QString cliExe(const QString& name) const;
    void refreshKernelStatus();

    QString m_cliDir;
    QString m_kernelBackend = QStringLiteral("auto");
    QLineEdit* m_oldEdit = nullptr;
    QLineEdit* m_newEdit = nullptr;
    QTextEdit* m_jsonView = nullptr;
    QLabel*   m_status   = nullptr;
    QPushButton* m_runBtn = nullptr;
};
