#include "diffsettings.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// 参数 → 内核 options
// ---------------------------------------------------------------------------
QJsonObject DiffParams::toOptions() const
{
    QJsonObject opt;
    opt[QStringLiteral("renderDpi")] = renderDpi;
    opt[QStringLiteral("pipeMatch")] = pipeMatch;
    opt[QStringLiteral("precision")] = precision;

    opt[QStringLiteral("lineWidth")]   = lineWidth;
    opt[QStringLiteral("fillOpacity")] = fillOpacity;
    opt[QStringLiteral("haloPx")]      = haloPx;

    QJsonObject colors;
    colors[QStringLiteral("trace")]       = traceColor;
    colors[QStringLiteral("change")]      = changeColor;
    colors[QStringLiteral("textAdded")]   = textAddedColor;
    colors[QStringLiteral("textRemoved")] = textRemovedColor;
    if (!markStrokeColor.isEmpty())
        colors[QStringLiteral("markStroke")] = markStrokeColor;
    opt[QStringLiteral("colors")] = colors;

    opt[QStringLiteral("markStyle")]          = markStyle;
    opt[QStringLiteral("textInsertDirection")] = textInsertDirection;

    opt[QStringLiteral("graphicMethod")]      = graphicMethod;
    opt[QStringLiteral("vectorTolPt")]        = vectorTolPt;
    opt[QStringLiteral("vectorAngleTolDeg")]  = vectorAngleTolDeg;
    opt[QStringLiteral("nearInkTolPt")]       = nearInkTolPt;

    opt[QStringLiteral("reportMode")]          = reportMode;
    opt[QStringLiteral("clusterRadiusPt")]     = clusterRadiusPt;
    opt[QStringLiteral("minClusterLengthPt")]  = minClusterLengthPt;
    opt[QStringLiteral("changeRadiusPt")]      = changeRadiusPt;

    opt[QStringLiteral("textBoxMode")]           = textBoxMode;
    opt[QStringLiteral("textSerialMatchMode")]   = textSerialMatchMode;

    opt[QStringLiteral("sheetNoRegionX")]      = sheetNoRegionX;
    opt[QStringLiteral("sheetNoRegionY")]      = sheetNoRegionY;
    opt[QStringLiteral("sheetNoMaxDistinct")]  = sheetNoMaxDistinct;

    opt[QStringLiteral("strokeWidthPt")] = strokeWidthPt;
    opt[QStringLiteral("strokeOpacity")] = strokeOpacity;
    opt[QStringLiteral("pageMode")]      = pageMode;
    return opt;
}

// ---------------------------------------------------------------------------
// 预设（比对类型）—— 对应 Bluebeam 的 comparison profiles。
// 用户 90% 的场景只需要"默认"；有明确目的时用下面几套，不必逐项调参。
// ---------------------------------------------------------------------------
QStringList diffPresetNames()
{
    return { QStringLiteral("默认（推荐）"),
             QStringLiteral("只看图形"),
             QStringLiteral("只看文字"),
             QStringLiteral("严格（不漏报，噪声多）"),
             QStringLiteral("宽松（少噪声，可能漏报）"),
             QStringLiteral("自定义") };
}

