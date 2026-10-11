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

    // ---- 颜色选择器：描红线 / 圆圈 / 文字（色号传入内核 options.colors） ----
    auto* colRow = new QHBoxLayout();
    m_traceColorBtn  = new QPushButton();
    m_circleColorBtn = new QPushButton();
    m_textColorBtn   = new QPushButton();
    for (QPushButton* b : {m_traceColorBtn, m_circleColorBtn, m_textColorBtn}) {
        b->setFixedSize(56, 24);
        b->setCursor(Qt::PointingHandCursor);
    }
    paintSwatch(m_traceColorBtn,  m_traceColor);
    paintSwatch(m_circleColorBtn, m_circleColor);
    paintSwatch(m_textColorBtn,   m_textColor);
    colRow->addWidget(new QLabel(QStringLiteral("颜色:")));
    colRow->addWidget(new QLabel(QStringLiteral("描红线")));
    colRow->addWidget(m_traceColorBtn);
    colRow->addWidget(new QLabel(QStringLiteral("圆圈")));
    colRow->addWidget(m_circleColorBtn);
    colRow->addWidget(new QLabel(QStringLiteral("文字")));
    colRow->addWidget(m_textColorBtn);
    colRow->addStretch();

    m_runBtn = new QPushButton(QStringLiteral("开始比对（红实线/圈=新增·蓝虚线/圈=删除·浅蓝斜体=消失文字）"));
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
        "● <b>描红线(实线)</b> = 新增 / 改动的图形（1:1 描出图形真实轮廓，非方框/圆圈）\n"
        "● <b>描红线(虚线)</b> = 被删除的图形（1:1 描出原轮廓）\n"
        "● <b>圆圈(实/虚线)</b> = 新增 / 删除的文字或序号\n"
        "<i>消失 / 被改掉的旧文字</i> = 浅色斜体在原位置重现\n"
        "三种颜色（描红线 / 圆圈 / 文字）可用上方颜色选择器自定，色号直接传给内核。")));
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
    connect(m_circleColorBtn, &QPushButton::clicked, this, &DiffPage::onPickCircleColor);
    connect(m_textColorBtn,   &QPushButton::clicked, this, &DiffPage::onPickTextColor);
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
    if (pickColor(QStringLiteral("选择描红线颜色（图形轮廓）"), m_traceColor, m_traceColorBtn))
        emit statusMessage(QStringLiteral("描红线颜色 = %1").arg(m_traceColor));
}

void DiffPage::onPickCircleColor()
{
    if (pickColor(QStringLiteral("选择圆圈颜色（文字标记）"), m_circleColor, m_circleColorBtn))
        emit statusMessage(QStringLiteral("圆圈颜色 = %1").arg(m_circleColor));
}

void DiffPage::onPickTextColor()
{
    if (pickColor(QStringLiteral("选择文字颜色（消失/变更标注）"), m_textColor, m_textColorBtn))
        emit statusMessage(QStringLiteral("文字颜色 = %1").arg(m_textColor));
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
    // 颜色选择器 → options.colors（描红线 / 圆圈 / 文字），内核据色号着色。
    QJsonObject colors;
    colors[QStringLiteral("trace")]  = m_traceColor;
    colors[QStringLiteral("circle")] = m_circleColor;
    colors[QStringLiteral("text")]   = m_textColor;
    opt[QStringLiteral("colors")] = colors;
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
            emit statusMessage(QStringLiteral("差异图已生成并预览：红/蓝圈画 + 浅蓝斜体消失文字"));
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
