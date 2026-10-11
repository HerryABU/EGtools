using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using EGtools.Core;
using MuPDFCore;
using MuPDFCore.StructuredText;
using OpenCvSharp;
using Size = OpenCvSharp.Size;

namespace EGpdf2df.Engine
{
    /// <summary>
    /// Drawing-diff kernel. Compares a baseline (old) and a target (new) drawing PDF
    /// in TWO independent stages — (1) graphic/figure differences with the text
    /// regions EXCLUDED from the pixel comparison, (2) text/annotation differences
    /// handled separately — then overlays RED markers and exports a new PDF.
    /// Fully decoupled from any front-end: it reads a JSON contract (stdin / file)
    /// and writes a JSON summary (stdout). No drawing-specific constant is hard-coded.
    /// </summary>
    public static class DiffEngine
    {
        public static int Run(DiffRequest req, Pdf2dfConfig cfg, Action<string>? log = null)
        {
            void L(string s) => (log ?? Console.Error.WriteLine)(s);

            if (string.IsNullOrEmpty(req.Baseline) || string.IsNullOrEmpty(req.Target))
            {
                L("[error] baseline and target PDF paths are required.");
                return 2;
            }
            if (!File.Exists(req.Baseline)) { L($"[error] baseline not found: {req.Baseline}"); return 2; }
            if (!File.Exists(req.Target)) { L($"[error] target not found: {req.Target}"); return 2; }

            // Request options override config defaults.
            int renderDpi = req.Options?.RenderDpi ?? cfg.RenderDpi;
            double scale = renderDpi / 72.0;

            // 精细度预设（fine/medium/coarse）：把单旋钮映射到底层阈值/最小面积/模糊/容差；
            // 随后由容差(毫米)换算成像素，供差异图侵蚀，从而"忽略偏离几毫米的误差"。
            cfg.ApplyPrecision(req.Options?.Precision);
            cfg.GraphicDiff.TolerancePx = (int)Math.Max(0, Math.Round(cfg.Registration.ToleranceMm * 2.834645669 * scale));

            string outPath = req.Output
                ?? Path.Combine(Path.GetDirectoryName(req.Target) ?? ".",
                                Path.GetFileNameWithoutExtension(req.Target) + "_diff.pdf");

            try
            {
                using var ctx = new MuPDFContext(512u * 1024 * 1024);
                var (baseCtxPath, baseDoc) = Open(ctx, req.Baseline);
                var (tgtCtxPath, tgtDoc) = Open(ctx, req.Target);
                try
                {
                    int nBase = baseDoc.Pages.Count;
                    int nTgt = tgtDoc.Pages.Count;

                    // Pair pages by drawing number (图纸号) so the comparison is 1:1 and
                    // robust to reordering. When the caller pins specific pages (region /
                    // scoped diff) we keep the old index-based behaviour.
                    List<(int basePno, int tgtPno, string no)> pagePairs;
                    List<(int page, string no)> addedPages = new();    // target-only
                    List<(int page, string no)> removedPages = new();  // baseline-only
                    if (req.Pages != null && req.Pages.Count > 0)
                    {
                        pagePairs = req.Pages.Where(p => p >= 0 && p < Math.Min(nBase, nTgt))
                                              .Select(p => (p, p, "")).ToList();
                    }
                    else
                    {
                        (pagePairs, addedPages, removedPages) = MatchPages(baseDoc, nBase, tgtDoc, nTgt, L);
                    }

                    var resp = new DiffResponse { Output = outPath };
                    var pageAnnos = new List<PageAnno>();
                    // Accumulate every target-page pipe/line number (with its y-centre and
                    // page height) so we can run the figure↔material-table 1:1 cross-check
                    // once all pages are processed.
                    var allTgtPipes = new List<(string no, double yc, double h)>();

                    foreach (var (basePno, tgtPno, no) in pagePairs)
                    {
                        L($"  图纸号 {no}: 基准页 {basePno + 1} ↔ 目标页 {tgtPno + 1}");
                        var baseB = RenderBgr(baseDoc, basePno, scale, out int bw, out int bh, out var bBounds);
                        var tgtB = RenderBgr(tgtDoc, tgtPno, scale, out int tw, out int th, out var tBounds);
                        using var baseGray = new Mat(); Cv2.CvtColor(baseB, baseGray, ColorConversionCodes.BGR2GRAY);
                        using var tgtGray = new Mat(); Cv2.CvtColor(tgtB, tgtGray, ColorConversionCodes.BGR2GRAY);

            using var targetST = tgtDoc.GetStructuredTextPage(tgtPno, false, StructuredTextFlags.None);
            using var baseST = baseDoc.GetStructuredTextPage(basePno, false, StructuredTextFlags.None);

            // ----- text words (used by both the text mask and the text-diff stage) -----
            var baseWords = TextGeometry.GetWords(baseST, bBounds.Y1 - bBounds.Y0, 0.18);
            var tgtWords = TextGeometry.GetWords(targetST, tBounds.Y1 - tBounds.Y0, 0.18);

            // Collect target pipe/line numbers for the figure↔material-table check.
            double tH = tBounds.Y1 - tBounds.Y0;
            foreach (var w in tgtWords)
                if (TextGeometry.LooksLikeLineNo(w.text))
                    allTgtPipes.Add((w.text.ToUpperInvariant(), w.yc, tH));

            // ----- per-page text masks, built in EACH page's own pixel frame -----
            // A base-frame mask must never be applied to a target-frame image (or
            // vice-versa): mismatched sizes trip OpenCV's mask-size assertion.
            using var baseTextMask = cfg.GraphicDiff.ExcludeText
                ? BuildTextMask(baseWords, bw, bh, scale, bBounds.X0, bBounds.Y0)
                : null;
            using var tgtTextMask = cfg.GraphicDiff.ExcludeText
                ? BuildTextMask(tgtWords, tw, th, scale, tBounds.X0, tBounds.Y0)
                : null;

            // ----- region restriction -----
            var pageRegions = (req.Regions ?? new List<ReqRegion>())
                .Where(r => r.Page == tgtPno).Select(r => r.Bbox).ToList();
            using var regionMask = pageRegions.Count > 0
                ? BuildRegionMask(pageRegions, tw, th, scale, tBounds.X0, tBounds.Y0)
                : null;

            // ----- registration: align baseline → target -----
            // These two drawing revisions are the SAME sheet at different paper sizes
            // (e.g. A0 vs A1): a uniform scale plus a small translation. We recover the
            // scale from the page bounds and the residual translation from the TEXT WORDS
            // (each base word scaled by the page-bounds scale is matched to its nearest
            // target word; the median residual shift is the translation). Word-based
            // alignment is far more robust than pixel correlation when drawing lines have
            // actually been re-inked between revisions. The single transform is applied to
            // BOTH the pixels AND the text coordinates so both diff stages share one frame.
            // Always recover a single affine (uniform scale + translation) and WARP the
            // baseline into the target frame. This is what makes "try to align first, then
            // compare" true even for drawings that are the SAME paper size but shifted
            // up/down/left/right — the old `sameFrame` early-return skipped translation
            // estimation in exactly that case and let shifted sheets mis-align.
            using var H = ComputeAlign(baseGray, tgtGray, bw, bh, tw, th, cfg.Registration,
                baseWords, tgtWords, scale, bBounds.X0, bBounds.Y0, tBounds.X0, tBounds.Y0);
            double alignScale = (double)tw / bw, alignDxPx = H.Get<double>(0, 2), alignDyPx = H.Get<double>(1, 2);
            L($"  对齐: base={bw}x{bh} target={tw}x{th} scale={alignScale:F4} dx(px)={alignDxPx:F1} dy(px)={alignDyPx:F1}");
            resp.Alignments.Add(new AlignInfo
            {
                Page = tgtPno,
                Scale = Math.Round(alignScale, 4),
                DxPt = Math.Round(alignDxPx / scale, 1),
                DyPt = Math.Round(alignDyPx / scale, 1),
            });

            Mat baseGrayAligned = new Mat();
            Cv2.WarpPerspective(baseGray, baseGrayAligned, H, new Size(tw, th));

            // Baseline text words, transformed into the TARGET point frame (correct even
            // when H is the identity — no float drift thanks to the rounding in Mk()).
            var baseWordsAligned = TransformWordsToTarget(baseWords, H, bBounds.X0, bBounds.Y0, tBounds.X0, tBounds.Y0, scale);

            // Unified text mask in the TARGET pixel frame (union, base mask warped in).
            Mat? textMask = null;
            if (tgtTextMask != null || baseTextMask != null)
            {
                textMask = new Mat(th, tw, MatType.CV_8UC1, Scalar.All(0));
                if (tgtTextMask != null) Cv2.BitwiseOr(textMask, tgtTextMask, textMask);
                if (baseTextMask != null)
                {
                    using var bm = new Mat();
                    Cv2.WarpPerspective(baseTextMask, bm, H, new Size(tw, th), InterpolationFlags.Nearest);
                    Cv2.BitwiseOr(textMask, bm, textMask);
                }
                int d = Math.Max(4, (int)Math.Round(scale * 3));
                using var k = Cv2.GetStructuringElement(MorphShapes.Rect, new Size(d, d));
                Cv2.Dilate(textMask, textMask, k);
            }

            var diffs = new List<DiffItem>();
            var pageAnno = new PageAnno
            {
                Page = tgtPno,
                Wpt = (float)(tBounds.X1 - tBounds.X0),
                Hpt = (float)(tBounds.Y1 - tBounds.Y0),
            };

                        // ----- STEP 1: GRAPHIC diff (text excluded) — trace the ACTUAL changed SHAPE -----
                        if (cfg.GraphicDiff.Enabled)
                        {
                            using var maskedBase = baseGrayAligned.Clone();
                            using var maskedTgt = tgtGray.Clone();
                            if (textMask != null)
                            {
                                maskedBase.SetTo(Scalar.All(255), textMask);
                                maskedTgt.SetTo(Scalar.All(255), textMask);
                            }
                            if (regionMask != null)
                            {
                                using var outside = new Mat();
                                Cv2.BitwiseNot(regionMask, outside);
                                maskedBase.SetTo(Scalar.All(255), outside);
                                maskedTgt.SetTo(Scalar.All(255), outside);
                            }
                            // Outline band radius (px) = the FULL annotation stroke width, so the
                            // traced silhouette keeps a visible hollow core (white gap on each side
                            // = radius - halfStroke = halfStroke) and can never self-fill; thin
                            // linework also merges into one clean outline instead of a tangle.
                            int bandPx = (int)Math.Max(1, Math.Round(Math.Max(1.2, cfg.Annotation.LineWidth) * scale));
                            var contours = GraphicDiff(maskedBase, maskedTgt, cfg.GraphicDiff, cfg.GraphicDiff.TolerancePx, bandPx);
                            // Defensive guard: a contour fully contained within a (margin-expanded)
                            // TEXT word box is a glyph artifact, never a figure change.
                            var textPx = TextWordRectsPx(tgtWords, scale, tBounds.X0, tBounds.Y0, 6)
                                .Concat(TextWordRectsPx(baseWordsAligned, scale, tBounds.X0, tBounds.Y0, 6))
                                .ToList();
                            foreach (var gc in contours)
                            {
                                int rx = Math.Max(0, gc.Rect.X); int ry = Math.Max(0, gc.Rect.Y);
                                int rw = Math.Min(gc.Rect.Width, tw - rx); int rh = Math.Min(gc.Rect.Height, th - ry);
                                if (rw <= 0 || rh <= 0) continue;
                                // Whole-page (or near-whole-page) contours are a misalignment /
                                // half-resolution-warp artifact, NOT a real drawing change.
                                if ((double)rw / tw > 0.6 && (double)rh / th > 0.6) continue;
                                double x = rx / scale + tBounds.X0;
                                double y = ry / scale + tBounds.Y0;
                                if (regionMask != null && !InsideAnyRegion(x, y, rw / scale, rh / scale, pageRegions, tBounds.X0, tBounds.Y0))
                                    continue;
                                var rectPx = new Rect(rx, ry, rw, rh);
                                if (textPx.Any(tr => tr.X <= rx && tr.Y <= ry && tr.Right >= rx + rw && tr.Bottom >= ry + rh))
                                    continue;
                                // Direction: compare ink presence in baseline (aligned) vs target,
                                // so we know whether the change is new-only or old-only.
                                string dir = gc.Dir;
                                // Store the ACTUAL changed-shape contour as a VECTOR polygon
                                // (target pixel frame, y-down). removed → BLUE DASHED; added /
                                // modified → RED SOLID.
                                if (dir == "removed") pageAnno.RemovedContours.Add(gc.Pts);
                                else pageAnno.AddedContours.Add(gc.Pts);
                                diffs.Add(new DiffItem
                                {
                                    Page = tgtPno,
                                    Type = "graphic",
                                    ChangeType = dir,
                                    Bbox = new Bbox { X = Round(x), Y = Round(y), Width = Round(rw / scale), Height = Round(rh / scale) },
                                    Confidence = 0.95
                                });
                            }
                        }

                        // ----- STEP 2: TEXT diff (independent of the pixel stage) -----
                        if (cfg.TextDiff.Enabled)
                        {
                            var textItems = TextDiff(baseWordsAligned, tgtWords, cfg.TextDiff, tgtPno, pageRegions, tBounds, scale);
                            diffs.AddRange(textItems);
                            pageAnno.TextItems.AddRange(textItems);
                        }

                        baseGrayAligned.Dispose();
                        resp.Summary.GraphicDiffs += diffs.Count(d => d.Type == "graphic");
                        resp.Summary.TextDiffs += diffs.Count(d => d.Type == "text");
                        resp.Diffs.AddRange(diffs);
                        pageAnnos.Add(pageAnno);
                        resp.Summary.TotalPages++;
                    }

                    // ---- report drawing-number pairing outcome ----
                    foreach (var (bp, tp, dno) in pagePairs)
                        resp.Summary.MatchedPages.Add($"基准页 {bp + 1} ↔ 目标页 {tp + 1} (#{dno})");
                    foreach (var (pg, dno) in addedPages)
                        resp.Summary.AddedPages.Add($"目标页 {pg + 1} (#{dno})");
                    foreach (var (pg, dno) in removedPages)
                        resp.Summary.RemovedPages.Add($"基准页 {pg + 1} (#{dno})");

                    // ---- figure ↔ material-table 1:1 cross-check ----
                    if (cfg.PipeMatch.Enabled && (req.Options?.PipeMatch ?? true))
                    {
                        resp.PipeMatch = CrossCheckPipes(allTgtPipes, req.CorrectedMaterialTable,
                            req.Options?.MaterialTableYFraction ?? cfg.PipeMatch.MaterialTableYFraction, L);
                    }

                    // ---- emit a VECTOR diff PDF: original target vector content + vector overlays ----
                    baseDoc?.Dispose(); tgtDoc?.Dispose();
                    PdfVectorDiff.Write(outPath, tgtCtxPath ?? req.Target, pageAnnos, scale, cfg.Annotation);
                    var json = JsonSerializer.Serialize(resp, new JsonSerializerOptions { WriteIndented = true });
                    Console.WriteLine(json);
                    L($"Done. output={outPath} graphic={resp.Summary.GraphicDiffs} text={resp.Summary.TextDiffs}");
                    return 0;
                }
                finally
                {
                    if (baseCtxPath != null) { try { File.Delete(baseCtxPath); } catch { } }
                    if (tgtCtxPath != null) { try { File.Delete(tgtCtxPath); } catch { } }
                }
            }
            catch (Exception ex)
            {
                L($"[error] {ex.Message}");
                return 1;
            }
        }