void applyDiffPreset(const QString& name, DiffParams* p)
{
    if (!p) return;
    if (name == QStringLiteral("默认（推荐）")) {
        DiffParams d;
        *p = d;
    } else if (name == QStringLiteral("只看图形")) {
        // 只关心线画/图元变化：把文字侧彻底静音（容差放到不可能命中的程度）。
        DiffParams d;
        d.textBoxMode = QStringLiteral("line");
        d.textSerialMatchMode = QStringLiteral("strict");
        d.changeRadiusPt = 0.0;              // 关闭"变更"第三态
        d.minClusterLengthPt = 0.0;
        *p = d;
    } else if (name == QStringLiteral("只看文字")) {
        // 只关心文字内容变化：把图形检测容差放得极宽 ⇒ 图形几乎全判"相同"。
        DiffParams d;
        d.vectorTolPt = 8.0;
        d.vectorAngleTolDeg = 30.0;
        d.nearInkTolPt = 3.0;
        d.textBoxMode = QStringLiteral("word");
        d.textSerialMatchMode = QStringLiteral("loose");
        *p = d;
    } else if (name == QStringLiteral("严格（不漏报，噪声多）")) {
        DiffParams d;
        d.vectorTolPt = 0.6;
        d.nearInkTolPt = 0.2;
        d.changeRadiusPt = 30.0;
        d.clusterRadiusPt = 10.0;
        d.minClusterLengthPt = 0.0;
        d.reportMode = QStringLiteral("run");
        d.textSerialMatchMode = QStringLiteral("strict");
        d.strokeWidthPt = 1.4;
        *p = d;
    } else if (name == QStringLiteral("宽松（少噪声，可能漏报）")) {
        DiffParams d;
        d.vectorTolPt = 3.0;
        d.nearInkTolPt = 1.2;
        d.clusterRadiusPt = 30.0;
        d.minClusterLengthPt = 12.0;         // 忽略小碎块
        d.reportMode = QStringLiteral("cluster");
        d.changeRadiusPt = 14.0;
        d.strokeWidthPt = 0.8;
        *p = d;
    }
    // "自定义" 不套任何东西。
}

// ---------------------------------------------------------------------------
// 对话框
// ---------------------------------------------------------------------------
namespace {

// 颜色按钮：点击取色，按钮本身当色块用。
QPushButton* makeColorButton(const QString& hex)
{
    auto* b = new QPushButton();
    b->setMinimumWidth(92);
    const QColor c(hex.isEmpty() ? QStringLiteral("#808080") : hex);
    const QString textCol = (c.lightness() > 150) ? QStringLiteral("#000000") : QStringLiteral("#FFFFFF");
    b->setText(hex.isEmpty() ? QStringLiteral("自动") : hex.toUpper());
    b->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;border:1px solid #888;"
                                    "border-radius:4px;font-size:11px;padding:3px;}")
                         .arg(c.name(), textCol));
    b->setProperty("hexValue", hex);
    return b;
}

void refreshColorButton(QPushButton* b, const QString& hex)
{
    if (!b) return;
    const QColor c(hex.isEmpty() ? QStringLiteral("#808080") : hex);
    const QString textCol = (c.lightness() > 150) ? QStringLiteral("#000000") : QStringLiteral("#FFFFFF");
    b->setText(hex.isEmpty() ? QStringLiteral("自动") : hex.toUpper());
    b->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;border:1px solid #888;"
                                    "border-radius:4px;font-size:11px;padding:3px;}")
                         .arg(c.name(), textCol));
    b->setProperty("hexValue", hex);
}

// 带说明文字的"标签 + 控件"行。
QWidget* row(const QString& label, QWidget* w, const QString& tip = QString())
{
    auto* host = new QWidget();
    auto* l = new QHBoxLayout(host);
    l->setContentsMargins(0, 0, 0, 0);
    auto* lb = new QLabel(label);
    lb->setMinimumWidth(132);
    l->addWidget(lb);
    l->addWidget(w, 1);
    if (!tip.isEmpty()) {
        host->setToolTip(tip);
        lb->setToolTip(tip);
        w->setToolTip(tip);
    }
    return host;
}

QDoubleSpinBox* mkD(double min, double max, double step, int decimals)
{
    auto* s = new QDoubleSpinBox();
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    return s;
}

} // namespace

DiffSettingsDialog::DiffSettingsDialog(const DiffParams& initial, QWidget* parent)
    : QDialog(parent), m_p(initial)
{
    buildUi();
    syncFromParams();
}

