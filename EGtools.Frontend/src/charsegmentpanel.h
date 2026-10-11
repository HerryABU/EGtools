#pragma once

#include <QWidget>
#include <QString>

class QLabel;
class QListWidget;
class QPushButton;
class QLineEdit;

// "字符段" (output column set / headers) configuration panel for the
// PDF->Excel workbench. Lets the user view / add / rename / remove / reorder
// the exported columns, save them to config.json, or restore the built-in
// default (which calls EGpdf2excel --reset-config).
class CharSegmentPanel : public QWidget
{
    Q_OBJECT
public:
    explicit CharSegmentPanel(QWidget* parent = nullptr);
    void setCliDir(const QString& dir) { m_cliDir = dir; reload(); }
    // 内核调用方式由 MainWindow 下发（本面板的调用是纯参数形式，两条后端都支持）。
    void setKernelBackend(const QString& mode) { m_kernelBackend = mode; }

signals:
    void statusMessage(const QString& msg);

private slots:
    void onAdd();
    void onRemove();
    void onUp();
    void onDown();
    void onRestoreDefault();
    void onSave();
    void reload();

private:
    QString cliExe(const QString& name) const;
    QString configPath() const;
    QStringList readHeaders() const;
    bool writeHeaders(const QStringList& headers);

    QString m_cliDir;
    QString m_kernelBackend = QStringLiteral("auto");
    QListWidget* m_list = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_delBtn = nullptr;
    QPushButton* m_upBtn  = nullptr;
    QPushButton* m_downBtn= nullptr;
    QPushButton* m_saveBtn= nullptr;
    QPushButton* m_resetBtn=nullptr;
    QLabel*     m_hint   = nullptr;
};
