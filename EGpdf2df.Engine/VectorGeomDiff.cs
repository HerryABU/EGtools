using System;
using System.Collections.Generic;
using System.Globalization;

namespace EGpdf2df.Engine
{
    /// <summary>
    /// 矢量级图形差异检测 —— 替代旧的两版渲染成位图再逐像素/逐轮廓比对。
    ///
    /// 旧路径为什么误报率极高（已实测）：
    ///   1. 像素级比对对 1~2pt 的配准误差毫无容忍度，细线整体错位 → 同一张图 16.5% 像素
    ///      被判为差异，整张图点亮；
    ///   2. 逐"图元/线段"1:1 比对也不行：旧版导出是 1 段 1 描边（166,010 条独立描边），
    ///      新版把多段合并成折线（37,090 条），**同一条线在两版被切成不同数量的原子段**，
    ///      实测逐段 1:1 命中率只有 ~7%。
    ///
    /// 本实现的三段式：
    ///   ① 几何标定：两版导出比例/原点不同（实测 旧 ≈ 2.01854×新 + (4.60,7.39)），
    ///      用包围盒给初值，再以"长线段中点命中率"为目标做坐标下降精修。
    ///   ② 共线覆盖匹配：**不对线段做 1:1**，而是沿一条线段采样若干点，若每个采样点都能在
    ///      对侧线画里找到"方向相近(角度容差)且距离 ≤ tol"的线段，则判定该段未变。
    ///      这样天然免疫切分粒度差异。
    ///   ③ 聚类 + 成链：未匹配线段按端点连通性合并成极大折线（避免碎成十万条小线段）。
    ///
    /// 输出的是**沿线描边**（不是填充块）——CAD 线画图上"这条线是新增/删除"才是正确语义，
    /// 且描边天然不遮挡底图。
    /// </summary>
    public static class VectorGeomDiff
    {
        public sealed class Run
        {
            public List<double> Pts = new();   // 内容空间 pt, y-up，扁平 [x,y,...]
            public string Dir = "added";       // added / removed / changed
            public double Length;
        }

        public sealed class Result
        {
            public List<Run> Runs = new();
            public double Scale = 1, Dx = 0, Dy = 0;
            public int BaseSegs, TgtSegs, TgtMatched, BaseMatched;
            /// <summary>标定质量诊断（长线段中点命中率），供日志/排障。</summary>
            public double CalibHitRate;
        }

        // ------------------------------------------------------------------
        //  ① 几何标定
        // ------------------------------------------------------------------
        /// <summary>
        /// 求把 baseline 坐标映射到 target 坐标的 (scale,dx,dy)：tgt = base/scale + (dx,dy)。
        ///
        /// ⚠️ 必须**由粗到细**两级标定，不能只做一次细标定：
        /// 实测两版导出除了平移，还存在约 0.4% 的尺度差（在 1683pt 宽的页面上，边角处累计
        /// 漂移可达 6.7pt）。细容差下标定打分面非常崎岖，爬山会卡在局部最优——实测只用
        /// bbox 初值 + 细标定，长段命中率只有 46%，看起来像"整页都变了"；换成大容差先定
        /// 尺度、再小容差细化，命中率升到 94%+。这是"误报率极高"的关键一环。
        /// </summary>
        public static (double s, double dx, double dy, double hitRate) Calibrate(
            List<VectorLinework.Seg> bsegs, List<VectorLinework.Seg> tsegs, double tol, double cosMin,
            Action<string>? log = null, bool quick = false)
        {
            var (bx0, by0, bx1, by1) = Span(bsegs);
            var (tx0, ty0, tx1, ty1) = Span(tsegs);
            double s0 = ((bx1 - bx0) + (by1 - by0)) / Math.Max(1e-9, (tx1 - tx0) + (ty1 - ty0));
            double dx0 = tx0 - bx0 / s0, dy0 = ty0 - by0 / s0;

            // ---- 粗标定：大容差 + 在 bbox 比例附近扫尺度（吸收导出尺度差）----
            double coarseTol = Math.Max(5.0, tol * 5);
            var coarse = CalibrateCore(bsegs, tsegs, coarseTol, cosMin, true, (s0, dx0, dy0), quick);
            log?.Invoke($"  [vec] 粗标定 tol={coarseTol:F1} → scale={coarse.s:F5} dx={coarse.dx:F2} dy={coarse.dy:F2} " +
                        $"命中={coarse.hit * 100:F1}%");

            // ---- 细标定：以粗结果为初值，用小步长在目标容差下细化 ----
            var fine = CalibrateCore(bsegs, tsegs, tol, cosMin, false, (coarse.s, coarse.dx, coarse.dy), quick);
            log?.Invoke($"  [vec] 细标定 tol={tol:F1} → scale={fine.s:F5} dx={fine.dx:F2} dy={fine.dy:F2} " +
                        $"命中={fine.hit * 100:F1}%");
            // 细标定若反而变差（打分面噪声），退回粗结果。
            return fine.hit >= coarse.hit ? fine : coarse;
        }