QWidget* DiffSettingsDialog::buildGeneralTab()
{
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);

    m_dpi = new QSpinBox();  m_dpi->setRange(72, 1200); m_dpi->setSuffix(QStringLiteral(" dpi"));
    m_prec = new QComboBox();
    m_prec->addItem(QStringLiteral("精细（推荐）"), QStringLiteral("fine"));
    m_prec->addItem(QStringLiteral("中等"),        QStringLiteral("medium"));
    m_prec->addItem(QStringLiteral("粗略"),        QStringLiteral("coarse"));
    m_pipe = new QCheckBox(QStringLiteral("同时做「管线号 ↔ 材料表」一一对应检查"));

    l->addWidget(row(QStringLiteral("渲染精度"), m_dpi,
                     QStringLiteral("栅格化兜底路径用的分辨率。纯矢量比对不受它影响。")));
    l->addWidget(row(QStringLiteral("精细度"), m_prec));
    l->addWidget(row(QStringLiteral("交叉检查"), m_pipe));

    auto* colorBox = new QGroupBox(QStringLiteral("标注颜色（直接传给内核，所见即所得）"));
    auto* cl = new QVBoxLayout(colorBox);
    m_traceBtn   = makeColorButton(m_p.traceColor);
    m_changeBtn  = makeColorButton(m_p.changeColor);
    m_addedBtn   = makeColorButton(m_p.textAddedColor);
    m_removedBtn = makeColorButton(m_p.textRemovedColor);
    m_strokeBtn  = makeColorButton(m_p.markStrokeColor);
    cl->addWidget(row(QStringLiteral("新增/删除图形"), m_traceBtn, QStringLiteral("描红线：新增=实线、删除=虚线。")));
    cl->addWidget(row(QStringLiteral("变更（绿）"),     m_changeBtn, QStringLiteral("同位置形状改了的图元。")));
    cl->addWidget(row(QStringLiteral("新增文字 (+)"),   m_addedBtn));
    cl->addWidget(row(QStringLiteral("删除文字 (−)"),   m_removedBtn));
    cl->addWidget(row(QStringLiteral("标记描边色"),     m_strokeBtn, QStringLiteral("留空（显示\"自动\"）= 由内核取标记色的深色版。")));
    l->addWidget(colorBox);

    auto* markBox = new QGroupBox(QStringLiteral("标记形式"));
    auto* ml = new QVBoxLayout(markBox);
    m_markStyle = new QComboBox();
    m_markStyle->addItem(QStringLiteral("半透明填充 + 深色描边（默认，底图可读）"), QStringLiteral("semi"));
    m_markStyle->addItem(QStringLiteral("实心填充（最醒目）"),                     QStringLiteral("solid"));
    m_markStyle->addItem(QStringLiteral("仅描边（最不遮挡）"),                     QStringLiteral("outline"));
    m_textDir = new QComboBox();
    m_textDir->addItem(QStringLiteral("下方"), QStringLiteral("down"));
    m_textDir->addItem(QStringLiteral("上方"), QStringLiteral("up"));
    m_textDir->addItem(QStringLiteral("右侧"), QStringLiteral("right"));
    m_textDir->addItem(QStringLiteral("左侧"), QStringLiteral("left"));
    ml->addWidget(row(QStringLiteral("图形标记"), m_markStyle));
    ml->addWidget(row(QStringLiteral("旧文字插入方向"), m_textDir,
                      QStringLiteral("变更时那条旧的蓝色文字插到新文字的哪一侧。")));
    l->addWidget(markBox);
    l->addStretch();
    return w;
}

