using System;
using System.Collections.Generic;
using System.IO;
using iText.Kernel.Colors;
using iText.Kernel.Pdf;
using iText.Kernel.Pdf.Canvas;
using iText.Kernel.Pdf.Extgstate;

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
        public List<List<double>> AddedContours = new();   // 新增墨元   → 红 实线填充(真描红)
        public List<List<double>> RemovedContours = new(); // 删除墨元   → 红 虚线填充(真描红)
        public List<List<double>> ChangedContours = new(); // 变更墨元   → 绿 实线填充(真描红)
        public List<DiffItem> TextItems = new();
    }

    /// <summary>
    /// Emits the diff as a VECTOR PDF: the ORIGINAL target drawing is opened and kept
    /// untouched (its native vector content is preserved — no rasterisation). Graphic
    /// differences are traced as the ACTUAL CHANGED SHAPE (1:1 polygon contour — never a
    /// circle or simulated line) and inserted BEFORE the original content stream
    /// (page.NewContentStreamBefore) so they sit BEHIND the drawing and never cover the
    /// original characters (requirement: 红线放在字下面). Text differences are handled by
    /// <see cref="TextRedlineDiff"/> on the CONTENT STREAM — recolor the original glyphs
    /// in place (added/changed-new → red) or copy the original glyph operators (removed/
    /// changed-old → blue). This keeps the exact original font, size, text matrix and
    /// rotation, so CAD PDFs (vertical text, big Tf + matrix scaling) render faithfully.
    ///
    /// ⚠️ /Rotate compensation: MuPDF renders a page with its /Rotate applied (DISPLAY
    /// frame), so DiffEngine's pixel coordinates live in the rotated, display-oriented
    /// frame. The PDF content stream, however, is authored in the UNROTATED user space.
    /// Writing display coordinates verbatim into the content stream therefore misplaces
    /// every annotation on a /Rotate≠0 page. We therefore map the display frame back into
    /// content space with <see cref="DisplayToContent"/>.
    /// </summary>
    public static class PdfVectorDiff
    {
        public static void Write(string outPath, string targetPath, string baselinePath, List<PageAnno> pages, double scale, Pdf2dfConfig.AnnotationCfg ann)
        {
            using var writer = new PdfWriter(outPath);
            // Smart mode reuses the source document's indirect objects (and their original
            // compression), so stamping a drawing that contains embedded raster underlays /
            // images does NOT balloon the file — the base content stays the same size.
            writer.SetSmartMode(true);
            using var pdf = new PdfDocument(new PdfReader(targetPath), writer);
            // 基准 PDF 以只读方式打开，供抽取"删除/变更旧"文字的原始文本运行字节。
            using var baselinePdf = new PdfDocument(new PdfReader(baselinePath));
            var traceCol = ParseColor(ann.TraceColor) ?? ColorConstants.RED;   // 新增/删除 图形描红(红)
            var changeCol = ParseColor(ann.ChangeColor) ?? new DeviceRgb(22, 163, 74); // 变更 图形描红(绿)
            // 文字差异：新增=红，删除=蓝(内容流重着色/抽取，不改字体/字号/矩阵/旋转)。
            var textAddedCol = ParseColor(ann.TextAddedColor) ?? ColorConstants.RED;
            var textRemovedCol = ParseColor(ann.TextRemovedColor) ?? new DeviceRgb(0, 0, 255);
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

                // ---- 文字差异 v8：内容流重着色(新增红) + 抽取(删除蓝) ----
                // 先构造内容流空间包围盒（供后面文字重绘匹配使用）。
                var addedBoxes = new List<TextRedlineDiff.WordBox>();
                var removedBoxes = new List<TextRedlineDiff.WordBox>();
                foreach (var it in a.TextItems)
                {
                    if ((it.ChangeType == "added" || it.ChangeType == "changed") && it.NewLines != null)
                        foreach (var ln in it.NewLines) addedBoxes.Add(ToContentBox(ln, mA, mB, mC, mD, mE, mF));
                    if ((it.ChangeType == "removed" || it.ChangeType == "changed") && it.OldLines != null)
                        foreach (var ln in it.OldLines) removedBoxes.Add(ToContentBox(ln, mA, mB, mC, mD, mE, mF));
                }

                // ---- 图形差异层：UNROTATED 内容空间下的描边(下垫) + 真描红填充(上层) ----
                float lw = Math.Max(1.2f, (float)ann.LineWidth);
                var underCanvas = new PdfCanvas(page.NewContentStreamBefore(), page.GetResources(), pdf);
                underCanvas.SaveState();
                underCanvas.ConcatMatrix(mA, mB, mC, mD, mE, mF);
                foreach (var c in a.AddedContours)
                    StrokePoly(underCanvas, c, scale, traceCol, lw, solid: true);
                foreach (var c in a.RemovedContours)
                    StrokePoly(underCanvas, c, scale, traceCol, lw, solid: false);
                foreach (var c in a.ChangedContours)
                    StrokePoly(underCanvas, c, scale, changeCol, lw, solid: true);
                underCanvas.RestoreState();

                // OVER 层：真描红填充变更图形(覆盖在变更线框之上，保证可见)。
                var overCanvas = new PdfCanvas(page);
                foreach (var c in a.AddedContours) FillPoly(overCanvas, c, scale, traceCol, ann.FillOpacity);
                foreach (var c in a.RemovedContours) FillPoly(overCanvas, c, scale, traceCol, ann.FillOpacity);
                foreach (var c in a.ChangedContours) FillPoly(overCanvas, c, scale, changeCol, ann.FillOpacity);

                // ---- 文字差异重绘（最后绘制，确保红/蓝字形压在图形填充之上，清晰可读）----
                if (addedBoxes.Count > 0)
                {
                    TextRedlineDiff.RecolorAdded(pdf, i, addedBoxes, textAddedCol);
                    textRenders += addedBoxes.Count;
                }
                if (removedBoxes.Count > 0)
                {
                    TextRedlineDiff.DrawRemoved(pdf, baselinePdf, i, removedBoxes, textRemovedCol);
                    textRenders += removedBoxes.Count;
                }
            }
            Console.Error.WriteLine($"  [font] 文字差异(内容流重着色/抽取)处理 {textRenders} 处（新增红/删除蓝，保留原始字体与版式）");
            pdf.Close();
            baselinePdf.Close();
        }

        // Display frame (origin top-left, y-DOWN, units pt, as rendered by MuPDF with
        // /Rotate applied) → content user space (origin bottom-left, y-UP).
        // PDF matrix [a b c d e f] maps (x,y) → (a*x + c*y + e, b*x + d*y + f).
        //   rot=0  : xc = xd,        yc = Hc - yd
        //   rot=90 : xc = yd,        yc = xd
        //   rot=180: xc = Wc - xd,   yc = yd
        //   rot=270: xc = Wc - yd,   yc = Hc - xd
        // (Verified to round-trip against PyMuPDF's page rotation_matrix for all four
        // rotations — every case keeps det = -1, i.e. the mandatory y-down→y-up flip.)
        public static void DisplayToContent(int rot, float Wc, float Hc, float ox, float oy,
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

        // 把一条 DISPLAY 帧文字包围盒(左上原点, y-down)映射到内容流空间(左下原点, y-up)的 AABB。
        static TextRedlineDiff.WordBox ToContentBox(TextLine ln, float a, float b, float c, float d, float e, float f)
        {
            double[] xs = new double[4], ys = new double[4];
            int k = 0;
            foreach (var (xd, yd) in new[] { (ln.X0, ln.Y0), (ln.X1, ln.Y0), (ln.X1, ln.Y1), (ln.X0, ln.Y1) })
            {
                xs[k] = a * xd + c * yd + e;
                ys[k] = b * xd + d * yd + f;
                k++;
            }
            return new TextRedlineDiff.WordBox
            {
                Text = ln.Text,
                X0 = Math.Min(xs[0], Math.Min(xs[1], Math.Min(xs[2], xs[3]))),
                Y0 = Math.Min(ys[0], Math.Min(ys[1], Math.Min(ys[2], ys[3]))),
                X1 = Math.Max(xs[0], Math.Max(xs[1], Math.Max(xs[2], xs[3]))),
                Y1 = Math.Max(ys[0], Math.Max(ys[1], Math.Max(ys[2], ys[3]))),
            };
        }

        static void PathPoly(PdfCanvas c, List<double> pts, double scale)
        {
            if (pts == null || pts.Count < 6) return;
            c.MoveTo((float)(pts[0] / scale), (float)(pts[1] / scale));
            for (int i = 2; i < pts.Count; i += 2)
                c.LineTo((float)(pts[i] / scale), (float)(pts[i + 1] / scale));
            c.ClosePath();
        }

        // Crisp outline stroke of a changed-shape polygon (drawn on the UNDER layer, behind
        // the drawing characters). Solid = new/changed, dashed = removed.
        static void StrokePoly(PdfCanvas c, List<double> pts, double scale, Color col, float lw, bool solid)
        {
            if (pts == null || pts.Count < 6) return;
            c.SaveState();
            c.SetStrokeColor(col).SetLineWidth(lw);
            if (!solid) c.SetLineDash(new float[] { 5f, 3f }, 0);
            PathPoly(c, pts, scale);
            c.Stroke();
            c.RestoreState();
        }

        // TRUE FILL (真描红): translucent vector fill of the PRECISE changed region — not a
        // rasterised highlight, not merely a stroked outline. FillOpacity controls visibility
        // vs. readability of the underlying linework.
        static void FillPoly(PdfCanvas c, List<double> pts, double scale, Color col, double opacity)
        {
            if (pts == null || pts.Count < 6) return;
            if (opacity <= 0) return;
            c.SaveState();
            c.SetExtGState(new PdfExtGState().SetFillOpacity((float)Math.Min(1.0, Math.Max(0.0, opacity))));
            c.SetFillColor(col);
            PathPoly(c, pts, scale);
            c.Fill();
            c.RestoreState();
        }

        // 解析用户传入的色号 #RRGGBB / #RRGGBBAA → iText DeviceRgb（忽略 alpha）。
        static Color ParseColor(string? hex)
        {
            if (string.IsNullOrWhiteSpace(hex)) return null!;
            try
            {
                var h = hex.Trim().TrimStart('#');
                if (h.Length == 6 || h.Length == 8)
                    return new DeviceRgb(
                        Convert.ToInt32(h.Substring(0, 2), 16),
                        Convert.ToInt32(h.Substring(2, 2), 16),
                        Convert.ToInt32(h.Substring(4, 2), 16));
            }
            catch { }
            return null!;
        }
    }
}
