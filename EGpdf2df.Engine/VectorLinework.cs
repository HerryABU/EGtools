using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace EGpdf2df.Engine
{
    /// <summary>
    /// 页面内容流来源：整份 PDF 只开一次，按页取内容流字节与 /Rotate。
    /// 用 iText 读（MuPDFCore 不暴露原始内容流），路径含非 ASCII 也没问题。
    /// </summary>
    public sealed class VectorPageSource : IDisposable
    {
        readonly iText.Kernel.Pdf.PdfDocument _doc;
        public VectorPageSource(string path)
            => _doc = new iText.Kernel.Pdf.PdfDocument(new iText.Kernel.Pdf.PdfReader(path));

        public int PageCount => _doc.GetNumberOfPages();

        public byte[] Content(int pageIdx) => _doc.GetPage(pageIdx + 1).GetContentBytes();

        /// <summary>页面资源字典（解析 /XObject 跟随 Do 时要用）。</summary>
        public iText.Kernel.Pdf.PdfDictionary? PageResources(int pageIdx)
            => _doc.GetPage(pageIdx + 1).GetResources()?.GetPdfObject();

        public int Rotation(int pageIdx)
        {
            int r = _doc.GetPage(pageIdx + 1).GetRotation();
            return ((r % 360) + 360) % 360;
        }

        public void Dispose() => _doc.Close();
    }

    /// <summary>
    /// 矢量线画提取：解析 PDF 页面**内容流**，按图形状态机还原每个被绘制路径在
    /// **内容空间(用户空间)** 下的线段集合（含线宽与描边色）。
    ///
    /// 为什么必须走矢量而不是渲染成位图：
    ///   本项目的 CAD 图纸是【纯矢量、零位图】(XObject Form=0 / Image=0)，一切线画都是
    ///   内容流里的 m/l/c/re + S/f 算子。渲染成位图再逐像素比对，会把 1~2pt 的配准误差
    ///   放大成满图误报（实测同页 16.5% 像素被判为差异），且违反"CAD 必须矢量级解析"的
    ///   项目铁律。矢量提取后配合几何标定 + 共线覆盖匹配，才能把误报压到接近 0。
    ///
    /// 关键实现要点（都是踩过坑的）：
    ///   • 矩阵合成必须按 PDF 约定展开：点变换 x'=a·x+c·y+e, y'=b·x+d·y+f，
    ///     故 M_x·M_y 的第 0 项是 x0*y0 + x2*y1（写成转置约定会让坐标跑到页面外）。
    ///   • 旧版导出用**分离色彩空间**上色：`/CS1 cs 0 0 0 scn` + `/CS1 CS .51 .51 .51 SCN`，
    ///     新版用 `RG/rg/g`。必须同时支持，否则旧版所有图元颜色都是黑的、无法区分线画。
    ///   • 退化零长度段不可见却数量巨大（实测单页 3.4 万~7.6 万条），必须过滤，
    ///     否则比对统计被彻底污染。
    /// </summary>
    public static class VectorLinework
    {
        /// <summary>一条线段（内容空间 pt, y-up）+ 绘制属性。</summary>
        public struct Seg
        {
            public double X0, Y0, X1, Y1;
            public double W;          // 线宽（已折算到内容空间）
            public byte R, G, B;      // 描边色
            public byte Op;           // 'S'=描边, 'F'=填充(轮廓), 'B'=既描又填
            public double Len => Math.Sqrt((X1 - X0) * (X1 - X0) + (Y1 - Y0) * (Y1 - Y0));
        }

        /// <summary>从页面内容流提取全部可见线段（不跟随 Form XObject——见页面级重载）。</summary>
        public static List<Seg> Extract(byte[] content, double minLen = 0.05)
            => new Extractor().Walk(content, null, 0, minLen);

        /// <summary>
        /// 页面级提取：内容流 + **跟随 /Name Do 进入 Form XObject**。
        ///
        /// ⚠️ 这一步是必须的，不是优化：实测本项目的图纸**大量页面把整套线画塞在一个
        /// Form XObject 里**，页面内容流只有 41 字节（`q /Meta0 Do Q`）。不跟随 Do 就会
        /// 得到"线画为空"，矢量比对直接失效。XObject 的 /Matrix 与 /Resources 必须按
        /// PDF 语义正确叠加/继承。
        /// </summary>
        public static List<Seg> Extract(VectorPageSource src, int pageIdx, double minLen = 0.05)
            => new Extractor().Walk(src.Content(pageIdx), src.PageResources(pageIdx), 0, minLen);

        sealed class Extractor
        {
            readonly List<Seg> _segs = new();
            readonly Stack<GState> _stack = new();
            GState _gs = new();

            public List<Seg> Walk(byte[] content, iText.Kernel.Pdf.PdfDictionary? resources, int depth, double minLen)
            {
                // 循环/超深引用保护（Form XObject 可以互相引用，甚至自引用）。
                if (content == null || content.Length == 0 || depth > 12) return _segs;

                var toks = Tokenize(content);
                var path = new List<double[]>();
                double cx = 0, cy = 0, sx = 0, sy = 0;
                var nums = new List<double>();

                void AddSeg(double x0, double y0, double x1, double y1, byte op)
                {
                    if (Math.Abs(x1 - x0) < minLen && Math.Abs(y1 - y0) < minLen) return; // 退化段丢弃
                    var p0 = GState.Apply(_gs.M, x0, y0);
                    var p1 = GState.Apply(_gs.M, x1, y1);
                    _segs.Add(new Seg
                    {
                        X0 = p0.x, Y0 = p0.y, X1 = p1.x, Y1 = p1.y,
                        W = _gs.LW * GState.ScaleOf(_gs.M),
                        R = _gs.R, G = _gs.G, B = _gs.B, Op = op,
                    });
                }

                void Flush(byte op)
                {
                    if (path.Count < 2) { path.Clear(); return; }
                    for (int i = 0; i + 1 < path.Count; i++)
                        AddSeg(path[i][0], path[i][1], path[i + 1][0], path[i + 1][1], op);
                    path.Clear();
                }

                for (int k = 0; k < toks.Count; k++)
                {
                    var t = toks[k];
                    if (t.Kind == 'n') { nums.Add(t.Num); continue; }
                    if (t.Kind == 'N') { _lastName = t.Op; continue; }
                    if (t.Kind != 'o') continue;

                    switch (t.Op)
                    {
                        case "q": _stack.Push(_gs.Clone()); break;
                        case "Q": if (_stack.Count > 0) _gs = _stack.Pop(); break;
                        case "cm":
                            if (nums.Count >= 6)
                                _gs.M = GState.Mul(_gs.M, new[]
                                {
                                    nums[^6], nums[^5], nums[^4], nums[^3], nums[^2], nums[^1]
                                });
                            break;
                        case "w": if (nums.Count >= 1) _gs.LW = nums[^1]; break;

                        // ---- 颜色：灰度 / RGB / CMYK / 分离色彩空间 ----
                        case "G": if (nums.Count >= 1) { byte v = To255(nums[^1]); _gs.R = _gs.G = _gs.B = v; } break;
                        case "RG":
                            if (nums.Count >= 3) { _gs.R = To255(nums[^3]); _gs.G = To255(nums[^2]); _gs.B = To255(nums[^1]); }
                            break;
                        case "K":
                            if (nums.Count >= 4) { var c = Cmyk(nums[^4], nums[^3], nums[^2], nums[^1]); _gs.R = c[0]; _gs.G = c[1]; _gs.B = c[2]; }
                            break;
                        // 旧版导出全靠这对算子上色（70 万次），不支持则旧版线画全黑。
                        case "SCN": case "SC":
                            if (nums.Count >= 1) { var c = FromComponents(nums); _gs.R = c[0]; _gs.G = c[1]; _gs.B = c[2]; }
                            break;
                        case "cs": case "CS": case "g": case "rg": case "k": case "ri": case "i":
                        case "d": case "J": case "j": case "M": case "gs": case "sh":
                        case "BI": case "ID": case "EI":
                            break;

                        // ---- 跟随 Form XObject（本项目大量页面靠这个承载线画）----
                        case "Do":
                            FollowXObject(resources, _lastName, depth, minLen);
                            break;

                        // ---- 路径构造 ----
                        case "m":
                            if (nums.Count >= 2) { cx = nums[^2]; cy = nums[^1]; sx = cx; sy = cy; path.Add(new[] { cx, cy }); }
                            break;
                        case "l":
                            if (nums.Count >= 2) { cx = nums[^2]; cy = nums[^1]; path.Add(new[] { cx, cy }); }
                            break;
                        case "c":
                            if (nums.Count >= 6)
                            {
                                Bez(path, cx, cy, nums[^6], nums[^5], nums[^4], nums[^3], nums[^2], nums[^1]);
                                cx = nums[^2]; cy = nums[^1];
                            }
                            break;
                        case "v":
                            if (nums.Count >= 4)
                            {
                                Bez(path, cx, cy, cx, cy, nums[^4], nums[^3], nums[^2], nums[^1]);
                                cx = nums[^2]; cy = nums[^1];
                            }
                            break;
                        case "y":
                            if (nums.Count >= 4)
                            {
                                Bez(path, cx, cy, nums[^4], nums[^3], nums[^2], nums[^1], nums[^2], nums[^1]);
                                cx = nums[^2]; cy = nums[^1];
                            }
                            break;
                        case "h": if (path.Count > 0) { path.Add(new[] { sx, sy }); cx = sx; cy = sy; } break;
                        case "re":
                            if (nums.Count >= 4)
                            {
                                double rx = nums[^4], ry = nums[^3], rw = nums[^2], rh = nums[^1];
                                path.Add(new[] { rx, ry }); path.Add(new[] { rx + rw, ry });
                                path.Add(new[] { rx + rw, ry + rh }); path.Add(new[] { rx, ry + rh });
                                path.Add(new[] { rx, ry });
                                cx = rx; cy = ry; sx = rx; sy = ry;
                            }
                            break;

                        // ---- 路径绘制 ----
                        case "S": Flush((byte)'S'); break;
                        case "s": if (path.Count > 0) path.Add(new[] { sx, sy }); Flush((byte)'S'); break;
                        case "f": case "F": case "f*": Flush((byte)'F'); break;
                        case "B": case "B*": Flush((byte)'B'); break;
                        case "b": case "b*":
                            if (path.Count > 0) path.Add(new[] { sx, sy });
                            Flush((byte)'B');
                            break;
                        case "n": path.Clear(); break;
                        case "W": case "W*": break;   // 裁剪不改几何

                        // ---- 文本/标记：文字另有 TextRedlineDiff 管线，这里不取 ----
                        case "BT": case "ET": case "Tj": case "TJ": case "Td": case "TD":
                        case "Tm": case "T*": case "Tf": case "TL": case "Tc": case "Tw":
                        case "Tz": case "Ts": case "Tr": case "MP": case "DP": case "BMC":
                        case "BDC": case "EMC": case "BX": case "EX": case "d0": case "d1":
                            break;
                    }
                    nums.Clear();
                }
                return _segs;
            }

            string _lastName = "";

            // 进入 /Name Do 指定的 Form XObject：叠加其 /Matrix、继承/覆盖其 /Resources，再递归解析。
            void FollowXObject(iText.Kernel.Pdf.PdfDictionary? resources, string name, int depth, double minLen)
            {
                if (string.IsNullOrEmpty(name) || resources == null) return;
                var xo = resources.GetAsDictionary(iText.Kernel.Pdf.PdfName.XObject);
                if (xo == null) return;
                var st = xo.GetAsStream(new iText.Kernel.Pdf.PdfName(name));
                if (st == null) return;
                if (st.GetAsName(iText.Kernel.Pdf.PdfName.Subtype)?.GetValue() != "Form") return;

                var savedM = _gs.M;
                var m = st.GetAsArray(iText.Kernel.Pdf.PdfName.Matrix);
                if (m != null && m.Size() == 6)
                {
                    var v = new double[6];
                    for (int i = 0; i < 6; i++) v[i] = m.GetAsNumber(i).DoubleValue();
                    _gs.M = GState.Mul(_gs.M, v);
                }
                var inner = st.GetAsDictionary(iText.Kernel.Pdf.PdfName.Resources) ?? resources;
                Walk(st.GetBytes(), inner, depth + 1, minLen);
                _gs.M = savedM;
            }
        }

        // 按操作数个数推断分离色彩空间的分量数：1=Gray, 3=RGB, 4=CMYK。
        // （不去解析 /ColorSpace 字典——分量个数已足以还原显示色，且对 ICCBased/Separation
        //   等变体都稳；解析字典反而要处理大量边缘格式。）
        static byte[] FromComponents(List<double> nums)
        {
            int n = nums.Count;
            if (n >= 4) return Cmyk(nums[^4], nums[^3], nums[^2], nums[^1]);
            if (n == 3) return new[] { To255(nums[^3]), To255(nums[^2]), To255(nums[^1]) };
            if (n == 2) return new[] { To255(nums[^2]), To255(nums[^2]), To255(nums[^2]) };
            return new[] { To255(nums[^1]), To255(nums[^1]), To255(nums[^1]) };
        }

        static byte To255(double v) => (byte)Math.Clamp((int)Math.Round(v * 255), 0, 255);

        static byte[] Cmyk(double c, double m, double y, double k)
            => new[]
            {
                To255(1 - Math.Min(1, c + k)),
                To255(1 - Math.Min(1, m + k)),
                To255(1 - Math.Min(1, y + k)),
            };

        static void Bez(List<double[]> path, double x0, double y0, double x1, double y1,
                        double x2, double y2, double x3, double y3, int steps = 8)
        {
            for (int i = 1; i <= steps; i++)
            {
                double t = (double)i / steps, u = 1 - t;
                path.Add(new[]
                {
                    u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                    u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3,
                });
            }
        }

        // ---------------------------------------------------------------
        //  图形状态
        // ---------------------------------------------------------------
        sealed class GState
        {
            public double[] M = { 1, 0, 0, 1, 0, 0 };
            public double LW = 1;
            public byte R, G = 0, B = 0;
            public GState Clone() => (GState)MemberwiseClone();

            // PDF 约定：点变换 x' = a·x + c·y + e, y' = b·x + d·y + f
            // 合成 M_x·M_y（先应用 y，再应用 x）：
            //   [a b c d e f] 按 (a,b,c,d,e,f) 存放
            public static double[] Mul(double[] x, double[] y) => new[]
            {
                x[0] * y[0] + x[2] * y[1],
                x[1] * y[0] + x[3] * y[1],
                x[0] * y[2] + x[2] * y[3],
                x[1] * y[2] + x[3] * y[3],
                x[0] * y[4] + x[2] * y[5] + x[4],
                x[1] * y[4] + x[3] * y[5] + x[5],
            };

            public static (double x, double y) Apply(double[] m, double x, double y)
                => (m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]);

            public static double ScaleOf(double[] m)
                => (Math.Sqrt(m[0] * m[0] + m[1] * m[1]) + Math.Sqrt(m[2] * m[2] + m[3] * m[3])) / 2.0;
        }

        // ---------------------------------------------------------------
        //  分词器：kind = n(数字) N(名字) S(字符串/数组/字典) o(算子)
        // ---------------------------------------------------------------
        sealed class Tok
        {
            public char Kind;
            public double Num;
            public string Op = "";
        }

        static List<Tok> Tokenize(byte[] b)
        {
            var res = new List<Tok>();
            int n = b.Length, i = 0;
            while (i < n)
            {
                byte c = b[i];
                if (IsWs(c)) { i++; continue; }
                if (c == '%') { while (i < n && b[i] != 0x0A) i++; continue; }
                if (c == '(')
                {
                    int j = i + 1, d = 1;
                    while (j < n && d > 0)
                    {
                        if (b[j] == (byte)'\\') j += 2;
                        else if (b[j] == (byte)'(') { d++; j++; }
                        else if (b[j] == (byte)')') { d--; j++; }
                        else j++;
                    }
                    res.Add(new Tok { Kind = 'S' }); i = j; continue;
                }
                if (c == (byte)'<')
                {
                    if (i + 1 < n && b[i + 1] == (byte)'<')
                    {
                        int j = i + 2;
                        while (j + 1 < n && !(b[j] == (byte)'>' && b[j + 1] == (byte)'>')) j++;
                        res.Add(new Tok { Kind = 'S' }); i = j + 2; continue;
                    }
                    int k = i + 1; while (k < n && b[k] != (byte)'>') k++;
                    res.Add(new Tok { Kind = 'S' }); i = k + 1; continue;
                }
                if (c == (byte)'[')
                {
                    int j = i + 1, d = 1;
                    while (j < n && d > 0)
                    {
                        if (b[j] == (byte)'\\') { j += 2; continue; }
                        if (b[j] == (byte)'(')
                        {
                            int d2 = 1; j++;
                            while (j < n && d2 > 0)
                            {
                                if (b[j] == (byte)'\\') j += 2;
                                else if (b[j] == (byte)'(') { d2++; j++; }
                                else if (b[j] == (byte)')') { d2--; j++; }
                                else j++;
                            }
                            continue;
                        }
                        if (b[j] == (byte)'[') d++;
                        else if (b[j] == (byte)']') d--;
                        j++;
                    }
                    res.Add(new Tok { Kind = 'S' }); i = j; continue;
                }
                if (c == (byte)'/')
                {
                    int j = i + 1;
                    while (j < n && !IsWs(b[j]) && !IsDelim(b[j])) j++;
                    // 名字以 Op 承载（Extractor 需要它来解析 /Name Do）
                    res.Add(new Tok { Kind = 'N', Op = Encoding.ASCII.GetString(b, i + 1, j - (i + 1)) });
                    i = j; continue;
                }
                if (c == (byte)']' || c == (byte)'}' || c == (byte)'>' ||
                    c == (byte)')' || c == (byte)'{')
                { i++; continue; }

                int e = i;
                while (e < n && !IsWs(b[e]) && !IsDelim(b[e])) e++;
                if (e == i) { i++; continue; }
                string s = Encoding.ASCII.GetString(b, i, e - i);
                if (double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v))
                    res.Add(new Tok { Kind = 'n', Num = v });
                else
                    res.Add(new Tok { Kind = 'o', Op = s });
                i = e;
            }
            return res;
        }

        static bool IsWs(byte c) => c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;

        static bool IsDelim(byte c)
            => c == (byte)'(' || c == (byte)')' || c == (byte)'<' || c == (byte)'>' ||
               c == (byte)'[' || c == (byte)']' || c == (byte)'{' || c == (byte)'}' ||
               c == (byte)'/' || c == (byte)'%';
    }
}