QWidget* DiffSettingsDialog::buildGraphicTab()
{
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);

    auto* detBox = new QGroupBox(QStringLiteral("检测方法与容差"));
    auto* dl = new QVBoxLayout(detBox);
    m_gfxMethod = new QComboBox();
    m_gfxMethod->addItem(QStringLiteral("纯矢量（推荐，精确且不看渲染）"), QStringLiteral("vector"));
    m_gfxMethod->addItem(QStringLiteral("渲染 + OpenCV（栅格兜底）"),      QStringLiteral("pixel"));
    m_vecTol  = mkD(0.1, 20.0, 0.1, 2);
    m_vecAng  = mkD(0.0, 90.0, 1.0, 1);
    m_nearInk = mkD(0.0, 10.0, 0.1, 2);
    dl->addWidget(row(QStringLiteral("检测方法"), m_gfxMethod));
    dl->addWidget(row(QStringLiteral("位置容差 (pt)"), m_vecTol,
                      QStringLiteral("小于此距离的图元视为同一处。调大=更宽松、更少噪声。")));
    dl->addWidget(row(QStringLiteral("方向容差 (°)"), m_vecAng,
                      QStringLiteral("共线覆盖匹配允许的方向偏差。")));
    dl->addWidget(row(QStringLiteral("墨迹重合豁免 (pt)"), m_nearInk,
                      QStringLiteral("短线段方向不可靠，距离小于此值即视为已重合（不是放宽容差）。")));
    l->addWidget(detBox);

    auto* repBox = new QGroupBox(QStringLiteral("报告粒度"));
    auto* rl = new QVBoxLayout(repBox);
    m_reportMode = new QComboBox();
    m_reportMode->addItem(QStringLiteral("按区域聚类（一坨算一条，推荐）"), QStringLiteral("cluster"));
    m_reportMode->addItem(QStringLiteral("逐条线段（最细，条数多）"),       QStringLiteral("run"));
    m_clusterR   = mkD(1.0, 200.0, 1.0, 1);
    m_minCluster = mkD(0.0, 500.0, 1.0, 1);
    m_changeR    = mkD(0.0, 100.0, 1.0, 1);
    rl->addWidget(row(QStringLiteral("报告方式"), m_reportMode));
    rl->addWidget(row(QStringLiteral("聚类半径 (pt)"), m_clusterR));
    rl->addWidget(row(QStringLiteral("忽略小区域 < (pt)"), m_minCluster,
                      QStringLiteral("总长小于此值的差异区域不报（压噪声）。0 = 不忽略。")));
    rl->addWidget(row(QStringLiteral("变更识别半径 (pt)"), m_changeR,
                      QStringLiteral("未匹配线段在此半径内能找到同向对侧线段 → 判为「变更」(绿)。0 = 关闭第三态。")));
    l->addWidget(repBox);

    auto* fillBox = new QGroupBox(QStringLiteral("填充外观"));
    auto* fl = new QVBoxLayout(fillBox);
    m_lw       = new QSpinBox(); m_lw->setRange(1, 10);
    m_fillOpac = mkD(0.0, 1.0, 0.05, 2);
    m_halo     = new QSpinBox(); m_halo->setRange(0, 20);
    fl->addWidget(row(QStringLiteral("标记线宽 (px)"), m_lw));
    fl->addWidget(row(QStringLiteral("填充透明度"), m_fillOpac));
    fl->addWidget(row(QStringLiteral("填充外扩 (px)"), m_halo));
    m_strokeW = mkD(0.1, 10.0, 0.1, 2);
    m_strokeO = mkD(0.05, 1.0, 0.05, 2);
    fl->addWidget(row(QStringLiteral("差异线线宽 (pt)"), m_strokeW,
                      QStringLiteral("沿真实变更的那几条线描边，不是填色块。")));
    fl->addWidget(row(QStringLiteral("差异线透明度"), m_strokeO,
                      QStringLiteral("上千条变更也不糊成一片；1.0 = 实色。")));
    l->addWidget(fillBox);
    l->addStretch();
    return w;
}

