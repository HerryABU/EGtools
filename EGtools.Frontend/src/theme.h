#pragma once

#include <QString>

// ---------------------------------------------------------------------------
// 主题（浅色 / 深色）
//
// 为什么集中在一个文件里：整个界面的观感必须一致（专业软件的基本要求），样式
// 分散在各页面里迟早会跑偏。这里给出两套完整的 QSS 变量与控件规则，界面只负责
// 按主题取色（Theme::color(...)），不自己写死颜色。
//
// 选择持久化到 QSettings("ui/theme")，下次启动沿用。
// ---------------------------------------------------------------------------
class Theme
{
public:
    enum Mode { Light, Dark };

    // 语义色（供自绘控件使用；QSS 用不到的自绘部分靠这些取色）。
    enum Role {
        WindowBg,      // 窗口底
        PanelBg,       // 面板/侧栏底
        CanvasBg,      // 画布底（PDF 页面周围）
        Text,          // 主文字
        TextMuted,     // 次要文字
        Accent,        // 主色（**全界面唯一的强调色**：选中/主操作/焦点）
        AccentHover,
        Border,        // 分隔线/边框
        Success,       // 成功
        Danger,        // 失败
        Warning,       // 进行中
        HeaderBg,      // 表头 / 面板标题栏 / 次级表面
        SelBg          // 选中/悬停的浅色底
    };

    static Mode    current();
    static void    setCurrent(Mode m);           // 应用 + 持久化
    static void    apply(Mode m);                // 只应用
    static Mode    fromString(const QString& s);
    static QString toString(Mode m);

    static QString qss(Mode m);
    static QString color(Role r);                // 当前主题下的语义色（十六进制）
    static QString color(Mode m, Role r);
};
