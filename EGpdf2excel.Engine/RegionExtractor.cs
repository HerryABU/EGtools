using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using EGtools.Core;
using MuPDFCore;
using MuPDFCore.StructuredText;

namespace EGpdf2excel.Engine
{
    /// <summary>
    /// Region-selection extraction kernel. The front-end (Qt) draws rubber-band
    /// boxes and ships them as rectangles in PDF points, origin TOP-LEFT, y-DOWN
    /// (the same space a rendered page image lives in). This kernel extracts the
    /// content of each box DYNAMICALLY — there are no hard-coded table coordinates.
    /// Vector text is preferred; MuPDF's native OCR is used only as a fallback when
    /// a region yields no usable vector glyphs. Multiple boxes of the same page+kind
    /// are merged (union) before extraction.
    ///
    /// Three semantic kinds are understood:
    ///   - "global_table"       : the whole bill-of-material table
    ///   - "detail_table"       : a sub-table / detail block
    ///   - "material_no_region" : the area where the drawing's pipe/line numbers sit
    /// </summary>
    public static class RegionExtractor
    {
        public static int RunFromJson(string? jsonFile, bool readStdin, Action<string>? log = null)
        {
            void L(string s) => (log ?? Console.Error.WriteLine)(s);
            string json;
            try
            {
                if (readStdin) { using var r = new StreamReader(Console.OpenStandardInput()); json = r.ReadToEnd(); }
                else if (!string.IsNullOrEmpty(jsonFile)) json = File.ReadAllText(jsonFile);
                else { L("[error] region mode needs --json FILE or --stdin"); return 2; }
            }
            catch (Exception ex) { L($"[error] reading request: {ex.Message}"); return 2; }

            RegionRequest? req = null;
            try { req = JsonSerializer.Deserialize<RegionRequest>(json, new JsonSerializerOptions { PropertyNameCaseInsensitive = true }); }
            catch (Exception ex) { L($"[error] invalid JSON: {ex.Message}"); return 2; }
            if (req == null || string.IsNullOrEmpty(req.Pdf)) { L("[error] request must contain \"pdf\"."); return 2; }
            if (req.Regions == null || req.Regions.Count == 0) { L("[error] request must contain at least one region."); return 2; }
            if (!File.Exists(req.Pdf)) { L($"[error] pdf not found: {req.Pdf}"); return 2; }

            RegionResponse resp;
            try { resp = Run(req, L); }
            catch (Exception ex) { L($"[error] {ex.Message}"); return 1; }

            var jsonOut = JsonSerializer.Serialize(resp, new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
            Console.WriteLine(jsonOut);
            return 0;
        }

        public static RegionResponse Run(RegionRequest req, Action<string>? log = null)
        {
            void L(string s) => (log ?? Console.Error.WriteLine)(s);
            var resp = new RegionResponse { Pdf = req.Pdf };
            var opts = req.Options ?? new RegionOptions();

            using var ctx = new MuPDFContext(512u * 1024 * 1024);
            var (tmpPath, doc) = OpenDoc(ctx, req.Pdf);
            try
            {
                int nPages = doc.Pages.Count;
                var pageList = (req.Pages != null && req.Pages.Count > 0)
                    ? req.Pages.Distinct().Where(p => p >= 0 && p < nPages).ToList()
                    : req.Regions!.Select(r => r.Page).Distinct().Where(p => p >= 0 && p < nPages).ToList();

                // Cache per-page vector words so overlapping regions reuse them.
                var wordCache = new Dictionary<int, List<TextGeometry.Word>>();

                foreach (int pno in pageList)
                {
                    if (!wordCache.TryGetValue(pno, out var vecWords))
                    {
                        using var st = doc.GetStructuredTextPage(pno, false, StructuredTextFlags.None);
                        double H = doc.Pages[pno].Bounds.Y1 - doc.Pages[pno].Bounds.Y0;
                        vecWords = TextGeometry.GetWords(st, H, opts.WordGapFraction);
                        wordCache[pno] = vecWords;
                    }

                    // Group this page's regions by semantic kind, then merge overlaps.
                    var kinds = req.Regions!.Where(r => r.Page == pno && r.Bbox != null).Select(r => r.Kind ?? "detail_table").Distinct();
                    foreach (var kind in kinds)
                    {
                        var regs = req.Regions!.Where(r => r.Page == pno && (r.Kind ?? "detail_table") == kind && r.Bbox != null)
                                           .Select(r => r.Bbox!).ToList();
                        var merged = opts.MergeOverlapping ? MergeBoxes(regs) : regs;
                        foreach (var box in merged)
                        {
                            var result = ExtractRegion(doc, pno, vecWords, box, kind, opts, L);
                            resp.Regions.Add(result);
                        }
                    }
                }
            }
            finally
            {
                doc.Dispose();
                if (tmpPath != null) { try { File.Delete(tmpPath); } catch { } }
            }
            L($"Done. regions={resp.Regions.Count}");
            return resp;
        }

        static RegionResult ExtractRegion(MuPDFDocument doc, int pno, List<TextGeometry.Word> vecWords, Bbox box, string kind, RegionOptions opts, Action<string>? log)
        {
            double x0 = box.X, y0 = box.Y, x1 = box.X + box.Width, y1 = box.Y + box.Height;
            // Vector words that fall inside the region — these are the PRIMARY,
            // highest-fidelity source ("vector").
            var vecIn = vecWords.Where(w => TextGeometry.Intersects(w, x0, y0, x1, y1)).ToList();

            // Combined list tagged with provenance. We keep vector with priority and
            // only append OCR where a vector glyph did NOT already cover that spot
            // (image-only text). When OcrAlways is off we behave exactly as before:
            // OCR is a pure fallback used only when the region yields no vector text.
            var combined = vecIn.Select(w => (w, src: "vector")).ToList();
            bool ocrUsed = false;

            bool needOcr = opts.OcrAlways || (vecIn.Count == 0 && opts.OcrFallback);
            if (needOcr)
            {
                var ocr = OcrRegion(doc, pno, box, opts, log);
                if (ocr.Count > 0)
                {
                    ocrUsed = true;
                    if (opts.OcrAlways)
                    {
                        // Append OCR words that are NOT already covered by a vector
                        // glyph — these are the image-only gaps OCR re-located.
                        foreach (var ow in ocr)
                            if (!IsCoveredByAny(ow, vecIn))
                                combined.Add((ow, src: "ocr"));
                    }
                    else
                    {
                        // Plain fallback: region had no vector text at all.
                        combined = ocr.Select(w => (w, src: "ocr")).ToList();
                    }
                }
            }

            var words = combined.Select(t => t.w).ToList();

            var result = new RegionResult
            {
                Page = pno,
                Kind = kind,
                Bbox = new Bbox { X = Round(box.X), Y = Round(box.Y), Width = Round(box.Width), Height = Round(box.Height) },
                OcrUsed = ocrUsed,
            };

            if (kind == "material_no_region")
            {
                var pipes = new List<string>();
                var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var w in words)
                {
                    string t = (w.text ?? "").Trim();
                    if (t.Length < 6) continue;
                    if (!TextGeometry.LooksLikeLineNo(t)) continue;
                    t = t.ToUpperInvariant();
                    if (seen.Add(t)) pipes.Add(t);
                }
                result.PipeNumbers = pipes;
            }
            else // global_table | detail_table — both reduce to a row/column grid
            {
                result.Table = BuildTable(words, opts);
            }

            result.Words = combined.Select(t => new WordJson
            {
                X0 = Round(t.w.x0), Y0 = Round(t.w.y0), X1 = Round(t.w.x1), Y1 = Round(t.w.y1),
                Xc = Round(t.w.xc), Yc = Round(t.w.yc), Text = t.w.text,
                Source = t.src
            }).ToList();
            if (ocrUsed && opts.OcrAlways)
                log?.Invoke($"[ocr] region(page {pno + 1}, {kind}) 补充 {combined.Count(c => c.src == "ocr")} 个 OCR 字（矢量优先，图像区用 OCR 补位）");
            return result;
        }