        static (double s, double dx, double dy, double hit) CalibrateCore(
            List<VectorLinework.Seg> bsegs, List<VectorLinework.Seg> tsegs, double tol, double cosMin,
            bool scanScale, (double s, double dx, double dy) init, bool quick = false)
        {
            var index = new LineIndex(tsegs, Math.Max(1.0, tol));
            // 只用长线段打分：方向明确、且不受两版切分粒度差异影响。
            var probes = new List<VectorLinework.Seg>();
            double minLen = Math.Max(20.0, tol * 8);
            foreach (var g in bsegs) if (g.Len >= minLen) probes.Add(g);
            int cap = quick ? 300 : 1500;
            if (probes.Count > cap)
            {
                int step = probes.Count / cap;
                var sub = new List<VectorLinework.Seg>();
                for (int i = 0; i < probes.Count; i += step) sub.Add(probes[i]);
                probes = sub;
            }
            if (probes.Count == 0) return (init.s, init.dx, init.dy, 0);

            double Score(double s, double dx, double dy)
            {
                int hit = 0;
                foreach (var g in probes)
                {
                    double mx = (g.X0 + g.X1) * 0.5 / s + dx;
                    double my = (g.Y0 + g.Y1) * 0.5 / s + dy;
                    if (index.Covered(mx, my, g.X1 - g.X0, g.Y1 - g.Y0, tol, cosMin)) hit++;
                }
                return (double)hit / probes.Count;
            }

            double bs = init.s, bx = init.dx, by = init.dy, best = Score(bs, bx, by);

            if (scanScale)
            {
                // 尺度扫描：±2%，步长 0.04%（quick 时 ±1%、步 0.2%）；每个候选尺度只做一次平移对齐。
                int half = quick ? 5 : 50;
                double rel = quick ? 0.002 : 0.0004;
                for (double k = -half; k <= half; k++)
                {
                    double s = init.s * (1 + k * rel);
                    if (s <= 0) continue;
                    double px = bx, py = by, loc = Score(s, px, py);
                    double st = tol;
                    for (int it = 0; it < 12; it++)
                    {
                        bool moved = false;
                        foreach (var (ax, ay) in new[] { (st, 0d), (-st, 0d), (0d, st), (0d, -st) })
                        {
                            double v = Score(s, px + ax, py + ay);
                            if (v > loc) { loc = v; px += ax; py += ay; moved = true; }
                        }
                        if (!moved) st /= 2;
                        if (st < tol * 0.05) break;
                    }
                    if (loc > best) { best = loc; bs = s; bx = px; by = py; }
                }
                return (bs, bx, by, best);
            }

            // 精标定：小步长爬山（尺度也比粗结果微调）
            double stS = Math.Abs(bs) * 0.0008, stD = Math.Max(0.2, tol * 0.5);
            for (int iter = 0; iter < 200; iter++)
            {
                bool moved = false;
                foreach (var (ds, ddx, ddy) in new[]
                {
                    (stS, 0d, 0d), (-stS, 0d, 0d),
                    (0d, stD, 0d), (0d, -stD, 0d),
                    (0d, 0d, stD), (0d, 0d, -stD),
                })
                {
                    double v = Score(bs + ds, bx + ddx, by + ddy);
                    if (v > best + 1e-9) { best = v; bs += ds; bx += ddx; by += ddy; moved = true; }
                }
                if (!moved) { stS /= 2; stD /= 2; }
                if (stS < 1e-7 && stD < 0.02) break;
            }
            return (bs, bx, by, best);
        }