QWidget* DiffSettingsDialog::buildTextTab()
{
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);

    m_textBox = new QComboBox();
    m_textBox->addItem(QStringLiteral("逐词框（更精确，推荐）"), QStringLiteral("word"));
    m_textBox->addItem(QStringLiteral("整行框"),                 QStringLiteral("line"));
    m_textSerial = new QComboBox();
    m_textSerial->addItem(QStringLiteral("宽松（含行首纯数字）"), QStringLiteral("loose"));
    m_textSerial->addItem(QStringLiteral("严格（只认管段号 / NOTE n）"), QStringLiteral("strict"));

    l->addWidget(row(QStringLiteral("文字框粒度"), m_textBox));
    l->addWidget(row(QStringLiteral("序号匹配"), m_textSerial,
                      QStringLiteral("决定哪些文字参与\"按序号配对\"的比较。")));
    l->addStretch();
    return w;
}

QWidget* DiffSettingsDialog::buildAdvancedTab()
{
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);

    auto* pairBox = new QGroupBox(QStringLiteral("页面配对（图号优先）"));
    auto* pl = new QVBoxLayout(pairBox);
    m_pageMode = new QComboBox();
    m_pageMode->addItem(QStringLiteral("图号优先：所选页只用于筛选（推荐）"), QStringLiteral("filter"));
    m_pageMode->addItem(QStringLiteral("索引钉死：按页号硬配对（仅排障）"),   QStringLiteral("pin"));
    m_sheetX = mkD(0.0, 1.0, 0.05, 2);
    m_sheetY = mkD(0.0, 1.0, 0.05, 2);
    m_sheetMax = new QSpinBox(); m_sheetMax->setRange(1, 500);
    pl->addWidget(row(QStringLiteral("页码语义"), m_pageMode));
    pl->addWidget(row(QStringLiteral("图号区 右 ≥"), m_sheetX,
                      QStringLiteral("相对页面宽度的比例：图号只在此区域右侧寻找。")));
    pl->addWidget(row(QStringLiteral("图号区 下 ≥"), m_sheetY,
                      QStringLiteral("相对页面高度的比例：图号只在此区域下方寻找。")));
    pl->addWidget(row(QStringLiteral("总图判定（种类数 >）"), m_sheetMax,
                      QStringLiteral("单页管段号种类数超过它即判为总图/封面，跳过比对。")));
    l->addWidget(pairBox);
    l->addStretch();
    return w;
}

