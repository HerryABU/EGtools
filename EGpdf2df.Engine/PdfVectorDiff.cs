using System;
using System.Collections.Generic;
using System.IO;
using iText.Kernel.Colors;
using iText.Kernel.Pdf;
using iText.Kernel.Pdf.Canvas;
using iText.Kernel.Pdf.Extgstate;
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
    /// original characters (requirement: 红线放在字下面). Text differences are NOT circled:
    /// the text itself is recoloured on a SEPARATE top layer (new PdfCanvas(page)) —
        /// added / changed-new = RED (overlaid on the target's black text — recolor in
        /// place), removed / changed-old = BLUE (git "-" line, inserted into the original
        /// text box; changed-old offset by TextInsertDirection: down/up/right/left). This is what
    /// makes the output a true vector drawing (infinite zoom, crisp lines) rather than an
    /// image.
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
            var traceCol = ParseColor(ann.TraceColor) ?? ColorConstants.RED;   // 新增/删除 图形描红(红)
            var changeCol = ParseColor(ann.ChangeColor) ?? new DeviceRgb(22, 163, 74); // 变更 图形描红(绿)
            // 文字差异：直接改文字颜色（不画圈/点）。新增=红，删除=蓝，变更=红(新)+蓝(旧)。
            var textAddedCol = ParseColor(ann.TextAddedColor) ?? ColorConstants.RED;
            var textRemovedCol = ParseColor(ann.TextRemovedColor) ?? new DeviceRgb(0, 0, 255);
            // 嵌入 Unicode 字体（默认 Arial，含土耳其语等字形），让差异文字(红/蓝重绘)
            // 真正渲染；字体缺失时回退内置 Helvetica（仅 ASCII）。
            var unicodeFont = LoadUnicodeFont(ann.FontPathItalic) ?? LoadUnicodeFont(ann.FontPathRegular);
            var italFont = unicodeFont ?? PdfFontFactory.CreateFont(StandardFonts.HELVETICA_OBLIQUE);
            Console.Error.WriteLine($"  [font] 差异文字字体: {(unicodeFont != null ? "EMBEDDED Unicode(Arial)" : "FALLBACK Helvetica(仅ASCII)")}");
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

                float lw = Math.Max(1.2f, (float)ann.LineWidth);

                // ---- UNDER layer: crisp outline strokes of graphic differences ----
                // Inserted BEFORE the original content stream so the original characters /
                // linework paint on top and are never obscured by the diff outlines. This
                // honours the "红线放在字下面" requirement for the outline edges.
                var underCanvas = new PdfCanvas(page.NewContentStreamBefore(), page.GetResources(), pdf);
                underCanvas.SaveState();
                underCanvas.ConcatMatrix(mA, mB, mC, mD, mE, mF);
                foreach (var c in a.AddedContours)
                    StrokePoly(underCanvas, c, scale, traceCol, lw, solid: true);
                foreach (var c in a.RemovedContours)
                    StrokePoly(underCanvas, c, scale, traceCol, lw, solid: false);
                foreach (var c in a.ChangedContours)
                    StrokePoly(underCanvas, c, scale, changeCol, lw, solid: true);
                // ⚠️ 文字差异不再在 UNDER 层画圈/点——改为在 OVER 层直接"改文字颜色"
                // （见下方 (b)），避免整页铺满圆圈（旧版崩溃根因）。UNDER 层只保留图形描边。
                underCanvas.RestoreState();

                // ---- OVER layer: TRUE FILL (真描红) of the changed shape + disappeared text ----
                // The fill is a translucent vector highlight drawn ON TOP of the actual
                // changed linework (so it is actually visible, unlike an under-layer fill
                // that the original black ink would hide). FillOpacity (default 0.40) keeps
                // the underlying drawing readable; this is a genuine vector fill of the
                // precise changed region — never a rasterised highlight.
                var overCanvas = new PdfCanvas(page);
                // (a) fill the changed shapes: added/removed = red, changed = green.
                foreach (var c in a.AddedContours) FillPoly(overCanvas, c, scale, traceCol, ann.FillOpacity);
                foreach (var c in a.RemovedContours) FillPoly(overCanvas, c, scale, traceCol, ann.FillOpacity);
                foreach (var c in a.ChangedContours) FillPoly(overCanvas, c, scale, changeCol, ann.FillOpacity);
                // (b) 文字差异：直接把文字重绘成对应颜色（不画圈/点，git +/- 风格）。
                //     新增(added) 与 变更的新内容(changed.NewText) → 红色，覆盖在目标原黑色文字之上（改原字色）；
                //     删除(removed) 与 变更的旧内容(changed.OldText) → 蓝色，插入到原文本框（git - 行）；
                //     变更时旧(蓝)文字按 TextInsertDirection 方向插入到"新(红)"文字文本框附近，避免重叠不可读。
                string dir = (ann.TextInsertDirection ?? "down").Trim().ToLowerInvariant();
                foreach (var it in a.TextItems)
                {
                    // 新增 / 变更新内容 → 红色，逐行按原字号 + 原位置覆盖在原黑色文字之上(改原字色)。
                    if ((it.ChangeType == "added" || it.ChangeType == "changed") && it.NewLines != null)
                        DrawColoredText(overCanvas, it.NewLines, textAddedCol, 0, 0,
                            tA, tB, tC, tD, mA, mB, mC, mD, mE, mF, italFont, ref textRenders);
                    // 删除 / 变更旧内容 → 蓝色，逐行按原字号 + 原位置插入到原文本框(git - 行)；
                    // 变更时旧(蓝)文字按 TextInsertDirection 方向插入到"新(红)"文字文本框附近。
                    if ((it.ChangeType == "removed" || it.ChangeType == "changed") && it.OldLines != null)
                    {
                        double dx = 0, dy = 0;
                        if (it.ChangeType == "changed")
                        {
                            double fs = 10;
                            if (it.OldLines.Count > 0)
                            { double s = 0; foreach (var l in it.OldLines) s += l.Size; fs = s / it.OldLines.Count; }
                            InsertOffset(dir, fs, out dx, out dy);
                        }
                        DrawColoredText(overCanvas, it.OldLines, textRemovedCol, dx, dy,
                            tA, tB, tC, tD, mA, mB, mC, mD, mE, mF, italFont, ref textRenders);
                    }
                }
            }
            Console.Error.WriteLine($"  [font] 实际重绘文字颜色 {textRenders} 处（新增红/删除蓝，已嵌入 Unicode 字体）");
            pdf.Close();
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

        // Emit a polygon path (no paint) in the target pixel frame (DISPLAY, y-down) → content space.
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
        // vs. readability of the underlying linework. No halo expansion is applied here (the
        // optional halo is baked into the contour by the kernel's dilation step).
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

        // 把一组"原始行"以指定颜色逐行重绘在 OVER 层（DISPLAY 坐标 → 内容空间）。这是
        // "文字改色/插入"的核心：每行都用**原字号 + 原左基线 x + 原基线 y** 绘制，所以
        // 重绘后字号与换行与原文完全一致，不会漂移也不会塌陷成一行。新增/变更新内容→红
        // (覆盖原黑字,改原字色)；删除/变更旧内容→蓝(插入原文本框,git - 行)。
        // xOffDisp/yOffDisp 为显示帧下的额外偏移(pt)，用于变更时把"旧(蓝)"文字按方向
        // 挪到"新(红)"文字文本框附近。
        static void DrawColoredText(PdfCanvas c, List<TextLine> lines, Color col,
            double xOffDisp, double yOffDisp,
            float tA, float tB, float tC, float tD,
            float mA, float mB, float mC, float mD, float mE, float mF,
            PdfFont font, ref int renders)
        {
            foreach (var ln in lines)
            {
                if (string.IsNullOrWhiteSpace(ln.Text)) continue;
                // 字号以"页面实际渲染高度"反推(÷≈0.9 的 ascent 比例)，规避 CAD PDF 惯用的
                // 大 Tf 字号 + 文本矩阵缩放导致字号失真；使重绘文字的视觉大小与原文一致。
                float fs = (float)Math.Max(4.0, ln.Height / 0.9);
                double bx = ln.X + xOffDisp;                              // 原行左基线 x
                double by = ln.Baseline + yOffDisp;                       // 原基线 y(display y-down)
                ApplyMatrix(mA, mB, mC, mD, mE, mF, bx, by, out double pcx, out double pcy);
                c.SaveState();
                c.SetFillColor(col);
                c.BeginText();
                c.SetFontAndSize(font, fs);
                c.SetTextMatrix(tA, tB, tC, tD, (float)pcx, (float)pcy);
                c.ShowText(Escape(ln.Text));
                c.EndText();
                c.RestoreState();
                renders++;
            }
        }

        // 变更时旧(蓝)文字相对新(红)文字的插入方向偏移（DISPLAY 帧，单位 pt）：
        // down=下方, up=上方, right=右侧, left=左侧；gap 取原字号 * 1.2(≈ 原行距)，
        // 与原文行距一致，避免重叠也不破坏版面节奏。
        static void InsertOffset(string dir, double fontSize, out double dx, out double dy)
        {
            double gap = Math.Max(6.0, fontSize * 1.2);
            switch (dir)
            {
                case "up":    dx = 0;            dy = -gap;       break;
                case "right": dx =  gap;         dy = 0;          break;
                case "left":  dx = -gap;         dy = 0;          break;
                default:      dx = 0;            dy =  gap;       break; // down
            }
        }

        static string Escape(string s) => s.Replace("\\", "\\\\").Replace("(", "\\(").Replace(")", "\\)");

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
