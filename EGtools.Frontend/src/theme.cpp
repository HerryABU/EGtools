#include "theme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QSettings>
#include <QStringList>

namespace {

// ---------------------------------------------------------------------------
// 一套配色 = 一组语义色。
//
// 设计原则（照专业桌面软件的通行做法，不是凭感觉配色）：
//   * **中性灰打底 + 只用一个强调色**：中性色承担 95% 的界面，强调色只用于
//     "当前选中 / 主操作 / 焦点"，于是视线永远知道该看哪里（两三个颜色互相争抢时，
//     反而什么都不突出）。
//   * 深浅两套只有**一处定义**，界面一律通过 Theme::color() 取色，不自己写死颜色
//     —— 这是"切主题不花脸"的硬前提。
//   * 对比度按 WCAG AA（正文 ≥ 4.5:1）取，次要文字也不能低到看不清。
// ---------------------------------------------------------------------------
struct Palette
{
    const char* windowBg;      // 窗口/内容底
    const char* panelBg;       // 面板/卡片底（比 windowBg 亮一档）
    const char* canvasBg;      // 画布底（图纸周围）
    const char* text;          // 主文字
    const char* textMuted;     // 次要文字（标签、说明、表头）
    const char* accent;        // 唯一强调色
    const char* accentHover;
    const char* border;        // 分隔线
    const char* success;
    const char* danger;
    const char* warning;
    const char* inputBg;       // 输入控件底
    const char* headerBg;      // 表头/面板标题栏/次级表面
    const char* selBg;         // 选中/悬停的浅色底
};

// 浅色：中性冷灰 + 单一蓝强调。
const Palette kLight = {
    "#f2f4f7",  // windowBg
    "#ffffff",  // panelBg
    "#3a3e44",  // canvasBg（图纸周围保持中性灰，深浅主题都不干扰判读）
    "#1f2430",  // text
    "#5f6b7a",  // textMuted
    "#1668c4",  // accent
    "#0f549e",  // accentHover
    "#d8dee7",  // border
    "#18813f",  // success
    "#c0392b",  // danger
    "#8a6a00",  // warning
    "#ffffff",  // inputBg
    "#f6f8fb",  // headerBg
    "#e3ecf8",  // selBg
};

// 深色：中性炭灰 + 单一亮蓝强调。长时间对着白底图纸时界面不过亮，且与图纸对比足。
const Palette kDark = {
    "#1b1f26",  // windowBg
    "#242932",  // panelBg
    "#14171b",  // canvasBg
    "#e8ecf2",  // text
    "#9aa5b4",  // textMuted
    "#4a9eff",  // accent
    "#6fb2ff",  // accentHover
    "#39414d",  // border
    "#3fbf7f",  // success
    "#e5645b",  // danger
    "#e0b04a",  // warning
    "#2b313a",  // inputBg
    "#2a3038",  // headerBg
    "#2c3d52",  // selBg
};

const Palette& pal(Theme::Mode m) { return m == Theme::Dark ? kDark : kLight; }

// ---------------------------------------------------------------------------
// 调色板落进 QPalette
//
// 只设 QSS 是不够的：Qt 自绘/原生绘制的部分（菜单阴影、ToolTip、禁用的下拉箭头、
// 滚动条轨道…）仍会读应用调色板，深色主题下会露出默认浅色 ⇒ 观感"拼凑"。
// 所以两件事一起做：QPalette（底层）+ QSS（控件层）。
// ---------------------------------------------------------------------------
void applyPalette(const Palette& p)
{
    if (!qApp) return;
    QPalette qp;
    qp.setColor(QPalette::Window,          QColor(p.windowBg));
    qp.setColor(QPalette::WindowText,      QColor(p.text));
    qp.setColor(QPalette::Base,            QColor(p.inputBg));
    qp.setColor(QPalette::AlternateBase,   QColor(p.headerBg));
    qp.setColor(QPalette::Text,            QColor(p.text));
    qp.setColor(QPalette::PlaceholderText, QColor(p.textMuted));
    qp.setColor(QPalette::Button,          QColor(p.inputBg));
    qp.setColor(QPalette::ButtonText,      QColor(p.text));
    qp.setColor(QPalette::BrightText,      QColor(p.danger));
    qp.setColor(QPalette::Highlight,       QColor(p.accent));
    qp.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    qp.setColor(QPalette::ToolTipBase,     QColor(p.panelBg));
    qp.setColor(QPalette::ToolTipText,     QColor(p.text));
    qp.setColor(QPalette::Link,            QColor(p.accent));
    qp.setColor(QPalette::Disabled, QPalette::Text,       QColor(p.textMuted));
    qp.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(p.textMuted));
    qp.setColor(QPalette::Disabled, QPalette::WindowText, QColor(p.textMuted));
    qApp->setPalette(qp);
}

