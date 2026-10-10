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
    ///
    /// ⚠️ /Rotate compensation: MuPDF renders a page with its /Rotate applied (DISPLAY
    /// frame), so DiffEngine's pixel coordinates live in the rotated, display-oriented
    /// frame. The PDF content stream, however, is authored in the UNROTATED user space.
    /// Writing display coordinates verbatim into the content stream therefore misplaces
    /// every annotation on a /Rotate≠0 page (observed: only ~1% of red ink landed on the
    /// linework vs ~66% on /Rotate=0 pages). We therefore map the display frame back into
    /// content space with <see cref="DisplayToContent"/> and rotate the annotation text
    /// with <see cref="TextRotation"/> so it still reads upright on screen.
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
                float Wc = mb.GetWidth(), Hc = mb.GetHeight();
                float ox = mb.GetX(), oy = mb.GetY();
                int rot = ((page.GetRotation() % 360) + 360) % 360;

                // Display frame (y-down, top-left origin, MuPDF's render) → content user space.
                DisplayToContent(rot, Wc, Hc, ox, oy,
                    out float mA, out float mB, out float mC, out float mD, out float mE, out float mF);
                // Pure rotation (det = +1, never mirrored) applied to annotation text so it
                // reads upright in the display frame.
                TextRotation(rot, out float tA, out float tB, out float tC, out float tD);

                var canvas = new PdfCanvas(page);
                float lw = Math.Max(1.2f, (float)ann.LineWidth);

                // ---- graphic + ellipse overlays, drawn in the DISPLAY frame ----
                canvas.SaveState();
                canvas.ConcatMatrix(mA, mB, mC, mD, mE, mF);
                foreach (var c in a.AddedContours)
                    DrawContour(canvas, c, scale, red, lw, solid: true);
                foreach (var c in a.RemovedContours)
                    DrawContour(canvas, c, scale, blue, lw, solid: false);
                foreach (var it in a.TextItems)
                {
                    bool oldSide = IsOldSide(it.ChangeType);
                    var col = oldSide ? blue : red;
                    float x = (float)it.Bbox.X, yTop = (float)it.Bbox.Y;
                    float w = (float)it.Bbox.Width, h = (float)it.Bbox.Height;
                    float rx = Math.Max(2f, w / 2 + 4f), ry = Math.Max(2f, h / 2 + 4f);
                    DrawEllipse(canvas, x + w / 2, yTop + h / 2, rx, ry, col, lw, solid: !oldSide);
                }
                canvas.RestoreState();

                // ---- disappeared / changed-away text: light-blue italic, readable on screen ----
                foreach (var it in a.TextItems)
                {
                    if (!IsOldSide(it.ChangeType) || string.IsNullOrWhiteSpace(it.OldText)) continue;
                    textRenders++;
                    float x = (float)it.Bbox.X, yTop = (float)it.Bbox.Y;
                    float h = (float)it.Bbox.Height;
                    float fs = Math.Max(6f, h / 2 + 4f);
                    double bx = x, by = Math.Max(1f, yTop + 1f);      // baseline in DISPLAY y-down
                    ApplyMatrix(mA, mB, mC, mD, mE, mF, bx, by, out double pcx, out double pcy);
                    canvas.SaveState();
                    canvas.SetFillColor(lightBlue);
                    canvas.BeginText();
                    canvas.SetFontAndSize(italFont, fs);
                    canvas.SetTextMatrix(tA, tB, tC, tD, (float)pcx, (float)pcy);
                    canvas.ShowText(Escape(it.OldText!));
                    canvas.EndText();
                    canvas.RestoreState();
                }
            }
            Console.Error.WriteLine($"  [font] 实际绘制消失文字 {textRenders} 处（浅蓝斜体，已嵌入 Unicode 字体）");
            pdf.Close();
        }

        static bool IsOldSide(string changeType)
            => changeType == "removed" || changeType == "modified" || changeType == "moved";

        // Display frame (origin top-left, y-DOWN, units pt, as rendered by MuPDF with
        // /Rotate applied) → content user space (origin bottom-left, y-UP).
        // PDF matrix [a b c d e f] maps (x,y) → (a*x + c*y + e, b*x + d*y + f).
        //   rot=0  : xc = xd,        yc = Hc - yd
        //   rot=90 : xc = yd,        yc = xd
        //   rot=180: xc = Wc - xd,   yc = yd
        //   rot=270: xc = Wc - yd,   yc = Hc - xd
        // (Verified to round-trip against PyMuPDF's page rotation_matrix for all four
        // rotations — every case keeps det = -1, i.e. the mandatory y-down→y-up flip.)
        static void DisplayToContent(int rot, float Wc, float Hc, float ox, float oy,
            out float a, out float b, out float c, out float d, out float e, out float f)
        {
            switch (rot)
            {
                case 90:  a = 0;  b = 1;  c = 1;  d = 0;  e = ox;      f = oy;      break;
                case 180: a = -1; b = 0;  c = 0;  d = 1;  e = ox + Wc; f = oy;      break;
                case 270: a = 0;  b = -1; c = -1; d = 0;  e = ox + Wc; f = oy + Hc; break;
                default:  a = 1;  b = 0;  c = 0;  d = -1; e = ox;      f = oy + Hc; break; // rot = 0
            }
        }

        // Rotation part of the display→content map WITHOUT the vertical flip — used as the
        // text matrix so glyphs stay upright and unmirrored (det = +1 for every case).
        static void TextRotation(int rot, out float a, out float b, out float c, out float d)
        {
            switch (rot)
            {
                case 90:  a = 0;  b = 1;  c = -1; d = 0;  break;
                case 180: a = -1; b = 0;  c = 0;  d = -1; break;
                case 270: a = 0;  b = -1; c = 1;  d = 0;  break;
                default:  a = 1;  b = 0;  c = 0;  d = 1;  break; // rot = 0
            }
        }

        static void ApplyMatrix(float a, float b, float c, float d, float e, float f,
            double x, double y, out double ox, out double oy)
        {
            ox = a * x + c * y + e;
            oy = b * x + d * y + f;
        }

        // Polygon contour in the target pixel frame (DISPLAY frame, y-down) → vector path.
        static void DrawContour(PdfCanvas c, List<double> pts, double scale, Color col, float lw, bool solid)
        {
            if (pts == null || pts.Count < 6) return;
            c.SaveState();
            c.SetStrokeColor(col).SetLineWidth(lw);
            if (!solid) c.SetLineDash(new float[] { 5f, 3f }, 0);
            c.MoveTo((float)(pts[0] / scale), (float)(pts[1] / scale));
            for (int i = 2; i < pts.Count; i += 2)
                c.LineTo((float)(pts[i] / scale), (float)(pts[i + 1] / scale));
            c.ClosePath().Stroke();
            c.RestoreState();
        }

        // Ellipse outline via four cubic Bézier arcs (iText has no native ellipse stroke).
        // Coordinates are in the DISPLAY frame (y-down) — the caller sets the CTM.
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
