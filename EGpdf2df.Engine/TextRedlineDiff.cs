using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using iText.Kernel.Pdf;
using iText.Kernel.Colors;
using iText.Kernel.Pdf.Canvas;

namespace EGpdf2df.Engine
{
    /// <summary>
    /// 文字差异渲染 v8 —— 直接在【内容流】层面把原始字形重绘为红/蓝，100% 保留原 CAD PDF 的
    /// 字体/字号/文本矩阵/旋转（竖排、Tm 缩放旋转文字全部正确，绝不崩版式）。
    ///
    /// 为什么不用 PdfCanvasProcessor / TextRenderInfo：iText 9.x 的 PdfCanvasProcessor 在解析
    /// 文本时会【加载字体程序并解析 CMap】；本发行随附的 itext.io 不含 UniGB-UTF16-H 等 CJK
    /// CMap，遇到中文 Type0 字体直接抛 "CMap not found"。因此本实现【完全不使用 iText 的文本
    /// 解析】，改用自研的 PDF 内容流分词器：只读取算子(Tj/TJ/Tm/Tf/cm…)与原始字节，从不解析字体，
    /// 故中文/竖排/旋转均 1:1 保真。
    ///
    /// 流程：
    ///   1. 分词页面内容流 → token 列表(记录字节区间，不去解析字体)。
    ///   2. 走状态机(跟踪 cm/q/Q 与 BT/Tm/Td/TD/T* 得到每个 Tj/TJ 的 CTM×Tm 起笔原点)，
    ///      与 MuPDF 算出的内容流空间包围盒做"原点命中 + 文本包含(拉丁)"匹配。
    ///   3. 命中所在 BT…ET 文本块整体重绘：新增/变更新(红)在目标页重绘；删除/变更旧(蓝)把基准页
    ///      文本块原样拷贝进目标页(字体字典【原样深拷贝】到目标 /Font，不经 iText 解析)。
    ///   4. 重绘时只在每个 Tj/TJ 前注入 rg/RG 颜色(覆盖原黑字)，其余算子逐字节复制 → 字体/字号/
    ///      矩阵/旋转与原文完全一致。
    /// </summary>
    public static class TextRedlineDiff
    {
        /// <summary>内容流空间(左下原点, y-up)下的一个待匹配文字包围盒。</summary>
        public sealed class WordBox
        {
            public string Text = "";
            public double X0, Y0, X1, Y1;
        }

        // 2D 仿射(PDF 矩阵 [a b c d e f])：(x,y) -> (a·x + c·y + e, b·x + d·y + f)
        private struct Affine
        {
            public float a, b, c, d, e, f;
            public Affine(float a, float b, float c, float d, float e, float f)
            { this.a = a; this.b = b; this.c = c; this.d = d; this.e = e; this.f = f; }
            public static readonly Affine Identity = new Affine(1, 0, 0, 1, 0, 0);
            public Affine Mul(Affine o) => new Affine(
                a * o.a + c * o.b, b * o.a + d * o.b,
                a * o.c + c * o.d, b * o.c + d * o.d,
                a * o.e + c * o.f + e, b * o.e + d * o.f + f);
            public (float x, float y) Apply(float x, float y) => (a * x + c * y + e, b * x + d * y + f);
        }

        // 一个 token（内容流算子/操作数），记录字节区间，避免重新序列化出错。
        private sealed class Tok
        {
            public int Start;          // 含
            public int End;            // 不含
            public string Op = "";     // 算子/数字关键字(原始文本)；结构型 token 为空
            public bool IsName;        // 是否为 /Name
            public string Name = "";   // Name 的纯名字(不含 /)
            public string Text = "";   // 字符串/数组解码出的文本（仅用于匹配）
            public bool IsShow;        // 是否为 Tj / TJ（文字显示）
        }