        /// <summary>
        /// True when an OCR word's centre already sits inside some vector glyph's
        /// ink box — i.e. it is a duplicate of text we already recovered as vector,
        /// so OCR should NOT add it again.
        /// </summary>
        static bool IsCoveredByAny(TextGeometry.Word ow, List<TextGeometry.Word> vec)
        {
            double cx = ow.xc, cy = ow.yc;
            foreach (var v in vec)
                if (cx >= v.x0 && cx <= v.x1 && cy >= v.y0 && cy <= v.y1) return true;
            return false;
        }

        /// <summary>
        /// Generic, coordinate-free table builder: cluster words into rows by vertical
        /// centre, then split each row into cells by horizontal ink-box gaps. No column
        /// positions are hard-coded — they emerge from the glyph geometry.
        /// </summary>
        static List<List<string>> BuildTable(List<TextGeometry.Word> words, RegionOptions opts)
        {
            var table = new List<List<string>>();
            if (words.Count == 0) return table;
            var sorted = words.OrderBy(w => w.yc).ThenBy(w => w.x0).ToList();

            var rows = new List<List<TextGeometry.Word>>();
            List<TextGeometry.Word>? cur = null; double rowY = -1;
            foreach (var w in sorted)
            {
                if (rowY < 0 || Math.Abs(w.yc - rowY) > opts.RowTolerance)
                { if (cur != null) rows.Add(cur); cur = new List<TextGeometry.Word>(); rowY = w.yc; }
                cur!.Add(w);
            }
            if (cur != null) rows.Add(cur);

            foreach (var row in rows)
            {
                row.Sort((a, b) => a.x0.CompareTo(b.x0));
                double capH = Math.Max(1e-6, row.Max(w => w.y1 - w.y0));
                var cells = new List<string>();
                TextGeometry.Word? cell = null; double cellEnd = double.MinValue;
                foreach (var w in row)
                {
                    if (cell == null) { cell = w; cellEnd = w.x1; continue; }
                    double gap = w.x0 - cellEnd;
                    if (gap > opts.ColGapFraction * capH)
                    {
                        cells.Add(cell.text); cell = w; cellEnd = w.x1;
                    }
                    else
                    {
                        cell.text = (cell.text.Length > 0 ? cell.text + " " : "") + w.text;
                        cellEnd = Math.Max(cellEnd, w.x1);
                    }
                }
                if (cell != null) cells.Add(cell.text);
                table.Add(cells);
            }
            return table;
        }

