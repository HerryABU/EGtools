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

            var pages = req.Pages;

            try
            {
                using var ctx = new MuPDFContext(512u * 1024 * 1024);
                var (baseCtxPath, baseDoc) = Open(ctx, req.Baseline);
                var (tgtCtxPath, tgtDoc) = Open(ctx, req.Target);
                try
                {
                    int nBase = baseDoc.Pages.Count;
                    int nTgt = tgtDoc.Pages.Count;
                    int n = pages == null ? Math.Min(nBase, nTgt) : pages.Max() + 1;
                    n = Math.Min(n, Math.Min(nBase, nTgt));
                    if (pages != null) pages = pages.Where(p => p >= 0 && p < n).ToList();
                    else pages = Enumerable.Range(0, n).ToList();

                    var resp = new DiffResponse { Output = outPath };
                    var pageImages = new List<(byte[] data, int w, int h)>();

                    foreach (int pno in pages)
                    {
                        L($"  page {pno + 1}: rendering + diff …");
                        var baseB = RenderBgr(baseDoc, pno, scale, out int bw, out int bh, out var bBounds);
                        var tgtB = RenderBgr(tgtDoc, pno, scale, out int tw, out int th, out var tBounds);
                        using var baseGray = new Mat(); Cv2.CvtColor(baseB, baseGray, ColorConversionCodes.BGR2GRAY);
                        using var tgtGray = new Mat(); Cv2.CvtColor(tgtB, tgtGray, ColorConversionCodes.BGR2GRAY);

                        using var targetST = tgtDoc.GetStructuredTextPage(pno, false, StructuredTextFlags.None);
                        using var baseST = baseDoc.GetStructuredTextPage(pno, false, StructuredTextFlags.None);

                        // ----- text mask (UNION of both pages' text quads → pixels) -----
                        // Masking the union guarantees no text — even changed glyphs —
                        // leaks into the graphic comparison (per spec: text must not be
                        // counted as a figure change).
                        using var textMask = cfg.GraphicDiff.ExcludeText
                            ? BuildTextMask(new[] { baseST, targetST }, tw, th, scale, tBounds.X0, tBounds.Y0)
                            : null;

                        // ----- region restriction -----
                        var pageRegions = (req.Regions ?? new List<ReqRegion>())
                            .Where(r => r.Page == pno).Select(r => r.Bbox).ToList();
                        using var regionMask = pageRegions.Count > 0
                            ? BuildRegionMask(pageRegions, tw, th, scale, tBounds.X0, tBounds.Y0)
                            : null;

                        // ----- registration: align baseline to target -----
                        Mat baseGrayAligned;
                        using (var H = cfg.Registration.Method == "orb"
                                   ? Register(baseGray, tgtGray, cfg.Registration, textMask)
                                   : null)
                        {
                            if (H != null)
                            {
                                baseGrayAligned = new Mat(); Cv2.WarpPerspective(baseGray, baseGrayAligned, H, new Size(tw, th));
                            }
                            else
                            {
                                baseGrayAligned = baseGray.Clone();
                            }
                        }

                        var diffs = new List<DiffItem>();

                        // ----- extract words once (used by both steps) -----
                        var baseWords = TextGeometry.GetWords(baseST, bBounds.Y1 - bBounds.Y0, 0.18);
                        var tgtWords = TextGeometry.GetWords(targetST, tBounds.Y1 - tBounds.Y0, 0.18);

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
                                .Concat(TextWordRectsPx(baseWords, scale, bBounds.X0, bBounds.Y0, 6))
                                .ToList();
                            foreach (var r in rects)
                            {
                                double x = r.X / scale + tBounds.X0;
                                double y = r.Y / scale + tBounds.Y0;
                                if (regionMask != null && !InsideAnyRegion(x, y, r.Width / scale, r.Height / scale, pageRegions, tBounds.X0, tBounds.Y0))
                                    continue;
                                if (textPx.Any(tr => tr.X <= r.X && tr.Y <= r.Y && tr.Right >= r.Right && tr.Bottom >= r.Bottom))
                                    continue;
                                diffs.Add(new DiffItem
                                {
                                    Page = pno,
                                    Type = "graphic",
                                    Bbox = new Bbox { X = Round(x), Y = Round(y), Width = Round(r.Width / scale), Height = Round(r.Height / scale) },
                                    Confidence = 0.95
                                });
                            }
                        }

                        // ----- STEP 2: TEXT diff (independent of the pixel stage) -----
                        if (cfg.TextDiff.Enabled)
                        {
                            var textItems = TextDiff(baseWords, tgtWords, cfg.TextDiff, pno, pageRegions, bBounds, tBounds, scale);
                            diffs.AddRange(textItems);
                        }

                        resp.Summary.GraphicDiffs += diffs.Count(d => d.Type == "graphic");
                        resp.Summary.TextDiffs += diffs.Count(d => d.Type == "text");
                        resp.Diffs.AddRange(diffs);

                        // ----- annotate target with RED markers -----
                        Annotate(tgtB, diffs, scale, tBounds.X0, tBounds.Y0, cfg.Annotation);
                        pageImages.Add((EncodeJpeg(tgtB, 92), tw, th));
                        resp.Summary.TotalPages++;
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
            // Estimate pixel size from bounds; MuPDF rounds/ceil's independently, so
            // we ALWAYS derive the exact w/h from the returned byte length to avoid
            // any buffer overrun when copying into the OpenCV Mat.
            int wEst = Math.Max(1, (int)Math.Round((bounds.X1 - bounds.X0) * scale));
            int hEst = Math.Max(1, (int)Math.Round((bounds.Y1 - bounds.Y0) * scale));
            byte[] bytes = doc.Render(pno, scale, PixelFormats.RGBA, false);
            int totalPx = bytes.Length / 4;
            if (totalPx % wEst == 0) { w = wEst; h = totalPx / wEst; }
            else if (totalPx % hEst == 0) { h = hEst; w = totalPx / hEst; }
            else { w = wEst; h = hEst; }   // fallback; should not happen for whole-page render
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

        static Mat BuildTextMask(IEnumerable<MuPDFStructuredTextPage> sts, int w, int h, double scale, double x0, double y0)
        {
            var mask = new Mat(h, w, MatType.CV_8UC1, Scalar.All(0));
            var polys = new List<Point[]>();
            foreach (var st in sts)
            foreach (var block in st)
            {
                if (block.Type != MuPDFStructuredTextBlock.Types.Text) continue;
                foreach (var line in block)
                    foreach (var c in line.Characters)
                    {
                        var q = c.BoundingQuad;
                        polys.Add(new[]
                        {
                            new Point((int)Math.Round((q.UpperLeft.X - x0) * scale), (int)Math.Round((q.UpperLeft.Y - y0) * scale)),
                            new Point((int)Math.Round((q.UpperRight.X - x0) * scale), (int)Math.Round((q.UpperRight.Y - y0) * scale)),
                            new Point((int)Math.Round((q.LowerRight.X - x0) * scale), (int)Math.Round((q.LowerRight.Y - y0) * scale)),
                            new Point((int)Math.Round((q.LowerLeft.X - x0) * scale), (int)Math.Round((q.LowerLeft.Y - y0) * scale)),
                        });
                    }
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

        static Mat? Register(Mat baseGray, Mat targetGray, Pdf2dfConfig.RegistrationCfg reg, Mat? textMask)
        {
            try
            {
                using var bClean = baseGray.Clone();
                using var tClean = targetGray.Clone();
                if (textMask != null) { bClean.SetTo(Scalar.All(255), textMask); tClean.SetTo(Scalar.All(255), textMask); }
                var orb = ORB.Create(reg.MaxFeatures);
                Mat d1 = new Mat();
                Mat d2 = new Mat();
                try
                {
                    KeyPoint[] k1, k2;
                    orb.DetectAndCompute(bClean, null, out k1, d1);
                    orb.DetectAndCompute(tClean, null, out k2, d2);
                    if (d1.Empty() || d2.Empty() || k1.Length < 4 || k2.Length < 4) return null;
                    var bf = new BFMatcher(NormTypes.Hamming);
                    DMatch[][] matches = bf.KnnMatch(d1, d2, 2);
                    var good = new List<DMatch>();
                    foreach (var m in matches)
                        if (m.Length == 2 && m[0].Distance < reg.MatchRatio * m[1].Distance) good.Add(m[0]);
                    if (good.Count < 10) return null;
                    var src = good.Select(m => k1[m.QueryIdx].Pt).ToArray();
                    var dst = good.Select(m => k2[m.TrainIdx].Pt).ToArray();
                    return Cv2.FindHomography(InputArray.Create(src), InputArray.Create(dst), HomographyMethods.Ransac, reg.RansacThreshold);
                }
                finally { d1.Dispose(); d2.Dispose(); }
            }
            catch { return null; }
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

        static List<DiffItem> TextDiff(List<TextGeometry.Word> baseW, List<TextGeometry.Word> tgtW,
            Pdf2dfConfig.TextCfg t, int page, List<Bbox> regions, Rectangle bBounds, Rectangle tBounds, double scale)
        {
            string norm(string s) => TextGeometry.Norm(s);
            var items = new List<DiffItem>();
            var consumed = new bool[baseW.Count];
            var inside = regions.Count == 0 ? (Func<double, double, bool>)((x, y) => true)
                                           : (x, y) => regions.Any(r => x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height);

            for (int i = 0; i < tgtW.Count; i++)
            {
                var tw = tgtW[i];
                if (!inside(tw.x0, tw.yc)) continue;
                double bestD = double.MaxValue; int bestJ = -1;
                for (int j = 0; j < baseW.Count; j++)
                {
                    if (consumed[j]) continue;
                    var bw = baseW[j];
                    double dx = tw.xc - bw.xc, dy = tw.yc - bw.yc;
                    if (Math.Abs(dx) > t.XTolerance || Math.Abs(dy) > t.YTolerance) continue;
                    double d = dx * dx + dy * dy;
                    if (d < bestD) { bestD = d; bestJ = j; }
                }
                if (bestJ < 0)
                {
                    items.Add(Mk(page, "text", tw, "added", oldText: null, newText: tw.text));
                }
                else
                {
                    consumed[bestJ] = true;
                    var bw = baseW[bestJ];
                    if (norm(bw.text) == norm(tw.text))
                    {
                        double move = Math.Max(Math.Abs(tw.xc - bw.xc), Math.Abs(tw.yc - bw.yc));
                        if (move > t.MoveTolerance)
                            items.Add(Mk(page, "text", tw, "moved", oldText: bw.text, newText: tw.text));
                    }
                    else
                    {
                        items.Add(Mk(page, "text", tw, "modified", oldText: bw.text, newText: tw.text));
                    }
                }
            }
            for (int j = 0; j < baseW.Count; j++)
            {
                if (consumed[j]) continue;
                var bw = baseW[j];
                if (!inside(bw.x0, bw.yc)) continue;
                items.Add(Mk(page, "text", bw, "removed", oldText: bw.text, newText: null, baseBounds: bBounds, targetBounds: tBounds, scale));
            }
            return items;
        }

        static DiffItem Mk(int page, string type, TextGeometry.Word w, string change, string? oldText, string? newText,
            Rectangle? baseBounds = null, Rectangle? targetBounds = null, double scale = 1)
        {
            var it = new DiffItem
            {
                Page = page,
                Type = type,
                ChangeType = change,
                OldText = oldText,
                NewText = newText,
                Confidence = 0.9,
            };
            // bbox in TARGET page pt (for removed, approximate via baseline→target px).
            if (baseBounds.HasValue && targetBounds.HasValue)
            {
                double bx = (w.x0 - baseBounds.Value.X0) * scale;
                double by = (w.y0 - baseBounds.Value.Y0) * scale;
                double bw = (w.x1 - w.x0) * scale;
                double bh = (w.y1 - w.y0) * scale;
                it.Bbox = new Bbox { X = Round(bx + targetBounds.Value.X0), Y = Round(by + targetBounds.Value.Y0), Width = Round(bw), Height = Round(bh) };
            }
            else
            {
                it.Bbox = new Bbox { X = Round(w.x0), Y = Round(w.y0), Width = Round(w.x1 - w.x0), Height = Round(w.y1 - w.y0) };
            }
            return it;
        }

        static void Annotate(Mat bgr, List<DiffItem> items, double scale, double x0, double y0, Pdf2dfConfig.AnnotationCfg ann)
        {
            var (b, g, r) = ParseColor(ann.Color);
            var color = new Scalar(b, g, r);
            foreach (var it in items)
            {
                int x = (int)Math.Round((it.Bbox.X - x0) * scale);
                int y = (int)Math.Round((it.Bbox.Y - y0) * scale);
                int w = (int)Math.Round(it.Bbox.Width * scale);
                int h = (int)Math.Round(it.Bbox.Height * scale);
                var rect = new Rect(x, y, Math.Max(2, w), Math.Max(2, h));
                if (it.Type == "text" && ann.Fill)
                {
                    using var overlay = bgr.Clone();
                    Cv2.Rectangle(overlay, rect, color, -1);
                    Cv2.AddWeighted(overlay, ann.FillAlpha / 255.0, bgr, 1 - ann.FillAlpha / 255.0, 0, bgr);
                }
                Cv2.Rectangle(bgr, rect, color, ann.LineWidth);
            }
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

        static double Round(double v) => Math.Round(v, 1);
    }
}