        static (double x0, double y0, double x1, double y1) Span(List<VectorLinework.Seg> segs)
        {
            double x0 = double.MaxValue, y0 = double.MaxValue, x1 = double.MinValue, y1 = double.MinValue;
            foreach (var s in segs)
            {
                if (s.X0 < x0) x0 = s.X0; if (s.X1 < x0) x0 = s.X1;
                if (s.Y0 < y0) y0 = s.Y0; if (s.Y1 < y0) y0 = s.Y1;
                if (s.X0 > x1) x1 = s.X0; if (s.X1 > x1) x1 = s.X1;
                if (s.Y0 > y1) y1 = s.Y0; if (s.Y1 > y1) y1 = s.Y1;
            }
            if (x0 > x1) { x0 = y0 = x1 = y1 = 0; }
            return (x0, y0, x1, y1);
        }

        // ------------------------------------------------------------------
        //  空间索引：把一个点集按网格分桶，支持"某点附近是否存在方向相近的线段"
        // ------------------------------------------------------------------
        public sealed class LineIndex
        {
            readonly double _cell;
            readonly Dictionary<long, List<int>> _grid = new();
            // 每个 chunk：x0,y0,x1,y1
            readonly List<double[]> _chunks = new();

            public LineIndex(List<VectorLinework.Seg> segs, double cellHint)
            {
                _cell = Math.Max(0.5, cellHint);
                for (int i = 0; i < segs.Count; i++)
                {
                    var s = segs[i];
                    double len = s.Len;
                    // 长线段切成 ≤cell 的 chunk：保证"命中点必然落在其 chunk 的邻域 1 格内"。
                    int parts = Math.Max(1, (int)Math.Ceiling(len / _cell));
                    for (int p = 0; p < parts; p++)
                    {
                        double t0 = (double)p / parts, t1 = (double)(p + 1) / parts;
                        double cx0 = s.X0 + (s.X1 - s.X0) * t0, cy0 = s.Y0 + (s.Y1 - s.Y0) * t0;
                        double cx1 = s.X0 + (s.X1 - s.X0) * t1, cy1 = s.Y0 + (s.Y1 - s.Y0) * t1;
                        int ci = _chunks.Count;
                        _chunks.Add(new[] { cx0, cy0, cx1, cy1 });
                        long key = Key((cx0 + cx1) * 0.5, (cy0 + cy1) * 0.5);
                        if (!_grid.TryGetValue(key, out var l)) _grid[key] = l = new List<int>();
                        l.Add(ci);
                    }
                }
            }

            long Key(double x, double y)
            {
                long cx = (long)Math.Floor(x / _cell), cy = (long)Math.Floor(y / _cell);
                return (cx + 0x4000000L) * 0x8000000L + (cy + 0x4000000L);
            }

            /// <summary>(x,y) 附近 tol 内是否存在与 (ux,uy) 方向相近(cos ≥ cosMin) 的线段。</summary>
            public bool Covered(double x, double y, double ux, double uy, double tol, double cosMin)
            {
                double ul = Math.Sqrt(ux * ux + uy * uy);
                if (ul < 1e-12) return false;
                ux /= ul; uy /= ul;
                long bx = (long)Math.Floor(x / _cell), by = (long)Math.Floor(y / _cell);
                for (long gx = bx - 1; gx <= bx + 1; gx++)
                    for (long gy = by - 1; gy <= by + 1; gy++)
                    {
                        if (!_grid.TryGetValue((gx + 0x4000000L) * 0x8000000L + (gy + 0x4000000L), out var l)) continue;
                        foreach (int ci in l)
                        {
                            var c = _chunks[ci];
                            double vx = c[2] - c[0], vy = c[3] - c[1];
                            double vl = Math.Sqrt(vx * vx + vy * vy);
                            if (vl > 1e-12)
                            {
                                // 方向平行性检验：防止密集区被"横穿而过"的无关线段误判为覆盖。
                                double cos = Math.Abs((ux * vx + uy * vy) / vl);
                                if (cos < cosMin) continue;
                            }
                            if (DistPointSeg(x, y, c[0], c[1], c[2], c[3]) <= tol) return true;
                        }
                    }
                return false;
            }

