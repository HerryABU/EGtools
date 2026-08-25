using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace EGpdf2excel.Engine
{
    /// <summary>
    /// External configuration for the PDF extraction engine. All vocabularies,
    /// regex patterns, column positions and page-layout parameters are read from
    /// here (a JSON file next to the executable) instead of being hard-coded, so
    /// the tool adapts to different drawing conventions without recompiling.
    /// A built-in default is used when no config file is present.
    /// </summary>
    public sealed class ExtractConfig
    {
        public string Version { get; set; } = "4.2.1";

        public PipeNoCfg PipeNo { get; set; } = new();
        public KeywordsCfg Keywords { get; set; } = new();
        public Dictionary<string, string> ComponentCanon { get; set; } = new();
        public ColumnsCfg Columns { get; set; } = new();
        public PageCfg Page { get; set; } = new();

        // ---- compiled helpers (built after load) ----
        [System.Text.Json.Serialization.JsonIgnore]
        public HashSet<string> ComponentKeywords { get; private set; } = new();
        [System.Text.Json.Serialization.JsonIgnore]
        public HashSet<string> SectionKeywords { get; private set; } = new();
        [System.Text.Json.Serialization.JsonIgnore]
        public HashSet<string> GroupKeywords { get; private set; } = new();
        [System.Text.Json.Serialization.JsonIgnore]
        public HashSet<string> TypeBanner { get; private set; } = new();
        [System.Text.Json.Serialization.JsonIgnore]
        public HashSet<string> Labels { get; private set; } = new();
        [System.Text.Json.Serialization.JsonIgnore]
        public string[] SummarySubstrings { get; private set; } = new string[0];
        [System.Text.Json.Serialization.JsonIgnore]
        public HashSet<string> DnHeaderTokens { get; private set; } = new();

        public class PipeNoCfg
        {
            public bool FallbackFromFileName { get; set; } = true;
        }

        public class KeywordsCfg
        {
            public List<string> Component { get; set; } = new();
            public List<string> Section { get; set; } = new();
            public List<string> TypeBanner { get; set; } = new();
            public List<string> Labels { get; set; } = new();
            public List<string> SummarySubstrings { get; set; } = new();
            public List<string> DnHeaderTokens { get; set; } = new();
        }

        public class ColumnsCfg
        {
            public double HeaderXLo { get; set; } = 900;
            public double HeaderXHi { get; set; } = 1800;
            public Dictionary<string, double> Fallback { get; set; } = new();
        }

        public class PageCfg
        {
            public double HeaderYMaxFraction { get; set; } = 0.95;
            public double? HardBottom { get; set; }
            public double AutoBottomFraction { get; set; } = 0.85;
            // Fraction of the font size used as the inter-word gap threshold.
            public double WordGapFraction { get; set; } = 0.18;
        }

        /// <summary>Compile the string lists/dictionaries into the fast lookup structures.</summary>
        public void Compile()
        {
            ComponentKeywords = Up(Keywords.Component);
            SectionKeywords = Up(Keywords.Section);
            TypeBanner = Up(Keywords.TypeBanner);
            Labels = Up(Keywords.Labels);
            DnHeaderTokens = Up(Keywords.DnHeaderTokens);
            SummarySubstrings = Keywords.SummarySubstrings.Where(s => !string.IsNullOrWhiteSpace(s)).ToArray();
            GroupKeywords = new HashSet<string>(ComponentKeywords, StringComparer.OrdinalIgnoreCase) { "FITTINGS" };
            // make ComponentCanon keys case-insensitive uppercase
            var canon = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var kv in ComponentCanon) canon[kv.Key.ToUpperInvariant()] = kv.Value;
            ComponentCanon = canon;
        }

        static HashSet<string> Up(List<string> l)
        {
            var h = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            if (l != null) foreach (var s in l) if (!string.IsNullOrWhiteSpace(s)) h.Add(s.Trim());
            return h;
        }

        // ------------------------------------------------------------------
        //  Default configuration (kept in sync with config.json). Used when
        //  no external config.json is present.
        // ------------------------------------------------------------------
        const string DEFAULT_JSON = @"
{
  ""version"": ""4.2.1"",
  ""pipeNo"": {
    ""fallbackFromFileName"": true
  },
  ""keywords"": {
    ""component"": [ ""PIPE"",""PIPES"",""TEE"",""ELBOW"",""ELBOWS"",""FLANGE"",""FLANGES"",""REDUCER"",""REDUCERS"",""OLET"",""OLETS"",""GASKET"",""GASKETS"",""BOLT"",""BOLTS"",""NUT"",""NUTS"",""VALVE"",""VALVES"",""SPECTACLE"",""BLIND"",""STUD"",""STUDS"",""WASHER"",""WASHERS"",""SUPPORT"",""SUPPORTS"",""WELD"",""NECK"",""NIPPLE"",""CAP"",""COUPLING"",""BUSH"",""UNION"",""CLAMP"",""BRANCH"",""SLEEVE"",""ADAPTER"" ],
    ""section"": [ ""fittings"",""flanges"",""valves"",""pipes"",""supports"",""bolts"",""nuts"",""washers"",""gaskets"",""olets"",""components"",""studs"",""screws"",""connectors"",""spools"",""materials"",""fabrication"",""erection"" ],
    ""typeBanner"": [ ""PIPE"", ""PIPES"" ],
    ""labels"": [ ""no"",""dn"",""qty"",""weight"",""total"",""fabrication"",""erection"",""materials"",""component"",""description"",""summary"" ],
    ""summarySubstrings"": [ ""total"",""fabrication"",""erection"",""materials"",""weight"",""summary"" ],
    ""dnHeaderTokens"": [ ""mm"", ""(mm)"", ""dn"", ""no"", ""n"", ""o"" ]
  },
  ""componentCanon"": {
    ""WELDOLET"": ""WELD OLET"",
    ""WELDNECKFLANGE"": ""WELD NECK FLANGE"",
    ""BLINDFLANGE"": ""BLIND FLANGE"",
    ""PIPESUPPORT"": ""PIPE SUPPORT"",
    ""SPECTACLEFLANGE"": ""SPECTACLE FLANGE""
  },
  ""columns"": {
    ""headerXLo"": 900,
    ""headerXHi"": 1800,
    ""fallback"": { ""NO"": 1123, ""DN"": 1142, ""COMPONENT"": 1221, ""DESCRIPTION"": 1276, ""QTY"": 1548, ""WEIGHT"": 1606 }
  },
  ""page"": {
    ""headerYMaxFraction"": 0.95,
    ""hardBottom"": null,
    ""autoBottomFraction"": 0.85,
    ""wordGapFraction"": 0.18
  }
}";

        static readonly JsonSerializerOptions Opts = new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true,
            ReadCommentHandling = JsonCommentHandling.Skip,
            AllowTrailingCommas = true
        };

        /// <summary>Load config from an explicit path, else from config.json next to the exe, else the built-in default.</summary>
        public static ExtractConfig Load(string? explicitPath)
        {
            ExtractConfig cfg;
            var candidates = new List<string>();
            if (!string.IsNullOrEmpty(explicitPath)) candidates.Add(explicitPath);
            try { candidates.Add(Path.Combine(AppContext.BaseDirectory, "config.json")); } catch { }
            try { candidates.Add(Path.Combine(Directory.GetCurrentDirectory(), "config.json")); } catch { }

            string? used = null;
            foreach (var c in candidates)
            {
                if (!string.IsNullOrEmpty(c) && File.Exists(c)) { used = c; break; }
            }

            try
            {
                if (used != null)
                {
                    cfg = JsonSerializer.Deserialize<ExtractConfig>(File.ReadAllText(used), Opts) ?? new ExtractConfig();
                }
                else
                {
                    cfg = JsonSerializer.Deserialize<ExtractConfig>(DEFAULT_JSON, Opts) ?? new ExtractConfig();
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[config] failed to load '{used}': {ex.Message}; using built-in defaults.");
                cfg = JsonSerializer.Deserialize<ExtractConfig>(DEFAULT_JSON, Opts) ?? new ExtractConfig();
            }

            // Merge an external file over the built-in default so a partial file
            // (e.g. only the keyword list) still keeps the other defaults.
            if (used != null)
            {
                var def = JsonSerializer.Deserialize<ExtractConfig>(DEFAULT_JSON, Opts) ?? new ExtractConfig();
                Merge(def, cfg);
            }
            cfg.Compile();
            if (used != null) Console.Error.WriteLine($"[config] loaded {used}");
            else Console.Error.WriteLine("[config] using built-in defaults");
            return cfg;
        }

        static void Merge(ExtractConfig def, ExtractConfig over)
        {
            if (over == null) return;
            if (over.PipeNo == null) over.PipeNo = def.PipeNo;
            if (over.Keywords == null) over.Keywords = def.Keywords;
            else
            {
                if (over.Keywords.Component == null || over.Keywords.Component.Count == 0) over.Keywords.Component = def.Keywords.Component;
                if (over.Keywords.Section == null || over.Keywords.Section.Count == 0) over.Keywords.Section = def.Keywords.Section;
                if (over.Keywords.TypeBanner == null || over.Keywords.TypeBanner.Count == 0) over.Keywords.TypeBanner = def.Keywords.TypeBanner;
                if (over.Keywords.Labels == null || over.Keywords.Labels.Count == 0) over.Keywords.Labels = def.Keywords.Labels;
                if (over.Keywords.SummarySubstrings == null || over.Keywords.SummarySubstrings.Count == 0) over.Keywords.SummarySubstrings = def.Keywords.SummarySubstrings;
                if (over.Keywords.DnHeaderTokens == null || over.Keywords.DnHeaderTokens.Count == 0) over.Keywords.DnHeaderTokens = def.Keywords.DnHeaderTokens;
            }
            if (over.ComponentCanon == null || over.ComponentCanon.Count == 0) over.ComponentCanon = def.ComponentCanon;
            if (over.Columns == null) over.Columns = def.Columns;
            else if (over.Columns.Fallback == null || over.Columns.Fallback.Count == 0) over.Columns.Fallback = def.Columns.Fallback;
            if (over.Page == null) over.Page = def.Page;
        }
    }
}
