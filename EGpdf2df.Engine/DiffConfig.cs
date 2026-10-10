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
        public GraphicCfg GraphicDiff { get; set; } = new();
        public TextCfg TextDiff { get; set; } = new();
        public RegistrationCfg Registration { get; set; } = new();
        public AnnotationCfg Annotation { get; set; } = new();

        public sealed class GraphicCfg
        {
            public bool Enabled { get; set; } = true;
            public string Method { get; set; } = "edge";   // "edge" | "absdiff" | "ssim"
            public int Threshold { get; set; } = 60;        // absdiff path: 0..255
            public bool ExcludeText { get; set; } = true;
            public int GaussianBlur { get; set; } = 5;
            public int MinComponentArea { get; set; } = 24;  // px², drops antialias noise
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
            public string Method { get; set; } = "similarity";   // "similarity" | "none"
            public int MaxFeatures { get; set; } = 5000;
            public double MatchRatio { get; set; } = 0.15;  // Lowe ratio
            public double RansacThreshold { get; set; } = 5.0; // px
        }

        public sealed class AnnotationCfg
        {
            public string ColorNew { get; set; } = "#FF0000";   // new-only / changed → RED SOLID
            public string ColorOld { get; set; } = "#1E66E0";   // old-only → BLUE DASHED
            public int LineWidth { get; set; } = 3;
            public bool Fill { get; set; } = false;             // text diffs (new-side): translucent fill
            public int FillAlpha { get; set; } = 60;            // 0..255
        }

        // ----------------------------------------------------------------
        //  Loading / merge over built-in defaults.
        // ----------------------------------------------------------------
        const string DEFAULT_JSON = @"
{
  ""renderDpi"": 200,
  ""graphicDiff"": { ""enabled"": true, ""method"": ""ssim"", ""threshold"": 60, ""excludeText"": true, ""gaussianBlur"": 5, ""minComponentArea"": 24 },
  ""textDiff"": { ""enabled"": true, ""yTolerance"": 12, ""xTolerance"": 40, ""ignoreCase"": true, ""ignoreWhitespace"": true, ""moveTolerance"": 6 },
  ""registration"": { ""method"": ""similarity"", ""maxFeatures"": 5000, ""matchRatio"": 0.15, ""ransacThreshold"": 5.0 },
  ""annotation"": { ""colorNew"": ""#FF0000"", ""colorOld"": ""#1E66E0"", ""lineWidth"": 3, ""fill"": false, ""fillAlpha"": 60 }
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
    }
}
