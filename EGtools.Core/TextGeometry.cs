using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using MuPDFCore;
using MuPDFCore.StructuredText;

namespace EGtools.Core
{
    /// <summary>
    /// Shared PDF vector-text helpers used by every EGtools kernel (PDF-&gt;Excel
    /// table extraction, region selection, drawing diff). By living in the common
    /// Core library they are defined once and reused without any kernel-to-kernel
    /// coupling. All routines read the embedded vector glyphs — no OCR, no pixel
    /// rasterisation, no hard-coded coordinates.
    /// </summary>
    public static class TextGeometry
    {
        /// <summary>A single extracted word with its PDF-page bounding box (pt, origin top-left) and centre.</summary>
        public class Word
        {
            public double x0, y0, x1, y1, xc, yc;
            public string text = "";
            public override string ToString() => $"[{text}] x0={x0:F1} xc={xc:F1} yc={yc:F1}";
        }

        public static string Norm(string t) => Regex.Replace(t ?? "", @"[^\w一-鿿]", "").ToLowerInvariant();
        public static string NormWs(string t) => Regex.Replace(t ?? "", @"\s+", " ").Trim();
        public static bool HasCjk(string s) { foreach (var c in s ?? "") if (c >= 0x4E00 && c <= 0x9FFF) return true; return false; }
        public static bool LooksNumeric(string text)
        {
            var t = Regex.Replace((text ?? "").Trim(), @"\s*(mm|kg|kgs)\s*$", "", RegexOptions.IgnoreCase);
            return Regex.IsMatch(t, @"^[\d.,\s/xX]+$") && t.Any(char.IsDigit);
        }
        public static bool IsNumericNo(string text) => Regex.IsMatch((text ?? "").Trim(), @"^\d+(\.\d+)?$");

        /// <summary>
        /// Turn a MuPDF structured-text page into a list of words with real ink-box
        /// geometry. MuPDF does NOT emit space characters, so a word boundary shows
        /// up as a gap between the INK BOXES of consecutive glyphs; we split words
        /// on that gap using a configurable fraction of the font size.
        /// </summary>
        public static List<Word> GetWords(MuPDFStructuredTextPage st, double H, double wordGapFraction)
        {
            var gs = new List<(double x0, double y0, double y1, double ulx, double urx, double sz, double yc, string text, bool space)>();
            foreach (var block in st)
            {
                if (block.Type != MuPDFStructuredTextBlock.Types.Text) continue;
                foreach (var line in block)
                {
                    var chars = line.Characters.ToList();
                    for (int i = 0; i < chars.Count; i++)
                    {
                        var c = chars[i];
                        string t = c.Character;
                        double x0 = c.Origin.X;
                        double baseline = c.Origin.Y;
                        double sz = Convert.ToDouble(c.Size);
                        var q = c.BoundingQuad;
                        double ulx = q.UpperLeft.X, urx = q.UpperRight.X;
                        if (urx - ulx < 0.1) urx = ulx + sz * 0.5;
                        double yc = baseline - sz / 2.0;
                        double y0 = baseline - sz;
                        double y1 = baseline;
                        bool isSpace = t.Length > 0 && char.IsWhiteSpace(t[0]);
                        gs.Add((x0, y0, y1, ulx, urx, sz, yc, isSpace ? "" : t, isSpace));
                    }
                }
            }
            if (gs.Count == 0) return new List<Word>();
            gs.Sort((a, b) =>
            {
                if (Math.Abs(a.yc - b.yc) > 4.0) return a.yc.CompareTo(b.yc);
                return a.x0.CompareTo(b.x0);
            });
            var rows = new List<List<(double x0, double y0, double y1, double ulx, double urx, double sz, double yc, string text, bool space)>>();
            var cur = new List<(double x0, double y0, double y1, double ulx, double urx, double sz, double yc, string text, bool space)>();
            double rowY = -1;
            foreach (var g in gs)
            {
                if (rowY < 0 || Math.Abs(g.yc - rowY) > 4.0) { if (cur.Count > 0) rows.Add(cur); cur = new List<(double x0, double y0, double y1, double ulx, double urx, double sz, double yc, string text, bool space)>(); rowY = g.yc; }
                cur.Add(g);
            }
            if (cur.Count > 0) rows.Add(cur);
            var words = new List<Word>();
            foreach (var row in rows)
            {
                row.Sort((a, b) => a.x0.CompareTo(b.x0));
                Word w = null; bool has = false; double wSz = 8;
                void Push() { if (has) { w.xc = (w.x0 + w.x1) / 2; w.yc = (w.y0 + w.y1) / 2; words.Add(w); } }
                foreach (var g in row)
                {
                    if (g.space) { Push(); has = false; continue; }
                    bool breakHere = false;
                    if (has) { double d = g.ulx - w.x1; breakHere = d > wordGapFraction * wSz; }
                    if (breakHere) Push();
                    if (!has || breakHere)
                    {
                        w = new Word { x0 = g.x0, y0 = g.y0, x1 = g.urx, y1 = g.y1, text = g.text };
                        wSz = g.sz;
                        has = true;
                    }
                    else
                    {
                        w.x1 = Math.Max(w.x1, g.urx); w.y0 = Math.Min(w.y0, g.y0); w.y1 = Math.Max(w.y1, g.y1);
                        w.text += g.text;
                    }
                }
                Push();
            }
            foreach (var wd in words) { wd.xc = (wd.x0 + wd.x1) / 2; wd.yc = (wd.y0 + wd.y1) / 2; }
            return words;
        }

