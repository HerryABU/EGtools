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
        // 页面**图号**（标题栏右下角那个每页唯一的管段号）的提取区域，按页宽/页高比例。
        // 实测图号字段在 x≈0.90W, y≈0.83H（显示帧，y 向下）；取 0.85/0.78 有足够裕度，
        // 同时把散落在图内的管线号排除掉。若你的标题栏布局不同，调这两个值即可。
        public double SheetNoRegionX { get; set; } = 0.85;
        public double SheetNoRegionY { get; set; } = 0.78;
        // 单页"管段号种类数"上限：超过它说明这页是**总图/封面**（布满上百个管线号），
        // 不是单张图纸，此时不认它的图号（返回 null，交给线画指纹兜底）。
        // 实测真图纸 ≤10 种、总图 33/124/127 种，25 有很宽裕度。
        public int SheetNoMaxDistinct { get; set; } = 25;
        // -p/--pages 的语义：
        //   "filter"(默认) = 先按图号做**全本配对**，再用所给页码（=基准页号, 0基）在配对结果里
        //                    **筛选**。这样不会因为新版插/删页而把两页不同图纸钉在一起比。
        //   "pin"          = 旧行为：仅按索引配对（所给页 p ↔ 目标同号页 p），不做全本配对。
        public string PageMode { get; set; } = "filter";

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
            // 矢量比对容差(pt)：两版同名线画允许的位置偏差 = "挪多远还算同一根线"。
            // 1pt ≈ 0.353mm；默认 1.5pt ≈ 0.53mm，是工程图纸复检的合理尺度。
            // ⚠️ 历史上默认取 3.0 是为了**掩盖两个 bug**：①提取器把连续 `m` 当"追加点"，
            // 凭空造出渲染中不存在的幻影线段；②标定被"大容差尺度扫描"带偏（tol=15 下任何尺度
            // 都刷满分）。两者修掉后实测**完全相同的页在任何容差下(含 0.5pt)都是 0 差异**
            // （页 100:101 / 150:151 / 9:10 在 tol=1.0 均 runs=0），不必再靠大容差压误报。
            // 容差越小越灵敏：同一页在 tol=1/1.5/2/3 下的变更折线数为 170/103/63/53。
            public double VectorTolPt { get; set; } = 1.5;
            // 矢量比对方向容差(度)：防止密集区被"横穿而过"的无关线段误判为覆盖。
            public double VectorAngleTolDeg { get; set; } = 12.0;
            // **墨迹重合**容差(pt)：方向判据在极短线段上不可靠（CAD 圆弧是密排短线逼近的，
            // 单段可能只有 0.4~0.7pt，方向噪音极大），故若某采样点距对侧墨迹（不限方向）
            // 小于此值，仍判"未变"。它**不是**放宽位置容差：只在墨迹实际重合到亚 pt 时生效，
            // 且长线段需全部采样点满足，真正的线画改动不会被放过。0=关闭该豁免（回到纯方向判据）。
            public double NearInkTolPt { get; set; } = 0.5;
            // 页面配对（按线画指纹）的最小余弦相似度：低于此值视为"不同图纸"，不配对。
            // 实测同图纸≈0.99+、不同图纸≈0.3~0.6，0.85 有很宽的裕度。
            public double PageMatchMinSim { get; set; } = 0.85;
            // 第三态"变更"识别半径(pt)：未匹配的线画若在此半径内存在**方向相近**的对侧未匹配线画，
            // 判为"变更"(同位置形状改了, 绿) 而不是"删一根+加一根"(红)。
            // 0 = 关闭第三态（全部按 added/removed 两态报）。
            public double ChangeRadiusPt { get; set; } = 20.0;
            // ---- 差异密度过滤与报告粒度（都可调；默认弱过滤，避免误杀真实改动）----
            // 报表粒度：run=逐条变更折线（信息最全，但大改页可达十几万条）；cluster=按区域汇总
            // （默认，数量降两个数量级，贴合"这一片图面改了"的语义）。
            public string ReportMode { get; set; } = "cluster";
            // 丢弃总长小于此值(pt)的变更折线（抗锯齿级碎屑）。0=不过滤。
            public double MinRunLengthPt { get; set; } = 1.0;
            // 邻近聚类半径(pt)：包围盒间距小于此值的折线归为同一差异区域。0=不聚类。
            public double ClusterRadiusPt { get; set; } = 24.0;
            // 丢弃折线总长小于此值(pt)的差异区域。0=不过滤。调大可把零散微调整体忽略。
            public double MinClusterLengthPt { get; set; } = 0.0;
            // 丢弃折线数少于 this 的差异区域。0=不过滤。
            public int MinClusterRuns { get; set; } = 0;
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
            // 变更旧(蓝)文字插入方向 down/up/right/left
            public string TextInsertDirection { get; set; } = "down";
            // 文字差异框粒度：line=整行墨迹包围盒（默认）；word=逐词包围盒（更精确，
            // 避免同行内无关文字被一并改色，但一行改动会拆成多个框）。
            public string TextBoxMode { get; set; } = "word";
            // 字形原点命中差异框时允许的绝对容差(pt)。⚠️ 必须是小绝对值：差异框是整行墨迹盒
            // （一行可宽达 1600pt），若按框尺寸成比例外扩，x 判定会退化成"整页宽度都命中"，
            // 同行无关文字（含页边框注）会被一并改色。
            public double TextBoxPadPt { get; set; } = 2.0;
            // 文字"序号"匹配键的宽严：
            //   loose(默认) = 管段号 / NOTE n / 以及【行首纯数字】(材料表 NO 列)
            //   strict      = 只用管段号与 NOTE n，**不**把行首纯数字当键
            // ⚠️ loose 下，行首纯数字会成为匹配键；若两版行号含义/行序不同，会把毫不相关的
            // 两行配成一对 → 假 changed（实测页 3 的 '2 …对焊法兰…' 配上 '2 FLANGES …'）。
            // 遇到这类假配对时改 strict：宁可报成 added+removed，也不产出错误的"变更"。
            public string TextSerialMatchMode { get; set; } = "loose";
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
            // ---- 矢量"差异线"（沿真实变更线段描边）的线宽与不透明度 ----
            // CAD 图纸一次修订常有成百上千条线变更，粗实线会糊成一片、盖住底图。默认：
            //   细线(1.0pt) + 半透明(0.55) —— 既看得见是哪几条线，又保留底图可读性。
            // 注意与上面 LineWidth（填充块/轮廓标记的线宽）区分：那是像素路径的标记。
            public double StrokeWidthPt { get; set; } = 1.0;
            public double StrokeOpacity { get; set; } = 0.55;
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
  ""graphicDiff"": { ""enabled"": true, ""method"": ""edge"", ""graphicMethod"": ""vector"", ""vectorTolPt"": 1.5, ""vectorAngleTolDeg"": 12.0, ""nearInkTolPt"": 0.5, ""pageMatchMinSim"": 0.85, ""changeRadiusPt"": 20.0, ""reportMode"": ""cluster"", ""minRunLengthPt"": 1.0, ""clusterRadiusPt"": 24.0, ""minClusterLengthPt"": 0.0, ""minClusterRuns"": 0, ""threshold"": 60, ""excludeText"": true, ""gaussianBlur"": 5, ""minComponentArea"": 24, ""tolerancePx"": 0 },
  ""textDiff"": { ""enabled"": true, ""yTolerance"": 12, ""xTolerance"": 40, ""ignoreCase"": true, ""ignoreWhitespace"": true, ""moveTolerance"": 6 },
  ""registration"": { ""method"": ""phase"", ""toleranceMm"": 2.0, ""maxFeatures"": 5000, ""matchRatio"": 0.15, ""ransacThreshold"": 5.0 },
  ""annotation"": { ""traceColor"": ""#FF0000"", ""changeColor"": ""#16A34A"", ""textAddedColor"": ""#FF0000"", ""textRemovedColor"": ""#0000FF"", ""textInsertDirection"": ""down"", ""textBoxMode"": ""word"", ""textBoxPadPt"": 2.0, ""markStyle"": ""semi"", ""markStrokeColor"": """", ""lineWidth"": 3, ""fillOpacity"": 0.40, ""outline"": true, ""haloPx"": 0, ""strokeWidthPt"": 1.0, ""strokeOpacity"": 0.55, ""fontPathRegular"": ""C:/Windows/Fonts/arial.ttf"", ""fontPathItalic"": ""C:/Windows/Fonts/ariali.ttf"" },
  ""sheetNoRegionX"": 0.85, ""sheetNoRegionY"": 0.78, ""sheetNoMaxDistinct"": 25, ""pageMode"": ""filter"",
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