        // 一个【匹配到的文字显示】(Tj / TJ) 的自包含重绘描述：把原始 CTM×Tm 折叠进一个
        // 文本矩阵 M，字体的原始字号 size，以及原始字符串/数组算子字节。重绘时不再依赖整块
        // 上下文，只重绘这一个字形 → 绝不过量标注同块内其它文字，且字体/字号/旋转 1:1 保真。
        private sealed class ShowRender
        {
            public byte[] Operand = Array.Empty<byte>();  // 原始 (...) / [...] 字节
            public bool IsTJ;
            public string FontName = "";                  // 原始字体资源名(不含 /)
            public float FontSize;
            public float[] M = new float[6];              // 折叠后的文本矩阵 [a b c d e f]
        }

        private static string Norm(string t)
        {
            if (string.IsNullOrEmpty(t)) return "";
            var sb = new StringBuilder();
            foreach (var ch in t)
                if (char.IsLetterOrDigit(ch) || ch == '.' || ch == '-' || ch == ' ')
                    sb.Append(char.ToLowerInvariant(ch));
            return sb.ToString().Trim();
        }

        private static bool TextMatch(string run, string box)
        {
            if (string.IsNullOrEmpty(box)) return true;
            var a = Norm(run);
            var b = Norm(box);
            if (a.Length == 0 || b.Length == 0) return true;
            return a == b || a.Contains(b) || b.Contains(a);
        }

        private static bool HasAlpha(string t)
        {
            if (string.IsNullOrEmpty(t)) return false;
            foreach (var ch in t) if (char.IsLetterOrDigit(ch)) return true;
            return false;
        }

        // 字体深拷贝缓存：来源字体字典 → 目标资源名(如 "Ft0")。
        private sealed class FontMover
        {
            private readonly PdfDocument _target;
            private readonly PdfPage _targetPage;
            private readonly Dictionary<PdfObject, string> _cache = new();
            public FontMover(PdfDocument target, PdfPage targetPage) { _target = target; _targetPage = targetPage; }

            // 把来源字体字典原样深拷贝进目标页 /Font 资源(不解析)，返回新资源名。
            public string Move(PdfObject srcFontDict)
            {
                if (_cache.TryGetValue(srcFontDict, out var name)) return name;
                var copied = srcFontDict.CopyTo(_target, true);
                var resDict = _targetPage.GetResources().GetPdfObject();
                var fonts = resDict.GetAsDictionary(PdfName.Font);
                if (fonts == null) { fonts = new PdfDictionary(); resDict.Put(PdfName.Font, fonts); }
                int n = 0; PdfName fn;
                do { fn = new PdfName("Ft" + n++); } while (fonts.Get(fn) != null);
                fonts.Put(fn, copied);
                name = fn.GetValue();
                _cache[srcFontDict] = name;
                return name;
            }
        }

        /// <summary>
        /// 在 targetPdf 第 pageNum 页上，把与 boxes 匹配的每个【目标页原有文字字形】独立重绘为 col
        /// （新增/变更新 → 红）。只重绘命中字符，绝不牵连同文本块内其它文字；字体/字号/文本矩阵/
        /// 旋转与原文 1:1 保真。
        /// </summary>
        public static void RecolorAdded(PdfDocument targetPdf, int pageNum, List<WordBox> boxes, Color col)
        {
            if (boxes == null || boxes.Count == 0) return;
            var page = targetPdf.GetPage(pageNum);
            var shows = Collect(page.GetContentBytes(), boxes);
            if (shows.Count == 0) return;
            var canvas = new PdfCanvas(page);
            Emit(canvas.GetContentStream().GetOutputStream(), shows, null, col);
        }

