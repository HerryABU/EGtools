using System;
using System.Collections.Generic;
using System.IO;
using iText.Kernel.Colors;
using iText.Kernel.Pdf;
using iText.Kernel.Pdf.Canvas;
using iText.Kernel.Font;
using iText.IO.Font;
using iText.IO.Font.Constants;

namespace EGpdf2df.Engine
{
    /// <summary>
    /// Per-target-page vector annotation payload, accumulated by DiffEngine and consumed
    /// by <see cref="Write"/>. Contour points are in the TARGET pixel frame (y-down, origin
    /// top-left of the rendered page) and are converted to PDF user space on write.
    /// </summary>
    public class PageAnno
    {
        public int Page;                                   // target page index (0-based)
        public float Wpt;                                  // page width  in PDF points
        public float Hpt;                                  // page height in PDF points
        public List<List<double>> AddedContours = new();   // new/changed ink → RED SOLID
        public List<List<double>> RemovedContours = new(); // old-only ink  → BLUE DASHED
        public List<DiffItem> TextItems = new();
    }

    /// <summary>
    /// Emits the diff as a VECTOR PDF: the ORIGINAL target drawing is opened and kept
    /// untouched (its native vector content is preserved — no rasterisation), and a vector
    /// annotation layer is appended on top with the red/blue markers. This is what makes
    /// the output a true vector drawing (infinite zoom, crisp lines) rather than an image.
    /// Graphic differences are traced as the ACTUAL CHANGED SHAPE (polygon contour);
    /// text/number differences are circled; disappeared text is rendered in light-blue
    /// italic directly on the page.
    /// </summary>
    public static class PdfVectorDiff
    {
        public static void Write(string outPath, string targetPath, List<PageAnno> pages, double scale, Pdf2dfConfig.AnnotationCfg ann)
        {
            using var writer = new PdfWriter(outPath);
            // Smart mode reuses the source document's indirect objects (and their original
            // compression), so stamping a drawing that contains embedded raster underlays /
            // images does NOT balloon the file — the base content stays the same size.
            writer.SetSmartMode(true);
            using var pdf = new PdfDocument(new PdfReader(targetPath), writer);
            var red = ColorConstants.RED;
            var blue = ColorConstants.BLUE;
            var lightBlue = new DeviceRgb(150, 200, 255);
            // 嵌入 Unicode 字体（默认 Arial，含土耳其语等字形），让"消失文字"的浅蓝斜体
            // 真正渲染；字体缺失时回退内置 Helvetica（仅 ASCII）。
            var unicodeFont = LoadUnicodeFont(ann.FontPathItalic) ?? LoadUnicodeFont(ann.FontPathRegular);
            var italFont = unicodeFont ?? PdfFontFactory.CreateFont(StandardFonts.HELVETICA_OBLIQUE);
            Console.Error.WriteLine($"  [font] 消失文字字体: {(unicodeFont != null ? "EMBEDDED Unicode(Arial)" : "FALLBACK Helvetica(仅ASCII)")}");
            int textRenders = 0;
            var byPage = new Dictionary<int, PageAnno>();
            foreach (var p in pages) byPage[p.Page] = p;

            int n = pdf.GetNumberOfPages();
            for (int i = 1; i <= n; i++)
            {
                if (!byPage.TryGetValue(i - 1, out var a)) continue;
                var page = pdf.GetPage(i);
                var mb = page.GetMediaBox();
                // Use the SAME page height the pixel frame was rendered against (tBounds),
                // not the PDF media box, so the pixel→point mapping is exactly invertible.
                float Hpt = a.Hpt > 0 ? a.Hpt : mb.GetHeight();
                float Wpt = a.Wpt > 0 ? a.Wpt : mb.GetWidth();
                var canvas = new PdfCanvas(page);
                float lw = Math.Max(1.2f, (float)ann.LineWidth);

                // ---- graphic differences: trace the actual changed shape ----
                foreach (var c in a.AddedContours)
                    DrawContour(canvas, c, scale, Hpt, red, lw, solid: true);
                foreach (var c in a.RemovedContours)
                    DrawContour(canvas, c, scale, Hpt, blue, lw, solid: false);

                // ---- text / number differences: circle + (for removed) italic echo ----
                foreach (var it in a.TextItems)
                {
                    bool oldSide = it.ChangeType == "removed" || it.ChangeType == "modified" || it.ChangeType == "moved";
                    var col = oldSide ? blue : red;
                    float x = (float)it.Bbox.X, yTop = (float)it.Bbox.Y;
                    float w = (float)it.Bbox.Width, h = (float)it.Bbox.Height;
                    float cyTopPdf = Hpt - yTop;                 // top edge, PDF y-up
                    float cyBotPdf = Hpt - (yTop + h);           // bottom edge
                    float cyCtr = (cyTopPdf + cyBotPdf) / 2;
                    float rx = Math.Max(2f, w / 2 + 4f), ry = Math.Max(2f, h / 2 + 4f);
                    DrawEllipse(canvas, x + w / 2, cyCtr, rx, ry, col, lw, solid: !oldSide);

                    // Render the disappeared / changed-away text in LIGHT-BLUE ITALIC at its
                    // original position so the change is readable directly inside the PDF.
                    if (oldSide && !string.IsNullOrWhiteSpace(it.OldText))
                    {
                        textRenders++;
                        float fs = Math.Max(6f, ry);
                        float baseY = cyTopPdf - 1f;             // sit just above the circle top
                        if (baseY > Hpt - 1) baseY = Hpt - 1;
                        canvas.SaveState();
                        canvas.SetFillColor(lightBlue);
                        canvas.BeginText();
                        canvas.SetFontAndSize(italFont, fs);
                        canvas.MoveText(x, baseY);
                        canvas.ShowText(Escape(it.OldText!));
                        canvas.EndText();
                        canvas.RestoreState();
                    }
                }
            }
            Console.Error.WriteLine($"  [font] 实际绘制消失文字 {textRenders} 处（浅蓝斜体，已嵌入 Unicode 字体）");
            pdf.Close();
        }

