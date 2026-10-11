using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace EGpdf2df.Engine
{
    /// <summary>
    /// Configuration for the drawing-diff kernel. Every threshold, DPI, registration
    /// and annotation parameter is read from here (a JSON file next to the exe, or
    /// the built-in default) so the tool adapts without recompiling. Nothing about a
    /// specific drawing is hard-coded.
    /// </summary>
    public sealed class Pdf2dfConfig
    {
        public int RenderDpi { get; set; } = 200;
        // 精细度预设：fine(灵敏，捕捉微小差异) | medium | coarse(忽略小差异)。
        // 会映射到底部的阈值/最小面积/高斯模糊/容差；前端或 JSON options.precision 可覆盖。
        public string Precision { get; set; } = "medium";
        public GraphicCfg GraphicDiff { get; set; } = new();
        public TextCfg TextDiff { get; set; } = new();
        public RegistrationCfg Registration { get; set; } = new();
        public AnnotationCfg Annotation { get; set; } = new();
        public PipeMatchCfg PipeMatch { get; set; } = new();

        public sealed class GraphicCfg
        {
            public bool Enabled { get; set; } = true;
            public string Method { get; set; } = "edge";   // "edge" | "absdiff" | "ssim"
            // 图形差异检测路径：
            //   "vector" (默认) = 直接解析内容流取线画 → 几何标定 → 共线覆盖匹配。
            //                     纯矢量、不做位图渲染，误报率低（本项目 CAD 图是纯矢量）。
            //   "pixel"         = 旧的两版渲染成位图 + OpenCV 逐像素/逐轮廓比对。
            //                     对 1~2pt 配准误差零容忍，细线整体错位会整图点亮，仅作回退。
            public string GraphicMethod { get; set; } = "vector";
            // 矢量比对容差(pt)：两版同名线画允许的位置偏差。
            // 实测两版导出存在 2~4pt 的残余配准差（不只是纯缩放/平移），tol=1 会把大量未变线画
            // 报成变更：实测某页 5870 条 → tol=3 时 244 条（24×），而"完全相同的页"在任何 tol 下
            // 都稳定在 2~3 条（不引入误报）。同时真正大改的页在 tol=3 仍保留 3000+ 条，
            // 到 tol=5 就会被吃掉（824 条）——3.0 是"不漏真差异、不产假差异"的平衡点。
            public double VectorTolPt { get; set; } = 3.0;
            // 矢量比对方向容差(度)：防止密集区被"横穿而过"的无关线段误判为覆盖。
            public double VectorAngleTolDeg { get; set; } = 12.0;
            // 页面配对（按线画指纹）的最小余弦相似度：低于此值视为"不同图纸"，不配对。
            // 实测同图纸≈0.99+、不同图纸≈0.3~0.6，0.85 有很宽的裕度。
            public double PageMatchMinSim { get; set; } = 0.85;
            public int Threshold { get; set; } = 60;        // absdiff path: 0..255
            public bool ExcludeText { get; set; } = true;
            public int GaussianBlur { get; set; } = 5;
            public int MinComponentArea { get; set; } = 24;  // px², drops antialias noise
            public int TolerancePx { get; set; } = 0;        // 运行期由 Registration.ToleranceMm 换算，作侵蚀核大小
        }

        public sealed class TextCfg
        {
            public bool Enabled { get; set; } = true;
            public int YTolerance { get; set; } = 12;       // pt
            public int XTolerance { get; set; } = 40;       // pt
            public bool IgnoreCase { get; set; } = true;
            public bool IgnoreWhitespace { get; set; } = true;
            public double MoveTolerance { get; set; } = 6;  // pt, position-only change
        }

        public sealed class RegistrationCfg
        {
            public string Method { get; set; } = "phase";       // "phase"(图像FFT相位相关) | "words" | "none"
            // 忽略"偏离几毫米"的误差/显示偏移：以毫米计的配准容差，运行期换算为像素
            // 对差异图做侵蚀，去除光晕伪影（消除"红线填满"观感）。
            public double ToleranceMm { get; set; } = 2.0;
            public int MaxFeatures { get; set; } = 5000;
            public double MatchRatio { get; set; } = 0.15;  // Lowe ratio
            public double RansacThreshold { get; set; } = 5.0; // px
        }

        public sealed class AnnotationCfg
        {
            // 标注颜色（传入色号 #RRGGBB，均可由用户 / 前端颜色选择器覆盖）：
            //   TraceColor     → 新增 / 删除 的图形描红（默认红）。
            //   ChangeColor    → 变更（同序号、内容不同）的图形描红（默认绿）。
            //   TextAddedColor → 新增文字的"改色"（默认红）。文字差异不再画圈/点，
            //                   而是直接把文字重绘成该颜色（覆盖在原黑色文字上）。
            //   TextRemovedColor → 删除文字的"改色"（默认蓝）。被删掉的旧文字以该颜色
            //                   插入到原文本框（如同 git 的 - 行）。
            //   TextInsertDirection → 变更(同序号、内容不同)时，"旧(蓝)"文字相对"新(红)"文字
            //                   的插入方向：down(下,默认) / up(上) / right(右) / left(左)。
            //                   新增/删除为单侧文字，直接改色/插入在原文本框，无需方向偏移。
            // 三种状态配色：图形 新增=红、删除=红(虚线)、变更=绿；
            //            文字 新增=红、删除=蓝、变更=红(新)+蓝(旧)。
            public string TraceColor { get; set; } = "#FF0000";   // 新增 / 删除 图形描红
            public string ChangeColor { get; set; } = "#16A34A";  // 变更 图形描红（绿）
            public string TextAddedColor { get; set; } = "#FF0000";   // 新增文字改色（红）
            public string TextRemovedColor { get; set; } = "#0000FF";  // 删除文字改色（蓝）
            public string TextInsertDirection { get; set; } = "down";  // 变更旧文字插入方向 down/up/right/left
            // 图形差异的【标记形式】（高密度图纸可选弱化标记，避免实心块盖住底图内容）：
            //   semi    = 半透明填充 + 深色描边（默认；填充很淡、描边清晰，底图仍可读）
            //   solid   = 实心填充（原行为，标记最强）
            //   outline = 仅描边、不填充（最不遮挡，适合极密的施工图）
            public string MarkStyle { get; set; } = "semi";
            // 描边色（#RRGGBB）。留空则自动取标记色的深色版（保证对比度且不刺眼）。
            public string MarkStrokeColor { get; set; } = "";
            // 线宽（描红轮廓边线粗细，可调参数：前端数值框 / 配置文件）。亦控制填充外扩后的边线。
            public int LineWidth { get; set; } = 3;
            // 真描红：填充实际变更区域（半透明），而非仅描边轮廓。
            public double FillOpacity { get; set; } = 0.40;       // 填充透明度 0~1（semi 模式下会自动再打 5 折）
            public bool Outline { get; set; } = true;             // 是否在填充外再描一条同形边线
            public int HaloPx { get; set; } = 0;                  // 填充向外扩张像素(0=精准贴合形状)
            // 嵌入 Unicode 字体（默认 Windows Arial，含土耳其语等字形），让"消失/变更文字"
            // 的标注真正渲染；字体文件缺失时回退内置 Helvetica（仅 ASCII）。
            public string FontPathRegular { get; set; } = "C:/Windows/Fonts/arial.ttf";
            public string FontPathItalic { get; set; } = "C:/Windows/Fonts/ariali.ttf";
        }

        public sealed class PipeMatchCfg
        {
            public bool Enabled { get; set; } = true;
            // 材料表（BOM）通常位于图纸底部；页高的比例阈值以上视为"图中管线号"、
            // 以下视为"材料表管线号"。若提供人工修正的材料表（CSV/TSV），则以该表为准。
            public double MaterialTableYFraction { get; set; } = 0.62;
        }

        // ----------------------------------------------------------------
        //  Loading / merge over built-in defaults.
        // ----------------------------------------------------------------
        const string DEFAULT_JSON = @"
{
  ""precision"": ""medium"",
  ""renderDpi"": 200,
  ""graphicDiff"": { ""enabled"": true, ""method"": ""edge"", ""graphicMethod"": ""vector"", ""vectorTolPt"": 3.0, ""vectorAngleTolDeg"": 12.0, ""pageMatchMinSim"": 0.85, ""threshold"": 60, ""excludeText"": true, ""gaussianBlur"": 5, ""minComponentArea"": 24, ""tolerancePx"": 0 },
  ""textDiff"": { ""enabled"": true, ""yTolerance"": 12, ""xTolerance"": 40, ""ignoreCase"": true, ""ignoreWhitespace"": true, ""moveTolerance"": 6 },
  ""registration"": { ""method"": ""phase"", ""toleranceMm"": 2.0, ""maxFeatures"": 5000, ""matchRatio"": 0.15, ""ransacThreshold"": 5.0 },
  ""annotation"": { ""traceColor"": ""#FF0000"", ""changeColor"": ""#16A34A"", ""textAddedColor"": ""#FF0000"", ""textRemovedColor"": ""#0000FF"", ""textInsertDirection"": ""down"", ""markStyle"": ""semi"", ""markStrokeColor"": """", ""lineWidth"": 3, ""fillOpacity"": 0.40, ""outline"": true, ""haloPx"": 0, ""fontPathRegular"": ""C:/Windows/Fonts/arial.ttf"", ""fontPathItalic"": ""C:/Windows/Fonts/ariali.ttf"" },
  ""pipeMatch"": { ""enabled"": true, ""materialTableYFraction"": 0.62 }
}";

        static readonly JsonSerializerOptions Opts = new()
        {
            PropertyNameCaseInsensitive = true,
            ReadCommentHandling = JsonCommentHandling.Skip,
            AllowTrailingCommas = true
        };

        public static Pdf2dfConfig Load(string? explicitPath)
        {
            var candidates = new List<string>();
            if (!string.IsNullOrEmpty(explicitPath)) candidates.Add(explicitPath);
            try { candidates.Add(Path.Combine(AppContext.BaseDirectory, "config.json")); } catch { }
            try { candidates.Add(Path.Combine(Directory.GetCurrentDirectory(), "config.json")); } catch { }
            string? used = candidates.FirstOrDefault(c => !string.IsNullOrEmpty(c) && File.Exists(c));

            Pdf2dfConfig cfg;
            try
            {
                cfg = used != null
                    ? (JsonSerializer.Deserialize<Pdf2dfConfig>(File.ReadAllText(used), Opts) ?? new Pdf2dfConfig())
                    : (JsonSerializer.Deserialize<Pdf2dfConfig>(DEFAULT_JSON, Opts) ?? new Pdf2dfConfig());
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[config] failed to load '{used}': {ex.Message}; using built-in defaults.");
                cfg = JsonSerializer.Deserialize<Pdf2dfConfig>(DEFAULT_JSON, Opts) ?? new Pdf2dfConfig();
            }
            if (used != null) Console.Error.WriteLine($"[config] loaded {used}");
            else Console.Error.WriteLine("[config] using built-in defaults");
            return cfg;
        }

        /// <summary>
        /// 精细度预设：把"灵敏↔粗略"的单旋钮映射到底层阈值/最小面积/高斯模糊/配准容差。
        /// fine = 灵敏（易报微小差异），coarse = 粗略（忽略小差异与几毫米偏移），medium = 默认。
        /// </summary>
        public void ApplyPrecision(string? p)
        {
            if (string.IsNullOrWhiteSpace(p)) p = Precision;
            switch (p.Trim().ToLowerInvariant())
            {
                case "fine":
                    GraphicDiff.Threshold = 40; GraphicDiff.MinComponentArea = 12; GraphicDiff.GaussianBlur = 3;
                    Registration.ToleranceMm = 0.5; break;
                case "coarse":
                    GraphicDiff.Threshold = 90; GraphicDiff.MinComponentArea = 60; GraphicDiff.GaussianBlur = 7;
                    Registration.ToleranceMm = 4.0; break;
                default: // medium
                    GraphicDiff.Threshold = 60; GraphicDiff.MinComponentArea = 24; GraphicDiff.GaussianBlur = 5;
                    Registration.ToleranceMm = 2.0; break;
            }
        }
    }
}