            /// <summary>
            /// 最近线段距离（不限方向），供诊断"未匹配是容差不够还是真的没有对侧线画"。
            /// 搜索 1 圈格子；找不到返回 cap。
            /// </summary>
            public double Nearest(double x, double y, double cap)
            {
                double bd = cap;
                long bx = (long)Math.Floor(x / _cell), by = (long)Math.Floor(y / _cell);
                for (long gx = bx - 1; gx <= bx + 1; gx++)
                    for (long gy = by - 1; gy <= by + 1; gy++)
                    {
                        if (!_grid.TryGetValue((gx + 0x4000000L) * 0x8000000L + (gy + 0x4000000L), out var l)) continue;
                        foreach (int ci in l)
                        {
                            var c = _chunks[ci];
                            double d = DistPointSeg(x, y, c[0], c[1], c[2], c[3]);
                            if (d < bd) bd = d;
                        }
                    }
                return bd;
            }

            static double DistPointSeg(double px, double py, double ax, double ay, double bx, double by)
            {
                double vx = bx - ax, vy = by - ay;
                double L2 = vx * vx + vy * vy;
                double t = L2 <= 1e-18 ? 0 : ((px - ax) * vx + (py - ay) * vy) / L2;
                if (t < 0) t = 0; else if (t > 1) t = 1;
                double qx = ax + t * vx, qy = ay + t * vy;
                return Math.Sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
            }
        }

        // ------------------------------------------------------------------
        //  页面指纹 + 按线画配对
        // ------------------------------------------------------------------
        /// <summary>
        /// 页面线画指纹：按包围盒归一化后的 48×32 占据网格（每格累计线段采样点数），L2 归一化。
        /// 用途是**判定两版页面对应关系**——标题栏每页都印同一个项目号，按"图纸号"配对会退化成
        /// 按页序配对；一旦新版插入/删除一页，其后全体错位，逐页比对的误报会爆炸（实测错位一对
        /// 页仍有 ~60% 命中率、上千条假差异，而正确对应页是 100%/0 条）。指纹只依赖线画几何，
        /// 与文字/标题栏无关，能稳准地找出真实对应页。
        /// </summary>
        public static double[] Fingerprint(List<VectorLinework.Seg> segs)
        {
            const int GW = 48, GH = 32;
            var g = new double[GW * GH];
            if (segs.Count == 0) return g;
            var (x0, y0, x1, y1) = Span(segs);
            double w = Math.Max(1e-6, x1 - x0), h = Math.Max(1e-6, y1 - y0);
            foreach (var s in segs)
            {
                int n = Math.Max(1, (int)Math.Ceiling(s.Len / 4.0));
                if (n > 24) n = 24;
                for (int i = 0; i <= n; i++)
                {
                    double t = (double)i / n;
                    double px = s.X0 + (s.X1 - s.X0) * t, py = s.Y0 + (s.Y1 - s.Y0) * t;
                    int cx = (int)((px - x0) / w * GW); if (cx < 0) cx = 0; else if (cx >= GW) cx = GW - 1;
                    int cy = (int)((py - y0) / h * GH); if (cy < 0) cy = 0; else if (cy >= GH) cy = GH - 1;
                    g[cy * GW + cx] += 1;
                }
            }
            double norm = 0; foreach (var v in g) norm += v * v;
            norm = Math.Sqrt(norm);
            if (norm > 1e-12) for (int i = 0; i < g.Length; i++) g[i] /= norm;
            return g;
        }

        /// <summary>指纹余弦相似度（都已 L2 归一化，直接点积）。</summary>
        public static double Similarity(double[] a, double[] b)
        {
            if (a == null || b == null || a.Length != b.Length) return 0;
            double s = 0;
            for (int i = 0; i < a.Length; i++) s += a[i] * b[i];
            return s;
        }

