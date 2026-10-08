using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace EGpdf2excel.Engine
{
    /// <summary>
    /// JSON contract for the region-selection extraction operation. The Qt front-end
    /// draws rubber-band boxes on the PDF and ships them here as rectangles in PDF
    /// points, origin TOP-LEFT, y-DOWN (the SAME convention a rendered page image
    /// uses: x grows right, y grows down, page height = Bounds height). This matches
    /// MuPDF's structured-text coordinate space exactly, so no flipping is needed.
    /// Nothing about a specific drawing is hard-coded: every region is supplied at
    /// run time. The kernel replies on stdout with the same contract, fully
    /// decoupled from any UI.
    /// </summary>
    public class RegionRequest
    {
        /// <summary>Input PDF path (supports non-ASCII paths via a temp copy).</summary>
        public string? Pdf { get; set; }
        /// <summary>0-based pages to consider. If null, every page referenced by a region.</summary>
        public List<int>? Pages { get; set; }
        /// <summary>Box selections. Each carries its page, its rectangle, and a semantic kind.</summary>
        public List<ReqRegion>? Regions { get; set; }
        /// <summary>Per-request option overrides (merged over the config defaults).</summary>
        public RegionOptions? Options { get; set; }
    }

    public class ReqRegion
    {
        /// <summary>0-based page index.</summary>
        public int Page { get; set; }
        /// <summary>Rectangle in PDF points, origin top-left, y-down (image convention).</summary>
        public Bbox? Bbox { get; set; }
        /// <summary>One of: "global_table" | "detail_table" | "material_no_region".</summary>
        public string? Kind { get; set; }
    }

    public class Bbox
    {
        public double X { get; set; }
        public double Y { get; set; }
        public double Width { get; set; }
        public double Height { get; set; }
    }

    public class RegionOptions
    {
        /// <summary>Use MuPDF's native OCR only when a region yields no/low vector text.</summary>
        public bool OcrFallback { get; set; } = true;
        /// <summary>OCR language hints, e.g. ["eng","chi_sim"] → TesseractLanguage.Best.*.</summary>
        public List<string>? Languages { get; set; }
        /// <summary>Merge overlapping boxes of the same page+kind into one union region.</summary>
        public bool MergeOverlapping { get; set; } = true;
        /// <summary>Word-splitting sensitivity (ink-box gap as a fraction of font size).</summary>
        public double WordGapFraction { get; set; } = 0.18;
        /// <summary>Row clustering tolerance in points.</summary>
        public double RowTolerance { get; set; } = 4.0;
        /// <summary>Column split sensitivity (gap as a fraction of the row's cap height).</summary>
        public double ColGapFraction { get; set; } = 0.6;
    }

    public class RegionResponse
    {
        public string? Pdf { get; set; }
        public List<RegionResult> Regions { get; set; } = new();
        public List<string> Warnings { get; set; } = new();
    }

    public class RegionResult
    {
        public int Page { get; set; }
        public string? Kind { get; set; }
        public Bbox? Bbox { get; set; }
        /// <summary>True when the result came from OCR rather than the vector layer.</summary>
        public bool OcrUsed { get; set; }
        /// <summary>material_no_region: every pipe/line number found in the region.</summary>
        public List<string>? PipeNumbers { get; set; }
        /// <summary>table kinds: row-major grid of cell strings.</summary>
        public List<List<string>>? Table { get; set; }
        /// <summary>All words recovered inside the region (handy for debugging / overlay).</summary>
        public List<WordJson>? Words { get; set; }
    }

    public class WordJson
    {
        public double X0 { get; set; }
        public double Y0 { get; set; }
        public double X1 { get; set; }
        public double Y1 { get; set; }
        public double Xc { get; set; }
        public double Yc { get; set; }
        public string? Text { get; set; }
    }
}
