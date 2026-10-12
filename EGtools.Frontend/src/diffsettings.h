#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QTabWidget;

// ---------------------------------------------------------------------------
// 比对参数 + 设置对话框
//
// 为什么要有这一层（而不是把几十个控件摊在主界面上）：
//   专业比对软件（Bluebeam 的 Compare Documents、Acrobat 的 Compare Files）都是
//   **主界面只放"选文件 + 开始"，参数收进一个对话框，并且给出预置的"比对类型"**。
//   原来把 12 行参数挤在页面左侧一列里，用户得不停上下滚动/拖分割条才能操作
//   —— 这正是"推拉很麻烦"的来源。
//
// 设计取舍：
//   * 参数**一个都没少**（功能不退化），只是搬进分类标签页；
//   * 顶部是 **预设（比对类型）**：默认 / 只看图形 / 只看文字 / 严格 / 宽松
//     —— 对应 Bluebeam 的 comparison profiles，选了就套一整组参数；
//   * 预设只是"起点"：改任何一项会自动变成"自定义"，不会被预设覆盖回去。
// ---------------------------------------------------------------------------
struct DiffParams
{
    // 常规
    int     renderDpi   = 200;
    bool    pipeMatch   = true;
    QString precision   = QStringLiteral("fine");     // fine / medium / coarse
    // 颜色
    QString traceColor        = QStringLiteral("#FF0000");
    QString changeColor       = QStringLiteral("#16A34A");
    QString textAddedColor    = QStringLiteral("#FF0000");
    QString textRemovedColor  = QStringLiteral("#0000FF");
    QString markStrokeColor;                          // 空 = 内核自动取深色版
    QString markStyle         = QStringLiteral("semi");      // semi / solid / outline
    QString textInsertDirection = QStringLiteral("down");    // 变更旧(蓝)字插入方向
    // 图形检测
    QString graphicMethod     = QStringLiteral("vector");    // vector / pixel
    double  vectorTolPt       = 1.5;
    double  vectorAngleTolDeg = 12.0;
    double  nearInkTolPt      = 0.5;
    // 差异线
    double  strokeWidthPt     = 1.0;
    double  strokeOpacity     = 0.55;
    QString pageMode          = QStringLiteral("filter");    // filter / pin
    // 报告粒度
    QString reportMode        = QStringLiteral("cluster");   // cluster / run
    double  clusterRadiusPt   = 20.0;
    double  minClusterLengthPt = 0.0;
    double  changeRadiusPt    = 20.0;
    // 文字
    QString textBoxMode       = QStringLiteral("word");      // word / line
    QString textSerialMatchMode = QStringLiteral("loose");   // loose / strict
    // 图号配对
    double  sheetNoRegionX      = 0.60;
    double  sheetNoRegionY      = 0.80;
    int     sheetNoMaxDistinct  = 25;
    // 描红/填充
    int     lineWidth   = 2;
    double  fillOpacity = 0.35;
    int     haloPx      = 2;

    // 转成内核的 options 对象（与命令行/JSON 契约一一对应）。
    QJsonObject toOptions() const;
};

// 内置预设（"比对类型"）。返回预设名列表；apply 一个名到参数对象。
QStringList diffPresetNames();
void        applyDiffPreset(const QString& name, DiffParams* p);

class DiffSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DiffSettingsDialog(const DiffParams& initial, QWidget* parent = nullptr);

    DiffParams params() const { return m_p; }

    // 应用预设（也会同步到对话框控件；用户手动改任意项即转为"自定义"）。
    void applyPreset(const QString& name);

protected:
    void accept() override;      // 关闭前把控件值收回参数对象

private:
    // 用户动了任意一项 ⇒ 预设下拉切到"自定义"（预设只是起点，不反过来覆盖用户）。
    void markCustom();
    void markCustomOnChange(QWidget* w);

    void buildUi();
    void syncFromParams();      // 参数 → 控件
    void syncToParams();        // 控件 → 参数
    QWidget* buildGeneralTab();
    QWidget* buildGraphicTab();
    QWidget* buildTextTab();
    QWidget* buildAdvancedTab();

    DiffParams   m_p;
    bool         m_syncing = false;   // 防止 syncFromParams 触发"自定义"标记

    QComboBox*   m_presetCombo = nullptr;
    QTabWidget*  m_tabs = nullptr;

    // 常规
    QSpinBox*    m_dpi = nullptr;
    QCheckBox*   m_pipe = nullptr;
    QComboBox*   m_prec = nullptr;
    QPushButton* m_traceBtn = nullptr;
    QPushButton* m_changeBtn = nullptr;
    QPushButton* m_addedBtn = nullptr;
    QPushButton* m_removedBtn = nullptr;
    QPushButton* m_strokeBtn = nullptr;
    QComboBox*   m_markStyle = nullptr;
    QComboBox*   m_textDir = nullptr;
    // 图形
    QComboBox*   m_gfxMethod = nullptr;
    QDoubleSpinBox* m_vecTol = nullptr;
    QDoubleSpinBox* m_vecAng = nullptr;
    QDoubleSpinBox* m_nearInk = nullptr;
    QDoubleSpinBox* m_strokeW = nullptr;
    QDoubleSpinBox* m_strokeO = nullptr;
    QDoubleSpinBox* m_clusterR = nullptr;
    QDoubleSpinBox* m_minCluster = nullptr;
    QDoubleSpinBox* m_changeR = nullptr;
    QComboBox*   m_reportMode = nullptr;
    QSpinBox*    m_lw = nullptr;
    QDoubleSpinBox* m_fillOpac = nullptr;
    QSpinBox*    m_halo = nullptr;
    // 文字
    QComboBox*   m_textBox = nullptr;
    QComboBox*   m_textSerial = nullptr;
    QComboBox*   m_pageMode = nullptr;
    QDoubleSpinBox* m_sheetX = nullptr;
    QDoubleSpinBox* m_sheetY = nullptr;
    QSpinBox*    m_sheetMax = nullptr;
};