        /// <summary>
        /// 按线画指纹做 1:1 页面配对（贪心：相似度从高到低取，已占用的页跳过）。
        /// 返回 已配对 / 目标独有(新增) / 基准独有(删除)。
        /// </summary>
        public static (List<(int b, int t, double sim)> pairs, List<int> added, List<int> removed)
            MatchPagesByLinework(List<double[]> bFp, List<double[]> tFp, double minSim, Action<string>? log = null)
        {
            int nB = bFp.Count, nT = tFp.Count;
            var cand = new List<(double sim, int b, int t)>();
            for (int i = 0; i < nB; i++)
                for (int j = 0; j < nT; j++)
                {
                    double s = Similarity(bFp[i], tFp[j]);
                    if (s >= minSim) cand.Add((s, i, j));
                }
            cand.Sort((x, y) => y.sim.CompareTo(x.sim));
            var bu = new bool[nB]; var tu = new bool[nT];
            var pairs = new List<(int, int, double)>();
            foreach (var (s, i, j) in cand)
            {
                if (bu[i] || tu[j]) continue;
                bu[i] = true; tu[j] = true;
                pairs.Add((i, j, s));
            }
            pairs.Sort((a, b) => a.Item1.CompareTo(b.Item1));
            var added = new List<int>(); for (int j = 0; j < nT; j++) if (!tu[j]) added.Add(j);
            var removed = new List<int>(); for (int i = 0; i < nB; i++) if (!bu[i]) removed.Add(i);
            log?.Invoke($"  [vec] 按线画配对: 匹配 {pairs.Count} 页, 目标独有 {added.Count} 页, 基准独有 {removed.Count} 页");
            return (pairs, added, removed);
        }
        /// <summary>
        /// 运行矢量差异检测。tol 为容差(pt, 目标空间)，angTolDeg 为方向容差(度)。
        /// </summary>
        public static Result Compare(List<VectorLinework.Seg> bsegs, List<VectorLinework.Seg> tsegs,
                                 double tol, double angTolDeg, Action<string>? log = null, bool quick = false)
        {
            var res = new Result { BaseSegs = bsegs.Count, TgtSegs = tsegs.Count };
            if (bsegs.Count == 0 || tsegs.Count == 0)
            {
                log?.Invoke($"  [vec] 线画为空(base={bsegs.Count} tgt={tsegs.Count})，跳过矢量比对");
                return res;
            }

            double cosMin = Math.Cos(angTolDeg * Math.PI / 180.0);
            var (s, dx, dy, hit) = Calibrate(bsegs, tsegs, tol, cosMin, log, quick);
            res.Scale = s; res.Dx = dx; res.Dy = dy; res.CalibHitRate = hit;

            // 基准侧线段变换到目标坐标系
            var bMoved = new List<VectorLinework.Seg>(bsegs.Count);
            foreach (var g in bsegs)
                bMoved.Add(new VectorLinework.Seg
                {
                    X0 = g.X0 / s + dx, Y0 = g.Y0 / s + dy,
                    X1 = g.X1 / s + dx, Y1 = g.Y1 / s + dy,
                    W = g.W / Math.Abs(s), R = g.R, G = g.G, B = g.B, Op = g.Op,
                });

            var tIndex = new LineIndex(tsegs, tol);
            var bIndex = new LineIndex(bMoved, tol);

            // 目标段：沿段采样，全部采样点都能被基准线画覆盖 → 未变
            var tUnmatched = new List<VectorLinework.Seg>();
            int tMatched = 0;
            foreach (var g in tsegs)
                if (FullyCovered(g, bIndex, tol, cosMin)) tMatched++;
                else tUnmatched.Add(g);
            res.TgtMatched = tMatched;

            var bUnmatched = new List<VectorLinework.Seg>();
            int bMatched = 0;
            foreach (var g in bMoved)
                if (FullyCovered(g, tIndex, tol, cosMin)) bMatched++;
                else bUnmatched.Add(g);
            res.BaseMatched = bMatched;

            log?.Invoke($"  [vec] 覆盖匹配: 目标 {tMatched}/{tsegs.Count} 未变, 基准 {bMatched}/{bMoved.Count} 未变");

            // 诊断：未匹配的目标线段，到对侧线画的最近距离分布。
            // 若绝大多数都远大于容差 → 是真实差异；若大量卡在容差附近 → 该放宽 tol。
            if (tUnmatched.Count > 0)
            {
                var dd = new List<double>(Math.Min(tUnmatched.Count, 3000));
                int step = Math.Max(1, tUnmatched.Count / 3000);
                for (int i = 0; i < tUnmatched.Count; i += step)
                {
                    var g = tUnmatched[i];
                    dd.Add(bIndex.Nearest((g.X0 + g.X1) * 0.5, (g.Y0 + g.Y1) * 0.5, tol * 4));
                }
                dd.Sort();
                double Near(double q) => dd.Count == 0 ? 0 : dd[(int)Math.Min(dd.Count - 1, q * dd.Count)];
                log?.Invoke($"  [vec] 未匹配目标段距对侧线画: p10={Near(0.10):F2} p50={Near(0.50):F2} " +
                            $"p90={Near(0.90):F2} (tol={tol:F1}) — p50≈tol 说明容差偏紧，p50≫tol 才是真差异");
            }

            // 成链：未匹配线段按端点连通性合并成极大折线
            res.Runs.AddRange(Chain(tUnmatched, "added", tol));
            res.Runs.AddRange(Chain(bUnmatched, "removed", tol));
            return res;
        }

