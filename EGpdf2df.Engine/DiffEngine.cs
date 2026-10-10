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
                    var pageImages = new List<(byte[] data, int w, int h)>();
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

                        // ----- STEP 1: GRAPHIC diff (text excluded) -----
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
                            var rects = GraphicDiff(maskedBase, maskedTgt, cfg.GraphicDiff);
                            // Defensive post-filter: drop any graphic rect that is fully
                            // contained within a (margin-expanded) TEXT word box. This is a
                            // second guarantee — on top of the text mask — that changed or
                            // anti-aliased glyphs are never reported as figure changes.
                            var textPx = TextWordRectsPx(tgtWords, scale, tBounds.X0, tBounds.Y0, 6)
                                .Concat(TextWordRectsPx(baseWordsAligned, scale, tBounds.X0, tBounds.Y0, 6))
                                .ToList();
                            foreach (var r in rects)
                            {
                                int rx = Math.Max(0, r.X); int ry = Math.Max(0, r.Y);
                                int rw = Math.Min(r.Width, tw - rx); int rh = Math.Min(r.Height, th - ry);
                                if (rw <= 0 || rh <= 0) continue;
                                // Whole-page (or near-whole-page) contours are a misalignment
                                // / half-resolution-warp artifact, NOT a real drawing change.
                                // Drop them so the diff never paints one giant box over the sheet.
                                if ((double)rw / tw > 0.6 && (double)rh / th > 0.6) continue;
                                double x = rx / scale + tBounds.X0;
                                double y = ry / scale + tBounds.Y0;
                                if (regionMask != null && !InsideAnyRegion(x, y, rw / scale, rh / scale, pageRegions, tBounds.X0, tBounds.Y0))
                                    continue;
                                if (textPx.Any(tr => tr.X <= rx && tr.Y <= ry && tr.Right >= rx + rw && tr.Bottom >= ry + rh))
                                    continue;
                                // Direction: compare ink presence in baseline (aligned) vs target,
                                // so we know whether the change is new-only or old-only.
                                double inkBase = MeanDark(baseGrayAligned, new Rect(rx, ry, rw, rh));
                                double inkTgt = MeanDark(tgtGray, new Rect(rx, ry, rw, rh));
                                string dir;
                                if (inkTgt > inkBase + 0.04) dir = "added";        // new has lines, old doesn't → RED solid
                                else if (inkBase > inkTgt + 0.04) dir = "removed"; // old had lines, new doesn't → BLUE dashed
                                else dir = "modified";                             // both carry ink but differ → RED solid
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
                        }

                        baseGrayAligned.Dispose();
                        resp.Summary.GraphicDiffs += diffs.Count(d => d.Type == "graphic");
                        resp.Summary.TextDiffs += diffs.Count(d => d.Type == "text");
                        resp.Diffs.AddRange(diffs);

                        // ----- annotate target with RED markers -----
                        Annotate(tgtB, diffs, scale, tBounds.X0, tBounds.Y0, cfg.Annotation);
                        pageImages.Add((EncodeJpeg(tgtB, 92), tw, th));
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

                    PdfFromImages.Write(outPath, pageImages);
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
        static Mat ComputeAlign(Mat baseGray, Mat tgtGray, int bw, int bh, int tw, int th,
            Pdf2dfConfig.RegistrationCfg reg,
            List<TextGeometry.Word> baseWords, List<TextGeometry.Word> tgtWords,
            double scale, double bX0, double bY0, double tX0, double tY0)
        {
            double sx = (double)tw / bw;
            double sy = (double)th / bh;
            double dx = 0, dy = 0;
            if (reg.Method != "none" && baseWords.Count > 0 && tgtWords.Count > 0)
            {
                // Project both word sets into the TARGET pixel frame:
                //   target word px = (tc - tX0) * scale
                //   base word px   = (bc - bX0) * scale, then scaled by (sx, sy)
                var tPts = tgtWords.Select(w => new Point2d((w.xc - tX0) * scale, (w.yc - tY0) * scale)).ToList();
                var bScaled = baseWords.Select(w => new Point2d((w.xc - bX0) * scale * sx, (w.yc - bY0) * scale * sy)).ToList();
                var dxx = new List<double>();
                var dyy = new List<double>();
                double maxShift2 = (tw * 0.5) * (tw * 0.5);   // sanity cap on a single match
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
                    if (double.IsNaN(dx) || double.IsNaN(dy)
                        || Math.Abs(dx) > tw * 0.4 || Math.Abs(dy) > th * 0.4)
                    { dx = 0; dy = 0; }
                }
                Console.Error.WriteLine($"  align(words): candidates={dxx.Count} dx(px)={dx:F1} dy(px)={dy:F1}");
            }
            var H = Mat.Eye(3, 3, MatType.CV_64FC1).ToMat();
            H.Set<double>(0, 0, sx); H.Set<double>(1, 1, sy);
            H.Set<double>(0, 2, dx); H.Set<double>(1, 2, dy);
            Console.Error.WriteLine($"  align: sx={sx:F4} sy={sy:F4} dx(px)={dx:F1} dy(px)={dy:F1}");
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

        static List<Rect> GraphicDiff(Mat a, Mat b, Pdf2dfConfig.GraphicCfg g)
        {
            using var diff = g.Method switch
            {
                "ssim" => SsimDiff(a, b),
                "absdiff" => AbsDiffMap(a, b, g),
                _ => EdgeDiff(a, b)   // "edge" — best for line/figure drawings
            };
            using var blur = new Mat();
            Cv2.GaussianBlur(diff, blur, new Size(g.GaussianBlur, g.GaussianBlur), 0);
            using var bin = new Mat();
            Cv2.Threshold(blur, bin, g.Threshold, 255, ThresholdTypes.Binary);
            using var kernel = Cv2.GetStructuringElement(MorphShapes.Rect, new Size(3, 3));
            using var opened = new Mat();
            Cv2.MorphologyEx(bin, opened, MorphTypes.Open, kernel);

            Cv2.FindContours(opened, out Point[][] contours, out _, RetrievalModes.External, ContourApproximationModes.ApproxSimple);
            var rects = new List<Rect>();
            foreach (var c in contours)
            {
                var r = Cv2.BoundingRect(c);
                if (r.Width * r.Height >= g.MinComponentArea) rects.Add(r);
            }
            return rects;
        }

        // Edge-based difference — the most robust method for CAD / isometric line
        // drawings. We extract Canny edges of both renders and compare them, so a
        // moved/resized/added line (even a hair-thin one) is caught, while grayscale
        // anti-aliasing and tonal variation are ignored.
        static Mat EdgeDiff(Mat a, Mat b)
        {
            using var ea = new Mat(); using var eb = new Mat();
            Cv2.Canny(a, ea, 40, 120);
            Cv2.Canny(b, eb, 40, 120);
            var d = new Mat();
            Cv2.Absdiff(ea, eb, d);
            return d;
        }

        static Mat AbsDiffMap(Mat a, Mat b, Pdf2dfConfig.GraphicCfg g)
        {
            using var d = new Mat();
            Cv2.Absdiff(a, b, d);
            var m = new Mat();
            Cv2.Threshold(d, m, g.Threshold, 255, ThresholdTypes.Binary);
            return m;
        }

        static Mat SsimDiff(Mat a, Mat b, int win = 11)
        {
            using var af = new Mat(); using var bf = new Mat();
            a.ConvertTo(af, MatType.CV_32F); b.ConvertTo(bf, MatType.CV_32F);
            float C1 = (float)((0.01 * 255) * (0.01 * 255));
            float C2 = (float)((0.03 * 255) * (0.03 * 255));
            using var muA = new Mat(); using var muB = new Mat();
            Cv2.GaussianBlur(af, muA, new Size(win, win), 1.5);
            Cv2.GaussianBlur(bf, muB, new Size(win, win), 1.5);
            using var af2 = new Mat(); using var bf2 = new Mat(); using var ab = new Mat();
            Cv2.Multiply(af, af, af2); Cv2.Multiply(bf, bf, bf2); Cv2.Multiply(af, bf, ab);
            using var muA2 = new Mat(); using var muB2 = new Mat(); using var muAB = new Mat();
            Cv2.GaussianBlur(af2, muA2, new Size(win, win), 1.5);
            Cv2.GaussianBlur(bf2, muB2, new Size(win, win), 1.5);
            Cv2.GaussianBlur(ab, muAB, new Size(win, win), 1.5);
            using var muA_sq = new Mat(); using var muB_sq = new Mat(); using var muAmuB = new Mat();
            Cv2.Multiply(muA, muA, muA_sq); Cv2.Multiply(muB, muB, muB_sq); Cv2.Multiply(muA, muB, muAmuB);
            using var sA2 = new Mat(); using var sB2 = new Mat(); using var sAB = new Mat();
            Cv2.Subtract(muA2, muA_sq, sA2); Cv2.Subtract(muB2, muB_sq, sB2); Cv2.Subtract(muAB, muAmuB, sAB);

            using var num = new Mat(); using var t1 = new Mat(); using var t2 = new Mat();
            Cv2.Multiply(muA, muB, t1); Cv2.Add(t1, t1, t1);          // 2*muA*muB
            Cv2.Add(t1, new Scalar(C1), num);                          // + C1
            Cv2.Multiply(sAB, new Scalar(2), t2); Cv2.Add(t2, new Scalar(C2), t2); // 2*sAB + C2
            Cv2.Multiply(num, t2, num);                                 // numerator

            using var den = new Mat(); using var d1m = new Mat(); using var d2m = new Mat();
            Cv2.Add(muA2, muB2, d1m); Cv2.Add(d1m, new Scalar(C1), d1m);
            Cv2.Add(sA2, sB2, d2m); Cv2.Add(d2m, new Scalar(C2), d2m);
            Cv2.Multiply(d1m, d2m, den);

            using var ssim = new Mat(); Cv2.Divide(num, den, ssim);
            using var one = new Mat(ssim.Size(), ssim.Type(), Scalar.All(1.0));
            using var ones = new Mat(); Cv2.Subtract(one, ssim, ones);
            var outp = new Mat(); ones.ConvertTo(outp, MatType.CV_8U, 255.0);
            return outp;
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

        // Annotate the target page with DIRECTIONAL markers:
        //   • graphic diffs  → rectangle outline (RED SOLID = new/changed, BLUE DASHED = old-only)
        //   • text/number    → CIRCLE (ellipse): RED SOLID = new-only/added; BLUE DASHED =
        //     removed / modified / moved. For any blue (old-side) item we ALSO render the
        //     disappeared text in LIGHT-BLUE ITALIC at its original position so the change
        //     is readable directly inside the emitted PDF.
        static void Annotate(Mat bgr, List<DiffItem> items, double scale, double x0, double y0, Pdf2dfConfig.AnnotationCfg ann)
        {
            foreach (var it in items)
            {
                int x = (int)Math.Round((it.Bbox.X - x0) * scale);
                int y = (int)Math.Round((it.Bbox.Y - y0) * scale);
                int w = (int)Math.Round(it.Bbox.Width * scale);
                int h = (int)Math.Round(it.Bbox.Height * scale);
                if (w <= 0 || h <= 0) continue;

                // old-only / changed-text → blue DASHED; new-only → RED SOLID
                bool oldSide = it.ChangeType == "removed" || it.ChangeType == "modified" || it.ChangeType == "moved";
                var (b, g, r) = ParseColor(oldSide ? ann.ColorOld : ann.ColorNew);
                var color = new Scalar(b, g, r);

                if (it.Type == "graphic")
                {
                    var rect = new Rect(x, y, Math.Max(2, w), Math.Max(2, h));
                    if (oldSide) DrawDashedRect(bgr, rect, color, ann.LineWidth);
                    else Cv2.Rectangle(bgr, rect, color, ann.LineWidth);
                }
                else // text / number → circle
                {
                    int cx = x + w / 2, cy = y + h / 2;
                    int ax = Math.Max(2, w / 2 + 5), ay = Math.Max(2, h / 2 + 5);
                    if (ann.CircleText)
                    {
                        if (oldSide) DrawDashedEllipse(bgr, cx, cy, ax, ay, color, ann.LineWidth);
                        else Cv2.Ellipse(bgr, new Point(cx, cy), new Size(ax, ay), 0, 0, 360, color, ann.LineWidth, LineTypes.AntiAlias);
                    }
                    else
                    {
                        var rect = new Rect(x, y, Math.Max(2, w), Math.Max(2, h));
                        if (oldSide) DrawDashedRect(bgr, rect, color, ann.LineWidth);
                        else Cv2.Rectangle(bgr, rect, color, ann.LineWidth);
                    }
                }

                // 把消失 / 被改掉的旧文字以浅蓝斜体显现在原位置（在生成的 PDF 中可直接看到）。
                // OpenCvSharp 的 PutText 仅支持 ASCII/Latin-1；遇到土耳其语变音或中文等字符会触发
                // "Cannot marshal" 异常，故此处做安全过滤 + 异常兜底，确保单个文字失败不会中断整页比对。
                if (oldSide && !string.IsNullOrWhiteSpace(it.OldText) && IsAsciiRenderable(it.OldText!))
                {
                    var (tb, tg, tr) = ParseColor(ann.ColorOldText);
                    var tcolor = new Scalar(tb, tg, tr);
                    double fs = Math.Max(0.55, h / 28.0);
                    int tx = x;
                    int ty = Math.Max((int)(12 * fs), y - (int)(5 * fs));
                    try
                    {
                        Cv2.PutText(bgr, it.OldText!, new Point(tx, ty),
                            HersheyFonts.HersheySimplex | HersheyFonts.Italic, fs, tcolor,
                            Math.Max(1, ann.LineWidth - 1), LineTypes.AntiAlias);
                    }
                    catch { /* 非 ASCII 字符无法用 Hershey 字体渲染，跳过（蓝虚线圈仍标记位置） */ }
                }
            }
        }

        static void DrawDashedRect(Mat bgr, Rect rect, Scalar color, int thickness)
        {
            int dash = 12, gap = 7;
            DrawDashedLine(bgr, rect.Left, rect.Top, rect.Right, rect.Top, color, thickness, dash, gap);
            DrawDashedLine(bgr, rect.Left, rect.Bottom, rect.Right, rect.Bottom, color, thickness, dash, gap);
            DrawDashedLine(bgr, rect.Left, rect.Top, rect.Left, rect.Bottom, color, thickness, dash, gap);
            DrawDashedLine(bgr, rect.Right, rect.Top, rect.Right, rect.Bottom, color, thickness, dash, gap);
        }

        static void DrawDashedLine(Mat bgr, int x0, int y0, int x1, int y1, Scalar color, int thickness, int dash, int gap)
        {
            double len = Math.Sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
            if (len == 0) return;
            int count = (int)(len / (dash + gap)) + 1;
            double dx = (x1 - x0) / len, dy = (y1 - y0) / len;
            for (int i = 0; i < count; i++)
            {
                double s0 = i * (dash + gap);
                double s1 = Math.Min(s0 + dash, len);
                int sx0 = (int)Math.Round(x0 + dx * s0), sy0 = (int)Math.Round(y0 + dy * s0);
                int sx1 = (int)Math.Round(x0 + dx * s1), sy1 = (int)Math.Round(y0 + dy * s1);
                Cv2.Line(bgr, new Point(sx0, sy0), new Point(sx1, sy1), color, thickness, LineTypes.AntiAlias);
            }
        }

        // Dashed ellipse used to CIRCLE old-side text/number diffs.
        static void DrawDashedEllipse(Mat bgr, int cx, int cy, int ax, int ay, Scalar color, int thickness)
        {
            int dash = 16, gap = 9;
            // Ramanujan perimeter approximation — good enough for even dash spacing.
            double perim = Math.PI * (3 * (ax + ay) - Math.Sqrt((3 * ax + ay) * (ax + 3 * ay)));
            double total = Math.Max(perim, 1);
            int n = (int)(total / (dash + gap)) + 1;
            for (int i = 0; i < n; i++)
            {
                double a0 = i * (dash + gap) / total * 360.0;
                double a1 = Math.Min(i * (dash + gap) + dash, total) / total * 360.0;
                Cv2.Ellipse(bgr, new Point(cx, cy), new Size(ax, ay), 0, a0, a1, color, thickness, LineTypes.AntiAlias);
            }
        }

        // Fraction of DARK pixels (ink / lines) inside a pixel-space rectangle. Used to
        // decide the DIRECTION of a graphic change: which side actually carries ink.
        static double MeanDark(Mat gray, Rect r)
        {
            if (gray.Empty() || r.Width <= 0 || r.Height <= 0) return 0;
            using var roi = new Mat(gray, r);
            using var bin = new Mat();
            Cv2.Threshold(roi, bin, 128, 1, ThresholdTypes.BinaryInv);  // dark → 1
            return Cv2.Mean(bin).Val0;                                  // average = dark fraction
        }

        static byte[] EncodeJpeg(Mat bgr, int quality)
        {
            Cv2.ImEncode(".jpg", bgr, out byte[] buf, new ImageEncodingParam(ImwriteFlags.JpegQuality, quality));
            return buf;
        }

        static (byte b, byte g, byte r) ParseColor(string hex)
        {
            hex = (hex ?? "#FF0000").TrimStart('#');
            if (hex.Length == 6)
                return (byte.Parse(hex.Substring(4, 2), System.Globalization.NumberStyles.HexNumber),
                        byte.Parse(hex.Substring(2, 2), System.Globalization.NumberStyles.HexNumber),
                        byte.Parse(hex.Substring(0, 2), System.Globalization.NumberStyles.HexNumber));
            return (0, 0, 255);
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