// ---------------------------------------------------------------------------
// 专业化 QSS
//
// 落实的几条规则（"看起来专业"其实主要是纪律，不是审美）：
//   1. **8px 节奏**：间距/内边距只用 4 / 6 / 8 / 10 / 12 / 16，不写 7、13 这类数。
//   2. **统一控件高度**：按钮/输入/下拉一律 22–24px（桌面密集界面），行高一致。
//   3. **统一圆角 4px**（大容器才 6px）—— 专业工具不会到处圆角。
//   4. **减少边框噪声**：能靠底色区分就不画边框；表格去掉竖线；树去掉连接线。
//   5. **层次靠字号/字重/灰度**，不靠加颜色。
// ---------------------------------------------------------------------------
QString qssFor(const Palette& p, bool /*dark*/)
{
    return QStringLiteral(R"(
/* ===================== 基础 ===================== */
QWidget { font-size: 12px; color: %(text)s; }
QMainWindow, QDialog { background: %(windowBg)s; }
QMainWindow::separator { width: 1px; height: 1px; background: %(border)s; }

/* ===================== 菜单 ===================== */
QMenuBar { background: %(panelBg)s; border-bottom: 1px solid %(border)s; padding: 1px 4px; }
QMenuBar::item { padding: 5px 10px; background: transparent; border-radius: 4px; }
QMenuBar::item:selected { background: %(selBg)s; }
QMenu { background: %(panelBg)s; border: 1px solid %(border)s; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 14px; border-radius: 4px; }
QMenu::item:selected { background: %(accent)s; color: #ffffff; }
QMenu::item:disabled { color: %(textMuted)s; }
QMenu::separator { height: 1px; background: %(border)s; margin: 4px 8px; }

/* ===================== 工具栏（扁平命令条） ===================== */
QToolBar { background: %(panelBg)s; border-bottom: 1px solid %(border)s; spacing: 2px; padding: 4px 6px; }
QToolBar::separator { background: %(border)s; width: 1px; margin: 4px 6px; }
QToolBar QToolButton { padding: 4px 10px; border-radius: 4px; color: %(text)s; }
QToolBar QToolButton:hover { background: %(selBg)s; }
QToolBar QToolButton:pressed, QToolBar QToolButton:checked { background: %(selBg)s; color: %(accent)s; }

/* ===================== 面板 / 侧栏 ===================== */
/* 画布视口底色（QScrollArea 的 viewport 只能靠 QSS 命 objectName 上色）*/
QWidget#canvasViewport { background: %(canvasBg)s; }
QWidget#sidePane { background: %(panelBg)s; border-right: 1px solid %(border)s; }
QWidget#sideRail { background: %(panelBg)s; border-right: 1px solid %(border)s; }
QToolButton#railBtn { border: none; border-radius: 4px; padding: 3px; color: %(textMuted)s; font-size: 14px; }
QToolButton#railBtn:hover { background: %(selBg)s; color: %(accent)s; }

/* 面板标题栏：专业软件每个面板都有，标题栏上放折叠按钮 —— 这就是"不推拉"的落点 */
QWidget#panelHeader { background: %(headerBg)s; border-bottom: 1px solid %(border)s; }
QLabel#panelTitle { color: %(textMuted)s; font-weight: 600; font-size: 11px; letter-spacing: 1px; }
QToolButton#panelToolBtn { border: none; border-radius: 4px; padding: 2px 6px; color: %(textMuted)s; font-size: 13px; }
QToolButton#panelToolBtn:hover { background: %(selBg)s; color: %(accent)s; }

/* ===================== 顶部页签（页面导航） ===================== */
QTabBar#topNav { background: %(panelBg)s; border-bottom: 1px solid %(border)s; }
QTabBar#topNav::tab { background: transparent; padding: 6px 14px; margin: 0 1px; border: none;
                      border-bottom: 2px solid transparent; color: %(textMuted)s; }
QTabBar#topNav::tab:hover:!selected { color: %(text)s; background: %(selBg)s; }
QTabBar#topNav::tab:selected { color: %(accent)s; font-weight: 600; border-bottom-color: %(accent)s; }

/* ===================== 导航树（去掉连接线与焦点噪声） ===================== */
QTreeWidget#navTree { background: transparent; border: none; outline: 0; padding: 4px 0; }
QTreeWidget#navTree::item { padding: 6px 6px; margin: 0 4px; border-radius: 4px; color: %(text)s; }
QTreeWidget#navTree::item:hover:!selected { background: %(selBg)s; }
QTreeWidget#navTree::item:selected { background: %(accent)s; color: #ffffff; }
/* 保留系统绘制的展开箭头（折叠/展开必须一眼可辨），只把背景清干净 */
QTreeWidget#navTree::branch { background: transparent; }
/* 分组项（有子项）加粗：与可点击的叶子项区分开 */
QTreeWidget#navTree::item:has-children { font-weight: 600; color: %(textMuted)s; }

/* ===================== 分组框（标题走小号灰字，弱化"盒子感"） ===================== */
QGroupBox { border: 1px solid %(border)s; border-radius: 6px; margin-top: 14px; padding: 10px 8px 8px 8px;
            background: %(panelBg)s; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: %(textMuted)s;
                   font-weight: 600; font-size: 11px; }

/* ===================== 按钮 ===================== */
QPushButton { background: %(inputBg)s; border: 1px solid %(border)s; border-radius: 4px;
              padding: 4px 12px; min-height: 22px; color: %(text)s; }
QPushButton:hover { background: %(selBg)s; border-color: %(accent)s; }
QPushButton:pressed { background: %(headerBg)s; }
QPushButton:disabled { color: %(textMuted)s; background: %(windowBg)s; border-color: %(border)s; }
QPushButton:default { border-color: %(accent)s; }
QPushButton:focus { outline: none; }
QToolButton { border: none; border-radius: 4px; padding: 3px 6px; }
QToolButton:hover { background: %(selBg)s; }

/* ===================== 输入控件（统一高度，视觉同族） ===================== */
QLineEdit, QPlainTextEdit, QTextEdit, QTextBrowser, QSpinBox, QDoubleSpinBox {
    background: %(inputBg)s; border: 1px solid %(border)s; border-radius: 4px; padding: 3px 6px;
    selection-background-color: %(accent)s; selection-color: #ffffff; }
QLineEdit { min-height: 22px; }
QSpinBox, QDoubleSpinBox { min-height: 22px; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QTextBrowser:focus,
QSpinBox:focus, QDoubleSpinBox:focus { border-color: %(accent)s; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: %(textMuted)s; background: %(windowBg)s; }
QLineEdit[readOnly="true"] { background: %(headerBg)s; }

QComboBox { background: %(inputBg)s; border: 1px solid %(border)s; border-radius: 4px;
            padding: 3px 8px; min-height: 22px; }
QComboBox:hover { border-color: %(accent)s; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: %(panelBg)s; border: 1px solid %(border)s;
                              selection-background-color: %(accent)s; selection-color: #ffffff;
                              outline: 0; padding: 2px; }

QCheckBox, QRadioButton { spacing: 6px; }
QCheckBox:disabled, QRadioButton:disabled { color: %(textMuted)s; }

/* ===================== 页签（面板内） ===================== */
QTabWidget::pane { border: 1px solid %(border)s; border-radius: 6px; top: -1px; background: %(panelBg)s; }
QTabBar::tab { background: transparent; padding: 5px 12px; margin-right: 2px; color: %(textMuted)s;
               border: 1px solid transparent; border-top-left-radius: 4px; border-top-right-radius: 4px; }
QTabBar::tab:selected { background: %(panelBg)s; border-color: %(border)s; border-bottom-color: %(panelBg)s;
                        color: %(accent)s; font-weight: 600; }
QTabBar::tab:hover:!selected { background: %(selBg)s; }

/* ===================== 列表 / 树 / 表（去竖线，减噪声） ===================== */
QTreeWidget, QListWidget, QTableWidget, QTreeView, QListView, QTableView {
    background: %(panelBg)s; border: 1px solid %(border)s; border-radius: 4px; outline: 0;
    alternate-background-color: %(headerBg)s; gridline-color: transparent; }
QTreeView::item, QListView::item { padding: 4px 4px; }
QTableWidget::item { padding: 3px 6px; }
QTreeWidget::item:selected, QListWidget::item:selected, QTableWidget::item:selected,
QTreeView::item:selected, QListView::item:selected, QTableView::item:selected {
    background: %(accent)s; color: #ffffff; }
QHeaderView::section { background: %(headerBg)s; color: %(textMuted)s; padding: 5px 8px; border: none;
                       border-bottom: 1px solid %(border)s; font-weight: 600; }
QHeaderView::section:hover { background: %(selBg)s; }
QTableCornerButton::section { background: %(headerBg)s; border: none; }

/* ===================== 状态栏 / 进度 ===================== */
QStatusBar { background: %(panelBg)s; border-top: 1px solid %(border)s; color: %(textMuted)s; }
QStatusBar QLabel { color: %(textMuted)s; padding: 0 6px; }
QStatusBar::item { border: none; }
QProgressBar { background: %(headerBg)s; border: 1px solid %(border)s; border-radius: 4px;
               text-align: center; color: %(text)s; min-height: 16px; font-size: 11px; }
QProgressBar::chunk { background: %(accent)s; border-radius: 3px; }

/* ===================== 滚动条（细、无箭头按钮） ===================== */
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: %(border)s; border-radius: 5px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: %(textMuted)s; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: %(border)s; border-radius: 5px; min-width: 28px; }
QScrollBar::handle:horizontal:hover { background: %(textMuted)s; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ===================== 分隔条（细线，仅作提示） ===================== */
QSplitter::handle { background: %(border)s; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QSplitter::handle:hover { background: %(accent)s; }

QToolTip { background: %(panelBg)s; color: %(text)s; border: 1px solid %(border)s; padding: 4px 6px; }
QScrollArea { border: none; }

/* ===================== 命令条 / 页面内专用（按 objectName） ===================== */
QWidget#cmdBar { background: %(panelBg)s; border: 1px solid %(border)s; border-radius: 6px; }
QLabel#cmdGroup { color: %(textMuted)s; font-size: 11px; }
QFrame#cmdSep { background: %(border)s; border: none; }
QLabel#recogBar { background: %(headerBg)s; border: 1px solid %(warning)s;
                  border-radius: 4px; padding: 4px 8px; color: %(text)s; }
QLabel#sectionTitle { color: %(accent)s; font-weight: 600; }
QLabel#mutedHint { color: %(textMuted)s; }
QLabel#pageBadge { color: %(text)s; font-weight: 600; }

/* 主操作按钮：全界面只允许"一个动作"用强调色（视线落点） */
QPushButton#primaryBtn { background: %(accent)s; color: #ffffff; font-weight: 600;
                         border: 1px solid %(accent)s; border-radius: 4px; padding: 4px 16px; min-height: 24px; }
QPushButton#primaryBtn:hover { background: %(accentHover)s; border-color: %(accentHover)s; }
QPushButton#primaryBtn:pressed { background: %(accentHover)s; }
QPushButton#primaryBtn:disabled { background: %(windowBg)s; color: %(textMuted)s; border-color: %(border)s; }

/* 危险动作（重置/删除）：只在真正危险的地方用 */
QPushButton#dangerBtn { color: %(danger)s; border-color: %(danger)s; background: transparent; }
QPushButton#dangerBtn:hover { background: %(danger)s; color: #ffffff; }

/* 说明/摘要文字 */
QLabel#diffSummary, QLabel#dlgHint, QLabel#hintText { color: %(textMuted)s; font-size: 11px; }

/* 状态徽章（就绪/未就绪）：颜色 + 文字同时表达，不只靠颜色 */
QLabel#badgeOk  { color: %(success)s; font-weight: 600; }
QLabel#badgeBad { color: %(danger)s;  font-weight: 600; }
QLabel#badgeRun { color: %(warning)s; font-weight: 600; }

/* 首页卡片内的行动按钮：左对齐、低调，不跟主按钮抢视线 */
QPushButton#cardCtaBtn { background: %(headerBg)s; color: %(accent)s; border: 1px solid %(border)s;
                         border-radius: 4px; padding: 6px 12px; text-align: left; }
QPushButton#cardCtaBtn:hover { background: %(selBg)s; border-color: %(accent)s; }

/* 首页卡片 */
QWidget#homePage { background: %(windowBg)s; }
QWidget#homeCard { background: %(panelBg)s; border: 1px solid %(border)s; border-radius: 6px; }
QWidget#homeCard:hover { border-color: %(accent)s; background: %(selBg)s; }
QLabel#homeCardTitle { font-size: 13px; font-weight: 600; color: %(text)s; }
QLabel#homeCardDesc  { color: %(textMuted)s; font-size: 11px; }
QLabel#homeCardIcon  { background: %(accent)s; color: #ffffff; border-radius: 6px;
                       font-size: 15px; font-weight: 600; }
QLabel#homeTitle { font-size: 19px; font-weight: 600; color: %(text)s; }
QLabel#homeSubtitle { color: %(textMuted)s; }
QLabel#homeStep { background: %(headerBg)s; border: 1px solid %(border)s; border-radius: 6px; }
)")
        .replace(QStringLiteral("%(text)s"), QLatin1String(p.text))
        .replace(QStringLiteral("%(windowBg)s"), QLatin1String(p.windowBg))
        .replace(QStringLiteral("%(panelBg)s"), QLatin1String(p.panelBg))
        .replace(QStringLiteral("%(canvasBg)s"), QLatin1String(p.canvasBg))
        .replace(QStringLiteral("%(textMuted)s"), QLatin1String(p.textMuted))
        .replace(QStringLiteral("%(accent)s"), QLatin1String(p.accent))
        .replace(QStringLiteral("%(accentHover)s"), QLatin1String(p.accentHover))
        .replace(QStringLiteral("%(border)s"), QLatin1String(p.border))
        .replace(QStringLiteral("%(success)s"), QLatin1String(p.success))
        .replace(QStringLiteral("%(danger)s"), QLatin1String(p.danger))
        .replace(QStringLiteral("%(warning)s"), QLatin1String(p.warning))
        .replace(QStringLiteral("%(inputBg)s"), QLatin1String(p.inputBg))
        .replace(QStringLiteral("%(headerBg)s"), QLatin1String(p.headerBg))
        .replace(QStringLiteral("%(selBg)s"), QLatin1String(p.selBg));
}

} // namespace