        // Polygon contour in the target pixel frame → vector path in PDF user space.
        static void DrawContour(PdfCanvas c, List<double> pts, double scale, float Hpt, Color col, float lw, bool solid)
        {
            if (pts == null || pts.Count < 6) return;
            c.SaveState();
            c.SetStrokeColor(col).SetLineWidth(lw);
            if (!solid) c.SetLineDash(new float[] { 5f, 3f }, 0);
            c.MoveTo((float)(pts[0] / scale), Hpt - (float)(pts[1] / scale));
            for (int i = 2; i < pts.Count; i += 2)
                c.LineTo((float)(pts[i] / scale), Hpt - (float)(pts[i + 1] / scale));
            c.ClosePath().Stroke();
            c.RestoreState();
        }

        // Ellipse outline via four cubic Bézier arcs (iText has no native ellipse stroke).
        static void DrawEllipse(PdfCanvas c, float cx, float cy, float rx, float ry, Color col, float lw, bool solid)
        {
            c.SaveState();
            c.SetStrokeColor(col).SetLineWidth(lw);
            if (!solid) c.SetLineDash(new float[] { 5f, 3f }, 0);
            const float k = 0.5522847498f;
            float ox = rx * k, oy = ry * k;
            c.MoveTo(cx + rx, cy);
            c.CurveTo(cx + rx, cy + oy, cx + ox, cy + ry, cx, cy + ry);
            c.CurveTo(cx - ox, cy + ry, cx - rx, cy + oy, cx - rx, cy);
            c.CurveTo(cx - rx, cy - oy, cx - ox, cy - ry, cx, cy - ry);
            c.CurveTo(cx + ox, cy - ry, cx + rx, cy - oy, cx + rx, cy);
            c.ClosePath().Stroke();
            c.RestoreState();
        }

        static string Escape(string s) => s.Replace("\\", "\\\\").Replace("(", "\\(").Replace(")", "\\)");

        // 嵌入 Unicode 字体（Identity-H），使土耳其语等非 ASCII 文字也能渲染；失败返回 null。
        static PdfFont LoadUnicodeFont(string? path)
        {
            try
            {
                if (!string.IsNullOrWhiteSpace(path) && File.Exists(path))
                    return PdfFontFactory.CreateFont(path, PdfEncodings.IDENTITY_H,
                        PdfFontFactory.EmbeddingStrategy.PREFER_EMBEDDED);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"  [font] LoadUnicodeFont failed for '{path}': {ex.Message}");
            }
            return null!;
        }
    }
}
