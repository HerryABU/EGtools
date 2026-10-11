#include "diffpage.h"
#include "clirunner.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QColor>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPixmap>
#include <QImageReader>
#include <QSplitter>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QTemporaryDir>

DiffPage::DiffPage(QWidget* parent) : QWidget(parent)
{
    auto* baseRow = new QHBoxLayout();
    m_baseEdit = new QLineEdit();
    m_baseEdit->setPlaceholderText(QStringLiteral("基准图 (baseline) PDF"));
    auto* baseBtn = new QPushButton(QStringLiteral("选择…"));
    baseRow->addWidget(new QLabel(QStringLiteral("基准图:")));
    baseRow->addWidget(m_baseEdit, 1);
    baseRow->addWidget(baseBtn);

    auto* tgtRow = new QHBoxLayout();
    m_tgtEdit = new QLineEdit();
    m_tgtEdit->setPlaceholderText(QStringLiteral("目标图 (target) PDF"));
    auto* tgtBtn = new QPushButton(QStringLiteral("选择…"));
    tgtRow->addWidget(new QLabel(QStringLiteral("目标图:")));
    tgtRow->addWidget(m_tgtEdit, 1);
    tgtRow->addWidget(tgtBtn);

    auto* csvRow = new QHBoxLayout();
    m_csvEdit = new QLineEdit();
    m_csvEdit->setPlaceholderText(QStringLiteral("人工修正材料表 (CSV/TSV，可选)"));
    auto* csvBtn = new QPushButton(QStringLiteral("选择…"));
    auto* csvClr = new QPushButton(QStringLiteral("清除"));
    csvRow->addWidget(new QLabel(QStringLiteral("材料表:")));
    csvRow->addWidget(m_csvEdit, 1);
    csvRow->addWidget(csvBtn);
    csvRow->addWidget(csvClr);

    auto* optRow = new QHBoxLayout();
    m_dpiSpin = new QSpinBox();
    m_dpiSpin->setRange(60, 400);
    m_dpiSpin->setValue(200);
    m_dpiSpin->setSuffix(QStringLiteral(" DPI"));
    m_pipeChk = new QCheckBox(QStringLiteral("管线号↔材料表一一对应"));
    m_pipeChk->setChecked(true);
    // 精细度：用户调参（灵敏捕捉微小差异 ↔ 粗略忽略小差异与几毫米偏移）。
    m_precCombo = new QComboBox();
    m_precCombo->addItem(QStringLiteral("灵敏 (fine)"),   QStringLiteral("fine"));
    m_precCombo->addItem(QStringLiteral("标准 (medium)"), QStringLiteral("medium"));
    m_precCombo->addItem(QStringLiteral("粗略 (coarse)"), QStringLiteral("coarse"));
    m_precCombo->setCurrentIndex(1);
    optRow->addWidget(new QLabel(QStringLiteral("渲染:")));
    optRow->addWidget(m_dpiSpin);
    optRow->addWidget(new QLabel(QStringLiteral("精细度:")));
    optRow->addWidget(m_precCombo);
    optRow->addWidget(m_pipeChk);
    optRow->addStretch();

    // ---- 颜色选择器：图形描红 / 变更绿 / 新增文字(红) / 删除文字(蓝)（色号传入内核 options.colors） ----
    auto* colRow = new QHBoxLayout();
    m_traceColorBtn  = new QPushButton();
    m_changeColorBtn = new QPushButton();
    m_textAddedColorBtn = new QPushButton();
    m_textRemovedColorBtn = new QPushButton();
    for (QPushButton* b : {m_traceColorBtn, m_changeColorBtn, m_textAddedColorBtn, m_textRemovedColorBtn}) {
        b->setFixedSize(56, 24);
        b->setCursor(Qt::PointingHandCursor);
    }
    paintSwatch(m_traceColorBtn,  m_traceColor);
    paintSwatch(m_changeColorBtn, m_changeColor);
    paintSwatch(m_textAddedColorBtn, m_textAddedColor);
    paintSwatch(m_textRemovedColorBtn, m_textRemovedColor);
    colRow->addWidget(new QLabel(QStringLiteral("颜色:")));
    colRow->addWidget(new QLabel(QStringLiteral("图形描红")));
    colRow->addWidget(m_traceColorBtn);
    colRow->addWidget(new QLabel(QStringLiteral("变更(绿)")));
    colRow->addWidget(m_changeColorBtn);
    colRow->addWidget(new QLabel(QStringLiteral("新增文字(+)")));
    colRow->addWidget(m_textAddedColorBtn);
    colRow->addWidget(new QLabel(QStringLiteral("删除文字(-)")));
    colRow->addWidget(m_textRemovedColorBtn);
    colRow->addWidget(new QLabel(QStringLiteral("变更旧字方向:")));
    m_textDirCombo = new QComboBox();
    m_textDirCombo->addItem(QStringLiteral("下 (down)"),   QStringLiteral("down"));
    m_textDirCombo->addItem(QStringLiteral("上 (up)"),     QStringLiteral("up"));
    m_textDirCombo->addItem(QStringLiteral("右 (right)"),  QStringLiteral("right"));
    m_textDirCombo->addItem(QStringLiteral("左 (left)"),   QStringLiteral("left"));
    m_textDirCombo->setCurrentIndex(0);   // 默认 下
    colRow->addWidget(m_textDirCombo);
    colRow->addStretch();

    // ---- 图形标记形式 + 描边色（高密度图纸默认淡填充+深色描边，避免实心块遮挡底图） ----
    auto* mkRow = new QHBoxLayout();
    m_markStyleCombo = new QComboBox();
    m_markStyleCombo->addItem(QStringLiteral("半透明填充 + 深色描边（推荐）"), QStringLiteral("semi"));
    m_markStyleCombo->addItem(QStringLiteral("实心填充（标记最强）"),          QStringLiteral("solid"));
    m_markStyleCombo->addItem(QStringLiteral("仅描边，不填充（最不遮挡）"),    QStringLiteral("outline"));
    m_markStyleCombo->setCurrentIndex(0);          // 默认 semi
    m_markStrokeBtn = new QPushButton();
    m_markStrokeBtn->setFixedSize(56, 24);
    m_markStrokeBtn->setCursor(Qt::PointingHandCursor);
    m_markStrokeBtn->setToolTip(QStringLiteral("留空则由内核自动取标记色的深色版"));
    paintSwatch(m_markStrokeBtn, m_markStrokeColor.isEmpty() ? QStringLiteral("#8A8A8A") : m_markStrokeColor);
    mkRow->addWidget(new QLabel(QStringLiteral("图形标记形式:")));
    mkRow->addWidget(m_markStyleCombo);
    mkRow->addWidget(new QLabel(QStringLiteral("描边色(留空=自动):")));
    mkRow->addWidget(m_markStrokeBtn);
    mkRow->addStretch();

    // ---- 矢量检测参数（纯矢量解析，误报率低；参数可调） ----
    auto* vecRow = new QHBoxLayout();
    m_gfxMethodCombo = new QComboBox();
    m_gfxMethodCombo->addItem(QStringLiteral("矢量（推荐，误报低）"), QStringLiteral("vector"));
    m_gfxMethodCombo->addItem(QStringLiteral("像素（旧路径，回退用）"), QStringLiteral("pixel"));
    m_gfxMethodCombo->setCurrentIndex(0);
    m_gfxMethodCombo->setToolTip(QStringLiteral("矢量：解析 PDF 内容流取线画比对，不做位图渲染。\n像素：旧的两版渲染成位图逐像素比对，对配准误差零容忍。"));
    m_vecTolSpin = new QDoubleSpinBox();
    m_vecTolSpin->setRange(0.5, 30.0); m_vecTolSpin->setSingleStep(0.5);
    m_vecTolSpin->setValue(1.5); m_vecTolSpin->setSuffix(QStringLiteral(" pt"));
    m_vecTolSpin->setToolTip(QStringLiteral("两版同名线画允许的位置偏差。调小更灵敏（可能把导出误差报成差异），调大更钝。"));
    m_vecAngSpin = new QDoubleSpinBox();
    m_vecAngSpin->setRange(1.0, 60.0); m_vecAngSpin->setSingleStep(1.0);
    m_vecAngSpin->setValue(12.0); m_vecAngSpin->setSuffix(QStringLiteral(" °"));
    m_vecAngSpin->setToolTip(QStringLiteral("方向容差：防密集区被横穿的无关线段误判为覆盖。"));
    vecRow->addWidget(new QLabel(QStringLiteral("图形检测:")));
    vecRow->addWidget(m_gfxMethodCombo);
    vecRow->addWidget(new QLabel(QStringLiteral("容差:")));
    vecRow->addWidget(m_vecTolSpin);
    vecRow->addWidget(new QLabel(QStringLiteral("方向:")));
    vecRow->addWidget(m_vecAngSpin);
    vecRow->addWidget(new QLabel(QStringLiteral("墨迹重合<:")));
    m_nearInkSpin = new QDoubleSpinBox();
    m_nearInkSpin->setRange(0.0, 5.0); m_nearInkSpin->setSingleStep(0.1);
    m_nearInkSpin->setValue(0.5); m_nearInkSpin->setSuffix(QStringLiteral(" pt"));
    m_nearInkSpin->setToolTip(QStringLiteral("短线段(圆弧逼近段)方向不可靠，若距对侧墨迹小于此值则仍判未变。\n"
        "它不是放宽位置容差：只在墨迹实际重合到亚 pt 时生效。0=关闭。"));
    vecRow->addWidget(m_nearInkSpin);
    vecRow->addStretch();

    // ---- 差异线样式（沿真实变更线段描边：细线 + 半透明，不遮底图） ----
    auto* stRow = new QHBoxLayout();
    m_strokeWidthSpin = new QDoubleSpinBox();
    m_strokeWidthSpin->setRange(0.2, 10.0); m_strokeWidthSpin->setSingleStep(0.1);
    m_strokeWidthSpin->setValue(1.0); m_strokeWidthSpin->setSuffix(QStringLiteral(" pt"));
    m_strokeWidthSpin->setToolTip(QStringLiteral("差异线线宽。CAD 一次修订常有上千条线变更，细线更清楚。"));
    m_strokeOpacitySpin = new QDoubleSpinBox();
    m_strokeOpacitySpin->setRange(0.05, 1.0); m_strokeOpacitySpin->setSingleStep(0.05);
    m_strokeOpacitySpin->setValue(0.55);
    m_strokeOpacitySpin->setToolTip(QStringLiteral("差异线不透明度。调低 → 更淡、底图更可读；1.0 → 实色。"));
    m_pageModeCombo = new QComboBox();
    m_pageModeCombo->addItem(QStringLiteral("图号优先（先用图号配对，页码=筛选）"), QStringLiteral("filter"));
    m_pageModeCombo->addItem(QStringLiteral("索引钉死（页码=按序号硬配）"), QStringLiteral("pin"));
    m_pageModeCombo->setCurrentIndex(0);
    m_pageModeCombo->setToolTip(QStringLiteral("选择页码时的语义：\n"
        "图号优先 = 先按标题栏图号做全本配对，再把所选页码(基准页号)在配对结果里筛选；\n"
        "索引钉死 = 旧行为，所选页 p 直接与目标第 p 页比（新版插页后会错位，仅排障用）。"));
    stRow->addWidget(new QLabel(QStringLiteral("差异线宽:")));
    stRow->addWidget(m_strokeWidthSpin);
    stRow->addWidget(new QLabel(QStringLiteral("透明度:")));
    stRow->addWidget(m_strokeOpacitySpin);
    stRow->addWidget(new QLabel(QStringLiteral("页码语义:")));
    stRow->addWidget(m_pageModeCombo);
    stRow->addStretch();

    // ---- 报告粒度与差异密度过滤（差异太多/太碎时调这里） ----
    auto* repRow = new QHBoxLayout();
    m_reportModeCombo = new QComboBox();
    m_reportModeCombo->addItem(QStringLiteral("按区域汇总（推荐）"), QStringLiteral("cluster"));
    m_reportModeCombo->addItem(QStringLiteral("逐条折线（最详细）"), QStringLiteral("run"));
    m_reportModeCombo->setCurrentIndex(0);
    m_clusterRadiusSpin = new QDoubleSpinBox();
    m_clusterRadiusSpin->setRange(0.0, 200.0); m_clusterRadiusSpin->setSingleStep(2.0);
    m_clusterRadiusSpin->setValue(24.0); m_clusterRadiusSpin->setSuffix(QStringLiteral(" pt"));
    m_clusterRadiusSpin->setToolTip(QStringLiteral("邻近折线归为同一差异区域的距离阈值。调大 → 区域更少更聚合。"));
    m_minClusterLenSpin = new QDoubleSpinBox();
    m_minClusterLenSpin->setRange(0.0, 2000.0); m_minClusterLenSpin->setSingleStep(5.0);
    m_minClusterLenSpin->setValue(0.0); m_minClusterLenSpin->setSuffix(QStringLiteral(" pt"));
    m_minClusterLenSpin->setToolTip(QStringLiteral("丢弃总长小于此值的差异区域（把零散微调整体忽略）。0=不过滤。"));
    m_changeRadiusSpin = new QDoubleSpinBox();
    m_changeRadiusSpin->setRange(0.0, 500.0); m_changeRadiusSpin->setSingleStep(2.0);
    m_changeRadiusSpin->setValue(20.0); m_changeRadiusSpin->setSuffix(QStringLiteral(" pt"));
    m_changeRadiusSpin->setToolTip(QStringLiteral("第三态\"变更\"识别半径：未匹配线画若在此半径内有方向相近的对侧线画，\n判为\"变更\"(绿) 而非\"删一根+加一根\"(红)。0=关闭第三态。"));
    repRow->addWidget(new QLabel(QStringLiteral("图形报告:")));
    repRow->addWidget(m_reportModeCombo);
    repRow->addWidget(new QLabel(QStringLiteral("聚类半径:")));
    repRow->addWidget(m_clusterRadiusSpin);
    repRow->addWidget(new QLabel(QStringLiteral("忽略小区域<:")));
    repRow->addWidget(m_minClusterLenSpin);
    repRow->addWidget(new QLabel(QStringLiteral("变更识别半径:")));
    repRow->addWidget(m_changeRadiusSpin);
    repRow->addStretch();

    // ---- 文字差异参数（框粒度 / 序号匹配宽严） ----
    auto* txtRow = new QHBoxLayout();
    m_textBoxModeCombo = new QComboBox();
    m_textBoxModeCombo->addItem(QStringLiteral("逐词框（精确，推荐）"), QStringLiteral("word"));
    m_textBoxModeCombo->addItem(QStringLiteral("整行框"), QStringLiteral("line"));
    m_textBoxModeCombo->setCurrentIndex(0);
    m_textBoxModeCombo->setToolTip(QStringLiteral("逐词框只改真正变动的词，不牵连同行其它文字。"));
    m_textSerialCombo = new QComboBox();
    m_textSerialCombo->addItem(QStringLiteral("宽松（含材料表行首数字）"), QStringLiteral("loose"));
    m_textSerialCombo->addItem(QStringLiteral("严格（只认管段号/NOTE n）"), QStringLiteral("strict"));
    m_textSerialCombo->setCurrentIndex(0);
    m_textSerialCombo->setToolTip(QStringLiteral("若出现毫不相关的两行被报成\"变更\"（两版行号含义不同），改严格模式。"));
    txtRow->addWidget(new QLabel(QStringLiteral("文字框:")));
    txtRow->addWidget(m_textBoxModeCombo);
    txtRow->addWidget(new QLabel(QStringLiteral("序号匹配:")));
    txtRow->addWidget(m_textSerialCombo);
    txtRow->addStretch();

    // ---- 图号配对（标题栏图号 = 每页唯一号；同图号才比对） ----
    auto* shRow = new QHBoxLayout();
    m_sheetNoXSpin = new QDoubleSpinBox();
    m_sheetNoXSpin->setRange(0.0, 1.0); m_sheetNoXSpin->setSingleStep(0.01); m_sheetNoXSpin->setDecimals(2);
    m_sheetNoXSpin->setValue(0.85);
    m_sheetNoXSpin->setToolTip(QStringLiteral("图号字段所处的页面右侧区域起点（按文本范围比例）。\n标题栏布局不同时调这里。"));
    m_sheetNoYSpin = new QDoubleSpinBox();
    m_sheetNoYSpin->setRange(0.0, 1.0); m_sheetNoYSpin->setSingleStep(0.01); m_sheetNoYSpin->setDecimals(2);
    m_sheetNoYSpin->setValue(0.78);
    m_sheetNoYSpin->setToolTip(QStringLiteral("图号字段所处的页面下方区域起点（按文本范围比例）。"));
    m_sheetNoMaxSpin = new QSpinBox();
    m_sheetNoMaxSpin->setRange(1, 9999); m_sheetNoMaxSpin->setValue(25);
    m_sheetNoMaxSpin->setToolTip(QStringLiteral("单页\"管段号种类数\"上限：超过即视为总图/封面，不认其图号（改由线画指纹兜底）。\n实测真图纸 ≤10 种、总图 33~127 种。"));
    shRow->addWidget(new QLabel(QStringLiteral("图号区 右≥:")));
    shRow->addWidget(m_sheetNoXSpin);
    shRow->addWidget(new QLabel(QStringLiteral("下≥:")));
    shRow->addWidget(m_sheetNoYSpin);
    shRow->addWidget(new QLabel(QStringLiteral("总图判定(种类数>):")));
    shRow->addWidget(m_sheetNoMaxSpin);
    shRow->addStretch();

    // ---- 描红/填充可调参数：线宽粗细 / 填充透明度 / 填充外扩 ----
    auto* parRow = new QHBoxLayout();
    m_lwSpin = new QSpinBox();
    m_lwSpin->setRange(1, 12);
    m_lwSpin->setValue(3);
    m_lwSpin->setSuffix(QStringLiteral(" px"));
    m_opacSpin = new QDoubleSpinBox();
    m_opacSpin->setRange(0.0, 1.0);
    m_opacSpin->setSingleStep(0.05);
    m_opacSpin->setValue(0.40);
    m_haloSpin = new QSpinBox();
    m_haloSpin->setRange(0, 20);
    m_haloSpin->setValue(0);
    m_haloSpin->setSuffix(QStringLiteral(" px"));
    parRow->addWidget(new QLabel(QStringLiteral("线宽:")));
    parRow->addWidget(m_lwSpin);
    parRow->addWidget(new QLabel(QStringLiteral("填充透明度:")));
    parRow->addWidget(m_opacSpin);
    parRow->addWidget(new QLabel(QStringLiteral("填充外扩:")));
    parRow->addWidget(m_haloSpin);
    parRow->addStretch();

    m_runBtn = new QPushButton(QStringLiteral("开始比对（图形标记按所选形式·文字直接改色：新增红(+)/删除蓝(-)）"));
    m_runBtn->setStyleSheet(QStringLiteral("QPushButton{background:#2d7d46;color:#fff;padding:6px 12px;border-radius:5px;font-weight:600;}"));
    auto* openBtn = new QPushButton(QStringLiteral("打开差异图…"));
    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(m_runBtn);
    btnRow->addWidget(openBtn);
    btnRow->addStretch();

    m_status = new QLabel(QStringLiteral("内核状态：未探测"));
    m_status->setWordWrap(true);

    // ---- 图例 ----
    auto* legend = new QGroupBox(QStringLiteral("图例（差异直接标注在新 PDF 中）"));
    auto* legendLay = new QVBoxLayout(legend);
    legendLay->addWidget(new QLabel(QStringLiteral(
        "● <b>图形标记形式</b>（可切换）：<b>semi</b> 半透明填充+深色描边（默认，底图仍可读）／"
        "<b>solid</b> 实心填充（标记最强）／<b>outline</b> 仅描边（最不遮挡）\n"
        "● <b>红(实线)</b> = 新增图形　<b>红(虚线)</b> = 删除图形　<b>绿</b> = 变更（同位置形状改了）\n"
        "● <b>差异线样式</b>：沿【真实变更的那几条线】描边（不是填充块），默认 <b>细线 1.0pt + 半透明 0.55</b>"
        "（上千条变更也不糊成一片、不遮底图）；线宽/透明度可调，1.0 = 实色\n"
        "● <b>页面配对</b>：先按【图号】（标题栏右下角的每页唯一号，如 408.101.P02）1:1 配对，"
        "图号只在一侧的页直接判为新增/删除、<b>不参与比对</b>；无图号的页（封面/总图）用线画指纹兜底\n"
        "● <b>页码语义</b>：默认「图号优先」——所选页码只用于在配对结果里<b>筛选</b>（不会因新版插页而错配）；"
        "「索引钉死」为旧行为，仅排障用\n"
        "● <b>文字直接改色（不画圈）</b>：<font color=\"#FF0000\">新增文字 = 红色</font>（覆盖在原黑色文字上，改原字色），"
        "<font color=\"#0000FF\">删除文字 = 蓝色</font>（插入到原文本框，git - 行）；变更则红(新,改原字色)+蓝(旧,按方向插入)\n"
        "记号色/描边色/线宽/透明度/标记形式/差异线宽与透明度/检测方法/容差/报告粒度/文字框粒度/图号区/页码语义均可自定，色号直接传给内核。")));
    legendLay->setSpacing(2);

    // ---- 管线号↔材料表一一对应结果 ----
    auto* pipeBox = new QGroupBox(QStringLiteral("管线号 ↔ 材料表 一一对应"));
    auto* pipeLay = new QVBoxLayout(pipeBox);
    m_pipeView = new QTextEdit();
    m_pipeView->setReadOnly(true);
    m_pipeView->setLineWrapMode(QTextEdit::NoWrap);
    m_pipeView->setPlainText(QStringLiteral("运行后显示：匹配 / 图中独有(材料表缺) / 材料表独有(图中未画)"));
    pipeLay->addWidget(m_pipeView);

    auto* ctrl = new QWidget();
    auto* ctrlLay = new QVBoxLayout(ctrl);
    ctrlLay->setContentsMargins(8, 8, 8, 8);
    ctrlLay->addLayout(baseRow);
    ctrlLay->addLayout(tgtRow);
    ctrlLay->addLayout(csvRow);
    ctrlLay->addLayout(optRow);
    ctrlLay->addLayout(colRow);
    ctrlLay->addLayout(mkRow);
    ctrlLay->addLayout(vecRow);
    ctrlLay->addLayout(stRow);
    ctrlLay->addLayout(repRow);
    ctrlLay->addLayout(txtRow);
    ctrlLay->addLayout(shRow);
    ctrlLay->addLayout(parRow);
    ctrlLay->addLayout(btnRow);
    ctrlLay->addWidget(m_status);
    ctrlLay->addWidget(legend);
    ctrlLay->addWidget(pipeBox, 1);
    ctrlLay->addStretch();

    m_jsonView = new QTextEdit();
    m_jsonView->setReadOnly(true);
    m_jsonView->setLineWrapMode(QTextEdit::NoWrap);
    m_jsonView->setPlainText(QStringLiteral("差异 JSON 摘要（运行后显示）"));

    m_preview = new QLabel(QStringLiteral("差异标注预览（运行后显示）"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setStyleSheet(QStringLiteral("QLabel{background:#f4f4f4;border:1px solid #ccc;border-radius:5px;color:#666;}"));
    m_preview->setMinimumHeight(280);

    auto* right = new QSplitter(Qt::Vertical);
    right->addWidget(m_preview);
    right->addWidget(m_jsonView);
    right->setStretchFactor(0, 2);
    right->setStretchFactor(1, 3);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(ctrl);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(split);

    connect(baseBtn, &QPushButton::clicked, this, &DiffPage::onPickBaseline);
    connect(tgtBtn,  &QPushButton::clicked, this, &DiffPage::onPickTarget);
    connect(csvBtn,  &QPushButton::clicked, this, &DiffPage::onPickCsv);
    connect(csvClr,  &QPushButton::clicked, this, &DiffPage::onClearCsv);
    connect(m_runBtn,&QPushButton::clicked, this, &DiffPage::onRun);
    connect(openBtn, &QPushButton::clicked, this, &DiffPage::onBrowsePreview);
    connect(m_traceColorBtn,  &QPushButton::clicked, this, &DiffPage::onPickTraceColor);
    connect(m_changeColorBtn, &QPushButton::clicked, this, &DiffPage::onPickChangeColor);
    connect(m_textAddedColorBtn, &QPushButton::clicked, this, &DiffPage::onPickTextAddedColor);
    connect(m_textRemovedColorBtn, &QPushButton::clicked, this, &DiffPage::onPickTextRemovedColor);
    connect(m_markStrokeBtn, &QPushButton::clicked, this, &DiffPage::onPickMarkStrokeColor);
}

// 同步颜色按钮的色块：背景=当前颜色；深色底自动换白字，浅色底用黑字。
void DiffPage::paintSwatch(QPushButton* btn, const QString& hex)
{
    if (!btn) return;
    const QColor c(hex);
    const QString textCol = (c.lightness() > 150) ? QStringLiteral("#000000") : QStringLiteral("#FFFFFF");
    btn->setText(hex.toUpper());
    btn->setStyleSheet(QStringLiteral(
        "QPushButton{background:%1;color:%2;border:1px solid #888;border-radius:4px;font-size:11px;}")
        .arg(c.name(), textCol));
}

// 打开系统取色器；确认后写入 color 并刷新按钮色块。
bool DiffPage::pickColor(const QString& title, QString& color, QPushButton* btn)
{
    const QColor init(color);
    const QColor picked = QColorDialog::getColor(init, this, title);
    if (!picked.isValid()) return false;
    color = picked.name(QColor::HexRgb).toUpper();   // 规范化 #RRGGBB
    paintSwatch(btn, color);
    return true;
}

void DiffPage::onPickTraceColor()
{
    if (pickColor(QStringLiteral("选择描红线颜色（新增/删除图形）"), m_traceColor, m_traceColorBtn))
        emit statusMessage(QStringLiteral("描红线颜色 = %1").arg(m_traceColor));
}

void DiffPage::onPickChangeColor()
{
    if (pickColor(QStringLiteral("选择变更绿颜色（内容改动图形/文字）"), m_changeColor, m_changeColorBtn))
        emit statusMessage(QStringLiteral("变更绿颜色 = %1").arg(m_changeColor));
}

void DiffPage::onPickTextAddedColor()
{
    if (pickColor(QStringLiteral("选择新增文字颜色（git +，默认红）"), m_textAddedColor, m_textAddedColorBtn))
        emit statusMessage(QStringLiteral("新增文字颜色 = %1").arg(m_textAddedColor));
}

void DiffPage::onPickTextRemovedColor()
{
    if (pickColor(QStringLiteral("选择删除文字颜色（git -，默认蓝）"), m_textRemovedColor, m_textRemovedColorBtn))
        emit statusMessage(QStringLiteral("删除文字颜色 = %1").arg(m_textRemovedColor));
}

void DiffPage::onPickMarkStrokeColor()
{
    const QColor init = m_markStrokeColor.isEmpty() ? QColor(0x8A, 0x8A, 0x8A) : QColor(m_markStrokeColor);
    const QColor picked = QColorDialog::getColor(init, this, QStringLiteral("选择图形标记描边色（留空则自动取标记色深色版）"));
    if (!picked.isValid()) return;
    m_markStrokeColor = picked.name(QColor::HexRgb).toUpper();
    paintSwatch(m_markStrokeBtn, m_markStrokeColor);
    emit statusMessage(QStringLiteral("图形标记描边色 = %1").arg(m_markStrokeColor));
}

QString DiffPage::cliExe(const QString& name) const
{
    const QString exe = name + QStringLiteral(".exe");
    QStringList cands;
    auto add = [&](const QString& d){ if (!d.isEmpty()) cands << QDir(d).filePath(exe); };
    add(m_cliDir);
    const QString appDir = QCoreApplication::applicationDirPath();
    add(appDir);
    add(QDir(appDir).filePath(QStringLiteral("cli")));
    add(QDir(appDir).filePath(QStringLiteral("..")));
    add(QDir(appDir).filePath(QStringLiteral("../cli")));
    for (const QString& c : cands) if (QFileInfo::exists(c)) return c;
    return cands.isEmpty() ? exe : cands.first();
}

void DiffPage::refreshKernelStatus()
{
    bool ok = QFileInfo::exists(cliExe(QStringLiteral("EGpdf2df")));
    m_status->setText(ok ? QStringLiteral("内核状态：已就绪（EGpdf2df）")
                         : QStringLiteral("内核状态：未找到 EGpdf2df.exe — 请在首页设置 CLI 目录"));
    m_runBtn->setEnabled(ok);
}

void DiffPage::onPickBaseline()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择基准图 PDF"),
        QString(), QStringLiteral("PDF (*.pdf)"));
    if (!p.isEmpty()) m_baseEdit->setText(p);
}