        // ------------------------------------------------------------------
        //  Helpers
        // ------------------------------------------------------------------
        static (string?, MuPDFDocument) Open(MuPDFContext ctx, string path)
        {
            if (path.Any(c => c > 127))
            {
                var tmp = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N") + ".pdf");
                File.Copy(path, tmp, true);
                return (tmp, new MuPDFDocument(ctx, tmp));
            }
            return (null, new MuPDFDocument(ctx, path));
        }

        static Mat RenderBgr(MuPDFDocument doc, int pno, double scale, out int w, out int h, out Rectangle bounds)
        {
            bounds = doc.Pages[pno].Bounds;
            double wPt = bounds.X1 - bounds.X0;
            double hPt = bounds.Y1 - bounds.Y0;
            // Derive the EXACT pixel size from the returned byte length. MuPDF rounds
            // each axis independently (and not necessarily via Math.Round), so we pick
            // the width nearest the estimate that actually divides totalPx — this
            // guarantees w*h == totalPx and prevents any buffer overrun on copy.
            byte[] bytes = doc.Render(pno, scale, PixelFormats.RGBA, false);
            int totalPx = bytes.Length / 4;
            (w, h) = PixelDims(totalPx, wPt, hPt, scale);
            var rgba = new Mat(h, w, MatType.CV_8UC4);
            Marshal.Copy(bytes, 0, rgba.Data, bytes.Length);
            // Composite the RGBA render over WHITE. PDF pages are documents on a
            // white background; MuPDF leaves unpainted (transparent) areas as
            // alpha=0, and dropping the alpha channel alone would turn them black.
            // Compositing guarantees correct contrast for both raster and edge diff.
            var bgr = new Mat(h, w, MatType.CV_8UC3);
            using var alpha = new Mat(); Cv2.ExtractChannel(rgba, alpha, 3);
            using var aF = new Mat(); alpha.ConvertTo(aF, MatType.CV_32FC1, 1.0 / 255.0);
            using var bgrF = new Mat();
            using var tmpBgr = new Mat(); Cv2.CvtColor(rgba, tmpBgr, ColorConversionCodes.RGBA2BGR);
            tmpBgr.ConvertTo(bgrF, MatType.CV_32FC3);
            using var one = new Mat(aF.Size(), aF.Type(), Scalar.All(1.0));
            using var inv = new Mat(); Cv2.Subtract(one, aF, inv);
            // Broadcast the 1-channel alpha to 3 channels for per-channel blending.
            using var a3 = new Mat(); Cv2.Merge(new[] { aF, aF, aF }, a3);
            using var inv3 = new Mat(); Cv2.Merge(new[] { inv, inv, inv }, inv3);
            using var whiteF = new Mat(bgrF.Size(), bgrF.Type(), Scalar.All(255.0));
            using var comp = new Mat();
            Cv2.Multiply(bgrF, a3, comp);
            using var whitePart = new Mat(); Cv2.Multiply(whiteF, inv3, whitePart);
            Cv2.Add(comp, whitePart, comp);
            comp.ConvertTo(bgr, MatType.CV_8UC3);
            rgba.Dispose();
            return bgr;
        }