Theme::Mode Theme::current()
{
    // 环境变量可强制（自动化截图/排障用）：EGTOOLS_THEME=dark|light
    const QByteArray env = qgetenv("EGTOOLS_THEME");
    if (!env.isEmpty()) return fromString(QString::fromLocal8Bit(env));
    return fromString(QSettings().value(QStringLiteral("ui/theme")).toString());
}

void Theme::setCurrent(Mode m)
{
    QSettings().setValue(QStringLiteral("ui/theme"), toString(m));
    apply(m);
}

Theme::Mode Theme::fromString(const QString& s)
{
    return s.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0 ? Dark : Light;
}

QString Theme::toString(Mode m)
{
    return m == Dark ? QStringLiteral("dark") : QStringLiteral("light");
}

QString Theme::qss(Mode m)
{
    return qssFor(pal(m), m == Dark);
}

QString Theme::color(Role r)
{
    return color(current(), r);
}

QString Theme::color(Mode m, Role r)
{
    const Palette& p = pal(m);
    switch (r)
    {
    case WindowBg:   return QLatin1String(p.windowBg);
    case PanelBg:    return QLatin1String(p.panelBg);
    case CanvasBg:   return QLatin1String(p.canvasBg);
    case Text:       return QLatin1String(p.text);
    case TextMuted:  return QLatin1String(p.textMuted);
    case Accent:     return QLatin1String(p.accent);
    case AccentHover:return QLatin1String(p.accentHover);
    case Border:     return QLatin1String(p.border);
    case Success:    return QLatin1String(p.success);
    case Danger:     return QLatin1String(p.danger);
    case Warning:    return QLatin1String(p.warning);
    case HeaderBg:   return QLatin1String(p.headerBg);
    case SelBg:      return QLatin1String(p.selBg);
    }
    return QLatin1String(p.text);
}

void Theme::apply(Mode m)
{
    // 两件事一起做：QPalette（底层/原生绘制）+ QSS（控件层）。
    // 只做 QSS 的话，深色主题下菜单阴影、禁用箭头、滚动条轨道等仍会露浅色，观感"拼凑"。
    applyPalette(pal(m));
    if (auto* app = qApp)
        app->setStyleSheet(qss(m));
}