        // 沿线段采样（含两端与中点），要求每个采样点都被对侧线画覆盖。
        // 采样步长随长度自适应：短段 2 点即可，长段按 tol×2 加密，避免"跨过一小段缺口"被误判为覆盖。
        static bool FullyCovered(VectorLinework.Seg g, LineIndex idx, double tol, double cosMin)
        {
            double len = g.Len;
            int n = Math.Max(2, (int)Math.Ceiling(len / Math.Max(0.8, tol * 2)) + 1);
            if (n > 64) n = 64;
            double ux = g.X1 - g.X0, uy = g.Y1 - g.Y0;
            for (int i = 0; i < n; i++)
            {
                double t = n == 1 ? 0 : (double)i / (n - 1);
                double x = g.X0 + ux * t, y = g.Y0 + uy * t;
                if (!idx.Covered(x, y, ux, uy, tol, cosMin)) return false;
            }
            return true;
        }

        // 端点连通性成链：把一堆离散线段拼成尽量少的连续折线，避免输出十万条 2 点小线段。
        static List<Run> Chain(List<VectorLinework.Seg> segs, string dir, double tol)
        {
            var runs = new List<Run>();
            if (segs.Count == 0) return runs;
            double q = Math.Max(0.05, tol);
            var buckets = new Dictionary<long, List<int>>();
            long K(double x, double y) => (long)Math.Floor(x / q) * 0x8000000L + (long)Math.Floor(y / q);
            void Reg(int i)
            {
                foreach (var (x, y) in new[] { (segs[i].X0, segs[i].Y0), (segs[i].X1, segs[i].Y1) })
                {
                    long k = K(x, y);
                    if (!buckets.TryGetValue(k, out var l)) buckets[k] = l = new List<int>();
                    l.Add(i);
                }
            }
            for (int i = 0; i < segs.Count; i++) Reg(i);

            int FindNext(double x, double y, bool[] used)
            {
                for (long gx = (long)Math.Floor(x / q) - 1; gx <= (long)Math.Floor(x / q) + 1; gx++)
                    for (long gy = (long)Math.Floor(y / q) - 1; gy <= (long)Math.Floor(y / q) + 1; gy++)
                    {
                        if (!buckets.TryGetValue(gx * 0x8000000L + gy, out var l)) continue;
                        foreach (int i in l) if (!used[i]) return i;
                    }
                return -1;
            }

            var used = new bool[segs.Count];
            for (int start = 0; start < segs.Count; start++)
            {
                if (used[start]) continue;
                used[start] = true;
                var pts = new List<double> { segs[start].X0, segs[start].Y0, segs[start].X1, segs[start].Y1 };
                double tx = segs[start].X1, ty = segs[start].Y1;
                // 向前延伸
                while (true)
                {
                    int nx = FindNext(tx, ty, used);
                    if (nx < 0) break;
                    used[nx] = true;
                    var s2 = segs[nx];
                    bool forward = Math.Abs(s2.X0 - tx) <= q * 2 && Math.Abs(s2.Y0 - ty) <= q * 2;
                    double nx2 = forward ? s2.X1 : s2.X0, ny2 = forward ? s2.Y1 : s2.Y0;
                    if (Math.Abs(nx2 - pts[0]) < q && Math.Abs(ny2 - pts[1]) < q) break; // 成环
                    pts.Add(nx2); pts.Add(ny2);
                    tx = nx2; ty = ny2;
                }
                var r = new Run { Dir = dir, Pts = pts };
                for (int i = 2; i < pts.Count; i += 2)
                    r.Length += Math.Sqrt(Math.Pow(pts[i] - pts[i - 2], 2) + Math.Pow(pts[i + 1] - pts[i - 1], 2));
                runs.Add(r);
            }
            return runs;
        }
    }
}
