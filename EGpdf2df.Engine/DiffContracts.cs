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
        // 标注颜色（前端颜色选择器传入色号 #RRGGBB）：
        //   trace  = 新增/删除 描红；change = 变更 描红(绿)；circle = 文字/序号圈画边色；text = 消失/变更文字。
        [JsonPropertyName("colors")] public ReqColors? Colors { get; set; }
        // 线宽（描红边线粗细，可调参数，覆盖配置文件 lineWidth）。
        [JsonPropertyName("lineWidth")] public double? LineWidth { get; set; }
        // 真描红填充透明度 0~1（覆盖配置文件 fillOpacity）。
        [JsonPropertyName("fillOpacity")] public double? FillOpacity { get; set; }
        // 是否在填充外再描一条同形边线（覆盖配置文件 outline）。
        [JsonPropertyName("outline")] public bool? Outline { get; set; }
        // 填充向外扩张像素(0=精准贴合形状，覆盖配置文件 haloPx)。
        [JsonPropertyName("haloPx")] public int? HaloPx { get; set; }
    }

    // 前端可选传入的标注颜色（缺省则使用配置文件默认）。
    public sealed class ReqColors
    {
        [JsonPropertyName("trace")] public string? Trace { get; set; }   // 新增/删除 描红
        [JsonPropertyName("change")] public string? Change { get; set; } // 变更 描红(绿)
        [JsonPropertyName("circle")] public string? Circle { get; set; } // 圆圈边色
        [JsonPropertyName("text")] public string? Text { get; set; }    // 文字
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
        //   "added"    → new-only              → TraceColor 实线填充(真描红)；文字用 CircleColor 实线圈
        //   "removed"  → old-only              → TraceColor 虚线填充(真描红)；文字用 CircleColor 虚线圈 + TextColor 浅色标注旧文字
        //   "changed"  → 同序号/同位置、内容不同 → ChangeColor 实线填充(真描红,绿)；文字用 ChangeColor 线圈 + 绿旧文字
        //   "modified" → 旧别名，等同 changed（文本按序号配对后发现内容改动）
        //   "moved"    → 文本仅位移、内容相同   → CircleColor 虚线圈（不报警，仅标记）
        [JsonPropertyName("changeType")] public string? ChangeType { get; set; }
        [JsonPropertyName("oldText")] public string? OldText { get; set; }
        [JsonPropertyName("newText")] public string? NewText { get; set; }
    }
}
