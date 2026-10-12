#pragma once

#include <QWidget>
#include <QRectF>
#include <QString>

#include "diffsettings.h"

class QLabel;
class QLineEdit;
class QTextEdit;
class QPushButton;
class QComboBox;
class QTabWidget;
class QTableWidget;
class QScrollArea;
class ContinuousPdfView;

// ---------------------------------------------------------------------------
// 「图纸比对（PDF2DF）」页
//
// 按专业比对软件的范式组织（Bluebeam Compare Documents / Acrobat Compare Files）：
//
//   ① 顶部只放"要什么"：基准图 / 目标图 / 输出 / 材料表 + [比对设置…] + [开始比对]。
//      几十个参数收进设置对话框，并给**预设（比对类型）** —— 不再让人在左侧
//      一列控件里上下"推拉"。
//   ② 结果是**真正的查看器**：内嵌 Adobe 式连续滚动视图直接看差异 PDF（默认适合宽度、
//      Ctrl+滚轮缩放、滚动即翻页），而不是把第 1 页拍成一张静态图片。
//   ③ **差异清单**（＝Bluebeam 的 Markups List）：每条差异一行，双击/回车即把查看器
//      跳到该差异并高亮它；带"上一个/下一个差异"与类型筛选 —— 这才是效率核心。
//   ④ 明细（JSON / 管线号↔材料表 / 图例）降级到底部标签页，不占主视区。
//
// 另修一个真 bug：差异 PDF 以前写在 onRun 的局部 QTemporaryDir 中，函数一返回就被
// 删除，用户其实**拿不到结果文件**。现在落到用户可见的输出目录，并支持"打开输出目录"。
// ---------------------------------------------------------------------------
class DiffPage : public QWidget
{
    Q_OBJECT
public:
    explicit DiffPage(QWidget* parent = nullptr);

    // CLI kernel directory (set by MainWindow whenever it changes).
    void setCliDir(const QString& dir) { m_cliDir = dir; refreshKernelStatus(); }
    // 内核调用方式（auto/embedded/cli）—— 由 MainWindow 下发，融合层据此选路。
    void setKernelBackend(const QString& mode) { m_kernelBackend = mode; }

    // ---- 自检钩子（只给无头测试用，不参与业务逻辑）----
    // 直接喂一份内核返回样式的 JSON，验证「差异清单 / 筛选 / 跳转」这套逻辑。
    // 这样不必每次跑几分钟的真实比对就能回归核心交互。
    void   feedDiffResultForTest(const QJsonObject& root);
    int    diffRowCountForTest() const;
    int    diffVisibleCountForTest() const;
    void   setFilterForTest(const QString& id);
    void   selectDiffRowForTest(int row);
    int    lastGotoPageForTest() const { return m_lastGotoPage; }
    QRectF lastGotoRectForTest() const { return m_lastGotoRect; }

signals:
    void statusMessage(const QString& msg);

private slots:
    void onPickBaseline();
    void onPickTarget();
    void onPickCsv();
    void onClearCsv();
    void onPickOutDir();
    void onOpenOutDir();
    void onOpenSettings();
    void updateSettingsSummary();
    void onRun();
    void onDiffRowActivated(int row, int col);
    void onNextDiff();
    void onPrevDiff();

private:
    QString cliExe(const QString& name) const;
    void refreshKernelStatus();
    bool outDirReady(bool createIfNeeded);
    // 把 JSON 里的 diffs[] 填进清单表；返回条数。
    int  fillDiffList(const QJsonObject& root);
    void showPipeMatch(const QJsonObject& root);
    void gotoDiff(int listRow);
    void updateDiffNav();
    void applyFilter();

    QString m_cliDir;
    QString m_kernelBackend = QStringLiteral("auto");
    DiffParams m_params;

    QLineEdit* m_baseEdit = nullptr;
    QLineEdit* m_tgtEdit  = nullptr;
    QLineEdit* m_csvEdit  = nullptr;
    QLineEdit* m_outEdit  = nullptr;
    QLabel*    m_status   = nullptr;
    QLabel*    m_settingsSummary = nullptr;
    QPushButton* m_runBtn = nullptr;
    QPushButton* m_openOutBtn = nullptr;

    // 结果：查看器 + 差异清单
    ContinuousPdfView* m_viewer = nullptr;
    QScrollArea*       m_viewScroll = nullptr;
    QTableWidget*      m_diffTable = nullptr;
    QComboBox*         m_filterCombo = nullptr;
    QLabel*            m_diffCountLabel = nullptr;
    QPushButton*       m_prevBtn = nullptr;
    QPushButton*       m_nextBtn = nullptr;
    QLabel*            m_zoomLabel = nullptr;

    QTabWidget* m_detailTabs = nullptr;
    QTextEdit*  m_jsonView = nullptr;
    QTextEdit*  m_pipeView = nullptr;

    QString m_outPdfPath;      // 上一次比对产出的差异 PDF（真实路径，不是临时目录）
    int     m_curDiffRow = -1; // 当前定位到的清单行（-1 = 无）
    int     m_lastGotoPage = -1;   // 最近一次定位到的页（自检/状态显示用）
    QRectF  m_lastGotoRect;        // 最近一次定位到的区域（pt）
};