void DiffPage::onPickTarget()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择目标图 PDF"),
        QString(), QStringLiteral("PDF (*.pdf)"));
    if (!p.isEmpty()) m_tgtEdit->setText(p);
}

void DiffPage::onPickCsv()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择人工修正材料表"),
        QString(), QStringLiteral("CSV/TSV (*.csv *.tsv *.txt)"));
    if (!p.isEmpty()) m_csvEdit->setText(p);
}

void DiffPage::onClearCsv()
{
    m_csvEdit->clear();
}

void DiffPage::onRun()
{
    const QString a = m_baseEdit->text().trimmed();
    const QString b = m_tgtEdit->text().trimmed();
    if (a.isEmpty() || b.isEmpty()) {
        emit statusMessage(QStringLiteral("请先选择基准图与目标图"));
        return;
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) { emit statusMessage(QStringLiteral("无法创建临时目录")); return; }
    QString reqPath = QDir(tmp.path()).filePath(QStringLiteral("req.json"));
    QString outPdf  = QDir(tmp.path()).filePath(QStringLiteral("diff.pdf"));

    // 构造请求 JSON（含选项、精细度、三个自定义颜色与可选的人工修正材料表）。
    QJsonObject opt;
    opt[QStringLiteral("renderDpi")] = m_dpiSpin->value();
    opt[QStringLiteral("pipeMatch")] = m_pipeChk->isChecked();
    if (m_precCombo)
        opt[QStringLiteral("precision")] = m_precCombo->currentData().toString();
    // 描红/填充可调参数（线宽粗细 / 填充透明度 / 填充外扩像素）。
    opt[QStringLiteral("lineWidth")]   = m_lwSpin->value();
    opt[QStringLiteral("fillOpacity")] = m_opacSpin->value();
    opt[QStringLiteral("haloPx")]      = m_haloSpin->value();
    // 颜色选择器 → options.colors（图形描红 / 变更绿 / 新增文字红 / 删除文字蓝），内核据色号着色。
    QJsonObject colors;
    colors[QStringLiteral("trace")]  = m_traceColor;
    colors[QStringLiteral("change")] = m_changeColor;
    colors[QStringLiteral("textAdded")]   = m_textAddedColor;
    colors[QStringLiteral("textRemoved")] = m_textRemovedColor;
    if (!m_markStrokeColor.isEmpty())
        colors[QStringLiteral("markStroke")] = m_markStrokeColor;   // 留空则由内核自动取深色版
    opt[QStringLiteral("colors")] = colors;
    // 图形标记形式：semi(半透明填充+深色描边) / solid(实心填充) / outline(仅描边)
    opt[QStringLiteral("markStyle")] = m_markStyleCombo->currentData().toString();
    // ---- 矢量检测参数（可调）----
    opt[QStringLiteral("graphicMethod")] = m_gfxMethodCombo->currentData().toString();
    opt[QStringLiteral("vectorTolPt")] = m_vecTolSpin->value();
    opt[QStringLiteral("vectorAngleTolDeg")] = m_vecAngSpin->value();
    opt[QStringLiteral("nearInkTolPt")] = m_nearInkSpin->value();
    // ---- 报告粒度与差异密度过滤 ----
    opt[QStringLiteral("reportMode")] = m_reportModeCombo->currentData().toString();
    opt[QStringLiteral("clusterRadiusPt")] = m_clusterRadiusSpin->value();
    opt[QStringLiteral("minClusterLengthPt")] = m_minClusterLenSpin->value();
    opt[QStringLiteral("changeRadiusPt")] = m_changeRadiusSpin->value();
    // ---- 文字差异参数 ----
    opt[QStringLiteral("textBoxMode")] = m_textBoxModeCombo->currentData().toString();
    opt[QStringLiteral("textSerialMatchMode")] = m_textSerialCombo->currentData().toString();
    // ---- 图号配对参数 ----
    opt[QStringLiteral("sheetNoRegionX")] = m_sheetNoXSpin->value();
    opt[QStringLiteral("sheetNoRegionY")] = m_sheetNoYSpin->value();
    opt[QStringLiteral("sheetNoMaxDistinct")] = m_sheetNoMaxSpin->value();
    // ---- 差异线样式 + 页码语义 ----
    opt[QStringLiteral("strokeWidthPt")] = m_strokeWidthSpin->value();
    opt[QStringLiteral("strokeOpacity")] = m_strokeOpacitySpin->value();
    opt[QStringLiteral("pageMode")] = m_pageModeCombo->currentData().toString();
    // 变更旧(蓝)文字插入方向（down/up/right/left），蓝字按方向插入到"新(红)"文字文本框附近。
    if (m_textDirCombo)
        opt[QStringLiteral("textInsertDirection")] = m_textDirCombo->currentData().toString();
    QJsonObject req;
    req[QStringLiteral("baseline")] = a;
    req[QStringLiteral("target")]   = b;
    req[QStringLiteral("output")]   = outPdf;
    req[QStringLiteral("options")]  = opt;
    const QString csv = m_csvEdit->text().trimmed();
    if (!csv.isEmpty()) req[QStringLiteral("correctedMaterialTable")] = csv;

    QFile rf(reqPath);
    if (!rf.open(QIODevice::WriteOnly | QIODevice::Text)
        || rf.write(QJsonDocument(req).toJson(QJsonDocument::Compact)) < 0) {
        emit statusMessage(QStringLiteral("无法写入请求 JSON"));
        return;
    }
    rf.close();

    QString out, err;
    int rc = CliRunner::run(cliExe(QStringLiteral("EGpdf2df")),
        {QStringLiteral("--json"), reqPath}, out, err);
    emit statusMessage(QStringLiteral("[EGpdf2df] rc=%1 %2").arg(rc).arg(err.trimmed()));
    m_jsonView->setPlainText(out);

    // 解析输出：对齐信息 + 管线号↔材料表一一对应。
    showPipeMatch(QJsonDocument::fromJson(out.toUtf8()).object());

    if (rc != 0) return;

    // Render the produced diff PDF to a BMP and show it as the annotated preview.
    QString bmp = QDir(tmp.path()).filePath(QStringLiteral("diff.bmp"));
    QString out2, err2;
    if (CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
            {QStringLiteral("--render"), QStringLiteral("--page"), QStringLiteral("0"),
             QStringLiteral("--scale"), QStringLiteral("0.6"),
             QStringLiteral("--render-out"), bmp, outPdf}, out2, err2) == 0) {
        QImage img = QImageReader(bmp).read();
        if (!img.isNull()) {
            m_preview->setPixmap(QPixmap::fromImage(img).scaled(
                m_preview->width() - 16, m_preview->height() - 16,
                Qt::KeepAspectRatio, Qt::SmoothTransformation));
            emit statusMessage(QStringLiteral("差异图已生成并预览：图形红/绿填充 + 文字改色(新增红/删除蓝)"));
            return;
        }
    }
    m_preview->setText(QStringLiteral("差异 PDF 已生成，但预览渲染失败：%1").arg(err2.trimmed()));
}

