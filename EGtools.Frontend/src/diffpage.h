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
// 图形红/绿差异 + 文字直接改色(新增红/删除蓝，git +/- 风格) + 管线号↔材料表一一对应。
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
    void onPickChangeColor();
    void onPickTextAddedColor();
    void onPickTextRemovedColor();
    void onPickMarkStrokeColor();

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
    // 可自定义标注色（传给内核 options.colors）：图形描红 / 变更绿 / 新增文字(红) / 删除文字(蓝)。
    QString m_traceColor  = QStringLiteral("#FF0000");  // 描红线（新增/删除图形）
    QString m_changeColor = QStringLiteral("#16A34A");  // 变更绿（内容改动的图形）
    QString m_textAddedColor  = QStringLiteral("#FF0000");  // 新增文字改色（红，git +）
    QString m_textRemovedColor = QStringLiteral("#0000FF"); // 删除文字改色（蓝，git -）
    QPushButton* m_traceColorBtn  = nullptr;
    QPushButton* m_changeColorBtn = nullptr;
    QPushButton* m_textAddedColorBtn = nullptr;
    QPushButton* m_textRemovedColorBtn = nullptr;
    QComboBox* m_textDirCombo = nullptr;   // 变更旧(蓝)文字插入方向 down/up/right/left
    // 图形标记形式 + 描边色：这类图信息密度很高，实心块会盖住底图，故默认"淡填充+深色描边"。
    QComboBox* m_markStyleCombo = nullptr;  // semi(半透明+描边) / solid(实心) / outline(仅描边)
    QString m_markStrokeColor = QString();   // 留空 = 内核自动取标记色深色版
    QPushButton* m_markStrokeBtn = nullptr;
    // ---- 矢量检测参数（可调）----
    QComboBox* m_gfxMethodCombo = nullptr;   // vector(默认,纯矢量) / pixel(渲染+OpenCV 回退)
    QDoubleSpinBox* m_vecTolSpin = nullptr;  // 矢量比对容差(pt)
    QDoubleSpinBox* m_vecAngSpin = nullptr;  // 方向容差(度)
    QComboBox* m_reportModeCombo = nullptr;  // cluster(按区域) / run(逐条)
    QDoubleSpinBox* m_clusterRadiusSpin = nullptr;  // 聚类半径(pt)
    QDoubleSpinBox* m_minClusterLenSpin = nullptr;  // 丢弃小于此总长(pt)的区域
    QDoubleSpinBox* m_changeRadiusSpin = nullptr;   // 第三态"变更"识别半径(pt)
    // ---- 文字差异参数 ----
    QComboBox* m_textBoxModeCombo = nullptr; // word(逐词框,更精确) / line(整行框)
    QComboBox* m_textSerialCombo = nullptr;  // loose(含行首纯数字) / strict(只认管段号/NOTE n)
    // ---- 图号配对参数（标题栏图号 = 每页唯一号，同图号才比对）----
    QDoubleSpinBox* m_sheetNoXSpin = nullptr;   // 图号区 右侧起点比例
    QDoubleSpinBox* m_sheetNoYSpin = nullptr;   // 图号区 下方起点比例
    QSpinBox* m_sheetNoMaxSpin = nullptr;       // 总图判定：单页管段号种类数上限
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
