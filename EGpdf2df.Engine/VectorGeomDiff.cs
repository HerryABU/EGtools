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
            /// <summary>本折线的原始序号（聚类后回溯用）。</summary>
            public int Index;
        }

        /// <summary>
        /// 一片"差异区域"：由邻近的若干条变更折线聚成。报告粒度用它比逐条折线实用得多——
        /// 实测全书变更折线可达 20 万条，逐条输出会让 JSON 与前端都不可用；聚成区域后
        /// 数量降到千级，且更贴近"这一片图面改了"的语义。
        /// </summary>
        public sealed class Cluster
        {
            public List<int> Runs = new();      // Run.Index 列表
            public double X0, Y0, X1, Y1;       // 内容空间包围盒
            public double Length;               // 折线总长
            public string Dir = "added";        // 多数方向
            public double Xc => (X0 + X1) / 2;
            public double Yc => (Y0 + Y1) / 2;
        }

        public sealed class Result
        {
            public List<Run> Runs = new();
            public List<Cluster> Clusters = new();
            public double Scale = 1, Dx = 0, Dy = 0;
            public int BaseSegs, TgtSegs, TgtMatched, BaseMatched;
            /// <summary>标定质量诊断（长线段中点命中率），供日志/排障。</summary>
            public double CalibHitRate;
            /// <summary>被最小长度过滤掉的噪声折线数（诊断用）。</summary>
            public int DroppedShortRuns;
        }

        // ------------------------------------------------------------------
        //  ① 几何标定
        // ------------------------------------------------------------------
        /// <summary>
        /// 求把 baseline 坐标映射到 target 坐标的 (scale,dx,dy)：tgt = base/scale + (dx,dy)。
        ///
        /// ⚠️ 标定必须**多候选 + 择优**，不能只做"由粗到细的单路搜索"：
        ///   ① 恒等候选：大量页面两版共享同一坐标系（同纸张尺寸、同源导出），此时 (1,0,0) 就是
        ///      精确解——实测多页恒等即 100% 命中。若先跑"大容差尺度扫描"，在 tol=15 下**任何**
        ///      尺度都能刷出 100% 命中，于是会挑到一个伪尺度（实测某页挑到 scale=0.9912），
        ///      把整页线画搬歪 10pt，覆盖率从 96% 掉到 56%，凭空产生上千条假差异。
        ///   ② bbox 候选：两版纸张尺寸/导出比例真的不同时（实测页 0 旧=2.01854×新）用它。
        ///   ③ 尺度扫描候选：仅当前两者都明显不够好时才启用（避免"任何尺度都 100%"的假象）。
        /// 三者都在**同一个小容差**下打分比较，选最优；同分优先更简单者（恒等 > bbox > 扫描）。
        /// </summary>
        public static (double s, double dx, double dy, double hitRate) Calibrate(
            List<VectorLinework.Seg> bsegs, List<VectorLinework.Seg> tsegs, double tol, double cosMin,
            Action<string>? log = null, bool quick = false)
        {
            var (bx0, by0, bx1, by1) = Span(bsegs);
            var (tx0, ty0, tx1, ty1) = Span(tsegs);
            double s0 = ((bx1 - bx0) + (by1 - by0)) / Math.Max(1e-9, (tx1 - tx0) + (ty1 - ty0));
            double dx0 = tx0 - bx0 / s0, dy0 = ty0 - by0 / s0;
            if (s0 <= 0 || double.IsNaN(s0) || double.IsInfinity(s0)) { s0 = 1; dx0 = dy0 = 0; }

            var probes = CollectProbes(bsegs, tol, quick);
            if (probes.Count == 0)
            {
                log?.Invoke($"  [vec] 标定: 无足够长的基准线段可做基准，退回 bbox 初值 (s={s0:F5})");
                return (s0, dx0, dy0, 0);
            }
            var index = new LineIndex(tsegs, Math.Max(1.0, tol));

            double Score(double s, double dx, double dy, double atol,
                         List<VectorLinework.Seg> ps)
            {
                int hit = 0;
                foreach (var g in ps)
                {
                    double mx = (g.X0 + g.X1) * 0.5 / s + dx;
                    double my = (g.Y0 + g.Y1) * 0.5 / s + dy;
                    if (index.Covered(mx, my, g.X1 - g.X0, g.Y1 - g.Y0, atol, cosMin)) hit++;
                }
                return ps.Count == 0 ? 0 : (double)hit / ps.Count;
            }

            // 候选打分统一用**配置容差**（绝不用大容差，否则任何尺度都满分）。
            double evalTol = Math.Max(0.5, tol);

            // ---- 候选① 恒等 ----
            double idScore = Score(1, 0, 0, evalTol, probes);
            log?.Invoke($"  [vec] 候选·恒等   命中={idScore * 100:F1}%");
            // 恒等已经几乎全中 → 两版同坐标系，直接采用（省时且避免被伪尺度带偏）。
            if (idScore >= 0.995)
            {
                log?.Invoke("  [vec] 采用恒等变换 (两版同坐标系)");
                return (1, 0, 0, idScore);
            }

            // ---- 候选② bbox 初值 + 平移精修（尺度固定）----
            var bbR = Refine(probes, Score, (s0, dx0, dy0), false, evalTol, tol);
            log?.Invoke($"  [vec] 候选·bbox   scale={bbR.s:F5} dx={bbR.dx:F2} dy={bbR.dy:F2} 命中={bbR.hit * 100:F1}%");

            // ---- 候选③ 尺度扫描（仅当前两者都不好）----
            (double s, double dx, double dy, double hit) scR = (s0, dx0, dy0, -1);
            double gate = quick ? 0.80 : 0.90;
            if (Math.Max(idScore, bbR.hit) < gate)
            {
                var coarse = CollectProbes(bsegs, Math.Max(5.0, tol * 5), quick, quick ? 120 : 400);
                var scanIndex = index;
                double ScoreC(double s, double dxx, double dyy, double atol)
                {
                    int hit = 0;
                    foreach (var g in coarse)
                    {
                        double mx = (g.X0 + g.X1) * 0.5 / s + dxx;
                        double my = (g.Y0 + g.Y1) * 0.5 / s + dyy;
                        if (scanIndex.Covered(mx, my, g.X1 - g.X0, g.Y1 - g.Y0, atol, cosMin)) hit++;
                    }
                    return coarse.Count == 0 ? 0 : (double)hit / coarse.Count;
                }
                double coarseTol = Math.Max(5.0, tol * 5);
                int half = quick ? 5 : 50;
                double rel = quick ? 0.002 : 0.0004;
                double bs = bbR.s, bxc = bbR.dx, byc = bbR.dy;
                double bestC = ScoreC(bs, bxc, byc, coarseTol);
                for (double k = -half; k <= half; k++)
                {
                    double s = s0 * (1 + k * rel);
                    if (s <= 0) continue;
                    double px = bxc, py = byc, loc = ScoreC(s, px, py, coarseTol);
                    double st = coarseTol;
                    for (int it = 0; it < 10; it++)
                    {
                        bool moved = false;
                        foreach (var (ax, ay) in new[] { (st, 0d), (-st, 0d), (0d, st), (0d, -st) })
                        {
                            double v = ScoreC(s, px + ax, py + ay, coarseTol);
                            if (v > loc) { loc = v; px += ax; py += ay; moved = true; }
                        }
                        if (!moved) st /= 2;
                        if (st < coarseTol * 0.05) break;
                    }
                    if (loc > bestC) { bestC = loc; bs = s; bxc = px; byc = py; }
                }
                // 扫描是在大容差下选出的"形似"解，必须回到配置容差下重新精修+打分才算数。
                var scRef = Refine(probes, Score, (bs, bxc, byc), true, evalTol, tol);
                log?.Invoke($"  [vec] 候选·扫描  scale={scRef.s:F5} dx={scRef.dx:F2} dy={scRef.dy:F2} 命中={scRef.hit * 100:F1}%");
                scR = scRef;
            }

            // ---- 择优（同分优先更简单者）----
            var chosen = idScore >= bbR.hit && idScore >= scR.hit
                ? ("恒等", (s: 1.0, dx: 0.0, dy: 0.0, hit: idScore))
                : bbR.hit >= scR.hit ? ("bbox", bbR) : ("扫描", scR);
            log?.Invoke($"  [vec] 标定采用 {chosen.Item1}: scale={chosen.Item2.s:F5} " +
                        $"dx={chosen.Item2.dx:F2} dy={chosen.Item2.dy:F2} 命中={chosen.Item2.hit * 100:F1}%");
            return (chosen.Item2.s, chosen.Item2.dx, chosen.Item2.dy, chosen.Item2.hit);
        }

        /// <summary>取"长线段"作为标定探针：方向明确、且不受两版切分粒度差异影响。</summary>
        static List<VectorLinework.Seg> CollectProbes(List<VectorLinework.Seg> segs, double tol, bool quick, int capOverride = 0)
        {
            var probes = new List<VectorLinework.Seg>();
            double minLen = Math.Max(20.0, tol * 8);
            foreach (var g in segs) if (g.Len >= minLen) probes.Add(g);
            int cap = capOverride > 0 ? capOverride : (quick ? 300 : 1500);
            if (probes.Count > cap)
            {
                int step = probes.Count / cap;
                var sub = new List<VectorLinework.Seg>();
                for (int i = 0; i < probes.Count; i += step) sub.Add(probes[i]);
                probes = sub;
            }
            return probes;
        }

        /// <summary>
        /// 固定/微调尺度做平移爬山（目标 = 长段中点被对侧线画覆盖的比例）。
        /// allowScaleStep=true 时允许尺度在小范围内微调（扫描候选需要）。
        /// </summary>
        static (double s, double dx, double dy, double hit) Refine(
            List<VectorLinework.Seg> probes,
            Func<double, double, double, double, List<VectorLinework.Seg>, double> score,
            (double s, double dx, double dy) init, bool allowScaleStep, double atol, double tol)
        {
            double bs = init.s, bx = init.dx, by = init.dy;
            double best = score(bs, bx, by, atol, probes);
            double stD = Math.Max(0.2, atol * 0.5);
            double stS = allowScaleStep ? Math.Abs(bs) * 0.0004 : 0;
            for (int iter = 0; iter < 140; iter++)
            {
                bool moved = false;
                var deltas = stS > 0
                    ? new[] { (stS, 0d, 0d), (-stS, 0d, 0d), (0d, stD, 0d), (0d, -stD, 0d), (0d, 0d, stD), (0d, 0d, -stD) }
                    : new[] { (0d, stD, 0d), (0d, -stD, 0d), (0d, 0d, stD), (0d, 0d, -stD) };
                foreach (var (ds, ddx, ddy) in deltas)
                {
                    double v = score(bs + ds, bx + ddx, by + ddy, atol, probes);
                    if (v > best + 1e-9) { best = v; bs += ds; bx += ddx; by += ddy; moved = true; }
                }
                if (!moved) { stD /= 2; stS /= 2; }
                if (stD < 0.01 && stS < 1e-6) break;
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
                    int np = Math.Max(1, (int)Math.Ceiling(len / _cell));
                    for (int p = 0; p < np; p++)
                    {
                        double t0 = (double)p / np, t1 = (double)(p + 1) / np;
                        double cx0 = s.X0 + (s.X1 - s.X0) * t0, cy0 = s.Y0 + (s.Y1 - s.Y0) * t0;
                        double cx1 = s.X0 + (s.X1 - s.X0) * t1, cy1 = s.Y0 + (s.Y1 - s.Y0) * t1;
                        int ci = _chunks.Count;
                        _chunks.Add(new[] { cx0, cy0, cx1, cy1 });
                        // ⚠️ chunk 必须登记到它**覆盖到的所有格子**，而不是只登记中点所在格。
                        // 只登记中点格 + 查询 ±1 圈，会让"距离 ≤ 容差但中点落在隔 2 格"的线段查不到
                        // → 明明有线画对得上却被判成新增/删除（实测页 150:151 有 26 条这样的假差异，
                        // 未匹配段到对侧线画的最近距离中位数只有 0.05pt = 几乎完全重合）。
                        RegisterCellRange(cx0, cy0, cx1, cy1, ci);
                    }
                }
            }

            void RegisterCellRange(double x0, double y0, double x1, double y1, int ci)
            {
                long gx0 = (long)Math.Floor(Math.Min(x0, x1) / _cell), gx1 = (long)Math.Floor(Math.Max(x0, x1) / _cell);
                long gy0 = (long)Math.Floor(Math.Min(y0, y1) / _cell), gy1 = (long)Math.Floor(Math.Max(y0, y1) / _cell);
                for (long gx = gx0; gx <= gx1; gx++)
                    for (long gy = gy0; gy <= gy1; gy++)
                    {
                        long k = CellKey(gx, gy);
                        if (!_grid.TryGetValue(k, out var l)) _grid[k] = l = new List<int>();
                        l.Add(ci);
                    }
            }

            static long CellKey(long cx, long cy) => (cx + 0x4000000L) * 0x8000000L + (cy + 0x4000000L);

            /// <summary>
            /// 查询半径 reach 需要搜多少圈格子。chunk 已登记到自身覆盖的所有格子，故只需
            /// ceil(reach/cell) + 1（+1 是查询点自身格子与坐标量化的裕度）。
            /// </summary>
            int Rings(double reach) => Math.Min(24, Math.Max(1, (int)Math.Ceiling(Math.Max(0, reach) / _cell) + 1));

            /// <summary>(x,y) 附近 tol 内是否存在与 (ux,uy) 方向相近(cos ≥ cosMin) 的线段。</summary>
            public bool Covered(double x, double y, double ux, double uy, double tol, double cosMin)
            {
                double ul = Math.Sqrt(ux * ux + uy * uy);
                if (ul < 1e-12) return false;
                ux /= ul; uy /= ul;
                long bx = (long)Math.Floor(x / _cell), by = (long)Math.Floor(y / _cell);
                int rings = Rings(tol);
                for (long gx = bx - rings; gx <= bx + rings; gx++)
                    for (long gy = by - rings; gy <= by + rings; gy++)
                    {
                        if (!_grid.TryGetValue(CellKey(gx, gy), out var l)) continue;
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
            /// 找不到返回 cap。
            /// </summary>
            public double Nearest(double x, double y, double cap)
            {
                double bd = cap;
                long bx = (long)Math.Floor(x / _cell), by = (long)Math.Floor(y / _cell);
                int rings = Rings(cap);
                for (long gx = bx - rings; gx <= bx + rings; gx++)
                    for (long gy = by - rings; gy <= by + rings; gy++)
                    {
                        if (!_grid.TryGetValue(CellKey(gx, gy), out var l)) continue;
                        foreach (int ci in l)
                        {
                            var c = _chunks[ci];
                            double d = DistPointSeg(x, y, c[0], c[1], c[2], c[3]);
                            if (d < bd) bd = d;
                        }
                    }
                return bd;
            }

            /// <summary>
            /// 最近且**方向相近**的线段距离（cap 为搜索上限）。用于"第三态变更"配对：
            /// 只有同向的邻居才算"同一根线被改了"，避免拿一根垂直交叉的线来配对。
            /// </summary>
            public double NearestDir(double x, double y, double ux, double uy, double cap, double cosMin)
            {
                double ul = Math.Sqrt(ux * ux + uy * uy);
                double bd = cap;
                if (ul < 1e-12) return bd;
                ux /= ul; uy /= ul;
                long bx = (long)Math.Floor(x / _cell), by = (long)Math.Floor(y / _cell);
                int rings = Rings(cap);
                for (long gx = bx - rings; gx <= bx + rings; gx++)
                    for (long gy = by - rings; gy <= by + rings; gy++)
                    {
                        if (!_grid.TryGetValue(CellKey(gx, gy), out var l)) continue;
                        foreach (int ci in l)
                        {
                            var c = _chunks[ci];
                            double vx = c[2] - c[0], vy = c[3] - c[1];
                            double vl = Math.Sqrt(vx * vx + vy * vy);
                            if (vl > 1e-12 && Math.Abs((ux * vx + uy * vy) / vl) < cosMin) continue;
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
                                 double tol, double angTolDeg, Action<string>? log = null, bool quick = false,
                                 FilterCfg? filter = null, double changeRadius = 0, double nearInkTol = 0.5)
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
                if (FullyCovered(g, bIndex, tol, cosMin, nearInkTol)) tMatched++;
                else tUnmatched.Add(g);
            res.TgtMatched = tMatched;

            var bUnmatched = new List<VectorLinework.Seg>();
            int bMatched = 0;
            foreach (var g in bMoved)
                if (FullyCovered(g, tIndex, tol, cosMin, nearInkTol)) bMatched++;
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
                log?.Invoke($"  [vec] 覆盖率: 目标 {tMatched * 100.0 / tsegs.Count:F2}% 未变, " +
                            $"基准 {bMatched * 100.0 / bMoved.Count:F2}% 未变");
            }

            // 详细取样（排障用）：把未匹配段自身长度 / 中点距离 / 两端点距离打出来，
            // 一眼区分"真差异"与"端点悬垂误报"（后者中点距离≈0 而端点距离≈悬垂长度）。
            if (Environment.GetEnvironmentVariable("EGPDF2DF_VEC_DUMP") == "1" && tUnmatched.Count > 0)
            {
                log?.Invoke($"  [vec] 未匹配目标段取样(最多8条): len | 中点距 | 起点距 | 终点距");
                int shown = 0, step = Math.Max(1, tUnmatched.Count / 8);
                for (int i = 0; i < tUnmatched.Count && shown < 8; i += step, shown++)
                {
                    var g = tUnmatched[i];
                    double mx = (g.X0 + g.X1) * 0.5, my = (g.Y0 + g.Y1) * 0.5;
                    double dm = bIndex.Nearest(mx, my, tol * 4);
                    double d0 = bIndex.Nearest(g.X0, g.Y0, tol * 4);
                    double d1 = bIndex.Nearest(g.X1, g.Y1, tol * 4);
                    log?.Invoke($"  [vec]   len={g.Len,8:F2} | {dm,6:F2} | {d0,6:F2} | {d1,6:F2}   " +
                                $"({g.X0:F2},{g.Y0:F2})-({g.X1:F2},{g.Y1:F2})");
                }
            }

            // 第三态识别：把"同一位置、形状改了"的线段从 added/removed 里分出来。
            // 否则一根线稍微挪动/改形状，会同时报"这里删了一根 + 这里加了一根"（红），
            // 而实际语义是"这根线改了"（绿）。
            // 判据：一条未匹配目标段，在其半径内**存在方向相近的未匹配基准段** → 判为变更；
            //       反向同理。两侧各自独立判定，再分别成链。
            int nChangedT = 0, nChangedB = 0;
            if (changeRadius > 0 && tUnmatched.Count > 0 && bUnmatched.Count > 0)
            {
                var tIdx = new LineIndex(tUnmatched, Math.Max(1.0, changeRadius));
                var bIdx = new LineIndex(bUnmatched, Math.Max(1.0, changeRadius));

                var tChanged = new List<VectorLinework.Seg>();
                var tStill = new List<VectorLinework.Seg>();
                foreach (var g in tUnmatched)
                {
                    double mx = (g.X0 + g.X1) * 0.5, my = (g.Y0 + g.Y1) * 0.5;
                    double d = bIdx.NearestDir(mx, my, g.X1 - g.X0, g.Y1 - g.Y0, changeRadius, cosMin);
                    if (d < changeRadius) tChanged.Add(g); else tStill.Add(g);
                }
                var bChanged = new List<VectorLinework.Seg>();
                var bStill = new List<VectorLinework.Seg>();
                foreach (var g in bUnmatched)
                {
                    double mx = (g.X0 + g.X1) * 0.5, my = (g.Y0 + g.Y1) * 0.5;
                    double d = tIdx.NearestDir(mx, my, g.X1 - g.X0, g.Y1 - g.Y0, changeRadius, cosMin);
                    if (d < changeRadius) bChanged.Add(g); else bStill.Add(g);
                }
                nChangedT = tChanged.Count; nChangedB = bChanged.Count;

                // 变更只画【目标侧】的新形状（绿色）——语义是"这根线现在长这样"。
                // 基准侧的旧形状不再单独出图，避免与绿色重叠造成"又红又绿"的视觉噪音。
                res.Runs.AddRange(Chain(tChanged, "changed", tol));
                res.Runs.AddRange(Chain(tStill, "added", tol));
                res.Runs.AddRange(Chain(bStill, "removed", tol));
                log?.Invoke($"  [vec] 第三态变更: 目标侧 {nChangedT}/{tUnmatched.Count} 段、基准侧 {nChangedB}/{bUnmatched.Count} 段" +
                            $" 与其对侧同向邻近 → 判为变更(r={changeRadius:F1}pt)");
            }
            else
            {
                res.Runs.AddRange(Chain(tUnmatched, "added", tol));
                res.Runs.AddRange(Chain(bUnmatched, "removed", tol));
            }
            for (int i = 0; i < res.Runs.Count; i++) res.Runs[i].Index = i;
            // 密度过滤 + 区域聚类（都可调；参数为 0/false 时保持逐条原样）
            ApplyFilterAndCluster(res, filter, log);
            return res;
        }

        // ------------------------------------------------------------------
        //  密度过滤 + 区域聚类
        // ------------------------------------------------------------------
        /// <summary>
        /// 过滤/聚类参数。全部可经配置文件 / 前端调整；取默认值时行为等同"不过滤、逐条"。
        /// 目的：CAD 图纸一次修订常改动成千上万条线，逐条输出既不可读也压垮前端；
        /// 聚成"差异区域"后数量降到千级，并可丢弃长度极短的抗锯齿级噪声。
        /// </summary>
        public sealed class FilterCfg
        {
            /// <summary>丢弃总长小于此值(pt)的变更折线。0=不过滤。建议 2~10 可滤掉碎屑。</summary>
            public double MinRunLengthPt = 0.0;
            /// <summary>把包围盒间距小于此值(pt)的折线聚成同一区域。0=不聚类（逐条）。</summary>
            public double ClusterRadiusPt = 0.0;
            /// <summary>丢弃"折线总长"小于此值(pt)的区域。0=不过滤。用于把零散微调归并/忽略。</summary>
            public double MinClusterLengthPt = 0.0;
            /// <summary>丢弃折线条数少于此值的区域。0=不过滤。</summary>
            public int MinClusterRuns = 0;
        }

        static void ApplyFilterAndCluster(Result res, FilterCfg? f, Action<string>? log)
        {
            if (f == null) return;

            // ① 长度过滤
            if (f.MinRunLengthPt > 0)
            {
                int before = res.Runs.Count;
                res.Runs.RemoveAll(r => r.Length < f.MinRunLengthPt);
                res.DroppedShortRuns = before - res.Runs.Count;
                for (int i = 0; i < res.Runs.Count; i++) res.Runs[i].Index = i;
                log?.Invoke($"  [vec] 长度过滤: 丢弃总长<{f.MinRunLengthPt:F1}pt 的折线 {res.DroppedShortRuns} 条，余 {res.Runs.Count} 条");
            }
            if (res.Runs.Count == 0) return;

            // ② 聚类（不聚类时每簇=一条折线，便于统一后续过滤/报告）
            if (f.ClusterRadiusPt > 0) res.Clusters = ClusterRuns(res.Runs, f.ClusterRadiusPt);
            else
            {
                res.Clusters = new List<Cluster>(res.Runs.Count);
                foreach (var r in res.Runs) res.Clusters.Add(OneClusterOf(r));
            }

            // ③ 区域级过滤
            if (f.MinClusterLengthPt > 0 || f.MinClusterRuns > 0)
            {
                int before = res.Clusters.Count;
                res.Clusters.RemoveAll(c =>
                    c.Length < f.MinClusterLengthPt || c.Runs.Count < f.MinClusterRuns);
                log?.Invoke($"  [vec] 区域过滤: 丢弃 {before - res.Clusters.Count} 个区域" +
                            $"(阈值 总长≥{f.MinClusterLengthPt:F1}pt 且 线数≥{f.MinClusterRuns})，余 {res.Clusters.Count} 个");
            }
            log?.Invoke($"  [vec] 聚类: {res.Runs.Count} 条折线 → {res.Clusters.Count} 个差异区域");
        }

        static Cluster OneClusterOf(Run r)
        {
            var c = new Cluster { Dir = r.Dir, Length = r.Length };
            c.Runs.Add(r.Index);
            double x0 = double.MaxValue, y0 = double.MaxValue, x1 = double.MinValue, y1 = double.MinValue;
            for (int k = 0; k + 1 < r.Pts.Count; k += 2)
            {
                x0 = Math.Min(x0, r.Pts[k]); x1 = Math.Max(x1, r.Pts[k]);
                y0 = Math.Min(y0, r.Pts[k + 1]); y1 = Math.Max(y1, r.Pts[k + 1]);
            }
            if (x0 > x1) { x0 = y0 = x1 = y1 = 0; }
            c.X0 = x0; c.Y0 = y0; c.X1 = x1; c.Y1 = y1;
            return c;
        }

        /// <summary>
        /// 按包围盒邻近关系聚类（union-find + 网格加速）。半径 r 内互相接近的折线归为同一区域，
        /// 这样一片连续改动的图面会收敛成一个区域，而不是成千上万条线段。
        /// </summary>
        static List<Cluster> ClusterRuns(List<Run> runs, double r)
        {
            int n = runs.Count;
            var parent = new int[n];
            for (int i = 0; i < n; i++) parent[i] = i;
            int Find(int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; }
            void Union(int a, int b) { a = Find(a); b = Find(b); if (a != b) parent[b] = a; }

            // 每条折线的包围盒
            var bb = new double[n][];
            for (int i = 0; i < n; i++)
            {
                double x0 = double.MaxValue, y0 = double.MaxValue, x1 = double.MinValue, y1 = double.MinValue;
                var p = runs[i].Pts;
                for (int k = 0; k + 1 < p.Count; k += 2)
                {
                    x0 = Math.Min(x0, p[k]); x1 = Math.Max(x1, p[k]);
                    y0 = Math.Min(y0, p[k + 1]); y1 = Math.Max(y1, p[k + 1]);
                }
                if (x0 > x1) { x0 = y0 = x1 = y1 = 0; }
                bb[i] = new[] { x0, y0, x1, y1 };
            }

            // 网格：把包围盒按 cell 号覆盖的所有格子登记，同格即为候选邻居
            double cell = Math.Max(0.5, r);
            var grid = new Dictionary<long, List<int>>();
            long Key(long cx, long cy) => (cx + 0x4000000L) * 0x8000000L + (cy + 0x4000000L);
            for (int i = 0; i < n; i++)
            {
                long cx0 = (long)Math.Floor((bb[i][0] - r) / cell), cx1 = (long)Math.Floor((bb[i][2] + r) / cell);
                long cy0 = (long)Math.Floor((bb[i][1] - r) / cell), cy1 = (long)Math.Floor((bb[i][3] + r) / cell);
                // 超长折线（跨很多格）限制登记范围，避免病态膨胀
                if ((cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 4096)
                { cx1 = Math.Min(cx1, cx0 + 63); cy1 = Math.Min(cy1, cy0 + 63); }
                for (long cx = cx0; cx <= cx1; cx++)
                    for (long cy = cy0; cy <= cy1; cy++)
                    {
                        long k = Key(cx, cy);
                        if (!grid.TryGetValue(k, out var l)) grid[k] = l = new List<int>();
                        l.Add(i);
                    }
            }
            foreach (var kv in grid)
            {
                var l = kv.Value;
                for (int a = 1; a < l.Count; a++)
                {
                    int i = l[0], j = l[a];
                    // 包围盒间距 ≤ r 才合并（同格不等于够近）
                    double gx = Math.Max(0, Math.Max(bb[i][0] - bb[j][2], bb[j][0] - bb[i][2]));
                    double gy = Math.Max(0, Math.Max(bb[i][1] - bb[j][3], bb[j][1] - bb[i][3]));
                    if (Math.Sqrt(gx * gx + gy * gy) <= r) Union(i, j);
                }
            }

            var groups = new Dictionary<int, Cluster>();
            var dirCount = new Dictionary<int, Dictionary<string, int>>();
            for (int i = 0; i < n; i++)
            {
                int root = Find(i);
                if (!groups.TryGetValue(root, out var c))
                {
                    c = new Cluster { X0 = double.MaxValue, Y0 = double.MaxValue, X1 = double.MinValue, Y1 = double.MinValue };
                    groups[root] = c;
                    dirCount[root] = new Dictionary<string, int>();
                }
                c.Runs.Add(i);
                c.Length += runs[i].Length;
                c.X0 = Math.Min(c.X0, bb[i][0]); c.X1 = Math.Max(c.X1, bb[i][2]);
                c.Y0 = Math.Min(c.Y0, bb[i][1]); c.Y1 = Math.Max(c.Y1, bb[i][3]);
                var dc = dirCount[root];
                dc.TryGetValue(runs[i].Dir, out int cnt); dc[runs[i].Dir] = cnt + 1;
            }
            // 区域方向 = 区域内折线的多数方向。⚠️ 必须显式支持 changed：
            // 否则第三态识别出来的绿色"变更"在聚类后会被吞掉（区域永远只报 added/removed）。
            // 同数时优先级 changed > removed > added（changed 语义最强，最不该被降级）。
            var prio = new Dictionary<string, int> { ["changed"] = 2, ["removed"] = 1, ["added"] = 0 };
            foreach (var kv in groups)
            {
                var dc = dirCount[kv.Key];
                string best = "added"; int bestN = -1;
                foreach (var d in dc)
                {
                    if (d.Value > bestN || (d.Value == bestN && prio.GetValueOrDefault(d.Key) > prio.GetValueOrDefault(best)))
                    { best = d.Key; bestN = d.Value; }
                }
                kv.Value.Dir = best;
            }
            var outp = new List<Cluster>(groups.Values);
            outp.Sort((a, b) => b.Length.CompareTo(a.Length));   // 大改动排前
            return outp;
        }

        // 沿线段采样（含两端与中点），要求每个采样点都被对侧线画覆盖。
        // 采样步长随长度自适应：短段 2 点即可，长段按 tol×2 加密，避免"跨过一小段缺口"被误判为覆盖。
        //
        // ⚠️ 两级判据（第二级 = "墨迹重合豁免"，不是放宽容差）：
        //   ① 平行覆盖：该点附近 tol 内存在**方向相近**的对侧线段 → 未变。方向检验用于防止
        //      密集区被"横穿而过"的交叉线误判为覆盖。
        //   ② 墨迹重合：若 ① 失败，但该点距**任意方向**的对侧线画 < nearInkTol（默认 0.5pt，
        //      远小于 tol），仍判未变。理由：CAD 的圆弧是密排短线逼近出来的，单段可能只有
        //      0.4~0.7pt，**这种长度上的方向本身就不可靠**（实测未匹配段方向 0°/24°/45°/66° 混杂，
        //      而它们距对侧墨迹只有 0.00~0.27pt = 完全重合）。此时若硬按方向判，会把同一根圆弧
        //      的两套不同逼近方式判成"新增+删除"，凭空产生成片的假差异（实测页 150:151 有 26 条）。
        //      注意：这不是把 tol 放宽——② 只在"墨迹实际重合到亚 pt"时才生效，且长线段需全部
        //      采样点都满足（真正的长线改动必然被 ① 拒掉，不会被②放过）。
        static bool FullyCovered(VectorLinework.Seg g, LineIndex idx, double tol, double cosMin, double nearInkTol)
        {
            double len = g.Len;
            int n = Math.Max(2, (int)Math.Ceiling(len / Math.Max(0.8, tol * 2)) + 1);
            if (n > 64) n = 64;
            double ux = g.X1 - g.X0, uy = g.Y1 - g.Y0;

            for (int i = 0; i < n; i++)
            {
                double t = (double)i / (n - 1);
                double x = g.X0 + ux * t, y = g.Y0 + uy * t;
                if (!idx.Covered(x, y, ux, uy, tol, cosMin)) goto fallback;
            }
            return true;

        fallback:
            if (nearInkTol <= 0) return false;
            // 稀疏复核（控成本）：每个采样点附近 nearInkTol 内是否都有对侧墨迹（不限方向）。
            int m = Math.Min(6, n);
            for (int i = 0; i < m; i++)
            {
                double t = m == 1 ? 0 : (double)i / (m - 1);
                double x = g.X0 + ux * t, y = g.Y0 + uy * t;
                if (idx.Nearest(x, y, nearInkTol) >= nearInkTol) return false;
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