        /// <summary>
        /// 从 baselinePdf 第 pageNum 页抽取与 boxes 匹配的每个文字字形，原样拷贝进 targetPdf 该页并以
        /// col（删除/变更旧 → 蓝）独立重绘。字体字典原样深拷贝(不解析)，中文/竖排/旋转全部保真。
        /// </summary>
        public static void DrawRemoved(PdfDocument targetPdf, PdfDocument baselinePdf, int pageNum, List<WordBox> boxes, Color col)
        {
            if (boxes == null || boxes.Count == 0) return;
            var basePage = baselinePdf.GetPage(pageNum);
            var shows = Collect(basePage.GetContentBytes(), boxes);
            if (shows.Count == 0) return;

            // 解析匹配字形引用到的字体名 → 基准字体字典 → 深拷贝进目标并拿 旧名→新名 映射。
            var baseRes = basePage.GetResources();
            var remap = new Dictionary<string, string>();
            var mover = new FontMover(targetPdf, targetPdf.GetPage(pageNum));
            foreach (var s in shows)
            {
                if (remap.ContainsKey(s.FontName)) continue;
                var fontsDict = baseRes.GetPdfObject().GetAsDictionary(PdfName.Font);
                var fontObj = fontsDict?.Get(new PdfName(s.FontName));
                if (fontObj == null) continue;
                remap[s.FontName] = mover.Move(fontObj);
            }
            var canvas = new PdfCanvas(targetPdf.GetPage(pageNum));
            Emit(canvas.GetContentStream().GetOutputStream(), shows, remap, col);
        }

        // 分词 + 状态机：跟踪 cm/q/Q 与 BT/Tm/Td/TD/T* 得到每个 Tj/TJ 的【折叠 CTM×Tm 矩阵】、
        // 当前字体与字号；与 MuPDF 算出的内容流空间包围盒做"原点命中 + 文本包含(拉丁)"匹配。
        // 命中即产出一个 ShowRender（携带该字形的原始字符串字节与精确矩阵）；不命中则不产出 →
        // 绝不误染同文本块内的其它文字。
        private static List<ShowRender> Collect(byte[] content, List<WordBox> boxes)
        {
            var toks = Tokenize(content);
            var shows = new List<ShowRender>();
            var ctm = Affine.Identity;
            var tm = Affine.Identity;
            var stack = new Stack<(Affine ctm, Affine tm)>();
            float lead = 0;
            float lastSize = 0;
            string lastName = "";

            for (int k = 0; k < toks.Count; k++)
            {
                var t = toks[k];
                if (t.Op == "q") { stack.Push((ctm, tm)); continue; }
                if (t.Op == "Q") { if (stack.Count > 0) { var s = stack.Pop(); ctm = s.ctm; tm = s.tm; } continue; }
                if (t.Op == "cm") { if (TryNums(toks, k - 6, 6, out var m)) ctm = ctm.Mul(new Affine(m[0], m[1], m[2], m[3], m[4], m[5])); continue; }
                if (t.Op == "BT") { tm = Affine.Identity; continue; }
                if (t.Op == "Tm") { if (TryNums(toks, k - 6, 6, out var m)) tm = new Affine(m[0], m[1], m[2], m[3], m[4], m[5]); continue; }
                if (t.Op == "Td") { if (TryNums(toks, k - 2, 2, out var d)) tm = tm.Mul(new Affine(1, 0, 0, 1, d[0], d[1])); continue; }
                if (t.Op == "TD") { if (TryNums(toks, k - 2, 2, out var td)) { lead = td[1]; tm = tm.Mul(new Affine(1, 0, 0, 1, td[0], td[1])); } continue; }
                if (t.Op == "T*") { tm = tm.Mul(new Affine(1, 0, 0, 1, 0, -lead)); continue; }
                if (t.Op == "Tf") { if (k >= 2) { if (toks[k - 2].IsName) lastName = toks[k - 2].Name; float.TryParse(toks[k - 1].Op, NumberStyles.Float, CultureInfo.InvariantCulture, out lastSize); } continue; }
                if (t.IsShow)
                {
                    var eff = ctm.Mul(tm);
                    var (ox, oy) = eff.Apply(0, 0);
                    var txt = (k - 1 >= 0) ? toks[k - 1].Text : "";
                    if (MatchBox(ox, oy, txt, boxes))
                    {
                        byte[] operand = Array.Empty<byte>();
                        if (k - 1 >= 0) { var op = toks[k - 1]; int len = op.End - op.Start; if (len > 0) { operand = new byte[len]; Array.Copy(content, op.Start, operand, 0, len); } }
                        shows.Add(new ShowRender
                        {
                            Operand = operand,
                            IsTJ = t.Op == "TJ",
                            FontName = lastName,
                            FontSize = lastSize,
                            M = new float[] { eff.a, eff.b, eff.c, eff.d, eff.e, eff.f }
                        });
                    }
                    continue;
                }
            }
            return shows;
        }

