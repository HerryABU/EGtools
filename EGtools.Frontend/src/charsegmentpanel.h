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
    QListWidget* m_list = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_delBtn = nullptr;
    QPushButton* m_upBtn  = nullptr;
    QPushButton* m_downBtn= nullptr;
    QPushButton* m_saveBtn= nullptr;
    QPushButton* m_resetBtn=nullptr;
    QLabel*     m_hint   = nullptr;
};
