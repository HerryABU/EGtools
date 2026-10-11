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
        //   trace  = 新增/删除 图形描红；change = 变更 图形描红(绿)；
        //   textAdded  = 新增文字改色(默认红)；textRemoved = 删除文字改色(默认蓝)。
        // 文字不再画圈/点，而是直接重绘成对应颜色（git +/- 风格：红=新增，蓝=删除）。
        [JsonPropertyName("colors")] public ReqColors? Colors { get; set; }
        // 图形差异的标记形式：semi(半透明填充+深色描边，默认) / solid(实心填充) / outline(仅描边)。
        [JsonPropertyName("markStyle")] public string? MarkStyle { get; set; }
        // 变更时"旧(蓝)"文字相对"新(红)"文字的插入方向：down(下) / up(上) / right(右) / left(左)。
        [JsonPropertyName("textInsertDirection")] public string? TextInsertDirection { get; set; }
        // 线宽（描红边线粗细，可调参数，覆盖配置文件 lineWidth）。
        [JsonPropertyName("lineWidth")] public double? LineWidth { get; set; }
        // 真描红填充透明度 0~1（覆盖配置文件 fillOpacity）。
        [JsonPropertyName("fillOpacity")] public double? FillOpacity { get; set; }
        // 是否在填充外再描一条同形边线（覆盖配置文件 outline）。
        [JsonPropertyName("outline")] public bool? Outline { get; set; }
        // 填充向外扩张像素(0=精准贴合形状，覆盖配置文件 haloPx)。
        [JsonPropertyName("haloPx")] public int? HaloPx { get; set; }
        // 矢量"差异线"（沿真实变更线段描边）的线宽(pt)与不透明度 0~1。默认 1.0 / 0.55。
        // 与上面的 lineWidth（填充/轮廓标记线宽）区分：那是像素路径的标记。
        [JsonPropertyName("strokeWidthPt")] public double? StrokeWidthPt { get; set; }
        [JsonPropertyName("strokeOpacity")] public double? StrokeOpacity { get; set; }
        // -p/--pages 的语义：filter(默认，全本按图号配对后筛选) / pin(仅按索引配对，旧行为)。
        [JsonPropertyName("pageMode")] public string? PageMode { get; set; }

        // ---- 图形差异检测路径与矢量参数（前端 / 命令行可调）----
        // 图形检测路径：vector(默认，纯矢量、误报低) / pixel(旧的渲染+OpenCV 回退)。
        [JsonPropertyName("graphicMethod")] public string? GraphicMethod { get; set; }
        // 矢量比对容差(pt)：两版同名线画允许的位置偏差。默认 3.0。
        [JsonPropertyName("vectorTolPt")] public double? VectorTolPt { get; set; }
        // 方向容差(度)：防密集区被无关线段误判为覆盖。默认 12。
        [JsonPropertyName("vectorAngleTolDeg")] public double? VectorAngleTolDeg { get; set; }
        // "墨迹重合"容差(pt)：方向判据在极短线段(0.4~0.7pt，圆弧逼近段)上不可靠，
        // 若采样点距对侧墨迹(不限方向)小于此值则仍判未变。默认 0.5，0=关闭豁免。
        [JsonPropertyName("nearInkTolPt")] public double? NearInkTolPt { get; set; }
        // 页面配对最小线画相似度。默认 0.85。
        [JsonPropertyName("pageMatchMinSim")] public double? PageMatchMinSim { get; set; }
        // 第三态"变更"识别半径(pt)：未匹配线画若在此半径内存在方向相近的对侧未匹配线画，
        // 判为变更(绿) 而非 删一根+加一根(红)。0=关闭第三态，只报新增/删除两态。
        [JsonPropertyName("changeRadiusPt")] public double? ChangeRadiusPt { get; set; }
        // 报告粒度：cluster(按差异区域汇总，默认) / run(逐条变更折线)。
        [JsonPropertyName("reportMode")] public string? ReportMode { get; set; }
        // 丢弃总长小于此值(pt)的变更折线（碎屑噪声）。
        [JsonPropertyName("minRunLengthPt")] public double? MinRunLengthPt { get; set; }
        // 邻近聚类半径(pt)：间距小于此值的折线归为同一差异区域。
        [JsonPropertyName("clusterRadiusPt")] public double? ClusterRadiusPt { get; set; }
        // 丢弃折线总长小于此值(pt)的差异区域（把零散微调整体忽略）。
        [JsonPropertyName("minClusterLengthPt")] public double? MinClusterLengthPt { get; set; }
        // 丢弃折线数少于此值的差异区域。
        [JsonPropertyName("minClusterRuns")] public int? MinClusterRuns { get; set; }

        // ---- 文字差异参数 ----
        // 差异框粒度：word(逐词框，更精确) / line(整行墨迹框)。
        [JsonPropertyName("textBoxMode")] public string? TextBoxMode { get; set; }
        // 字形原点命中差异框的绝对容差(pt)。必须是小绝对值（见 AnnotationCfg 注释）。
        [JsonPropertyName("textBoxPadPt")] public double? TextBoxPadPt { get; set; }
        // 序号匹配宽严：loose(含材料表行首纯数字) / strict(只认管段号与 NOTE n)。
        [JsonPropertyName("textSerialMatchMode")] public string? TextSerialMatchMode { get; set; }

        // ---- 图号配对参数 ----
        // 图号字段所处的页面右侧起点比例（按文本范围，默认 0.85）。标题栏布局不同时调它。
        [JsonPropertyName("sheetNoRegionX")] public double? SheetNoRegionX { get; set; }
        // 图号字段所处的页面下方起点比例（默认 0.78）。
        [JsonPropertyName("sheetNoRegionY")] public double? SheetNoRegionY { get; set; }
        // 单页"管段号种类数"上限（默认 25）：超过即视为总图/封面，不认其图号。
        [JsonPropertyName("sheetNoMaxDistinct")] public int? SheetNoMaxDistinct { get; set; }
    }

    // 前端可选传入的标注颜色（缺省则使用配置文件默认）。
    public sealed class ReqColors
    {
        [JsonPropertyName("trace")] public string? Trace { get; set; }   // 新增/删除 图形描红
        [JsonPropertyName("change")] public string? Change { get; set; } // 变更 图形描红(绿)
        [JsonPropertyName("textAdded")] public string? TextAdded { get; set; }     // 新增文字改色(红)
        [JsonPropertyName("textRemoved")] public string? TextRemoved { get; set; } // 删除文字改色(蓝)
        [JsonPropertyName("markStroke")] public string? MarkStroke { get; set; } // 图形标记描边色(留空=自动取标记色深色版)
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
        //   "added"    → new-only              → 图形 TraceColor 实线填充(真描红)；文字重绘为 TextAddedColor(红)
        //   "removed"  → old-only              → 图形 TraceColor 虚线填充(真描红)；旧文字重绘为 TextRemovedColor(蓝)
        //   "changed"  → 同序号/同位置、内容不同 → 图形 ChangeColor 实线填充(真描红,绿)；
        //                                       文字 新内容→红(覆盖原黑字)、旧内容→蓝(按 textInsertDirection 插入原文本框附近)
        //   "modified" → 旧别名，等同 changed（文本按序号配对后发现内容改动）
        //   "moved"    → 文本仅位移、内容相同   → 不报警（仅记录）
        [JsonPropertyName("changeType")] public string? ChangeType { get; set; }
        [JsonPropertyName("oldText")] public string? OldText { get; set; }
        [JsonPropertyName("newText")] public string? NewText { get; set; }
        // 原始行结构（用于重绘时保持"字号 + 换行"与原文一致）：每条目按行记录
        // 原始左基线 x、基线 y(display 帧, y-down)、原始字号 size、该行文字。
        // 新增 → NewLines；删除 → OldLines；变更 → 两者都有（红=新, 蓝=旧）。
        [JsonPropertyName("fontSize")] public double FontSize { get; set; }
        [JsonPropertyName("oldLines")] public List<TextLine>? OldLines { get; set; }
        [JsonPropertyName("newLines")] public List<TextLine>? NewLines { get; set; }
    }

    // 单条文字的原始一行（重绘保真用）。坐标均为 DISPLAY 帧(pt, y-down, 原点左上)。
    public sealed class TextLine
    {
        [JsonPropertyName("text")] public string Text { get; set; } = "";
        [JsonPropertyName("x")] public double X { get; set; }          // 行左基线 x
        [JsonPropertyName("baseline")] public double Baseline { get; set; } // 基线 y(≈ 字底部)
        [JsonPropertyName("height")] public double Height { get; set; } // 页面实际渲染高度(pt，含缩放)
        [JsonPropertyName("size")] public double Size { get; set; }    // 原始 Tf 字号(pt，可能含矩阵缩放，仅参考)
        // 该行在 DISPLAY 帧下的真实墨水包围盒(原点左上, y-down)，由原始 Word 几何得出，
        // 供内容流重着色/抽取时按几何精确匹配 Tj/TJ 文本运行(避免坐标/字号猜测)。
        [JsonPropertyName("x0")] public double X0 { get; set; }
        [JsonPropertyName("y0")] public double Y0 { get; set; }
        [JsonPropertyName("x1")] public double X1 { get; set; }
        [JsonPropertyName("y1")] public double Y1 { get; set; }
        // 逐词包围盒（DISPLAY 帧 pt, y-down）。textBoxMode=word 时按它出框 → 只改真正变动的词，
        // 不牵连同一行内的其它文字。
        [JsonPropertyName("words")] public List<TextWord>? Words { get; set; }
    }

    // 单个词的墨迹包围盒（DISPLAY 帧, y-down）。
    public sealed class TextWord
    {
        [JsonPropertyName("t")] public string T { get; set; } = "";
        [JsonPropertyName("x0")] public double X0 { get; set; }
        [JsonPropertyName("y0")] public double Y0 { get; set; }
        [JsonPropertyName("x1")] public double X1 { get; set; }
        [JsonPropertyName("y1")] public double Y1 { get; set; }
    }
}