        public static double PageH(List<Word> words) => words.Count > 0 ? words.Max(w => w.y1) : 1191;

        /// <summary>
        /// Recognise the pipe/line number on a page purely by its *shape* (a dotted,
        /// segmented identifier with a numeric "area.unit" prefix and one letter-led
        /// segment). The most frequent matching token wins. Returns it verbatim.
        /// </summary>
        public static string? ExtractPipeNo(List<Word> words, string? pdfPath, bool fallbackFromFileName)
        {
            var counts = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            foreach (var w in words)
            {
                string t = w.text.Trim();
                if (t.Length < 6 || !LooksLikeLineNo(t)) continue;
                counts.TryGetValue(t, out int c); counts[t] = c + 1;
            }
            if (counts.Count > 0)
                return counts.OrderByDescending(kv => kv.Value).ThenBy(kv => kv.Key.Length).First().Key.ToUpperInvariant();

            if (fallbackFromFileName && !string.IsNullOrEmpty(pdfPath))
            {
                var fn = Path.GetFileNameWithoutExtension(pdfPath);
                foreach (var part in fn.Split('_', ' ', '-', '.'))
                {
                    var t = part.Trim();
                    if (LooksLikeLineNo(t)) return t.ToUpperInvariant();
                }
            }
            return null;
        }

        /// <summary>Structural classifier — pure character scanning (no regex, no position).</summary>
        public static bool LooksLikeLineNo(string s)
        {
            if (s.IndexOf('.') < 0) return false;
            string[] segs = s.Split('.');
            if (segs.Length < 3) return false;
            int numericSegs = 0, letterDigitSegs = 0;
            foreach (var raw in segs)
            {
                string seg = raw;
                int u = seg.IndexOf('_');
                if (u == 0) return false;
                if (u > 0) seg = seg.Substring(0, u);
                if (seg.Length == 0) return false;
                if (IsAllDigits(seg)) { numericSegs++; continue; }
                if (IsLetterThenDigits(seg)) { letterDigitSegs++; continue; }
                return false;
            }
            return numericSegs >= 2 && letterDigitSegs == 1;
        }

        public static bool IsAllDigits(string s)
        {
            foreach (char c in s) if (!char.IsDigit(c)) return false;
            return true;
        }

        public static bool IsLetterThenDigits(string s)
        {
            int i = 0;
            while (i < s.Length && i < 4 && char.IsLetter(s[i])) i++;
            if (i == 0 || i == s.Length) return false;
            for (int j = i; j < s.Length; j++) if (!char.IsDigit(s[j])) return false;
            return true;
        }

        /// <summary>True when a word's ink box intersects the rectangle r (PDF pt, top-left origin).</summary>
        public static bool Intersects(Word w, double rx0, double ry0, double rx1, double ry1)
        {
            return w.x1 >= rx0 && w.x0 <= rx1 && w.y1 >= ry0 && w.y0 <= ry1;
        }
    }
}
