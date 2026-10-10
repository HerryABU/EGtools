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
            public string ColorNew { get; set; } = "#FF0000";   // new-only / changed → RED SOLID
            public string ColorOld { get; set; } = "#1E66E0";   // old-only → BLUE DASHED
            public string ColorOldText { get; set; } = "#66B2FF"; // 消失文字（浅蓝斜体）— disappeared text
            public int LineWidth { get; set; } = 3;
            public bool Fill { get; set; } = false;             // text diffs (new-side): translucent fill
            public int FillAlpha { get; set; } = 60;            // 0..255
            // 文字/序号差异用"圈画"（椭圆）而非方框；图像差异仍用矩形描红。
            public bool CircleText { get; set; } = true;
            // 嵌入 Unicode 字体（默认 Windows Arial，含土耳其语等字形），让"消失文字"
            // 的浅蓝斜体真正渲染；字体文件缺失时回退内置 Helvetica（仅 ASCII）。
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
  ""graphicDiff"": { ""enabled"": true, ""method"": ""edge"", ""threshold"": 60, ""excludeText"": true, ""gaussianBlur"": 5, ""minComponentArea"": 24, ""tolerancePx"": 0 },
  ""textDiff"": { ""enabled"": true, ""yTolerance"": 12, ""xTolerance"": 40, ""ignoreCase"": true, ""ignoreWhitespace"": true, ""moveTolerance"": 6 },
  ""registration"": { ""method"": ""phase"", ""toleranceMm"": 2.0, ""maxFeatures"": 5000, ""matchRatio"": 0.15, ""ransacThreshold"": 5.0 },
  ""annotation"": { ""colorNew"": ""#FF0000"", ""colorOld"": ""#1E66E0"", ""colorOldText"": ""#66B2FF"", ""lineWidth"": 3, ""fill"": false, ""fillAlpha"": 60, ""circleText"": true, ""fontPathRegular"": ""C:/Windows/Fonts/arial.ttf"", ""fontPathItalic"": ""C:/Windows/Fonts/ariali.ttf"" },
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
