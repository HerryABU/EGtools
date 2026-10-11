#pragma once

#include <QWidget>
#include <QString>

class QLabel;
class QLineEdit;
class QTextEdit;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
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
    void onPickTraceColor();
    void onPickCircleColor();
    void onPickChangeColor();
    void onPickTextColor();

private:
    QString cliExe(const QString& name) const;
    void refreshKernelStatus();
    void showPipeMatch(const QJsonObject& root);
    // 弹出颜色选择器；确认后更新按钮色块与颜色值。返回是否改动。
    bool pickColor(const QString& title, QString& color, QPushButton* btn);
    // 同步按钮色块（背景=当前颜色，深色底用白字）。
    static void paintSwatch(QPushButton* btn, const QString& hex);

    QString m_cliDir;
    QLineEdit* m_baseEdit = nullptr;
    QLineEdit* m_tgtEdit  = nullptr;
    QLineEdit* m_csvEdit  = nullptr;
    QSpinBox*  m_dpiSpin = nullptr;
    QCheckBox* m_pipeChk = nullptr;
    QComboBox* m_precCombo = nullptr;            // 精细度 fine/medium/coarse
    // 四个可自定义标注色（传给内核 options.colors）：描红/变更绿/圆圈/文字。
    QString m_traceColor  = QStringLiteral("#FF0000");  // 描红线（新增/删除图形）
    QString m_changeColor = QStringLiteral("#16A34A");  // 变更绿（内容改动的图形/文字）
    QString m_circleColor = QStringLiteral("#FF0000");  // 圆圈（文字/序号标记）
    QString m_textColor   = QStringLiteral("#66B2FF");  // 文字（消失/变更标注）
    QPushButton* m_traceColorBtn  = nullptr;
    QPushButton* m_changeColorBtn = nullptr;
    QPushButton* m_circleColorBtn = nullptr;
    QPushButton* m_textColorBtn   = nullptr;
    // 描红/填充可调参数（线宽粗细、填充透明度、填充外扩像素）。
    QSpinBox*      m_lwSpin   = nullptr;   // 线宽(描边粗细) 1..10
    QDoubleSpinBox* m_opacSpin = nullptr;  // 填充透明度 0..1
    QSpinBox*      m_haloSpin = nullptr;   // 填充外扩像素 0..20
    QTextEdit* m_jsonView = nullptr;
    QTextEdit* m_pipeView = nullptr;
    QLabel*   m_preview   = nullptr;
    QLabel*   m_status    = nullptr;
    QPushButton* m_runBtn  = nullptr;
};