        // 把每个匹配字形【自包含】重绘：把原始 CTM×Tm 折叠进一个 Tm，配合原字号与原始字符串字节，
        // 在 q…Q 内重设 rg/RG 颜色并强制填充模式(0 Tr)后只画这一个字 → 与原文同字体/字号/旋转/位置，
        // 且不牵连同块其它文字。原始字节含 CJK CID / 8-bit 编码，List<byte> 逐字节搬运零解释、零损坏。
        private static void Emit(PdfOutputStream os, List<ShowRender> shows, Dictionary<string, string>? remap, Color col)
        {
            var rgb = col.GetColorValue();
            string rc = rgb[0].ToString("F3", CultureInfo.InvariantCulture);
            string gc = rgb[1].ToString("F3", CultureInfo.InvariantCulture);
            string bc = rgb[2].ToString("F3", CultureInfo.InvariantCulture);

            foreach (var s in shows)
            {
                if (s.Operand.Length == 0) continue;
                string fontName = s.FontName;
                if (remap != null && remap.TryGetValue(fontName, out var nn)) fontName = nn;
                if (string.IsNullOrEmpty(fontName)) continue;

                var outB = new List<byte>();
                AppendAscii(outB, "q\n");
                AppendAscii(outB, rc); outB.Add((byte)' '); AppendAscii(outB, gc); outB.Add((byte)' '); AppendAscii(outB, bc); AppendAscii(outB, " rg\n");
                AppendAscii(outB, rc); outB.Add((byte)' '); AppendAscii(outB, gc); outB.Add((byte)' '); AppendAscii(outB, bc); AppendAscii(outB, " RG\n");
                AppendAscii(outB, "BT\n");   // 文本算子(Tf/Tm/Tr/Tj/TJ)必须在 BT…ET 文本对象内，否则语法非法
                // 字体 + 原字号
                outB.Add((byte)'/'); AppendAscii(outB, fontName); outB.Add((byte)' ');
                AppendAscii(outB, s.FontSize.ToString("F3", CultureInfo.InvariantCulture)); AppendAscii(outB, " Tf\n");
                AppendAscii(outB, "0 Tr\n");   // 强制填充模式，确保颜色可见（覆盖原描边文字）
                // 折叠后的文本矩阵（CTM×Tm → 单一 Tm，外层无变换，等价还原定位/缩放/旋转）
                AppendAscii(outB, s.M[0].ToString("F3", CultureInfo.InvariantCulture)); outB.Add((byte)' ');
                AppendAscii(outB, s.M[1].ToString("F3", CultureInfo.InvariantCulture)); outB.Add((byte)' ');
                AppendAscii(outB, s.M[2].ToString("F3", CultureInfo.InvariantCulture)); outB.Add((byte)' ');
                AppendAscii(outB, s.M[3].ToString("F3", CultureInfo.InvariantCulture)); outB.Add((byte)' ');
                AppendAscii(outB, s.M[4].ToString("F3", CultureInfo.InvariantCulture)); outB.Add((byte)' ');
                AppendAscii(outB, s.M[5].ToString("F3", CultureInfo.InvariantCulture)); AppendAscii(outB, " Tm\n");
                // 原始字符串/数组字节（含 CID/8-bit，1:1 保真）
                AppendRaw(outB, s.Operand, 0, s.Operand.Length); outB.Add((byte)' ');
                AppendAscii(outB, s.IsTJ ? "TJ\n" : "Tj\n");
                AppendAscii(outB, "ET\n");
                AppendAscii(outB, "Q\n");
                os.Write(new PdfLiteral(outB.ToArray()));
            }
        }