void DiffSettingsDialog::buildUi()
{
    setWindowTitle(QStringLiteral("比对设置"));
    resize(660, 620);

    auto* root = new QVBoxLayout(this);

    // ---- 预设（比对类型）----
    auto* presetRow = new QHBoxLayout();
    presetRow->addWidget(new QLabel(QStringLiteral("比对类型：")));
    m_presetCombo = new QComboBox();
    m_presetCombo->addItems(diffPresetNames());
    m_presetCombo->setToolTip(QStringLiteral(
        "预设选一个即可，不必逐项调参。\n"
        "改了任意一项会自动变成「自定义」，不会被预设覆盖回去。"));
    presetRow->addWidget(m_presetCombo, 1);
    root->addLayout(presetRow);

    auto* hint = new QLabel(QStringLiteral(
        "参数只是「起点」：选一个比对类型就能跑；只有结果不合预期时才需要动下面的选项。"));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("dlgHint"));
    root->addWidget(hint);

    // ---- 分类标签页 ----
    m_tabs = new QTabWidget();
    m_tabs->addTab(buildGeneralTab(),  QStringLiteral("常规"));
    m_tabs->addTab(buildGraphicTab(),  QStringLiteral("图形差异"));
    m_tabs->addTab(buildTextTab(),     QStringLiteral("文字差异"));
    m_tabs->addTab(buildAdvancedTab(), QStringLiteral("页面配对 / 高级"));
    root->addWidget(m_tabs, 1);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* resetBtn = bb->addButton(QStringLiteral("恢复默认"), QDialogButtonBox::ResetRole);
    root->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(resetBtn, &QPushButton::clicked, this, [this]{
        applyPreset(QStringLiteral("默认（推荐）"));
    });

    // 预设切换
    connect(m_presetCombo, &QComboBox::currentTextChanged, this, [this](const QString& name){
        if (m_syncing) return;
        if (name == QStringLiteral("自定义")) return;
        applyPreset(name);
    });

    // 颜色按钮
    auto pickInto = [this](QString* field, QPushButton* btn, bool allowEmpty) {
        const QColor init = field->isEmpty() ? QColor(0x80, 0x80, 0x80) : QColor(*field);
        const QColor picked = QColorDialog::getColor(init, this, QStringLiteral("选择颜色"));
        if (!picked.isValid()) return;
        *field = picked.name(QColor::HexRgb).toUpper();
        refreshColorButton(btn, *field);
        markCustom();
    };
    connect(m_traceBtn,   &QPushButton::clicked, this, [this, pickInto]{ pickInto(&m_p.traceColor, m_traceBtn, false); });
    connect(m_changeBtn,  &QPushButton::clicked, this, [this, pickInto]{ pickInto(&m_p.changeColor, m_changeBtn, false); });
    connect(m_addedBtn,   &QPushButton::clicked, this, [this, pickInto]{ pickInto(&m_p.textAddedColor, m_addedBtn, false); });
    connect(m_removedBtn, &QPushButton::clicked, this, [this, pickInto]{ pickInto(&m_p.textRemovedColor, m_removedBtn, false); });
    connect(m_strokeBtn,  &QPushButton::clicked, this, [this, pickInto]{
        // 描边色允许"自动"（清空）；右键/清空按钮暂不做，这里用取色 + 一个自动选项的简单交互。
        const QColor init = m_p.markStrokeColor.isEmpty() ? QColor(0x8A, 0x8A, 0x8A) : QColor(m_p.markStrokeColor);
        const QColor picked = QColorDialog::getColor(init, this, QStringLiteral("选择标记描边色（取消 = 保持自动）"));
        if (!picked.isValid()) return;
        m_p.markStrokeColor = picked.name(QColor::HexRgb).toUpper();
        refreshColorButton(m_strokeBtn, m_p.markStrokeColor);
        markCustom();
    });

    // 任意控件改动 → 标为"自定义"
    const QList<QWidget*> widgets = {
        m_dpi, m_prec, m_pipe, m_markStyle, m_textDir,
        m_gfxMethod, m_vecTol, m_vecAng, m_nearInk,
        m_reportMode, m_clusterR, m_minCluster, m_changeR,
        m_lw, m_fillOpac, m_halo, m_strokeW, m_strokeO,
        m_textBox, m_textSerial, m_pageMode, m_sheetX, m_sheetY, m_sheetMax
    };
    for (QWidget* c : widgets) markCustomOnChange(c);
}

void DiffSettingsDialog::applyPreset(const QString& name)
{
    if (name == QStringLiteral("自定义")) return;
    applyDiffPreset(name, &m_p);
    const QString keep = name;
    m_syncing = true;
    syncFromParams();
    m_presetCombo->setCurrentText(keep);
    m_syncing = false;
}