void DiffPage::showPipeMatch(const QJsonObject& root)
{
    m_pipeView->clear();
    if (root.isEmpty()) { m_pipeView->setPlainText(QStringLiteral("无 JSON 输出")); return; }

    QStringList lines;
    // 对齐信息
    QJsonArray aligns = root.value(QStringLiteral("alignments")).toArray();
    if (!aligns.isEmpty()) {
        lines << QStringLiteral("【对齐】先按图纸号配对，再估算平移后比对：");
        for (const QJsonValue& v : aligns) {
            QJsonObject o = v.toObject();
            lines << QStringLiteral("  页%1  缩放=%2  平移 dx=%3pt dy=%4pt")
                        .arg(o.value(QStringLiteral("page")).toInt() + 1)
                        .arg(o.value(QStringLiteral("scale")).toDouble(), 0, 'f', 4)
                        .arg(o.value(QStringLiteral("dxPt")).toDouble(), 0, 'f', 1)
                        .arg(o.value(QStringLiteral("dyPt")).toDouble(), 0, 'f', 1);
        }
    }

    QJsonObject pm = root.value(QStringLiteral("pipeMatch")).toObject();
    if (!pm.isEmpty()) {
        QString src = pm.value(QStringLiteral("tableSource")).toString();
        QJsonArray matched   = pm.value(QStringLiteral("matched")).toArray();
        QJsonArray figureOnly = pm.value(QStringLiteral("figureOnly")).toArray();
        QJsonArray tableOnly  = pm.value(QStringLiteral("tableOnly")).toArray();
        lines << QStringLiteral("");
        lines << QStringLiteral("【管线号↔材料表 一一对应】  材料表来源：%1").arg(src);
        lines << QStringLiteral("  匹配(两侧都有)：%1 项").arg(matched.size());
        lines << QStringLiteral("  图中独有 / 材料表缺(风险)：%1 项").arg(figureOnly.size());
        lines << QStringLiteral("  材料表独有 / 图中未画(风险)：%1 项").arg(tableOnly.size());
        auto dump = [&](const QString& title, const QJsonArray& arr) {
            if (arr.isEmpty()) return;
            lines << QStringLiteral("  %1：").arg(title);
            QStringList items;
            for (const QJsonValue& v : arr) items << v.toString();
            lines << QStringLiteral("    ") + items.join(QStringLiteral(", "));
        };
        dump(QStringLiteral("匹配"), matched);
        dump(QStringLiteral("图中独有"), figureOnly);
        dump(QStringLiteral("材料表独有"), tableOnly);
    } else {
        lines << QStringLiteral("");
        lines << QStringLiteral("【管线号↔材料表】未启用或未输出");
    }
    m_pipeView->setPlainText(lines.join(QStringLiteral("\n")));
}

void DiffPage::onBrowsePreview()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("打开差异图 PDF"),
        QString(), QStringLiteral("PDF (*.pdf)"));
    if (p.isEmpty()) return;
    QTemporaryDir tmp;
    if (!tmp.isValid()) return;
    QString bmp = QDir(tmp.path()).filePath(QStringLiteral("view.bmp"));
    QString out, err;
    if (CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
            {QStringLiteral("--render"), QStringLiteral("--page"), QStringLiteral("0"),
             QStringLiteral("--scale"), QStringLiteral("0.6"),
             QStringLiteral("--render-out"), bmp, p}, out, err) == 0) {
        QImage img = QImageReader(bmp).read();
        if (!img.isNull()) {
            m_preview->setPixmap(QPixmap::fromImage(img).scaled(
                m_preview->width() - 16, m_preview->height() - 16,
                Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
}
