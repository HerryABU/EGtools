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
        /// <summary>
        /// 本目标页对应的**基准页号(1-based)**。两版页序可以不同（实测新版插入一页后整体错位），
        /// 抽取"删除文字"必须用基准页自己的页号去查基准文档，不能想当然用目标页号——
        /// 目标 175 页 / 基准 170 页时用目标页号查基准会直接越界。
        /// </summary>
        public int BasePage1;
        public float Wpt;                                  // page width  in PDF points
        public float Hpt;                                  // page height in PDF points
        public List<List<double>> AddedContours = new();   // 新增墨元   → 红 实线填充(真描红)
        public List<List<double>> RemovedContours = new(); // 删除墨元   → 红 虚线填充(真描红)
        public List<List<double>> ChangedContours = new(); // 变更墨元   → 绿 实线填充(真描红)
        public List<DiffItem> TextItems = new();
        /// <summary>
        /// 矢量线画差异：沿实际变更的线段描边（内容空间 pt, y-up，**不做 /Rotate 补偿**——
        /// 它本来就在内容空间）。CAD 线画图上"这条线是新增/删除"才是正确语义，
        /// 且沿线描边天然不遮挡底图（对比实心填充块会盖住图面）。
        /// </summary>
        public List<VectorGeomDiff.Run> Strokes = new();
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
                // 粒度：line=整行墨迹盒（默认）；word=逐词盒（更精确，不牵连同行无关文字）。
                bool byWord = string.Equals(ann.TextBoxMode, "word", StringComparison.OrdinalIgnoreCase);
                var addedBoxes = new List<TextRedlineDiff.WordBox>();
                var removedBoxes = new List<TextRedlineDiff.WordBox>();
                void AddBoxes(List<TextLine>? lines, List<TextRedlineDiff.WordBox> dst)
                {
                    if (lines == null) return;
                    foreach (var ln in lines)
                    {
                        if (byWord && ln.Words is { Count: > 0 })
                            foreach (var w in ln.Words)
                            {
                                double pa = mA * w.X0 + mC * w.Y0 + mE, pb = mB * w.X0 + mD * w.Y0 + mF;
                                double qa = mA * w.X1 + mC * w.Y1 + mE, qb = mB * w.X1 + mD * w.Y1 + mF;
                                dst.Add(new TextRedlineDiff.WordBox
                                {
                                    Text = w.T,
                                    X0 = Math.Min(pa, qa), X1 = Math.Max(pa, qa),
                                    Y0 = Math.Min(pb, qb), Y1 = Math.Max(pb, qb),
                                });
                            }
                        else
                            dst.Add(ToContentBox(ln, mA, mB, mC, mD, mE, mF));
                    }
                }
                foreach (var it in a.TextItems)
                {
                    if (it.ChangeType == "added" || it.ChangeType == "changed") AddBoxes(it.NewLines, addedBoxes);
                    if (it.ChangeType == "removed" || it.ChangeType == "changed") AddBoxes(it.OldLines, removedBoxes);
                }

                // ---- 图形差异层：标记形式可选（semi / solid / outline）----
                //   semi(默认) = 淡填充 + 顶部深色描边 —— 底图仍可读，高密度图纸首选
                //   solid      = 实心填充 + 细描边（原行为，标记最强）
                //   outline    = 仅描边、不填充 —— 最不遮挡
                string mstyle = (ann.MarkStyle ?? "semi").Trim().ToLowerInvariant();
                if (mstyle != "solid" && mstyle != "outline") mstyle = "semi";
                Color? userStroke = ParseColor(ann.MarkStrokeColor);
                Color traceStroke = userStroke ?? Darken(traceCol, 0.45f);
                Color changeStroke = userStroke ?? Darken(changeCol, 0.45f);
                float lwBase = Math.Max(1.2f, (float)ann.LineWidth);
                float lwMark = mstyle == "solid" ? lwBase : lwBase * 2f;   // 弱填充时需要更粗的描边才看得清
                double fillAlpha = mstyle switch
                {
                    "solid" => ann.FillOpacity,
                    "semi" => ann.FillOpacity * 0.5,
                    _ => 0.0,
                };

                // OVER 层：与下垫层用同一套 /Rotate 补偿矩阵，保证旋转页上的标注不错位。
                // 必须 Save/Restore 配对——后面 TextRedlineDiff 会往同一内容流追加原始算子，
                // 泄漏的 cm 会把那些字形一起搬歪。
                var overCanvas = new PdfCanvas(page);
                overCanvas.SaveState();
                overCanvas.ConcatMatrix(mA, mB, mC, mD, mE, mF);

                // ① 填充层（semi / solid）
                if (fillAlpha > 0)
                {
                    foreach (var c in a.AddedContours) FillPoly(overCanvas, c, scale, traceCol, fillAlpha);
                    foreach (var c in a.RemovedContours) FillPoly(overCanvas, c, scale, traceCol, fillAlpha);
                    foreach (var c in a.ChangedContours) FillPoly(overCanvas, c, scale, changeCol, fillAlpha);
                }

                // ② 描边层。solid 沿用原行为——垫在内容流之前(字下面)避免遮住原图文字；
                //    semi / outline 的描边就是标记本身，压在填充之上让边缘锐利可辨。
                if (mstyle == "solid")
                {
                    if (ann.Outline)
                    {
                        var underCanvas = new PdfCanvas(page.NewContentStreamBefore(), page.GetResources(), pdf);
                        underCanvas.SaveState();
                        underCanvas.ConcatMatrix(mA, mB, mC, mD, mE, mF);
                        foreach (var c in a.AddedContours) StrokePoly(underCanvas, c, scale, traceCol, lwMark, solid: true);
                        foreach (var c in a.RemovedContours) StrokePoly(underCanvas, c, scale, traceCol, lwMark, solid: false);
                        foreach (var c in a.ChangedContours) StrokePoly(underCanvas, c, scale, changeCol, lwMark, solid: true);
                        underCanvas.RestoreState();
                    }
                }
                else
                {
                    foreach (var c in a.AddedContours) StrokePoly(overCanvas, c, scale, traceStroke, lwMark, solid: true);
                    foreach (var c in a.RemovedContours) StrokePoly(overCanvas, c, scale, traceStroke, lwMark, solid: false);
                    foreach (var c in a.ChangedContours) StrokePoly(overCanvas, c, scale, changeStroke, lwMark, solid: true);
                }
                overCanvas.RestoreState();

                // ---- 矢量线画差异：沿变更线段描边（内容空间，无需 /Rotate 补偿）----
                // 相比像素路径的填充块，这里画的是"真正变掉的那几条线"，语义精确且不遮挡底图。
                if (a.Strokes.Count > 0)
                {
                    overCanvas.SaveState();
                    float vw = Math.Max(1.0f, (float)ann.LineWidth);
                    foreach (var run in a.Strokes)
                    {
                        if (run.Pts.Count < 4) continue;
                        bool removed = run.Dir == "removed";
                        var col = removed ? traceCol : run.Dir == "changed" ? changeCol : traceCol;
                        overCanvas.SetStrokeColor(col).SetLineWidth(vw);
                        if (removed) overCanvas.SetLineDash(new float[] { 5f, 3f }, 0);
                        else overCanvas.SetLineDash(new float[0], 0);
                        overCanvas.MoveTo((float)run.Pts[0], (float)run.Pts[1]);
                        for (int k = 2; k < run.Pts.Count; k += 2)
                            overCanvas.LineTo((float)run.Pts[k], (float)run.Pts[k + 1]);
                        overCanvas.Stroke();
                    }
                    overCanvas.RestoreState();
                }

                // ---- 文字差异重绘（最后绘制，确保红/蓝字形压在图形填充之上，清晰可读）----
                if (addedBoxes.Count > 0)
                {
                    TextRedlineDiff.RecolorAdded(pdf, i, addedBoxes, textAddedCol, ann.TextBoxPadPt);
                    textRenders += addedBoxes.Count;
                }
                if (removedBoxes.Count > 0)
                {
                    // ⚠️ 用【基准页自己的页号】：两版页序可不同（新版插页会使整体错位），
                    // 用目标页号查基准文档会越界或抽错页。
                    int bp = a.BasePage1 > 0 ? a.BasePage1 : i;
                    bool ok = bp <= baselinePdf.GetNumberOfPages();
                    // 页框尺寸/旋转必须一致，否则 DISPLAY 帧坐标与基准内容空间对不上，
                    // 抽出来的字形会跑位。不一致就跳过（宁可少标，不标错）。
                    if (ok)
                    {
                        var bpg = baselinePdf.GetPage(bp);
                        var bmb = bpg.GetMediaBox();
                        int brot = ((bpg.GetRotation() % 360) + 360) % 360;
                        ok = Math.Abs(bmb.GetWidth() - Wc) < 0.5 && Math.Abs(bmb.GetHeight() - Hc) < 0.5 && brot == rot;
                    }
                    if (ok)
                    {
                        TextRedlineDiff.DrawRemoved(pdf, baselinePdf, bp, removedBoxes, textRemovedCol, ann.TextBoxPadPt);
                        textRenders += removedBoxes.Count;
                    }
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

        // 上式的逆变换：内容空间 → DISPLAY 帧（供报告统一口径）。
        public static void ContentToDisplay(int rot, float Wc, float Hc, float ox, float oy,
            double xc, double yc, out double xd, out double yd)
        {
            switch (rot)
            {
                case 90:  xd = yc - oy;          yd = xc - ox;          break;
                case 180: xd = ox + Wc - xc;     yd = yc - oy;          break;
                case 270: xd = oy + Hc - yc;     yd = ox + Wc - xc;     break;
                default:  xd = xc - ox;          yd = oy + Hc - yc;     break; // rot = 0
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

        // 取标记色的深色版作描边：保证与底图/填充的对比度，且不像纯黑那样在红填充上显得脏。
        static Color Darken(Color c, float f)
        {
            var v = c.GetColorValue();
            return new DeviceRgb(
                (int)Math.Round(Math.Max(0, v[0] * (1 - f))),
                (int)Math.Round(Math.Max(0, v[1] * (1 - f))),
                (int)Math.Round(Math.Max(0, v[2] * (1 - f))));
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