        static Mat BuildTextMask(List<TextGeometry.Word> words, int w, int h, double scale, double x0, double y0)
        {
            var mask = new Mat(h, w, MatType.CV_8UC1, Scalar.All(0));
            var polys = new List<Point[]>();
            foreach (var wd in words)
            {
                polys.Add(new[]
                {
                    new Point((int)Math.Round((wd.x0 - x0) * scale), (int)Math.Round((wd.y0 - y0) * scale)),
                    new Point((int)Math.Round((wd.x1 - x0) * scale), (int)Math.Round((wd.y0 - y0) * scale)),
                    new Point((int)Math.Round((wd.x1 - x0) * scale), (int)Math.Round((wd.y1 - y0) * scale)),
                    new Point((int)Math.Round((wd.x0 - x0) * scale), (int)Math.Round((wd.y1 - y0) * scale)),
                });
            }
            if (polys.Count > 0) Cv2.FillPoly(mask, polys, Scalar.All(255));
            // Dilate proportional to render scale so anti-aliased glyph edges and the
            // em-box→glyph-box overhang are fully excluded from the pixel comparison
            // (prevents changed text from leaking into the graphic diff at any DPI).
            int d = Math.Max(4, (int)Math.Round(scale * 3));
            using var k = Cv2.GetStructuringElement(MorphShapes.Rect, new Size(d, d));
            Cv2.Dilate(mask, mask, k);
            return mask;
        }

