#include "theme.h"

#include <QApplication>
#include <QSettings>
#include <QStringList>

namespace {

struct Palette
{
    const char* windowBg;
    const char* panelBg;
    const char* canvasBg;
    const char* text;
    const char* textMuted;
    const char* accent;
    const char* accentHover;
    const char* border;
    const char* success;
    const char* danger;
    const char* warning;
    const char* inputBg;
    const char* headerBg;
    const char* selBg;
};

// 两套配色：浅色沿用原蓝灰体系（企业软件观感），深色采用中性炭灰 + 青绿强调，
// 保证在长时间看图（CAD 白底图纸）时界面不过亮、与图纸有足够对比。
const Palette kLight = {
    "#f5f7fb",  // windowBg
    "#ffffff",  // panelBg
    "#3a3e44",  // canvasBg（图纸周围，深浅都保持中性灰，避免影响判读）
    "#1a2b3c",  // text
    "#5a6472",  // textMuted
    "#0b5cad",  // accent
    "#0a4e93",  // accentHover
    "#dfe4ec",  // border
    "#1b7f3b",  // success
    "#c0392b",  // danger
    "#8a6d00",  // warning
    "#ffffff",  // inputBg
    "#eef3fa",  // headerBg
    "#dce9f8",  // selBg
};

const Palette kDark = {
    "#1e2228",  // windowBg
    "#262b33",  // panelBg
    "#16191d",  // canvasBg
    "#e6eaf0",  // text
    "#9aa4b2",  // textMuted
    "#3d9be9",  // accent
    "#5aaeef",  // accentHover
    "#3a424d",  // border
    "#4cc38a",  // success
    "#e5645b",  // danger
    "#e0b04a",  // warning
    "#2f3540",  // inputBg
    "#2b313a",  // headerBg
    "#33465c",  // selBg
};

const Palette& pal(Theme::Mode m) { return m == Theme::Dark ? kDark : kLight; }

QString qssFor(const Palette& p, bool dark)
{
    // 说明：Qt 的 QSS 不支持变量，这里用 arg 组装。所有颜色都来自调色板，
    // 保证两套主题只有一处定义，不会出现某个控件漏改而"花脸"。
    return QStringLiteral(R"(
QWidget { font-size: 9.75pt; color: %(text)s; }
QMainWindow, QDialog { background: %(windowBg)s; }
QToolBar { background: %(panelBg)s; border-bottom: 1px solid %(border)s; spacing: 4px; padding: 3px; }
QToolBar QToolButton { padding: 4px 9px; border-radius: 4px; }
QToolBar QToolButton:hover { background: %(selBg)s; }

QStatusBar { background: %(panelBg)s; border-top: 1px solid %(border)s; color: %(textMuted)s; }

QMenuBar { background: %(panelBg)s; border-bottom: 1px solid %(border)s; }
QMenuBar::item { padding: 5px 10px; background: transparent; }
QMenuBar::item:selected { background: %(selBg)s; }
QMenu { background: %(panelBg)s; border: 1px solid %(border)s; padding: 4px; }
QMenu::item { padding: 5px 22px 5px 14px; border-radius: 4px; }
QMenu::item:selected { background: %(accent)s; color: #ffffff; }
QMenu::separator { height: 1px; background: %(border)s; margin: 4px 6px; }

QGroupBox { border: 1px solid %(border)s; border-radius: 6px; margin-top: 12px; padding-top: 6px;
            background: %(panelBg)s; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; color: %(textMuted)s; font-weight: 600; }

QPushButton { background: %(inputBg)s; border: 1px solid %(border)s; border-radius: 5px;
              padding: 5px 12px; min-height: 20px; }
QPushButton:hover { background: %(selBg)s; border-color: %(accent)s; }
QPushButton:pressed { background: %(headerBg)s; }
QPushButton:disabled { color: %(textMuted)s; background: %(windowBg)s; }
QPushButton:default { border-color: %(accent)s; }

QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox {
    background: %(inputBg)s; border: 1px solid %(border)s; border-radius: 5px; padding: 4px 6px;
    selection-background-color: %(accent)s; selection-color: #ffffff; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus { border-color: %(accent)s; }
QLineEdit:disabled { color: %(textMuted)s; background: %(windowBg)s; }

QComboBox { background: %(inputBg)s; border: 1px solid %(border)s; border-radius: 5px;
            padding: 4px 8px; min-height: 20px; }
QComboBox:hover { border-color: %(accent)s; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: %(panelBg)s; border: 1px solid %(border)s;
                              selection-background-color: %(accent)s; selection-color: #ffffff; }

QCheckBox, QRadioButton { spacing: 6px; }
QCheckBox:disabled, QRadioButton:disabled { color: %(textMuted)s; }

QTabWidget::pane { border: 1px solid %(border)s; border-radius: 6px; top: -1px; background: %(panelBg)s; }
QTabBar::tab { background: transparent; padding: 6px 14px; margin-right: 2px;
               border: 1px solid transparent; border-top-left-radius: 5px; border-top-right-radius: 5px; }
QTabBar::tab:selected { background: %(panelBg)s; border-color: %(border)s; border-bottom-color: %(panelBg)s;
                        color: %(accent)s; font-weight: 600; }
QTabBar::tab:hover:!selected { background: %(selBg)s; }

QTreeWidget, QListWidget, QTableWidget, QTreeView, QListView, QTableView {
    background: %(panelBg)s; border: 1px solid %(border)s; border-radius: 5px;
    alternate-background-color: %(headerBg)s; gridline-color: %(border)s; }
QTreeWidget::item, QListWidget::item { padding: 5px 4px; border-radius: 4px; }
QTreeWidget::item:selected, QListWidget::item:selected, QTableWidget::item:selected {
    background: %(accent)s; color: #ffffff; }
QTreeWidget::item:hover:!selected, QListWidget::item:hover:!selected { background: %(selBg)s; }
QHeaderView::section { background: %(headerBg)s; color: %(textMuted)s; padding: 5px 6px;
                       border: none; border-right: 1px solid %(border)s; border-bottom: 1px solid %(border)s;
                       font-weight: 600; }
QTableCornerButton::section { background: %(headerBg)s; border: none; }

QProgressBar { background: %(windowBg)s; border: 1px solid %(border)s; border-radius: 5px;
               text-align: center; color: %(text)s; min-height: 18px; }
QProgressBar::chunk { background: %(accent)s; border-radius: 4px; }

QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: %(border)s; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: %(textMuted)s; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle:horizontal { background: %(border)s; border-radius: 5px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: %(textMuted)s; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QSplitter::handle { background: %(border)s; }
QSplitter::handle:horizontal { width: 3px; }
QSplitter::handle:vertical { height: 3px; }
QSplitter::handle:hover { background: %(accent)s; }

QToolTip { background: %(panelBg)s; color: %(text)s; border: 1px solid %(border)s; padding: 4px; }

QScrollArea { border: none; }

/* ---- 工作区专用（按 objectName 生效，随主题自动变色）----
   注意：凡是被这两个主题覆盖的控件，都不要再在代码里 setStyleSheet 写死颜色，
   否则切换主题时会"花脸"（这是之前浅色/深色切换不彻底的原因）。 */
QWidget#cmdBar { background: %(panelBg)s; border: 1px solid %(border)s; border-radius: 6px; }
QLabel#cmdGroup { color: %(textMuted)s; }
QFrame#cmdSep { background: %(border)s; border: none; }
QLabel#recogBar { background: %(headerBg)s; border: 1px solid %(warning)s;
                  border-radius: 4px; padding: 4px 8px; color: %(text)s; }
QLabel#sectionTitle { color: %(accent)s; font-weight: 600; }
QLabel#mutedHint { color: %(textMuted)s; }
QLabel#pageBadge { color: %(text)s; font-weight: 600; }
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
        .replace(QStringLiteral("%(selBg)s"), QLatin1String(p.selBg))
        + (dark ? QStringLiteral("QWidget { background: %1; }").arg(QLatin1String(p.windowBg))
                : QString());
}

} // namespace

Theme::Mode Theme::current()
{
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
    }
    return QLatin1String(p.text);
}

void Theme::apply(Mode m)
{
    if (auto* app = qApp)
        app->setStyleSheet(qss(m));
}
