using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.RegularExpressions;
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

                    // 矢量线画路径用的页面内容流来源（整份只开一次；只有走 vector 方法时才需要）。
                    using var baseSrc = new VectorPageSource(baseCtxPath ?? req.Baseline);
                    using var tgtSrc = new VectorPageSource(tgtCtxPath ?? req.Target);
                    bool useVectorGraphic = cfg.GraphicDiff.Enabled
                        && string.Equals(cfg.GraphicDiff.GraphicMethod, "vector", StringComparison.OrdinalIgnoreCase);

                    //⚠️ 配对必须按【线画本身】，不能用"图纸号"：本项目标题栏每页都印同一个项目号
                    // (408-101-051)，按图纸号分组会退化成按页序配对；新版插入一页后其后全体错位，
                    // 逐页比对两页不同图纸 → 上千条假差异（实测错位页命中率仅 ~60%、上千 runs，
                    // 而真实对应页是 100% / 0 runs）。这是"误判率极高"的头号原因。
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
                        // ① 图号优先配对（同图号才比；图号只在一侧的页直接判新增/删除，不比对）。
                        var (dn, dnAdd, dnRem, bNoNo, tNoNo) = MatchPagesByDrawingNo(
                            baseDoc, nBase, tgtDoc, nTgt, cfg.SheetNoRegionX, cfg.SheetNoRegionY, cfg.SheetNoMaxDistinct, L);
                        pagePairs = dn.Select(x => (x.b, x.t, x.no)).ToList();
                        addedPages = dnAdd.ToList();
                        removedPages = dnRem.ToList();

                        // ② 无图号的页（封面/总图）：用线画指纹在其中兜底 1:1。
                        //    仅当两版都还剩无图号页、且走矢量路径时才做；否则按页序。
                        if (bNoNo.Count > 0 && tNoNo.Count > 0)
                        {
                            if (useVectorGraphic)
                            {
                                var bFp = bNoNo.Select(i => VectorGeomDiff.Fingerprint(VectorLinework.Extract(baseSrc, i))).ToList();
                                var tFp = tNoNo.Select(j => VectorGeomDiff.Fingerprint(VectorLinework.Extract(tgtSrc, j))).ToList();
                                var (fp, fAdd, fRem) = VectorGeomDiff.MatchPagesByLinework(bFp, tFp, cfg.GraphicDiff.PageMatchMinSim, L);
                                foreach (var x in fp) pagePairs.Add((bNoNo[x.b], tNoNo[x.t], "(指纹)"));
                                foreach (var j in fAdd) addedPages.Add((tNoNo[j], "(指纹)"));
                                foreach (var i in fRem) removedPages.Add((bNoNo[i], "(指纹)"));
                            }
                            else
                            {
                                int n = Math.Min(bNoNo.Count, tNoNo.Count);
                                for (int k = 0; k < n; k++) pagePairs.Add((bNoNo[k], tNoNo[k], "(无图号)"));
                                for (int k = n; k < tNoNo.Count; k++) addedPages.Add((tNoNo[k], "(无图号)"));
                                for (int k = n; k < bNoNo.Count; k++) removedPages.Add((bNoNo[k], "(无图号)"));
                            }
                        }
                        else
                        {
                            foreach (var i in bNoNo) removedPages.Add((i, "(无图号·目标缺失)"));
                            foreach (var j in tNoNo) addedPages.Add((j, "(无图号·基准缺失)"));
                        }
                        pagePairs.Sort((a, b) => a.basePno.CompareTo(b.basePno));
                        addedPages.Sort((a, b) => a.page.CompareTo(b.page));
                        removedPages.Sort((a, b) => a.page.CompareTo(b.page));
                        L($"  最终配对: 比对 {pagePairs.Count} 对, 新增 {addedPages.Count} 页, 删除 {removedPages.Count} 页");
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
                        // ⚠️ 必须释放：dpi 200 下这两张彩色大图各约 360MB，不释放会随页数
                        // 线性累积 → 全本文档跑到几十页就 OOM 崩溃(用户报的"崩溃很厉害")。
                        using var baseB = RenderBgr(baseDoc, basePno, scale, out int bw, out int bh, out var bBounds);
                        using var tgtB = RenderBgr(tgtDoc, tgtPno, scale, out int tw, out int th, out var tBounds);
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
                BasePage1 = basePno + 1,          // 抽取"删除文字"要按基准页自己的页号查基准文档
                Wpt = (float)(tBounds.X1 - tBounds.X0),
                Hpt = (float)(tBounds.Y1 - tBounds.Y0),
            };

                        // ----- STEP 1: GRAPHIC diff -----
                        // 两条路径：
                        //   vector (默认) —— 解析内容流取线画 + 几何标定 + 共线覆盖匹配，
                        //                   纯矢量、不做位图渲染，误报率低；
                        //   pixel         —— 旧的两版渲染成位图后逐像素/逐轮廓比对（回退路径）。
                        bool vecMethod = cfg.GraphicDiff.Enabled
                            && string.Equals(cfg.GraphicDiff.GraphicMethod, "vector", StringComparison.OrdinalIgnoreCase);
                        if (vecMethod)
                        {
                            // ⚠️ 必须用【页面级】重载：本项目大量页面把整套线画塞在 Form XObject 里，
                            // 页面内容流只有几十字节（`q /Meta0 Do Q`）。用纯 content 重载会得到空线画。
                            var bLine = VectorLinework.Extract(baseSrc, basePno);
                            var tLine = VectorLinework.Extract(tgtSrc, tgtPno);
                            var vfilter = new VectorGeomDiff.FilterCfg
                            {
                                MinRunLengthPt = cfg.GraphicDiff.MinRunLengthPt,
                                ClusterRadiusPt = cfg.GraphicDiff.ClusterRadiusPt,
                                MinClusterLengthPt = cfg.GraphicDiff.MinClusterLengthPt,
                                MinClusterRuns = cfg.GraphicDiff.MinClusterRuns,
                            };
                            var vr = VectorGeomDiff.Compare(bLine, tLine,
                                cfg.GraphicDiff.VectorTolPt, cfg.GraphicDiff.VectorAngleTolDeg, L,
                                filter: vfilter, changeRadius: cfg.GraphicDiff.ChangeRadiusPt);
                            pageAnno.Strokes.AddRange(vr.Runs);
                            int rotT = tgtSrc.Rotation(tgtPno);
                            float WcT = tBounds.X1 - tBounds.X0, HcT = tBounds.Y1 - tBounds.Y0;
                            float oxT = tBounds.X0, oyT = tBounds.Y0;
                            bool byCluster = !string.Equals(cfg.GraphicDiff.ReportMode, "run", StringComparison.OrdinalIgnoreCase);
                            // 报告条目：按区域(cluster)或逐条折线(run)。大改页逐条可达十几万条，
                            // 让 JSON 与前端都不可用，故默认按区域。
                            var boxes = new List<(double x0, double y0, double x1, double y1, string dir)>();
                            if (byCluster)
                                foreach (var c in vr.Clusters)
                                    boxes.Add((c.X0, c.Y0, c.X1, c.Y1, c.Dir));
                            else
                                foreach (var run in vr.Runs)
                                {
                                    if (run.Pts.Count == 0) continue;
                                    double a0 = double.MaxValue, b0 = double.MaxValue, a1 = double.MinValue, b1 = double.MinValue;
                                    for (int k = 0; k + 1 < run.Pts.Count; k += 2)
                                    {
                                        a0 = Math.Min(a0, run.Pts[k]); a1 = Math.Max(a1, run.Pts[k]);
                                        b0 = Math.Min(b0, run.Pts[k + 1]); b1 = Math.Max(b1, run.Pts[k + 1]);
                                    }
                                    boxes.Add((a0, b0, a1, b1, run.Dir));
                                }
                            foreach (var (rminx, rminy, rmaxx, rmaxy, rdir) in boxes)
                            {
                                // 转成 DISPLAY 帧(y-down) 供报告统一口径
                                PdfVectorDiff.ContentToDisplay(rotT, WcT, HcT, oxT, oyT, rminx, rminy, out double dx0, out double dy0);
                                PdfVectorDiff.ContentToDisplay(rotT, WcT, HcT, oxT, oyT, rmaxx, rmaxy, out double dx1, out double dy1);
                                diffs.Add(new DiffItem
                                {
                                    Page = tgtPno,
                                    Type = "graphic",
                                    ChangeType = rdir == "changed" ? "changed" : rdir,
                                    Bbox = new Bbox
                                    {
                                        X = Round(Math.Min(dx0, dx1)), Y = Round(Math.Min(dy0, dy1)),
                                        Width = Round(Math.Abs(dx1 - dx0)), Height = Round(Math.Abs(dy1 - dy0)),
                                    },
                                    Confidence = 0.98
                                });
                            }
                            L($"  [vec] 线画差异: 变更折线 {vr.Runs.Count} 条 " +
                              $"(新增 {vr.Runs.Count(r => r.Dir == "added")} / 删除 {vr.Runs.Count(r => r.Dir == "removed")}" +
                              $" / 变更 {vr.Runs.Count(r => r.Dir == "changed")})" +
                              $"，差异区域 {vr.Clusters.Count} 个（报告粒度={(byCluster ? "区域" : "逐条")}）");
                        }
                        else if (cfg.GraphicDiff.Enabled)
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
                            // 填充外扩半径(px) = 配置 HaloPx × 缩放。HaloPx=0 → 精准贴合变更
                            // 形状(真描红)，>0 → 填充区向外略微扩张以便极细线也可见。裁剪边距
                            // (2+halo) 必须 >= 扩张半径，否则扩张被裁边界截断而无效。
                            int bandPx = (int)Math.Round(cfg.Annotation.HaloPx * scale);
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
                                // so we know whether the change is new-only / old-only / changed.
                                string dir = gc.Dir;
                                // Store the ACTUAL changed-shape contour as a VECTOR polygon
                                // (target pixel frame, y-down). removed → 红虚线填充；added → 红
                                // 实线填充；modified(两版都有但形状不同) → 绿实线填充(变更)。
                                if (dir == "removed") pageAnno.RemovedContours.Add(gc.Pts);
                                else if (dir == "modified") pageAnno.ChangedContours.Add(gc.Pts);
                                else pageAnno.AddedContours.Add(gc.Pts);
                                diffs.Add(new DiffItem
                                {
                                    Page = tgtPno,
                                    Type = "graphic",
                                    ChangeType = dir == "modified" ? "changed" : dir,
                                    Bbox = new Bbox { X = Round(x), Y = Round(y), Width = Round(rw / scale), Height = Round(rh / scale) },
                                    Confidence = 0.95
                                });
                            }
                        }

                        // ----- STEP 2: TEXT diff (independent of the pixel stage) -----
                        if (cfg.TextDiff.Enabled)
                        {
                            var textItems = TextDiff(baseWordsAligned, tgtWords, cfg.TextDiff, tgtPno, pageRegions, tBounds, scale,
                                string.Equals(cfg.Annotation.TextSerialMatchMode, "strict", StringComparison.OrdinalIgnoreCase));
                            diffs.AddRange(textItems);
                            pageAnno.TextItems.AddRange(textItems);
                        }

                        textMask?.Dispose();      // 单页文本掩膜(约 120MB)必须随页释放，否则累积 OOM
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
                    PdfVectorDiff.Write(outPath, tgtCtxPath ?? req.Target, req.Baseline, pageAnnos, scale, cfg.Annotation);
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
            // 快速路径：整页不透明(alpha 全 255，绝大多数工程图)时直接去掉 alpha 通道。
            // 避免构造多张 CV_32FC3 浮点大图(dpi200 下每张约 1.4GB)，这是全本文档
            // 跑到几十页就 OOM 崩溃的主因之一。
            using (var aChk = new Mat())
            {
                Cv2.ExtractChannel(rgba, aChk, 3);
                Cv2.MinMaxLoc(aChk, out double minA, out _);
                if (minA >= 255.0)
                {
                    Cv2.CvtColor(rgba, bgr, ColorConversionCodes.RGBA2BGR);
                    rgba.Dispose();
                    return bgr;
                }
            }
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
        // ------------------------------------------------------------------
        //  Text diff keyed by 序号 (serial number) — matches the user's drawings
        //  where every annotation (BOM rows, NOTE legend, …) carries a stable
        //  serial that is repositioned but content-stable. We therefore:
        //    1) group each page's words into ITEMS, each item headed by a 序号;
        //    2) pair items across baseline/target by 序号 VALUE (position-independent,
        //       so a mere shift of an item never flags a change);
        //    3) compare the item CONTENT → 变更(changed, green) when the serial is the
        //       same but the text differs; 新增(added, red) / 删除(removed, red) when a
        //       serial exists on only one side. Free text without a serial falls back to
        //       position matching.
        // ------------------------------------------------------------------
        static readonly Regex RePureNum = new(@"^\d{1,5}$", RegexOptions.Compiled);
        static readonly Regex ReLabelNum = new(@"^(NOTE|ITEM|NO|N|REF|REV)\.?$", RegexOptions.IgnoreCase | RegexOptions.Compiled);

        class TextItem
        {
            public string? Serial;                  // 序号(匹配键)，可为 null(自由文本)
            public string Content = "";              // 该条目的拼接内容(原始)
            public List<TextGeometry.Word> Words = new();
            // 原始行结构：每行记录 (文字, 行左基线x, 基线y(display y-down), 渲染高度, 原始字号,
            //   DISPLAY 帧真实墨水包围盒 x0/y0/x1/y1)。包围盒由原始 Word 几何得出，供内容流
            // 重着色/抽取时按几何精确匹配 Tj/TJ 文本运行(避免坐标/字号猜测)。
            public List<(string text, double x0, double baselineY, double height, double size, double bx0, double by0, double bx1, double by1)> Lines = new();
            // 与 Lines 一一对应：每行的逐词包围盒（DISPLAY 帧, y-down）。textBoxMode=word 时
            // 用它出差异框 → 只改真正变动的词，不牵连同行其它文字。
            public List<List<TextWord>> LineWords = new();
            public double X0, Y0, X1, Y1;           // 合并包围盒(目标 pt 帧)
            public double LastYc;                    // 末行中心 y(续行判断)
            public double Xc => (X0 + X1) / 2;
            public double Yc => (Y0 + Y1) / 2;
        }

        // Group words into lines (by baseline), then into items. A line that begins with a
        // serial token starts a new item; subsequent non-serial lines that hug the item's
        // left edge / line spacing are absorbed as continuation (wrapped notes).
        // 把页面词流切成"有序号的条目"。序号优先取管道/管线号(408.101.P76 这类点分标识符)，
        // 其次纯数字(材料表 NO 列)、NOTE n / ITEM n。
        // ⚠️ 关键：一行(row, 按 yc 聚类)里可能出现多个独立标签(如两个管段号并排)，必须在
        // 每个"序号起点"处切分，否则并排的两个编号会被并成一条、导致与另一版对不上 → 误报。
        static List<TextItem> Itemize(List<TextGeometry.Word> words, bool strictSerial)
        {
            var sorted = words.Where(w => !string.IsNullOrWhiteSpace(w.text))
                              .OrderBy(w => w.yc).ThenBy(w => w.x0).ToList();
            var rows = new List<List<TextGeometry.Word>>();
            foreach (var w in sorted)
            {
                var last = rows.Count > 0 ? rows[rows.Count - 1] : null;
                double lh = w.y1 - w.y0;
                if (last != null && Math.Abs(w.yc - last.Average(x => x.yc)) <= Math.Max(4.0, lh))
                    last.Add(w);
                else rows.Add(new List<TextGeometry.Word> { w });
            }
            var items = new List<TextItem>();
            TextItem? curItem = null;
            List<TextGeometry.Word>? curWords = null;
            void Flush()
            {
                if (curItem != null && curWords != null && curWords.Count > 0)
                { AddWords(curItem, curWords); items.Add(curItem); }
                curItem = null; curWords = null;
            }
            foreach (var row in rows)
            {
                for (int i = 0; i < row.Count; i++)
                {
                    var w = row[i];
                    var wt = (w.text ?? "").Trim();
                    string? ser = SerialOf(wt, row, i, strictSerial);
                    if (ser != null)
                    {
                        Flush();
                        curWords = new List<TextGeometry.Word> { w };
                        curItem = new TextItem { Serial = ser, Content = wt };
                    }
                    else if (curItem == null)
                    {
                        curWords = new List<TextGeometry.Word> { w };
                        curItem = new TextItem { Serial = null, Content = wt };
                    }
                    else
                    {
                        curWords!.Add(w);
                        curItem.Content = string.IsNullOrEmpty(curItem.Content) ? wt : curItem.Content + " " + wt;
                    }
                }
                Flush();
            }
            return items;
        }

        // 序号识别：管道/管线号 > 纯数字 > NOTE n / ITEM n。均为原样保留(不剥离后缀)。
        static string? SerialOf(string wt, List<TextGeometry.Word> row, int i, bool strict)
        {
            if (TextGeometry.LooksLikeLineNo(wt)) return wt.ToUpperInvariant();  // 管段号 408.101.P76
            // strict：不把"行首纯数字"当匹配键。行首数字并非唯一标识（一张表多行都以小数字开头），
            // 两版行号含义/行序不同时会把无关两行配成一对 → 假 changed。
            if (!strict && i == 0 && RePureNum.IsMatch(wt)) return wt;           // 纯数字序号(材料表 NO 列)
            if (i == 0 && ReLabelNum.IsMatch(wt) && i + 1 < row.Count
                && RePureNum.IsMatch((row[i + 1].text ?? "").Trim()))
                return wt + " " + (row[i + 1].text ?? "").Trim();               // NOTE 1 / ITEM 2 …
            return null;
        }

        static bool IsContinuation(List<TextGeometry.Word> line, TextItem cur)
        {
            double lineH = line.Average(w => w.y1 - w.y0);
            double gap = line.Min(w => w.yc) - cur.LastYc;
            if (gap < -lineH * 0.5 || gap > lineH * 1.4) return false;          // 必须紧接下一行
            if (Math.Abs(line.Min(w => w.x0) - cur.X0) > Math.Max(20.0, lineH * 2)) return false; // 左对齐
            return true;
        }

        static void AddWords(TextItem it, List<TextGeometry.Word> line)
        {
            double bx0 = double.MaxValue, by0 = double.MaxValue, bx1 = double.MinValue, by1 = double.MinValue;
            double baselineSum = 0, sizeSum = 0; int n = 0;
            foreach (var w in line)
            {
                bool first = it.Words.Count == 0;
                if (first) { it.X0 = w.x0; it.Y0 = w.y0; }
                it.X0 = Math.Min(it.X0, w.x0); it.Y0 = Math.Min(it.Y0, w.y0);
                it.X1 = Math.Max(it.X1, w.x1); it.Y1 = Math.Max(it.Y1, w.y1);
                it.Words.Add(w);
                bx0 = Math.Min(bx0, w.x0);
                by0 = Math.Min(by0, w.y0); bx1 = Math.Max(bx1, w.x1); by1 = Math.Max(by1, w.y1);
                baselineSum += w.y1;   // y1 = 基线(display y-down, 字底部)
                sizeSum += w.size;
                n++;
            }
            if (n > 0)
            {
                // 该行：左基线 x = min(x0)，基线 y = 平均 y1，渲染高度 = 行顶到基线(by1-by0)，
                // 原始字号 = 平均 size；DISPLAY 帧真实墨水包围盒 = 该行所有字并集。
                double height = Math.Max(1.0, by1 - by0);
                it.Lines.Add((JoinContent(line), bx0, baselineSum / n, height, sizeSum / n,
                              bx0, by0, bx1, by1));
                it.LineWords.Add(line.Select(w => new TextWord
                {
                    T = w.text, X0 = w.x0, Y0 = w.y0, X1 = w.x1, Y1 = w.y1,
                }).ToList());
            }
            it.LastYc = line.Average(w => w.yc);
        }

        static string JoinContent(List<TextGeometry.Word> line)
            => string.Join(" ", line.Select(w => w.text.Trim()));

        // 词集合签名：忽略词序与空白，仅看"由哪些词组成"。两版导出顺序不同导致
        // 的词序差异(同一组词)会被判定为相同内容，避免伪差异。
        static string TokenSet(string s)
            => string.Join(" ", (s ?? "").Split(new[] { ' ', '\t', '\n' }, StringSplitOptions.RemoveEmptyEntries).OrderBy(x => x, StringComparer.OrdinalIgnoreCase));

        static Bbox BboxOf(TextItem it)
            => new Bbox { X = Round(it.X0), Y = Round(it.Y0), Width = Round(it.X1 - it.X0), Height = Round(it.Y1 - it.Y0) };

        // 把内部 TextItem 的原始行结构转成契约 TextLine 列表（含 x / 基线 / 字号），
        // 供 PdfVectorDiff 逐行按原字号、原位置重绘，确保"字号 + 换行"与原文一致。
        static List<TextLine>? ToLines(TextItem? it)
        {
            if (it == null || it.Lines.Count == 0) return null;
            var out_ = new List<TextLine>();
            for (int i = 0; i < it.Lines.Count; i++)
            {
                var ln = it.Lines[i];
                out_.Add(new TextLine
                {
                    Text = ln.text, X = ln.x0, Baseline = ln.baselineY, Height = ln.height, Size = ln.size,
                    X0 = ln.bx0, Y0 = ln.by0, X1 = ln.bx1, Y1 = ln.by1,
                    Words = i < it.LineWords.Count ? it.LineWords[i] : null,
                });
            }
            return out_;
        }

        static DiffItem MkItem(int page, TextItem it, string change)
            => new DiffItem
            {
                Page = page,
                Type = "text",
                ChangeType = change,
                OldText = change == "added" ? null : it.Content,
                NewText = change == "removed" ? null : it.Content,
                OldLines = change == "added" ? null : ToLines(it),
                NewLines = change == "removed" ? null : ToLines(it),
                Confidence = 0.9,
                Bbox = BboxOf(it),
            };

        static List<DiffItem> TextDiff(List<TextGeometry.Word> wordsA, List<TextGeometry.Word> wordsB,
            Pdf2dfConfig.TextCfg t, int page, List<Bbox> regions, Rectangle tBounds, double scale,
            bool strictSerial = false)
        {
            // 只保留"有序号"的条目参与比对（管道号 408.101.P## / NOTE n / 材料表 NO 等）。
            // 无序号的自有文本(标题、图框、英文说明)在两版导出时分词/换行极易不一致，
            // 逐条比对会产生海量伪差异并把整页画满标记 —— 用户明确要求：先按序号配对，
            // 再看序号对应内容是否相同，无序号文本不报差异。
            var IA = Itemize(wordsA.Where(w => !string.IsNullOrWhiteSpace(w.text)).ToList(), strictSerial)
                     .Where(it => it.Serial != null).ToList();
            var IB = Itemize(wordsB.Where(w => !string.IsNullOrWhiteSpace(w.text)).ToList(), strictSerial)
                     .Where(it => it.Serial != null).ToList();
            var inside = regions.Count == 0 ? (Func<double, double, bool>)((x, y) => true)
                                           : (x, y) => regions.Any(r => x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height);
            IA = IA.Where(it => inside(it.Xc, it.Yc)).ToList();
            IB = IB.Where(it => inside(it.Xc, it.Yc)).ToList();

            // 匹配键：有"序号"→ 以序号为键(按序号配对，再看内容是否变化)；无序号的自由
            // 文本 → 以"内容词集合"为键。两侧存在相同词集合即视为未变(位移/重排不影响)，
            // 仅当内容确实只在一侧出现才报新增/删除。彻底消除导出顺序/位置微调造成的伪差异。
            string Key(TextItem it) => it.Serial != null
                ? "S:" + it.Serial
                : "T:" + TokenSet(TextGeometry.Norm(it.Content));

            var baseByKey = new Dictionary<string, List<TextItem>>();
            foreach (var a in IA)
            {
                var k = Key(a);
                if (!baseByKey.TryGetValue(k, out var lst)) baseByKey[k] = lst = new List<TextItem>();
                lst.Add(a);
            }
            var consumedA = new HashSet<TextItem>();
            var items = new List<DiffItem>();

            foreach (var b in IB)
            {
                var k = Key(b);
                TextItem? a = baseByKey.TryGetValue(k, out var cand) ? cand.FirstOrDefault(x => !consumedA.Contains(x)) : null;
                if (a != null)
                {
                    consumedA.Add(a);
                    // 仅"序号配对"且内容词集合不同 → 变更(绿)；其余(位移/重排/同内容)忽略。
                    if (b.Serial != null && TokenSet(TextGeometry.Norm(a.Content)) != TokenSet(TextGeometry.Norm(b.Content)))
                        items.Add(new DiffItem
                        {
                            Page = page, Type = "text", ChangeType = "changed",
                            OldText = a.Content, NewText = b.Content,
                            OldLines = ToLines(a), NewLines = ToLines(b),
                            Confidence = 0.92, Bbox = BboxOf(b),
                        });
                }
                else
                {
                    items.Add(MkItem(page, b, "added"));
                }
            }
            foreach (var a in IA)
                if (!consumedA.Contains(a))
                    items.Add(MkItem(page, a, "removed"));

            int nAdded = items.Count(x => x.ChangeType == "added");
            int nRemoved = items.Count(x => x.ChangeType == "removed");
            int nChanged = items.Count(x => x.ChangeType == "changed");
            Console.Error.WriteLine($"  textdiff(serial): IA={IA.Count} IB={IB.Count} added={nAdded} removed={nRemoved} changed={nChanged}");
            return items;
        }

        // ------------------------------------------------------------------
        //  图号 (per-sheet drawing number) based page pairing
        // ------------------------------------------------------------------
        /// <summary>
        /// 页面**图号**（每页唯一）。
        ///
        /// ⚠️ 不能取 `408-101-051` 那种"项目号"：它每页标题栏都印同一个，用它分组会退化成
        /// **按页序配对**——新版插入/删除一页后，其后全体错位，逐页比对两张不同图纸，
        /// 产生上千条假差异（实测这就是"误判率极高"的头号原因）。
        /// 真正的每页图号是【标题栏右下角那个管段号】，实测为 `408.101.P02 / P03 / ...`，
        /// 顺序连续且基本唯一。取不到（封面/总图）返回 null，交给线画指纹兜底。
        /// </summary>
        static string? DrawingNoOfPage(MuPDFDocument doc, int pno, double rx, double ry, int maxDistinct)
            => SheetNoOf(doc, pno, rx, ry, maxDistinct);

        /// <summary>公开版：供 CLI 诊断（--sheetnos）列出每页图号，核对配对是否合理。</summary>
        public static string? SheetNoOf(MuPDFDocument doc, int pno, double rx, double ry, int maxDistinct = 25)
        {
            using var st = doc.GetStructuredTextPage(pno, false, StructuredTextFlags.None);
            double H = doc.Pages[pno].Bounds.Y1 - doc.Pages[pno].Bounds.Y0;
            var words = TextGeometry.GetWords(st, H, 0.18);
            if (words.Count == 0) return null;

            // ⚠️ 不要用页宽/页高比例来框区域：/Rotate≠0 的页上，结构化文本的坐标系与
            // 页框（display）坐标系不一致，实测会把图内的管线号误认成图号（例如把 P138
            // 安到一张根本没有图号的封面页上，进而把两张毫不相干的图纸配成一对）。
            // 改为：**用这些词自身的包围盒**求右下角，坐标系无关，旋转页同样成立。
            double minX = double.MaxValue, maxX = double.MinValue, minY = double.MaxValue, maxY = double.MinValue;
            foreach (var w in words)
            {
                if (w.x0 < minX) minX = w.x0; if (w.x1 > maxX) maxX = w.x1;
                if (w.y0 < minY) minY = w.y0; if (w.y1 > maxY) maxY = w.y1;
            }
            double spanX = Math.Max(1e-6, maxX - minX), spanY = Math.Max(1e-6, maxY - minY);
            double xCut = maxX - (1 - rx) * spanX;   // 右侧 rx 以右
            double yCut = maxY - (1 - ry) * spanY;   // 下方 ry 以下

            var cnt = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            var distinctAll = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var w in words)
            {
                var t = (w.text ?? "").Trim();
                if (t.Length < 6) continue;
                if (!TextGeometry.LooksLikeLineNo(t)) continue;
                distinctAll.Add(t);
                if (w.x0 < xCut || w.y0 < yCut) continue;      // 图号字段位于标题栏右下角
                cnt.TryGetValue(t, out int c); cnt[t] = c + 1;
            }
            // 整页管段号种类过多 ⇒ 这是总图/封面（布满上百个管线号），不认图号。
            if (distinctAll.Count > maxDistinct) return null;
            if (cnt.Count > 0)
                return cnt.OrderByDescending(kv => kv.Value).ThenBy(kv => kv.Key.Length).First().Key.ToUpperInvariant();
            return null;
        }

        /// <summary>
        /// **图号优先**配对：同图号 1:1（组内按页序 k↔k）。图号只在一侧的 → 直接判为新增/删除，
        /// **不参与比对**（这正是"图号不一样就不比"的要求）。无图号的页返回给调用方，
        /// 由线画指纹兜底配对。
        /// </summary>
        static (List<(int b, int t, string no)> pairs,
                List<(int page, string no)> addedTgt,
                List<(int page, string no)> removedBase,
                List<int> baseNoNo, List<int> tgtNoNo)
            MatchPagesByDrawingNo(MuPDFDocument baseDoc, int nBase, MuPDFDocument tgtDoc, int nTgt,
                                  double rx, double ry, int maxDistinct, Action<string> L)
        {
            var bNo = new string?[nBase];
            var tNo = new string?[nTgt];
            for (int p = 0; p < nBase; p++) bNo[p] = DrawingNoOfPage(baseDoc, p, rx, ry, maxDistinct);
            for (int p = 0; p < nTgt; p++) tNo[p] = DrawingNoOfPage(tgtDoc, p, rx, ry, maxDistinct);

            var bG = new Dictionary<string, List<int>>(StringComparer.OrdinalIgnoreCase);
            var tG = new Dictionary<string, List<int>>(StringComparer.OrdinalIgnoreCase);
            for (int p = 0; p < nBase; p++) if (bNo[p] != null)
                { var k = bNo[p]!; if (!bG.TryGetValue(k, out var l)) bG[k] = l = new(); l.Add(p); }
            for (int p = 0; p < nTgt; p++) if (tNo[p] != null)
                { var k = tNo[p]!; if (!tG.TryGetValue(k, out var l)) tG[k] = l = new(); l.Add(p); }

            var pairs = new List<(int, int, string)>();
            var added = new List<(int, string)>();
            var removed = new List<(int, string)>();
            foreach (var kv in bG)
            {
                var bl = kv.Value.OrderBy(x => x).ToList();
                if (tG.TryGetValue(kv.Key, out var tlRaw))
                {
                    var tl = tlRaw.OrderBy(x => x).ToList();
                    int n = Math.Min(bl.Count, tl.Count);
                    for (int k = 0; k < n; k++) pairs.Add((bl[k], tl[k], kv.Key));
                    // 同图号多出来的页（图号重复的多页图纸）：多出的按新增/删除报。
                    for (int k = n; k < tl.Count; k++) added.Add((tl[k], kv.Key));
                    for (int k = n; k < bl.Count; k++) removed.Add((bl[k], kv.Key));
                }
                else foreach (var p in bl) removed.Add((p, kv.Key));
            }
            foreach (var kv in tG)
                if (!bG.ContainsKey(kv.Key)) foreach (var p in kv.Value) added.Add((p, kv.Key));

            var baseNoNo = Enumerable.Range(0, nBase).Where(p => bNo[p] == null).ToList();
            var tgtNoNo = Enumerable.Range(0, nTgt).Where(p => tNo[p] == null).ToList();
            pairs.Sort((a, b) => a.Item1.CompareTo(b.Item1));
            added.Sort((a, b) => a.Item1.CompareTo(b.Item1));
            removed.Sort((a, b) => a.Item1.CompareTo(b.Item1));
            L($"  图号配对: 有图号 基准 {nBase - baseNoNo.Count} 页 / 目标 {nTgt - tgtNoNo.Count} 页；" +
              $"同图号配对 {pairs.Count} 对，目标独有 {added.Count}，基准独有 {removed.Count}；" +
              $"无图号 基准 {baseNoNo.Count} / 目标 {tgtNoNo.Count} 页待兜底");
            return (pairs, added, removed, baseNoNo, tgtNoNo);
        }

        static (List<(int basePno, int tgtPno, string no)> pairs,
                List<(int page, string no)> addedTgt,
                List<(int page, string no)> removedBase)
            MatchPages(MuPDFDocument baseDoc, int nBase, MuPDFDocument tgtDoc, int nTgt, Action<string> L)
        {
            var baseNo = new Dictionary<int, string>();
            var tgtNo = new Dictionary<int, string>();
            for (int p = 0; p < nBase; p++) baseNo[p] = DrawingNoOfPage(baseDoc, p, 0.85, 0.78, 25) ?? "?";
            for (int p = 0; p < nTgt; p++) tgtNo[p] = DrawingNoOfPage(tgtDoc, p, 0.85, 0.78, 25) ?? "?";
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
