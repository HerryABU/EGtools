using System;
using System.Collections.Generic;
using System.IO;

namespace EGtools.Gui;

// Lightweight bilingual (中文 / English) localization. All UI strings live here
// as key -> (zh, en) pairs. The active language is persisted to
// %LOCALAPPDATA%/EGtools/ so it survives restarts. Pages subscribe to Changed
// and re-apply their texts via an ApplyLoc() method.
public enum Lang { Zh, En }

public static class Loc
{
    public static event Action<object?, object?>? Changed;

    public static Lang Language { get; private set; } = Lang.Zh;

    private static readonly Dictionary<string, (string Zh, string En)> S = new()
    {
        // generic
        ["lang.toggle"] = ("EN", "中文"),
        ["common.working"] = ("处理中…", "Working…"),
        ["common.done"] = ("完成。", "Done."),
        ["common.open"] = ("打开", "Open"),
        ["common.clear"] = ("清空", "Clear"),
        ["common.add"] = ("添加", "Add"),

        // nav
        ["nav.extract"] = ("PDF → Excel 提取", "PDF → Excel Extract"),
        ["nav.compare"] = ("Excel 图纸对比", "Excel Drawing Compare"),
        ["nav.pipeline"] = ("串联：提取并对比", "Pipeline: Extract & Compare"),
        ["nav.settings"] = ("设置", "Settings"),
        ["nav.about"] = ("关于", "About"),

        // extract
        ["extract.title"] = ("提取材料表", "Extract Material Tables"),
        ["extract.drop"] = ("拖拽 PDF 文件到此处，或点击选择", "Drag PDF files here, or click to choose"),
        ["extract.files"] = ("已选文件", "Selected files"),
        ["extract.count"] = ("共 {0} 个文件", "{0} file(s)"),
        ["extract.format"] = ("输出格式", "Output format"),
        ["extract.fmt.csv"] = ("CSV", "CSV"),
        ["extract.fmt.xlsx"] = ("XLSX", "XLSX"),
        ["extract.fmt.both"] = ("两者 (CSV + XLSX)", "Both (CSV + XLSX)"),
        ["extract.layout"] = ("组件列布局", "Component column"),
        ["extract.layout.merged"] = ("合并 (8 列)", "Merged (8 cols)"),
        ["extract.layout.separate"] = ("单列 (9 列)", "Separate (9 cols)"),
        ["extract.group"] = ("分组小标题", "Group banner"),
        ["extract.group.embed"] = ("嵌入 (embed)", "Embed"),
        ["extract.group.omit"] = ("省略 (omit)", "Omit"),
        ["extract.ref"] = ("参考 Excel（可选）", "Reference Excel (optional)"),
        ["extract.ref.pick"] = ("选择参考…", "Choose reference…"),
        ["extract.tag"] = ("文件名标签", "Filename tag"),
        ["extract.out"] = ("输出目录", "Output directory"),
        ["extract.out.pick"] = ("选择目录…", "Choose folder…"),
        ["extract.run"] = ("开始提取", "Start extraction"),
        ["extract.open"] = ("打开输出目录", "Open output folder"),
        ["extract.err.nofile"] = ("请先添加至少一个 PDF 文件。", "Add at least one PDF file first."),

        // compare
        ["compare.title"] = ("对比两份图纸", "Compare two drawings"),
        ["compare.old"] = ("旧图纸 Excel", "Old drawing Excel"),
        ["compare.new"] = ("新图纸 Excel", "New drawing Excel"),
        ["compare.out"] = ("输出报告", "Output report"),
        ["compare.old.pick"] = ("选择旧图纸…", "Choose old…"),
        ["compare.new.pick"] = ("选择新图纸…", "Choose new…"),
        ["compare.out.pick"] = ("选择输出…", "Choose output…"),
        ["compare.run"] = ("开始对比", "Start compare"),
        ["compare.open"] = ("打开对比结果", "Open result"),
        ["compare.err.nofile"] = ("请先选择旧图纸与新图纸两个文件。", "Choose both old and new Excel files first."),
        ["compare.same"] = ("两个图纸完全一致，没有任何变化！", "Drawings are identical — no changes!"),

        // pipeline
        ["pipeline.title"] = ("串联：提取并对比", "Pipeline: Extract & Compare"),
        ["pipeline.desc"] = ("将旧/新来源（PDF 或 Excel）自动提取为 Excel 后对比，一步生成变化清单。",
                             "Auto-extract old/new sources (PDF or Excel) then compare — one step to a change list."),
        ["pipeline.old"] = ("旧版本来源", "Old source"),
        ["pipeline.new"] = ("新版本来源", "New source"),
        ["pipeline.drop"] = ("拖拽文件到此处（PDF 或 Excel）", "Drop a file here (PDF or Excel)"),
        ["pipeline.pick"] = ("选择文件…", "Choose file…"),
        ["pipeline.out"] = ("输出报告", "Output report"),
        ["pipeline.out.pick"] = ("选择输出…", "Choose output…"),
        ["pipeline.run"] = ("开始串联处理", "Run pipeline"),
        ["pipeline.open"] = ("打开报告", "Open report"),
        ["pipeline.err"] = ("请提供旧版本与新版本两个来源文件。", "Provide both old and new source files."),

        // settings
        ["settings.title"] = ("设置", "Settings"),
        ["settings.lang"] = ("界面语言", "Interface language"),
        ["settings.lang.zh"] = ("中文", "Chinese"),
        ["settings.lang.en"] = ("English", "English"),
        ["settings.theme"] = ("主题", "Theme"),
        ["settings.theme.light"] = ("浅色", "Light"),
        ["settings.theme.dark"] = ("深色", "Dark"),
        ["settings.theme.system"] = ("跟随系统", "System"),

        // about
        ["about.title"] = ("关于 EGtools", "About EGtools"),
        ["about.desc"] = ("工程图纸材料表提取与对比工具箱。从 CAD 等轴测 PDF 读取可编辑矢量文本，生成材料表；并对比两个版本图纸的变化。",
                          "Toolkit to extract and compare engineering drawing material tables. Reads editable vector text from CAD isometric PDFs and diffs drawing revisions."),
        ["about.features"] = ("功能", "Features"),
        ["about.feature1"] = ("• PDF → Excel 提取（CSV / XLSX）", "• PDF → Excel extraction (CSV / XLSX)"),
        ["about.feature2"] = ("• Excel 图纸版本对比（变化清单）", "• Excel revision compare (change list)"),
        ["about.feature3"] = ("• 串联流水线：提取并自动对比", "• Pipeline: extract & auto-compare"),
        ["about.feature4"] = ("• 批量处理 · 拖拽上传 · 中英双语", "• Batch · Drag-drop · Bilingual"),
        ["about.author"] = ("作者", "Author"),
        ["about.version"] = ("版本", "Version"),
        ["about.license"] = ("许可", "License"),
        ["about.docs"] = ("查看使用文档", "View documentation"),
    };

    public static string T(string key, params object[] args)
    {
        string t = S.TryGetValue(key, out var v) ? (Language == Lang.Zh ? v.Zh : v.En) : key;
        return args.Length == 0 ? t : string.Format(t, args);
    }

    public static void SetLanguage(Lang lang)
    {
        if (Language == lang) return;
        Language = lang;
        Save();
        Changed?.Invoke(null, null);
    }

    public static void Init()
    {
        try
        {
            var p = SettingsPath("lang.txt");
            if (File.Exists(p)) Language = File.ReadAllText(p).Trim() == "en" ? Lang.En : Lang.Zh;
        }
        catch { }
    }

    private static void Save()
    {
        try
        {
            var p = SettingsPath("lang.txt");
            Directory.CreateDirectory(Path.GetDirectoryName(p)!);
            File.WriteAllText(p, Language == Lang.En ? "en" : "zh");
        }
        catch { }
    }

    private static string SettingsPath(string name)
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return Path.Combine(local, "EGtools", name);
    }
}