void DiffSettingsDialog::syncFromParams()
{
    m_dpi->setValue(m_p.renderDpi);
    m_prec->setCurrentIndex(qMax(0, m_prec->findData(m_p.precision)));
    m_pipe->setChecked(m_p.pipeMatch);
    refreshColorButton(m_traceBtn,   m_p.traceColor);
    refreshColorButton(m_changeBtn,  m_p.changeColor);
    refreshColorButton(m_addedBtn,   m_p.textAddedColor);
    refreshColorButton(m_removedBtn, m_p.textRemovedColor);
    refreshColorButton(m_strokeBtn,  m_p.markStrokeColor);
    m_markStyle->setCurrentIndex(qMax(0, m_markStyle->findData(m_p.markStyle)));
    m_textDir->setCurrentIndex(qMax(0, m_textDir->findData(m_p.textInsertDirection)));
    m_gfxMethod->setCurrentIndex(qMax(0, m_gfxMethod->findData(m_p.graphicMethod)));
    m_vecTol->setValue(m_p.vectorTolPt);
    m_vecAng->setValue(m_p.vectorAngleTolDeg);
    m_nearInk->setValue(m_p.nearInkTolPt);
    m_reportMode->setCurrentIndex(qMax(0, m_reportMode->findData(m_p.reportMode)));
    m_clusterR->setValue(m_p.clusterRadiusPt);
    m_minCluster->setValue(m_p.minClusterLengthPt);
    m_changeR->setValue(m_p.changeRadiusPt);
    m_lw->setValue(m_p.lineWidth);
    m_fillOpac->setValue(m_p.fillOpacity);
    m_halo->setValue(m_p.haloPx);
    m_strokeW->setValue(m_p.strokeWidthPt);
    m_strokeO->setValue(m_p.strokeOpacity);
    m_textBox->setCurrentIndex(qMax(0, m_textBox->findData(m_p.textBoxMode)));
    m_textSerial->setCurrentIndex(qMax(0, m_textSerial->findData(m_p.textSerialMatchMode)));
    m_pageMode->setCurrentIndex(qMax(0, m_pageMode->findData(m_p.pageMode)));
    m_sheetX->setValue(m_p.sheetNoRegionX);
    m_sheetY->setValue(m_p.sheetNoRegionY);
    m_sheetMax->setValue(m_p.sheetNoMaxDistinct);
}

void DiffSettingsDialog::syncToParams()
{
    m_p.renderDpi = m_dpi->value();
    m_p.precision = m_prec->currentData().toString();
    m_p.pipeMatch = m_pipe->isChecked();
    m_p.markStyle = m_markStyle->currentData().toString();
    m_p.textInsertDirection = m_textDir->currentData().toString();
    m_p.graphicMethod = m_gfxMethod->currentData().toString();
    m_p.vectorTolPt = m_vecTol->value();
    m_p.vectorAngleTolDeg = m_vecAng->value();
    m_p.nearInkTolPt = m_nearInk->value();
    m_p.reportMode = m_reportMode->currentData().toString();
    m_p.clusterRadiusPt = m_clusterR->value();
    m_p.minClusterLengthPt = m_minCluster->value();
    m_p.changeRadiusPt = m_changeR->value();
    m_p.lineWidth = m_lw->value();
    m_p.fillOpacity = m_fillOpac->value();
    m_p.haloPx = m_halo->value();
    m_p.strokeWidthPt = m_strokeW->value();
    m_p.strokeOpacity = m_strokeO->value();
    m_p.textBoxMode = m_textBox->currentData().toString();
    m_p.textSerialMatchMode = m_textSerial->currentData().toString();
    m_p.pageMode = m_pageMode->currentData().toString();
    m_p.sheetNoRegionX = m_sheetX->value();
    m_p.sheetNoRegionY = m_sheetY->value();
    m_p.sheetNoMaxDistinct = m_sheetMax->value();
}

void DiffSettingsDialog::markCustom()
{
    if (m_syncing) return;
    m_presetCombo->setCurrentText(QStringLiteral("自定义"));
}

void DiffSettingsDialog::markCustomOnChange(QWidget* w)
{
    if (auto* cb = qobject_cast<QComboBox*>(w))
        connect(cb, &QComboBox::currentIndexChanged, this, [this](int){ markCustom(); });
    else if (auto* sp = qobject_cast<QSpinBox*>(w))
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int){ markCustom(); });
    else if (auto* dsp = qobject_cast<QDoubleSpinBox*>(w))
        connect(dsp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double){ markCustom(); });
    else if (auto* chk = qobject_cast<QCheckBox*>(w))
        connect(chk, &QCheckBox::toggled, this, [this](bool){ markCustom(); });
}

void DiffSettingsDialog::accept()
{
    syncToParams();
    QDialog::accept();
}
