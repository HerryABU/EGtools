using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace EGpdf2df.Engine
{
    // ---- JSON contract between the (Qt) front-end and this CLI kernel. ----
    // All coordinates are PDF page coordinates (pt, origin top-left) unless noted.

    public sealed class DiffRequest
    {
        [JsonPropertyName("baseline")] public string Baseline { get; set; } = "";
        [JsonPropertyName("target")] public string Target { get; set; } = "";
        [JsonPropertyName("output")] public string? Output { get; set; }
        [JsonPropertyName("pages")] public List<int>? Pages { get; set; }
        [JsonPropertyName("regions")] public List<ReqRegion>? Regions { get; set; }
        [JsonPropertyName("options")] public ReqOptions? Options { get; set; }
    }

    public sealed class ReqRegion
    {
        [JsonPropertyName("page")] public int Page { get; set; }
        [JsonPropertyName("bbox")] public Bbox Bbox { get; set; } = new();
    }

    public sealed class ReqOptions
    {
        [JsonPropertyName("renderDpi")] public int? RenderDpi { get; set; }
        [JsonPropertyName("graphicDiff")] public bool? GraphicDiff { get; set; }
        [JsonPropertyName("textDiff")] public bool? TextDiff { get; set; }
        [JsonPropertyName("excludeText")] public bool? ExcludeText { get; set; }
        [JsonPropertyName("diffThreshold")] public int? DiffThreshold { get; set; }
        [JsonPropertyName("yTolerance")] public int? YTolerance { get; set; }
        [JsonPropertyName("xTolerance")] public int? XTolerance { get; set; }
    }

    public sealed class Bbox
    {
        [JsonPropertyName("x")] public double X { get; set; }
        [JsonPropertyName("y")] public double Y { get; set; }
        [JsonPropertyName("width")] public double Width { get; set; }
        [JsonPropertyName("height")] public double Height { get; set; }
    }

    public sealed class DiffResponse
    {
        [JsonPropertyName("output")] public string Output { get; set; } = "";
        [JsonPropertyName("summary")] public DiffSummary Summary { get; set; } = new();
        [JsonPropertyName("diffs")] public List<DiffItem> Diffs { get; set; } = new();
    }

    public sealed class DiffSummary
    {
        [JsonPropertyName("graphicDiffs")] public int GraphicDiffs { get; set; }
        [JsonPropertyName("textDiffs")] public int TextDiffs { get; set; }
        [JsonPropertyName("totalPages")] public int TotalPages { get; set; }
    }

    public sealed class DiffItem
    {
        [JsonPropertyName("page")] public int Page { get; set; }
        [JsonPropertyName("type")] public string Type { get; set; } = "";   // "graphic" | "text"
        [JsonPropertyName("bbox")] public Bbox Bbox { get; set; } = new();
        [JsonPropertyName("confidence")] public double Confidence { get; set; }
        [JsonPropertyName("changeType")] public string? ChangeType { get; set; } // text only
        [JsonPropertyName("oldText")] public string? OldText { get; set; }
        [JsonPropertyName("newText")] public string? NewText { get; set; }
    }
}