        static Mat BuildRegionMask(List<Bbox> regions, int w, int h, double scale, double x0, double y0)
        {
            var mask = new Mat(h, w, MatType.CV_8UC1, Scalar.All(0));
            foreach (var r in regions)
            {
                int x = (int)Math.Round((r.X - x0) * scale);
                int y = (int)Math.Round((r.Y - y0) * scale);
                int rw = (int)Math.Round(r.Width * scale);
                int rh = (int)Math.Round(r.Height * scale);
                Cv2.Rectangle(mask, new Rect(x, y, Math.Max(1, rw), Math.Max(1, rh)), Scalar.All(255), -1);
            }
            return mask;
        }

        static bool InsideAnyRegion(double x, double y, double w, double h, List<Bbox> regions, double x0, double y0)
        {
            foreach (var r in regions)
                if (x + w >= r.X && x <= r.X + r.Width && y + h >= r.Y && y <= r.Y + r.Height) return true;
            return regions.Count == 0;
        }

        // Pixel-space bounding boxes of every text word (union of both pages),
        // expanded by `margin` px. Used as a defensive guard so no glyph — even one
        // that slips past the text mask — is reported as a figure change.
        static List<Rect> TextWordRectsPx(List<TextGeometry.Word> words, double scale, double x0, double y0, int margin)
        {
            var rects = new List<Rect>();
            foreach (var w in words)
            {
                int x = (int)Math.Round((w.x0 - x0) * scale) - margin;
                int y = (int)Math.Round((w.y0 - y0) * scale) - margin;
                int ww = (int)Math.Round((w.x1 - w.x0) * scale) + 2 * margin;
                int hh = (int)Math.Round((w.y1 - w.y0) * scale) + 2 * margin;
                rects.Add(new Rect(x, y, Math.Max(1, ww), Math.Max(1, hh)));
            }
            return rects;
        }

        // Similarity registration: the two revisions are the SAME sheet at a different
        // paper scale, so we recover a uniform scale from the page bounds and the residual
        // translation from the TEXT WORDS — each base word (scaled by the page-bounds
        // scale) is matched to its nearest target word, and the median of the residual
        // shifts is the translation. This is far more robust than pixel correlation for
        // re-inked/revised drawings where line content has actually changed.
        // Returns a 3x3 homography (base px → target px) = [sx 0 dx; 0 sy dy; 0 0 1].
        // Registration: align the BASELINE into the TARGET frame BEFORE diffing. The two
        // revisions are the SAME sheet, so we recover a UNIFORM scale from the page bounds
        // and the residual TRANSLATION from the IMAGE CONTENT (FFT phase correlation aligns
        // the bulk of the drawing — exactly "图像大部分基本上对齐"), with a word-median
        // fallback when phase correlation is unreliable. A few-mm residual offset is then
        // tolerated by the diff-stage erosion (Registration.ToleranceMm). Always produces a
        // single affine (uniform scale + translation) and warps the baseline — so "先对齐再比较".
        static Mat ComputeAlign(Mat baseGray, Mat tgtGray, int bw, int bh, int tw, int th,
            Pdf2dfConfig.RegistrationCfg reg,
            List<TextGeometry.Word> baseWords, List<TextGeometry.Word> tgtWords,
            double scale, double bX0, double bY0, double tX0, double tY0)
        {
            double sx = (double)tw / bw;
            double sy = (double)th / bh;
            double dx = 0, dy = 0;
            string src = "none";

            // Warp baseline to TARGET size (uniform page-bounds scale) so only the residual
            // translation remains — this image is what we register against the target.
            using var baseScaled = new Mat();
            var Hs = Mat.Eye(3, 3, MatType.CV_64FC1).ToMat();
            Hs.Set<double>(0, 0, sx); Hs.Set<double>(1, 1, sy);
            Cv2.WarpPerspective(baseGray, baseScaled, Hs, new Size(tw, th));
            Hs.Dispose();

            if (reg.Method != "none")
            {
                // PRIMARY: FFT phase correlation on the grayscale images. Aligns the BULK of
                // the drawing content (robust to local line/figure changes between revisions).
                if (reg.Method == "phase" || reg.Method == "similarity")
                {
                    try
                    {
                        using var bF = new Mat(); using var tF = new Mat();
                        baseScaled.ConvertTo(bF, MatType.CV_32F);
                        tgtGray.ConvertTo(tF, MatType.CV_32F);
                        Point2d shift = Cv2.PhaseCorrelate(bF, tF, null, out double response);
                        if (response > 0.08 && !double.IsNaN(shift.X) && !double.IsNaN(shift.Y)
                            && Math.Abs(shift.X) < tw * 0.4 && Math.Abs(shift.Y) < th * 0.4)
                        {
                            dx = shift.X; dy = shift.Y; src = "phase";
                            Console.Error.WriteLine($"  align(phase): response={response:F3} dx(px)={dx:F1} dy(px)={dy:F1}");
                        }
                    }
                    catch (Exception ex) { Console.Error.WriteLine($"  align(phase) failed: {ex.Message}"); }
                }

                // FALLBACK: word-median translation (used when phase is off, or method=="words").
                if (src == "none" && baseWords.Count > 0 && tgtWords.Count > 0)
                {
                    var tPts = tgtWords.Select(w => new Point2d((w.xc - tX0) * scale, (w.yc - tY0) * scale)).ToList();
                    var bScaled = baseWords.Select(w => new Point2d((w.xc - bX0) * scale * sx, (w.yc - bY0) * scale * sy)).ToList();
                    var dxx = new List<double>();
                    var dyy = new List<double>();
                    double maxShift2 = (tw * 0.5) * (tw * 0.5);
                    foreach (var bp in bScaled)
                    {
                        double best = double.MaxValue, bx = 0, by = 0;
                        foreach (var tp in tPts)
                        {
                            double ex = tp.X - bp.X, ey = tp.Y - bp.Y;
                            double d2 = ex * ex + ey * ey;
                            if (d2 < best) { best = d2; bx = ex; by = ey; }
                        }
                        if (best < maxShift2) { dxx.Add(bx); dyy.Add(by); }
                    }
                    if (dxx.Count >= 5)
                    {
                        dxx.Sort(); dyy.Sort();
                        dx = Median(dxx); dy = Median(dyy);
                        if (double.IsNaN(dx) || double.IsNaN(dy) || Math.Abs(dx) > tw * 0.4 || Math.Abs(dy) > th * 0.4)
                            { dx = 0; dy = 0; }
                        src = "words";
                        Console.Error.WriteLine($"  align(words): candidates={dxx.Count} dx(px)={dx:F1} dy(px)={dy:F1}");
                    }
                }
            }

            var H = Mat.Eye(3, 3, MatType.CV_64FC1).ToMat();
            H.Set<double>(0, 0, sx); H.Set<double>(1, 1, sy);
            H.Set<double>(0, 2, dx); H.Set<double>(1, 2, dy);
            Console.Error.WriteLine($"  align: sx={sx:F4} sy={sy:F4} dx(px)={dx:F1} dy(px)={dy:F1} src={src}");
            return H;
        }

