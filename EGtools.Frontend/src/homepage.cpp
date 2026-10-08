#include "homepage.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace {

QFrame* cardFrame()
{
    auto* f = new QFrame();
    f->setFrameShape(QFrame::StyledPanel);
    f->setStyleSheet(
        "QFrame { background:#ffffff; border:1px solid #dfe4ec; border-radius:10px; }");
    f->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return f;
}

QLabel* heading(const QString& text, int pt, const QString& color)
{
    auto* l = new QLabel(text);
    l->setStyleSheet(QStringLiteral("font-size:%1pt; font-weight:600; color:%2; border:none;").arg(pt).arg(color));
    return l;
}

} // namespace

HomePage::HomePage(QWidget* parent) : QWidget(parent)
{
    setStyleSheet("background:#f5f7fb;");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea{background:#f5f7fb;border:none;}");

    auto* page = new QWidget();
    page->setStyleSheet("background:#f5f7fb;");
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(40, 28, 40, 36);
    col->setSpacing(18);

    // ---------------- Header ----------------
    auto* header = new QWidget();
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(16);

    auto* logo = new QLabel();
    QPixmap pm(QStringLiteral(":/app_icon.png"));
    if (!pm.isNull()) {
        pm = pm.scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        logo->setPixmap(pm);
    }
    hl->addWidget(logo);

    auto* ht = new QVBoxLayout();
    ht->setSpacing(2);
    ht->addWidget(heading(QStringLiteral("EGtools"), 22, "#0b5cad"));
    ht->addWidget(new QLabel(QStringLiteral("工程图纸材料表提取与对比工具集 · 区域框选 / 图纸差异")));
    m_kernelBadge = new QLabel();
    m_kernelBadge->setStyleSheet("color:#5a6472;");
    ht->addWidget(m_kernelBadge);
    hl->addLayout(ht, 1);
    col->addWidget(header);

    // ---------------- Quick start ----------------
    {
        auto* box = cardFrame();
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(22, 18, 22, 20);
        bl->setSpacing(10);
        bl->addWidget(heading(QStringLiteral("快速开始"), 14, "#0b5cad"));
        bl->addWidget(new QLabel(QStringLiteral(
            "四步完成一次区域提取：打开图纸 → 选择框选模式并框住目标区域 → 交给内核解析 → 查看/导出结果。")));

        auto* steps = new QHBoxLayout();
        steps->setSpacing(14);
        const char* texts[] = {
            "1  打开 PDF\n   顶部「打开 PDF」选择等轴测图纸",
            "2  框选区域\n   选「全域表 / 细表 / 材料编号区」后拖拽",
            "3  提取\n   点「提取 (内核)」由 CLI 内核解析",
            "4  查看结果\n   表格 / 管线号分栏，字框回叠",
        };
        for (const char* t : texts) {
            auto* s = new QFrame();
            s->setStyleSheet("QFrame{background:#eef3fa;border:none;border-radius:8px;}");
            auto* sl = new QVBoxLayout(s);
            sl->setContentsMargins(14, 12, 14, 12);
            auto* lbl = new QLabel(QString::fromUtf8(t));
            lbl->setStyleSheet("border:none;");
            sl->addWidget(lbl);
            steps->addWidget(s, 1);
        }
        bl->addLayout(steps);

        auto* actions = new QHBoxLayout();
        auto* primary = new QPushButton(QStringLiteral("开始使用 — 打开 PDF 框选提取"));
        primary->setMinimumHeight(38);
        primary->setStyleSheet(
            "QPushButton{background:#0b5cad;color:white;border:none;border-radius:6px;padding:0 18px;font-size:10.5pt;}"
            "QPushButton:hover{background:#0a4e93;}");
        connect(primary, &QPushButton::clicked, this, &HomePage::startExtract);
        actions->addWidget(primary);

        auto* openOnly = new QPushButton(QStringLiteral("仅打开 PDF"));
        connect(openOnly, &QPushButton::clicked, this, &HomePage::openPdfRequested);
        actions->addWidget(openOnly);
        actions->addStretch();
        bl->addLayout(actions);

        col->addWidget(box);
    }

    // ---------------- Feature entries ----------------
    col->addWidget(heading(QStringLiteral("全部功能"), 14, "#0b5cad"));

    auto* grid = new QGridLayout();
    grid->setSpacing(14);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    QPushButton* btn = nullptr;
    struct Entry { int r, c; const char* icon; const char* title; const char* desc; const char* cta; };
    const Entry entries[] = {
        {0, 0, "\xF0\x9D\x97", "区域框选提取",
            "在图纸上框选任意区域，三种语义：全域表 / 细表 / 材料编号区。导出 RegionRequest JSON 交给内核，零硬编码坐标。",
            "进入工作区"},
        {0, 1, "\xE0\xB4\x9E", "整页材料表抽取",
            "无需框选，整页自动定位材料表并导出 CSV / XLSX（等价于命令行 EGpdf2excel）。",
            "选择 PDF 批量抽取"},
        {1, 0, "\xE8\xB7\xAF", "图纸差异对比（PDF）",
            "新旧两张图纸 PDF 对比：图形差异（排除文字干扰）与文字差异分两步独立处理，输出红标注 PDF。",
            "选择基准图 / 目标图"},
        {1, 1, "\xE0\xA5\x8F", "Excel 图纸对比",
            "两份管道材料表 Excel 以 PIPE NO 为主键对比，输出新增 / 删除 / QTY 变化清单。",
            "选择两份 Excel"},
        {2, 0, "\xE2\x8A\x99", "框选方案（复用 / 修改 / 导出）",
            "把常用框选存成方案：可复用到其他图纸、在表格里改字段、重写框选，并导出为内核可用的 JSON。",
            "管理框选方案"},
        {2, 1, "\xE2\x9C\x93", "使用说明书",
            "内置完整说明书（界面、框选、JSON 契约、坐标系、命令行速查），随程序走。",
            "打开说明书"},
    };

    for (const Entry& e : entries) {
        auto* card = makeCard(QString::fromUtf8(e.icon), QString::fromUtf8(e.title),
                              QString::fromUtf8(e.desc), QString::fromUtf8(e.cta), &btn);
        grid->addWidget(card, e.r, e.c);
        const QString kind = QString::fromUtf8(e.title);
        if (kind == QStringLiteral("整页材料表抽取"))            connect(btn, &QPushButton::clicked, this, &HomePage::wholePageExtract);
        else if (kind == QStringLiteral("图纸差异对比（PDF）"))   connect(btn, &QPushButton::clicked, this, &HomePage::diffRequested);
        else if (kind == QStringLiteral("Excel 图纸对比"))        connect(btn, &QPushButton::clicked, this, &HomePage::excelCompareRequested);
        else if (kind.startsWith(QStringLiteral("框选方案")))connect(btn, &QPushButton::clicked, this, &HomePage::schemesRequested);
        else if (kind == QStringLiteral("使用说明书"))            connect(btn, &QPushButton::clicked, this, &HomePage::manualRequested);
        else if (kind == QStringLiteral("关于 EGtools"))          connect(btn, &QPushButton::clicked, this, &HomePage::aboutRequested);
        else                                                      connect(btn, &QPushButton::clicked, this, &HomePage::startExtract);
    }
    col->addLayout(grid);

    col->addWidget(new QLabel(QStringLiteral(
        "提示：Qt 界面只负责交互与编排，所有 PDF / Excel 处理均由独立的 .NET 命令行内核完成，界面与内核通过 JSON 契约解耦。")));

    scroll->setWidget(page);
    outer->addWidget(scroll);
    setKernelStatus(false, QStringLiteral("内核状态：未检测"));
}

QWidget* HomePage::makeCard(const QString& iconText, const QString& title,
                            const QString& desc, const QString& cta, QPushButton** outBtn)
{
    auto* box = cardFrame();
    auto* l = new QVBoxLayout(box);
    l->setContentsMargins(20, 16, 20, 16);
    l->setSpacing(8);

    auto* top = new QHBoxLayout();
    top->setSpacing(12);
    auto* ic = new QLabel(iconText);
    ic->setFixedSize(38, 38);
    ic->setAlignment(Qt::AlignCenter);
    ic->setStyleSheet("background:#0b5cad;color:white;border-radius:8px;font-size:15pt;border:none;");
    top->addWidget(ic);
    auto* t = heading(title, 12, "#1a2b3c");
    top->addWidget(t, 1);
    l->addLayout(top);

    auto* d = new QLabel(desc);
    d->setWordWrap(true);
    d->setStyleSheet("color:#4a5568;border:none;");
    l->addWidget(d);

    auto* b = new QPushButton(cta);
    b->setStyleSheet(
        "QPushButton{background:#eef3fa;color:#0b5cad;border:1px solid #c6d8ef;"
        "border-radius:6px;padding:7px 12px;text-align:left;}"
        "QPushButton:hover{background:#e2ecf9;}");
    l->addWidget(b);
    *outBtn = b;
    return box;
}

void HomePage::setKernelStatus(bool found, const QString& detail)
{
    if (!m_kernelBadge) return;
    m_kernelBadge->setText(detail);
    m_kernelBadge->setStyleSheet(found ? QStringLiteral("color:#1b7f3b;")
                                       : QStringLiteral("color:#c0392b;"));
}