        private static void AppendAscii(List<byte> dst, string s) => dst.AddRange(Encoding.ASCII.GetBytes(s));
        private static void AppendRaw(List<byte> dst, byte[] src, int off, int len)
        {
            for (int i = 0; i < len; i++) dst.Add(src[off + i]);
        }

        // ---------- 内容流分词（纯字节操作，不解析字体） ----------

        private static List<Tok> Tokenize(byte[] b)
        {
            var toks = new List<Tok>();
            int n = b.Length;
            int i = 0;
            while (i < n)
            {
                byte c = b[i];
                if (c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20) { i++; continue; }
                if (c == '%') { while (i < n && b[i] != 0x0A) i++; continue; }
                if (c == '(')
                {
                    int j = i + 1, depth = 1; var inner = new StringBuilder();
                    while (j < n && depth > 0)
                    {
                        byte d = b[j];
                        if (d == '\\')
                        {
                            if (j + 1 < n)
                            {
                                byte e = b[j + 1];
                                if (e == 'n') inner.Append('\n'); else if (e == 'r') inner.Append('\r');
                                else if (e == 't') inner.Append('\t'); else if (e == 'b') inner.Append('\b');
                                else if (e == 'f') inner.Append('\f'); else if (e >= '0' && e <= '7')
                                {
                                    int oct = 0, cnt = 0; int jj = j + 1;
                                    while (jj < n && cnt < 3 && b[jj] >= '0' && b[jj] <= '7') { oct = oct * 8 + (b[jj] - '0'); jj++; cnt++; }
                                    inner.Append((char)oct); j = jj - 1;
                                }
                                else inner.Append((char)e);
                                j += 2;
                            }
                            else j++;
                        }
                        else if (d == '(') { depth++; inner.Append('('); j++; }
                        else if (d == ')') { depth--; if (depth > 0) inner.Append(')'); j++; }
                        else { inner.Append((char)d); j++; }
                    }
                    toks.Add(new Tok { Start = i, End = Math.Min(j, n), Text = inner.ToString() });
                    i = j; continue;
                }
                if (c == '<')
                {
                    if (i + 1 < n && b[i + 1] == '<')
                    {
                        int j = i + 2, depth = 1;
                        while (j < n && depth > 0)
                        {
                            if (b[j] == '<' && j + 1 < n && b[j + 1] == '<') { depth++; j += 2; }
                            else if (b[j] == '>' && j + 1 < n && b[j + 1] == '>') { depth--; j += 2; }
                            else j++;
                        }
                        toks.Add(new Tok { Start = i, End = Math.Min(j, n) });
                        i = j; continue;
                    }
                    int jj = i + 1; var hex = new StringBuilder();
                    while (jj < n && b[jj] != '>') { if (!char.IsWhiteSpace((char)b[jj])) hex.Append((char)b[jj]); jj++; }
                    jj++;
                    var sb2 = new StringBuilder(); string h = hex.ToString();
                    for (int p = 0; p + 1 < h.Length; p += 2)
                        if (byte.TryParse(h.Substring(p, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out byte v)) sb2.Append((char)v);
                    toks.Add(new Tok { Start = i, End = Math.Min(jj, n), Text = sb2.ToString() });
                    i = jj; continue;
                }
                if (c == '[')
                {
                    int j = i + 1, depth = 1; var inner = new StringBuilder();
                    while (j < n && depth > 0)
                    {
                        byte d = b[j];
                        if (d == '[') depth++;
                        else if (d == ']') depth--;
                        else if (d == '(')
                        {
                            int depth2 = 1; j++;
                            while (j < n && depth2 > 0)
                            {
                                byte e = b[j];
                                if (e == '\\') { j += 2; }
                                else if (e == '(') { depth2++; j++; }
                                else if (e == ')') { depth2--; j++; }
                                else { if (e >= 0x20 && e < 0x7F) inner.Append((char)e); j++; }
                            }
                            continue;
                        }
                        else if (d == '<')
                        {
                            int j2 = j + 1; var hx = new StringBuilder();
                            while (j2 < n && b[j2] != '>') { if (!char.IsWhiteSpace((char)b[j2])) hx.Append((char)b[j2]); j2++; }
                            string hh = hx.ToString();
                            for (int p = 0; p + 1 < hh.Length; p += 2)
                                if (byte.TryParse(hh.Substring(p, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out byte v)) inner.Append((char)v);
                            j = j2 + 1; continue;
                        }
                        else j++;
                    }
                    // 数组算子本身不是显示算子(显示的是随后的 TJ)；仅携带内部文本用于匹配，
                    // 不作为 IsShow，避免 Collect 把数组当作字形、且保证 operand 落在 show 的 k-1。
                    // 注意：while 退出时 j 正好停在闭合 ']' 上（depth→0 不额外 j++），End 为不含端，
                    // 故 End=j+1 才能把 ']' 收进数组 token，否则 ']' 会被下方独立分隔符分支再次分词，
                    // 导致 TJ 前取到的 operand 变成孤立 ']'（TJ 重绘出空字形）。
                    toks.Add(new Tok { Start = i, End = Math.Min(j + 1, n), Text = inner.ToString() });
                    i = j + 1; continue;
                }
                if (c == '/' || c == ']' || c == ')' || c == '>' || c == '}' || c == '{')
                {
                    int j = i + 1;
                    if (c == '/')
                    {
                        while (j < n && !IsDelim(b[j]) && !IsWs(b[j])) j++;
                        toks.Add(new Tok { Start = i, End = j, IsName = true, Name = Encoding.ASCII.GetString(b, i + 1, j - (i + 1)) });
                    }
                    else { j = i + 1; toks.Add(new Tok { Start = i, End = j, Op = ((char)c).ToString() }); }
                    i = j; continue;
                }
                int end = i;
                while (end < n && !IsWs(b[end]) && !IsDelim(b[end])) end++;
                string op = Encoding.ASCII.GetString(b, i, end - i);
                var tok = new Tok { Start = i, End = end, Op = op };
                if (op == "Tj" || op == "TJ") tok.IsShow = true;
                toks.Add(tok);
                i = end;
            }
            return toks;
        }

        private static bool IsWs(byte c) => c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
        private static bool IsDelim(byte c) => c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';

        private static bool TryNums(List<Tok> toks, int start, int n, out float[] v)
        {
            v = new float[n];
            if (start < 0) return false;
            for (int i = 0; i < n; i++)
            {
                if (start + i >= toks.Count) return false;
                if (!float.TryParse(toks[start + i].Op, NumberStyles.Float, CultureInfo.InvariantCulture, out v[i])) return false;
            }
            return true;
        }

        private static bool MatchBox(float ox, float oy, string text, List<WordBox> boxes)
        {
            foreach (var bx in boxes)
            {
                double mx = Math.Max(2.0, bx.X1 - bx.X0);
                double my = Math.Max(2.0, bx.Y1 - bx.Y0);
                bool pos = ox >= bx.X0 - mx && ox <= bx.X1 + mx && oy >= bx.Y0 - my && oy <= bx.Y1 + my;
                if (!pos) continue;
                bool textOk = !HasAlpha(text) || !HasAlpha(bx.Text) || TextMatch(text, bx.Text);
                if (pos && textOk) return true;
            }
            return false;
        }
    }
}