        static double Median(List<double> xs)
        {
            int n = xs.Count; if (n == 0) return 0;
            if (n % 2 == 1) return xs[n / 2];
            return (xs[n / 2 - 1] + xs[n / 2]) / 2.0;
        }

        // Map baseline words (base point frame) into the TARGET point frame using the
        // pixel-space homography H and the two pages' bounds. Each bbox corner is
        // transformed; the new bbox is the corner bounding box in target pt.
        static List<TextGeometry.Word> TransformWordsToTarget(List<TextGeometry.Word> words, Mat H,
            double bX0, double bY0, double tX0, double tY0, double scale)
        {
            var outw = new List<TextGeometry.Word>(words.Count);
            foreach (var w in words)
            {
                double minx = double.MaxValue, miny = double.MaxValue, maxx = double.MinValue, maxy = double.MinValue;
                foreach (var (cx, cy) in new[] { (w.x0, w.y0), (w.x1, w.y0), (w.x1, w.y1), (w.x0, w.y1) })
                {
                    double bx = (cx - bX0) * scale;
                    double by = (cy - bY0) * scale;
                    double w0 = H.Get<double>(0, 0) * bx + H.Get<double>(0, 1) * by + H.Get<double>(0, 2);
                    double w1 = H.Get<double>(1, 0) * bx + H.Get<double>(1, 1) * by + H.Get<double>(1, 2);
                    double w2 = H.Get<double>(2, 0) * bx + H.Get<double>(2, 1) * by + H.Get<double>(2, 2);
                    double tx = w0 / w2, ty = w1 / w2;
                    double ptX = tx / scale + tX0;
                    double ptY = ty / scale + tY0;
                    minx = Math.Min(minx, ptX); maxx = Math.Max(maxx, ptX);
                    miny = Math.Min(miny, ptY); maxy = Math.Max(maxy, ptY);
                }
                outw.Add(new TextGeometry.Word
                {
                    x0 = minx, y0 = miny, x1 = maxx, y1 = maxy,
                    xc = (minx + maxx) / 2, yc = (miny + maxy) / 2,
                    text = w.text,
                });
            }
            return outw;
        }

        // A connected changed region: its simplified polygon (the actual changed SHAPE,
        // in target pixel frame y-down) plus the direction (added / removed / modified).
        public class GraphicContour
        {
            public List<double> Pts = new();   // flat [x,y,x,y,...] in target pixel frame (y-down)
            public string Dir = "added";
            public Rect Rect;
        }

        // ---- TRUE 1:1 SHAPE tracing (not a circle, box, or simulated line) ----
        // Two-stage, like the text diff: (1) LOCALIZE where the drawing changed with a
        // clean EDGE difference — few, well-placed candidate regions (robust to the noise a
        // raw ink difference explodes into when one revision is up-scaled), then (2) for each
        // candidate, trace the ACTUAL full outline of the element that changed:
        //   • added ink   → full outline of the new primitive in the TARGET  (TraceColor, solid)
        //   • removed ink → full outline of the old primitive in the BASELINE (TraceColor, dashed)
        // The outline is the real connected-component boundary of the linework (OpenCV
        // FindContours on the element's ink mask) — a genuine trace of the pipe/equipment,
        // never a thin "difference shell" and never an approximated circle/box.
        // Library: OpenCvSharp FindContours — the standard, professional contour tracer.
        static List<GraphicContour> GraphicDiff(Mat a, Mat b, Pdf2dfConfig.GraphicCfg g, int tolPx, int bandPx)
        {
            // a = baseline (aligned, text excluded, white bg); b = target.
            using var baseInk = new Mat(); using var tgtInk = new Mat();
            Cv2.Threshold(a, baseInk, 128, 255, ThresholdTypes.BinaryInv); // dark pixels = ink
            Cv2.Threshold(b, tgtInk, 128, 255, ThresholdTypes.BinaryInv);

            // (1) localize changed regions cleanly with an edge difference.
            using var edge = EdgeDiff(a, b);

            // (2) full ink-component labels on each side → lift the REAL element outline.
            Mat tgtLabels = new Mat(), tgtStats = new Mat(), tgtC = new Mat();
            Cv2.ConnectedComponentsWithStats(tgtInk, tgtLabels, tgtStats, tgtC, PixelConnectivity.Connectivity8);
            Mat baseLabels = new Mat(), baseStats = new Mat(), baseC = new Mat();
            Cv2.ConnectedComponentsWithStats(baseInk, baseLabels, baseStats, baseC, PixelConnectivity.Connectivity8);

            Cv2.FindContours(edge, out Point[][] seeds, out _, RetrievalModes.External, ContourApproximationModes.ApproxNone);
            var items = new List<GraphicContour>();
            foreach (var s in seeds)
            {
                var r = Cv2.BoundingRect(s);
                if (r.Width * r.Height < g.MinComponentArea) continue;
                if ((double)r.Width / a.Width > 0.6 && (double)r.Height / a.Height > 0.6) continue; // whole-page guard
                int cx = Clamp(r.X + r.Width / 2, a.Width - 1);
                int cy = Clamp(r.Y + r.Height / 2, a.Height - 1);
                // direction from ink presence at the candidate (which side actually has ink).
                double inkBase = MeanDark(a, r), inkTgt = MeanDark(b, r);
                string dir = inkTgt > inkBase + 0.04 ? "added" : inkBase > inkTgt + 0.04 ? "removed" : "modified";
                bool removed = dir != "added";
                var labels = removed ? baseLabels : tgtLabels;
                var stats = removed ? baseStats : tgtStats;
                int lab = labels.At<int>(cy, cx);
                if (lab <= 0) continue;
                int lx = stats.At<int>(lab, 0), ly = stats.At<int>(lab, 1), lw = stats.At<int>(lab, 2), lh = stats.At<int>(lab, 3);
                int area = stats.At<int>(lab, 4); // CC_STAT_AREA
                if (area > a.Width * a.Height * 0.25) continue; // skip a giant connected network blob
                // Drop tiny specks on the ORIGINAL ink area (before dilation inflates them) so
                // the outline band can't resurrect antialias noise as a "diff".
                if (area < g.MinComponentArea) continue;
                // crop the labels image to this element's box (+margin) and mask just this label.
                // The margin must exceed the dilation radius, otherwise the dilation below is
                // CLIPPED at the crop edge and has no effect (the outline stays razor-thin).
                int marg = 2 + bandPx;
                int mx = Math.Max(0, lx - marg), my = Math.Max(0, ly - marg);
                int mw = Math.Min(a.Width - mx, lw + 2 * marg), mh = Math.Min(a.Height - my, lh + 2 * marg);
                using var crop = new Mat(labels, new Rect(mx, my, mw, mh));
                using var labVal = new Mat(crop.Size(), crop.Type(), new Scalar(lab));
                using var comp = new Mat();
                Cv2.Compare(crop, labVal, comp, CmpType.EQ);
                labVal.Dispose();
                // DILATE the element's ink before contouring → the trace is a clean HOLLOW
                // silhouette instead of hugging every hairline stroke:
                //   • thin strokes merge into one band → no "double-line" tangle;
                //   • the band is always wider than the annotation stroke, so a 3pt stroke
                //     can never self-fill a thin sliver → fixes the "红线填满" artefact.
                // The band radius = one stroke width, so the hollow core stays visible
                // (white gap on each side = radius - halfStroke) while the red still hugs the
                // element's real boundary.
                if (bandPx > 0)
                {
                    using var k = Cv2.GetStructuringElement(MorphShapes.Ellipse, new Size(2 * bandPx + 1, 2 * bandPx + 1));
                    Cv2.Dilate(comp, comp, k);
                }
                Cv2.FindContours(comp, out Point[][] cs, out _, RetrievalModes.External, ContourApproximationModes.ApproxNone);
                foreach (var cc in cs)
                {
                    var rr = Cv2.BoundingRect(cc);
                    if (rr.Width * rr.Height < g.MinComponentArea) continue;
                    var shifted = new Point[cc.Length];
                    for (int i = 0; i < cc.Length; i++) shifted[i] = new Point(cc[i].X + mx, cc[i].Y + my);
                    EmitContour(items, shifted, dir, g);
                }
            }
            tgtLabels.Dispose(); tgtStats.Dispose(); tgtC.Dispose();
            baseLabels.Dispose(); baseStats.Dispose(); baseC.Dispose();
            // Ignore a few-mm REGISTRATION error: an added trace and a removed trace that are
            // the SAME shape merely shifted within ToleranceMm cancel each other (user:
            // "偏离几毫米…忽略"). A larger real move / add / delete stays.
            return CancelShiftedTraces(items, Math.Max(2, tolPx));
        }