        /// <summary>
        /// OCR fallback. Renders the page at 300 DPI, asks MuPDF for the native
        /// Tesseract text layer, then keeps only the glyphs inside the region.
        /// Returns an empty list (and logs a warning) if OCR data is unavailable.
        /// </summary>
        static List<TextGeometry.Word> OcrRegion(MuPDFDocument doc, int pno, Bbox box, RegionOptions opts, Action<string>? log)
        {
            try
            {
                var lang = MapLang(opts.Languages);
                using var st = doc.GetStructuredTextPage(pno, lang, false, StructuredTextFlags.None, CancellationToken.None, null);
                double H = doc.Pages[pno].Bounds.Y1 - doc.Pages[pno].Bounds.Y0;
                var all = TextGeometry.GetWords(st, H, opts.WordGapFraction);
                double x0 = box.X, y0 = box.Y, x1 = box.X + box.Width, y1 = box.Y + box.Height;
                return all.Where(w => TextGeometry.Intersects(w, x0, y0, x1, y1)).ToList();
            }
            catch (Exception ex) { log?.Invoke($"[warn] OCR unavailable for region on page {pno + 1}: {ex.Message}"); return new List<TextGeometry.Word>(); }
        }

        static TesseractLanguage MapLang(List<string>? langs)
        {
            // TesseractLanguage is a class; the OCR overload needs an instance built
            // from the nested Best/Fast enum via its (Best, bool) constructor.
            TesseractLanguage.Best v = TesseractLanguage.Best.Eng;
            if (langs != null)
                foreach (var l in langs)
                    if (l.Contains("chi", StringComparison.OrdinalIgnoreCase) || l.StartsWith("zh", StringComparison.OrdinalIgnoreCase))
                        v = TesseractLanguage.Best.Chi_Sim;
            return new TesseractLanguage(v, false);
        }

        /// <summary>Union of bounding boxes — the merge step for multiple overlapping selections.</summary>
        static List<Bbox> MergeBoxes(List<Bbox> boxes)
        {
            if (boxes.Count <= 1) return boxes;
            var merged = new List<Bbox>();
            var remaining = boxes.ToList();
            while (remaining.Count > 0)
            {
                var seed = remaining[0]; remaining.RemoveAt(0);
                double x0 = seed.X, y0 = seed.Y, x1 = seed.X + seed.Width, y1 = seed.Y + seed.Height;
                bool grew;
                do
                {
                    grew = false;
                    for (int i = remaining.Count - 1; i >= 0; i--)
                    {
                        var b = remaining[i];
                        double bx0 = b.X, by0 = b.Y, bx1 = b.X + b.Width, by1 = b.Y + b.Height;
                        if (bx0 <= x1 + 1 && bx1 >= x0 - 1 && by0 <= y1 + 1 && by1 >= y0 - 1)
                        {
                            x0 = Math.Min(x0, bx0); y0 = Math.Min(y0, by0);
                            x1 = Math.Max(x1, bx1); y1 = Math.Max(y1, by1);
                            remaining.RemoveAt(i); grew = true;
                        }
                    }
                } while (grew);
                merged.Add(new Bbox { X = x0, Y = y0, Width = x1 - x0, Height = y1 - y0 });
            }
            return merged;
        }

        static (string?, MuPDFDocument) OpenDoc(MuPDFContext ctx, string path)
        {
            if (path.Any(c => c > 127))
            {
                var tmp = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N") + ".pdf");
                File.Copy(path, tmp, true);
                return (tmp, new MuPDFDocument(ctx, tmp));
            }
            return (null, new MuPDFDocument(ctx, path));
        }

        static double Round(double v) => Math.Round(v, 2);
    }
}
