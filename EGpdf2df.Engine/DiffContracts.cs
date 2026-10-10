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
        // 用户人工修正后重新上传的材料表（CSV/TSV）。提供后，管线号"材料表侧"以此表为准，
        // 与"图中管线号"做一一对应核对。
        [JsonPropertyName("correctedMaterialTable")] public string? CorrectedMaterialTable { get; set; }
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
        // 是否启用"图中管线号 ↔ 材料表"一一对应核对。
        [JsonPropertyName("pipeMatch")] public bool? PipeMatch { get; set; }
        // 材料表区域判定：页高比例阈值（默认 0.62）。可覆盖配置文件。
        [JsonPropertyName("materialTableYFraction")] public double? MaterialTableYFraction { get; set; }
        // 精细度预设：fine | medium | coarse（覆盖配置文件精度，映射阈值/最小面积/容差）。
        [JsonPropertyName("precision")] public string? Precision { get; set; }
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
        [JsonPropertyName("alignments")] public List<AlignInfo> Alignments { get; set; } = new();
        [JsonPropertyName("pipeMatch")] public PipeMatch? PipeMatch { get; set; }
    }

    public sealed class AlignInfo
    {
        [JsonPropertyName("page")] public int Page { get; set; }
        [JsonPropertyName("scale")] public double Scale { get; set; }      // 统一缩放（基准→目标）
        [JsonPropertyName("dxPt")] public double DxPt { get; set; }        // 平移 dx（PDF pt）
        [JsonPropertyName("dyPt")] public double DyPt { get; set; }        // 平移 dy（PDF pt）
    }

    // 图中管线号 ↔ 材料表 一一对应核对结果。
    public sealed class PipeMatch
    {
        [JsonPropertyName("enabled")] public bool Enabled { get; set; }
        [JsonPropertyName("tableSource")] public string TableSource { get; set; } = ""; // "pdf" | "upload:<file>"
        [JsonPropertyName("matched")] public List<string> Matched { get; set; } = new();       // 两侧都有
        [JsonPropertyName("figureOnly")] public List<string> FigureOnly { get; set; } = new(); // 图中画出但材料表缺失
        [JsonPropertyName("tableOnly")] public List<string> TableOnly { get; set; } = new();   // 材料表有但图中未画出
    }

    public sealed class DiffSummary
    {
        [JsonPropertyName("graphicDiffs")] public int GraphicDiffs { get; set; }
        [JsonPropertyName("textDiffs")] public int TextDiffs { get; set; }
        [JsonPropertyName("totalPages")] public int TotalPages { get; set; }
        [JsonPropertyName("matchedPages")] public List<string> MatchedPages { get; set; } = new();
        [JsonPropertyName("addedPages")] public List<string> AddedPages { get; set; } = new();    // target-only (new drawings/sheets)
        [JsonPropertyName("removedPages")] public List<string> RemovedPages { get; set; } = new(); // baseline-only (dropped drawings/sheets)
    }

    public sealed class DiffItem
    {
        [JsonPropertyName("page")] public int Page { get; set; }
        [JsonPropertyName("type")] public string Type { get; set; } = "";   // "graphic" | "text"
        [JsonPropertyName("bbox")] public Bbox Bbox { get; set; } = new();
        [JsonPropertyName("confidence")] public double Confidence { get; set; }
        // Direction of the change (drives the PDF annotation):
        //   "added"    → new-only              → RED SOLID (rectangle for graphics, circle for text)
        //   "removed"  → old-only              → BLUE DASHED + light-blue italic "disappeared" text
        //   "modified" → text changed          → BLUE DASHED + light-blue italic old text
        //   "moved"    → text shifted position → BLUE DASHED + light-blue italic old text (old spot)
        [JsonPropertyName("changeType")] public string? ChangeType { get; set; }
        [JsonPropertyName("oldText")] public string? OldText { get; set; }
        [JsonPropertyName("newText")] public string? NewText { get; set; }
    }
}