        // Cancel added/removed element traces that are the same shape merely shifted within
        // `tol` px (registration error). Returns the kept traces.
        static List<GraphicContour> CancelShiftedTraces(List<GraphicContour> items, int tol)
        {
            var added = new List<GraphicContour>();
            var rem = new List<GraphicContour>();
            foreach (var it in items)
                if (it.Dir == "removed") rem.Add(it); else added.Add(it);
            var remSkipped = new HashSet<int>();
            var kept = new List<GraphicContour>();
            foreach (var a in added)
            {
                bool cancelled = false;
                for (int j = 0; j < rem.Count; j++)
                {
                    if (remSkipped.Contains(j)) continue;
                    var ar = new Rect(a.Rect.X - tol, a.Rect.Y - tol, a.Rect.Width + 2 * tol, a.Rect.Height + 2 * tol);
                    var rr = new Rect(rem[j].Rect.X - tol, rem[j].Rect.Y - tol, rem[j].Rect.Width + 2 * tol, rem[j].Rect.Height + 2 * tol);
                    if (ar.IntersectsWith(rr)) { remSkipped.Add(j); cancelled = true; break; }
                }
                if (!cancelled) kept.Add(a);
            }
            for (int j = 0; j < rem.Count; j++) if (!remSkipped.Contains(j)) kept.Add(rem[j]);
            return kept;
        }

        // Simplify a contour (vertex decimator only — never rounds the shape) and emit as a
        // VECTOR polygon carrying its change direction.
        static void EmitContour(List<GraphicContour> items, Point[] c, string dir, Pdf2dfConfig.GraphicCfg g)
        {
            Point[] approx = c;
            if (c.Length > 200)
            {
                double perim = 0;
                for (int i = 0; i < c.Length; i++)
                {
                    var p0 = c[i]; var p1 = c[(i + 1) % c.Length];
                    perim += Math.Sqrt((p1.X - p0.X) * (p1.X - p0.X) + (p1.Y - p0.Y) * (p1.Y - p0.Y));
                }
                approx = Cv2.ApproxPolyDP(c, Math.Max(1.0, perim * 0.002), true);
            }
            var pts = new List<double>(approx.Length * 2);
            foreach (var p in approx) { pts.Add(p.X); pts.Add(p.Y); }
            items.Add(new GraphicContour { Pts = pts, Dir = dir, Rect = Cv2.BoundingRect(c) });
        }

        static int Clamp(int v, int max) => v < 0 ? 0 : v > max ? max : v;

        // Edge-based difference for LOCALIZING changed regions: Canny edges of both renders
        // (robust to grayscale anti-aliasing) compared, thresholded, opened. Cheap & clean.
        static Mat EdgeDiff(Mat a, Mat b)
        {
            using var ea = new Mat(); using var eb = new Mat();
            Cv2.Canny(a, ea, 40, 120);
            Cv2.Canny(b, eb, 40, 120);
            var d = new Mat();
            Cv2.Absdiff(ea, eb, d);
            using var k = Cv2.GetStructuringElement(MorphShapes.Rect, new Size(3, 3));
            Cv2.MorphologyEx(d, d, MorphTypes.Open, k); // de-noise
            return d;
        }

        // Fraction of DARK pixels (ink / lines) inside a pixel-space rectangle. Decides the
        // DIRECTION of a localized change: which side actually carries ink.
        static double MeanDark(Mat gray, Rect r)
        {
            if (gray.Empty() || r.Width <= 0 || r.Height <= 0) return 0;
            using var roi = new Mat(gray, r);
            using var bin = new Mat();
            Cv2.Threshold(roi, bin, 128, 1, ThresholdTypes.BinaryInv);  // dark → 1
            return Cv2.Mean(bin).Val0;                                  // average = dark fraction
        }

