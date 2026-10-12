#pragma once

#include <QSplitter>
#include <QWidget>

// ---------------------------------------------------------------------------
// 让 QSplitter **只做按比例布局，不给用户拖动**。
//
// 背景：用户反复反馈"怎么还是推拉"。把所有面板都改成"固定尺寸 + 折叠 + 尺寸预设"当然最彻底，
// 但 `QSplitter` 还承担着一条很实用的职责：**按比例分配剩余空间**（窗口变大时两块一起长）。
// 直接换成 QHBoxLayout 会把这个能力也一起丢掉，而且要改动大量已有的"面板在分割器/标签页
// 之间搬家"的逻辑（容易引入回归）。
//
// 所以做法是：**保留 QSplitter 的比例布局能力，只去掉"可拖动"这个交互**：
//   * `setHandleWidth(0)`：拖拽把手宽度为 0 ⇒ 鼠标**根本没有可抓的地方**（这是关键，
//     仅 `setEnabled(false)` 在部分样式下仍可能露出可抓区域）。
//   * 把手控件再显式 `setEnabled(false)`：双保险。
//   * `setChildrenCollapsible(false)`：不允许把某一侧拖没了（本来也拖不动，防御性设置）。
// 分隔线由相邻面板的 QSS 边框负责（1px），视觉上仍然有界线。
//
// 想调整尺寸？走"尺寸预设 / 折叠开关"（见 视图 菜单），不要靠拖。
// ---------------------------------------------------------------------------
inline void makeSplitterNonDraggable(QSplitter* s)
{
    if (!s) return;
    s->setChildrenCollapsible(false);
    s->setHandleWidth(0);          // ← 真正的保证：把手宽度 0 ⇒ 鼠标无处可抓（且跨把手重建也不变）
    s->setOpaqueResize(false);
    for (int i = 0; i < s->count() - 1; ++i) {
        if (QWidget* h = s->handle(i)) {
            h->setEnabled(false);
            h->setCursor(Qt::ArrowCursor);
        }
    }
}

// ⚠️ Qt 在 `insertWidget` / 控件换父（面板在分割器与标签页之间搬家）时会**重建把手**，
// 新把手又是 enabled 状态。`handleWidth(0)` 已经让它抓不到了（这是硬保证），
// 但为了"连 enabled 状态也干净"，搬完控件后再刷一遍。
inline void refreshNonDraggableSplitters(QWidget* root)
{
    if (!root) return;
    const auto splitters = root->findChildren<QSplitter*>();
    for (QSplitter* s : splitters) {
        s->setHandleWidth(0);
        s->setChildrenCollapsible(false);
        for (int i = 0; i < s->count() - 1; ++i) {
            if (QWidget* h = s->handle(i)) {
                h->setEnabled(false);
                h->setCursor(Qt::ArrowCursor);
            }
        }
    }
}

// 自检用：分割器是否不可拖。
// 判据以 `handleWidth()` 为准 —— 那是"鼠标能不能抓到"的直接决定因素
// （把手宽度为 0 时它收不到任何鼠标事件，即使 enabled 也拖不动）。
inline bool splitterIsNonDraggable(const QSplitter* s)
{
    return !s || s->handleWidth() <= 0;
}

// 更严格的判据：把手既不占宽度、也不处于 enabled（搬完控件刷新后应当满足）。
inline bool splitterHandlesFullyDisabled(const QSplitter* s)
{
    if (!s) return true;
    if (s->handleWidth() > 0) return false;
    for (int i = 0; i < s->count() - 1; ++i)
        if (const QWidget* h = s->handle(i))
            if (h->isEnabled()) return false;
    return true;
}