        // Both word lists are expected in the SAME (target) point frame.
        // `wordsA` = baseline (already transformed into the target frame); `wordsB` = target.
        //
        // Matching is done in TWO position-tolerant passes so that re-sequenced /
        // relocated text does NOT cascade into false "modified" pairs (the classic
        // failure of pure nearest-neighbour matching: OIL→FUEL, SYSTEM→OIL, …).
        //   PASS 1  content match  — pair a target word with a base word of the SAME
        //            normalized text within tolerance (handles moved/unchanged text);
        //            identical text that also shifted → "moved".
        //   PASS 2  positional match — an unmatched target word that now lines up with a
        //            remaining base word but with DIFFERENT text → "modified".
        //   PASS 3  remaining target words → "added" (new-only → RED solid).
        //   PASS 4  remaining base words → "removed" (old-only → BLUE dashed).
        static List<DiffItem> TextDiff(List<TextGeometry.Word> wordsA, List<TextGeometry.Word> wordsB,
            Pdf2dfConfig.TextCfg t, int page, List<Bbox> regions, Rectangle tBounds, double scale)
        {
            string norm(string s) => TextGeometry.Norm(s);
            var items = new List<DiffItem>();
            // Defensive: drop empty / whitespace-only words so they can never produce a
            // bogus ("None" => "None") diff item.
            var A = wordsA.Where(w => !string.IsNullOrWhiteSpace(w.text)).ToList();
            var B = wordsB.Where(w => !string.IsNullOrWhiteSpace(w.text)).ToList();
            var consumed = new bool[A.Count];
            var bMatched = new bool[B.Count];
            var inside = regions.Count == 0 ? (Func<double, double, bool>)((x, y) => true)
                                           : (x, y) => regions.Any(r => x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height);

            // ----- PASS 1: content match (same normalized text, near position) -----
            for (int i = 0; i < B.Count; i++)
            {
                var tw = B[i];
                if (!inside(tw.x0, tw.yc)) continue;
                string tn = norm(tw.text);
                double bestD = double.MaxValue; int bestJ = -1;
                for (int j = 0; j < A.Count; j++)
                {
                    if (consumed[j] || norm(A[j].text) != tn) continue;
                    double dx = tw.xc - A[j].xc, dy = tw.yc - A[j].yc;
                    if (Math.Abs(dx) > t.XTolerance || Math.Abs(dy) > t.YTolerance) continue;
                    double d = dx * dx + dy * dy;
                    if (d < bestD) { bestD = d; bestJ = j; }
                }
                if (bestJ >= 0)
                {
                    consumed[bestJ] = true; bMatched[i] = true;
                    var aw = A[bestJ];
                    double move = Math.Max(Math.Abs(tw.xc - aw.xc), Math.Abs(tw.yc - aw.yc));
                    if (move > t.MoveTolerance)
                        items.Add(Mk(page, "text", tw, "moved", oldText: aw.text, newText: tw.text));
                }
            }

            // ----- PASS 2: positional match for still-unmatched target words -----
            for (int i = 0; i < B.Count; i++)
            {
                if (bMatched[i] || !inside(B[i].x0, B[i].yc)) continue;
                var tw = B[i];
                double bestD = double.MaxValue; int bestJ = -1;
                for (int j = 0; j < A.Count; j++)
                {
                    if (consumed[j]) continue;
                    double dx = tw.xc - A[j].xc, dy = tw.yc - A[j].yc;
                    if (Math.Abs(dx) > t.XTolerance || Math.Abs(dy) > t.YTolerance) continue;
                    double d = dx * dx + dy * dy;
                    if (d < bestD) { bestD = d; bestJ = j; }
                }
                if (bestJ >= 0)
                {
                    consumed[bestJ] = true; bMatched[i] = true;
                    var aw = A[bestJ];
                    items.Add(Mk(page, "text", tw, "modified", oldText: aw.text, newText: tw.text));
                }
            }

            // ----- PASS 3: remaining target words are new-only (added → RED) -----
            for (int i = 0; i < B.Count; i++)
                if (!bMatched[i] && inside(B[i].x0, B[i].yc))
                    items.Add(Mk(page, "text", B[i], "added", oldText: null, newText: B[i].text));

            // ----- PASS 4: remaining base words are old-only (removed → BLUE) -----
            for (int j = 0; j < A.Count; j++)
                if (!consumed[j] && inside(A[j].x0, A[j].yc))
                    items.Add(Mk(page, "text", A[j], "removed", oldText: A[j].text, newText: null));

            int added = items.Count(i => i.ChangeType == "added");
            int removed = items.Count(i => i.ChangeType == "removed");
            int modified = items.Count(i => i.ChangeType == "modified");
            int moved = items.Count(i => i.ChangeType == "moved");
            Console.Error.WriteLine($"  textdiff: A={A.Count} B={B.Count} matched={A.Count - removed - moved} added={added} removed={removed} modified={modified} moved={moved}");
            return items;
        }

        static DiffItem Mk(int page, string type, TextGeometry.Word w, string change, string? oldText, string? newText)
        {
            return new DiffItem
            {
                Page = page,
                Type = type,
                ChangeType = change,
                OldText = oldText,
                NewText = newText,
                Confidence = 0.9,
                // word is already in the target point frame → bbox directly.
                Bbox = new Bbox { X = Round(w.x0), Y = Round(w.y0), Width = Round(w.x1 - w.x0), Height = Round(w.y1 - w.y0) },
            };
        }

        // ------------------------------------------------------------------
        //  Drawing-number (图纸号) based page pairing
        // ------------------------------------------------------------------
        static string? DrawingNoOfPage(MuPDFDocument doc, int pno)
        {
            using var st = doc.GetStructuredTextPage(pno, false, StructuredTextFlags.None);
            double H = doc.Pages[pno].Bounds.Y1 - doc.Pages[pno].Bounds.Y0;
            var words = TextGeometry.GetWords(st, H, 0.18);
            return TextGeometry.ExtractDrawingNo(words, null);
        }

        // Pair every baseline page with a target page that carries the SAME drawing
        // number (图纸号), 1:1 within each number group (k-th base sheet ↔ k-th target
        // sheet). Pages whose number exists only in the target are reported as added
        // (new drawings/sheets); pages only in the baseline as removed.
        static (List<(int basePno, int tgtPno, string no)> pairs,
                List<(int page, string no)> addedTgt,
                List<(int page, string no)> removedBase)
            MatchPages(MuPDFDocument baseDoc, int nBase, MuPDFDocument tgtDoc, int nTgt, Action<string> L)
        {
            var baseNo = new Dictionary<int, string>();
            var tgtNo = new Dictionary<int, string>();
            for (int p = 0; p < nBase; p++) baseNo[p] = DrawingNoOfPage(baseDoc, p) ?? "?";
            for (int p = 0; p < nTgt; p++) tgtNo[p] = DrawingNoOfPage(tgtDoc, p) ?? "?";
            var baseGroups = baseNo.GroupBy(kv => kv.Value).ToDictionary(g => g.Key, g => g.Select(kv => kv.Key).OrderBy(x => x).ToList());
            var tgtGroups = tgtNo.GroupBy(kv => kv.Value).ToDictionary(g => g.Key, g => g.Select(kv => kv.Key).OrderBy(x => x).ToList());

            var pairs = new List<(int, int, string)>();
            var addedTgt = new List<(int, string)>();
            var removedBase = new List<(int, string)>();
            foreach (var g in baseGroups)
            {
                if (g.Key == "?") { foreach (var pg in g.Value) removedBase.Add((pg, "?")); continue; }
                if (tgtGroups.TryGetValue(g.Key, out var tpages))
                {
                    int n = Math.Min(g.Value.Count, tpages.Count);
                    for (int k = 0; k < n; k++) pairs.Add((g.Value[k], tpages[k], g.Key));
                    for (int k = g.Value.Count; k < tpages.Count; k++) addedTgt.Add((tpages[k], g.Key));
                    for (int k = tpages.Count; k < g.Value.Count; k++) removedBase.Add((g.Value[k], g.Key));
                }
                else { foreach (var pg in g.Value) removedBase.Add((pg, g.Key)); }
            }
            foreach (var g in tgtGroups)
            {
                if (g.Key == "?") { foreach (var pg in g.Value) addedTgt.Add((pg, "?")); continue; }
                if (!baseGroups.ContainsKey(g.Key)) foreach (var pg in g.Value) addedTgt.Add((pg, g.Key));
            }
            L($"  图纸号配对: 匹配 {pairs.Count} 页, 新增 {addedTgt.Count} 页, 删除 {removedBase.Count} 页");
            return (pairs, addedTgt, removedBase);
        }

        // Figure (drawn pipes) ↔ material-table (BOM) 1:1 cross-check.
        //   • figure side  = pipe numbers found in the upper part of the target pages
        //     (above `yFraction` of the page height — the isometric drawing itself);
        //   • table side   = either the user's corrected material table (CSV/TSV upload,
        //     authoritative) OR the pipe numbers found in the lower band of the pages.
        // The result reports which numbers are on both sides, which are drawn-but-missing
        // from the BOM, and which are in the BOM-but-not-drawn.
        static PipeMatch CrossCheckPipes(List<(string no, double yc, double h)> tgtPipes,
            string? correctedTable, double yFraction, Action<string> L)
        {
            var result = new PipeMatch { Enabled = true };
            var figure = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var table = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (var p in tgtPipes)
            {
                bool isTable = p.yc >= yFraction * p.h;
                if (isTable) table.Add(p.no);
                else figure.Add(p.no);
            }

            if (!string.IsNullOrWhiteSpace(correctedTable) && File.Exists(correctedTable))
            {
                result.TableSource = "upload:" + Path.GetFileName(correctedTable);
                table.Clear();
                try
                {
                    foreach (var line in File.ReadLines(correctedTable))
                    {
                        if (string.IsNullOrWhiteSpace(line)) continue;
                        // Accept comma OR tab separated; tolerate quotes.
                        var cells = line.Split(new[] { ',', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                        foreach (var raw in cells)
                        {
                            var cell = raw.Trim().Trim('"', '\'');
                            if (TextGeometry.LooksLikeLineNo(cell)) table.Add(cell.ToUpperInvariant());
                        }
                    }
                    L($"  材料表交叉核对: 使用上传表 {Path.GetFileName(correctedTable)}，提取管线号 {table.Count} 个");
                }
                catch (Exception ex)
                {
                    L($"  [warn] 读取上传材料表失败，回退到 PDF 内材料表: {ex.Message}");
                    // Re-derive the table side from the PDF lower band.
                    table.Clear();
                    foreach (var p in tgtPipes)
                        if (p.yc >= yFraction * p.h) table.Add(p.no);
                    result.TableSource = "pdf";
                }
            }
            else
            {
                result.TableSource = "pdf";
            }

            var matched = new List<string>();
            var figureOnly = new List<string>();
            var tableOnly = new List<string>();
            foreach (var f in figure) (table.Contains(f) ? matched : figureOnly).Add(f);
            foreach (var t in table) if (!figure.Contains(t)) tableOnly.Add(t);
            matched.Sort(); figureOnly.Sort(); tableOnly.Sort();
            result.Matched = matched;
            result.FigureOnly = figureOnly;
            result.TableOnly = tableOnly;
            L($"  管线号一一对应: 匹配 {matched.Count}，图中独有(材料表缺) {figureOnly.Count}，材料表独有(图中未画) {tableOnly.Count}");
            return result;
        }

        // OpenCvSharp PutText uses the Hershey fonts which only cover printable ASCII.
        // Turkish/CJK/accents would throw on marshaling or render as mojibake, so we
        // only attempt to draw text that is purely ASCII (pipe numbers, English labels).
        static bool IsAsciiRenderable(string s)
        {
            foreach (char c in s) if (c < 0x20 || c > 0x7E) return false;
            return true;
        }

        static double Round(double v) => Math.Round(v, 1);

        // Derive the true (w,h) of a whole-page RGBA render from its byte length.
        // MuPDF rounds each axis independently, so we search widths within ±2px of the
        // Math.Round estimate for one that divides totalPx; the matching h is then exact
        // (w*h == totalPx). This makes the w/h we hand to the pixel buffer always correct.
        static (int w, int h) PixelDims(int totalPx, double wPt, double hPt, double scale)
        {
            int wEst = Math.Max(1, (int)Math.Round(wPt * scale));
            int bestW = wEst, bestErr = int.MaxValue;
            for (int dw = -2; dw <= 2; dw++)
            {
                int w = wEst + dw;
                if (w <= 0) continue;
                if (totalPx % w == 0)
                {
                    int err = Math.Abs(w - wEst);
                    if (err < bestErr) { bestErr = err; bestW = w; }
                }
            }
            if (bestErr == int.MaxValue) bestW = wEst;          // no divisor found (rare)
            int h = Math.Max(1, totalPx / bestW);                // exact when a divisor was found
            return (bestW, h);
        }
    }
}
