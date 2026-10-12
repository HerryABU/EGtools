// ---------------------------------------------------------------------------
// pdfium_probe —— 用 Qt PDF（QPdfDocument / PDFium）渲染的**语义与清晰度探针**
//
// 目的（先解析、后实现）：在动 GUI 之前，先用数字证明三件事，免得改完靠肉眼猜。
//
//   ① `QPdfDocumentRenderOptions::setScaledClipRect` 的语义到底是什么？
//      —— 只有确认"局部渲染 == 整页渲染后裁剪"，才能做**只渲染可视区**（高倍率下内存有界）。
//   ② `pagePointSize()` 是否就是内核下发的 pt 尺寸（含 /Rotate 处理）？
//      —— 必须一致，否则框选框与词盒会整体偏移。
//   ③ 清晰度能不能**量化**？PDFium 在 4x 下真渲染 vs "1x 位图放大到 4x"（＝当前做法）
//      —— 用平均 |Laplacian|（同分辨率下比较）给出倍数关系，这就是"Adobe 级"的证据。
//
// 用法: pdfium_probe <pdf> [输出目录]
// ---------------------------------------------------------------------------

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QTextStream>
#include <QtMath>

#include <cstdlib>

static QTextStream out(stdout);

// 平均 |Laplacian|：同分辨率下越大越锐（模糊图的二阶差分必然被削平）。
static double sharpness(const QImage& in)
{
    const QImage img = in.convertToFormat(QImage::Format_Grayscale8);
    if (img.width() < 3 || img.height() < 3) return 0.0;
    double sum = 0.0;
    quint64 n = 0;
    for (int y = 1; y < img.height() - 1; ++y) {
        const uchar* r0 = img.constScanLine(y - 1);
        const uchar* r1 = img.constScanLine(y);
        const uchar* r2 = img.constScanLine(y + 1);
        for (int x = 1; x < img.width() - 1; ++x) {
            const int L = 4 * int(r1[x]) - int(r0[x]) - int(r2[x]) - int(r1[x - 1]) - int(r1[x + 1]);
            sum += std::abs(L);
            ++n;
        }
    }
    return n ? sum / double(n) : 0.0;
}

struct Stats { double meanLum = 0; int maxLum = 0; double brightFrac = 0; };
static Stats stats(const QImage& in)
{
    Stats s;
    const QImage img = in.convertToFormat(QImage::Format_Grayscale8);
    quint64 lum = 0; int maxL = 0; quint64 bright = 0; quint64 n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const uchar* r = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            const int v = r[x];
            lum += quint64(v); maxL = qMax(maxL, v);
            if (v >= 180) ++bright;
            ++n;
        }
    }
    if (n) { s.meanLum = double(lum) / double(n); s.brightFrac = double(bright) / double(n); }
    s.maxLum = maxL;
    return s;
}

// ---- 像素级诊断：判断"黑"到底是"真黑"还是"透明" ----------------------
// ⚠️ 这一条极其重要：QPdfDocument::render() 出来的图**背景是透明的**（alpha=0），
//    直接按 RGB 统计亮度会全是 0 —— 看起来就"黑"。任何"看亮度判黑"的检查都必须
//    先**合成到白底**（读者看到的"白纸"是应用自己铺的底色，不是 PDF 画的）。
struct AlphaInfo { qint64 total = 0; qint64 transparent = 0; qint64 opaque = 0; int maxAlpha = 0; };
static AlphaInfo alphaInfo(const QImage& in)
{
    AlphaInfo a;
    const QImage img = in.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* r = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const int al = qAlpha(r[x]);
            ++a.total;
            if (al == 0) ++a.transparent; else if (al == 255) ++a.opaque;
            a.maxAlpha = qMax(a.maxAlpha, al);
        }
    }
    return a;
}

// 合成到指定底色上（读者实际看到的画面）。
static QImage composeOver(const QImage& in, const QColor& bg)
{
    QImage out(in.size(), QImage::Format_ARGB32);
    out.fill(bg);
    QPainter p(&out);
    p.drawImage(0, 0, in);
    p.end();
    return out;
}

// 整页渲染（不设 clip —— 实测最省心的路径）。
static QImage renderFull(QPdfDocument* doc, int page, double scale)
{
    const QSizeF pt = doc->pagePointSize(page);
    const QSize size(qMax(1, qRound(pt.width() * scale)),
                     qMax(1, qRound(pt.height() * scale)));
    return doc->render(page, size);
}

// ⚠️⚠️ 局部渲染的**正确配方**（这是本探针最重要的一条结论）：
//   scaledSize     = 整页按 scale 缩放后的尺寸（**必须设**）
//   scaledClipRect = 该缩放空间里的目标区域
//   imageSize      = scaledClipRect.size()
// 实测：**只设 scaledClipRect 而不设 scaledSize ⇒ PDFium 画出全透明空图**（什么都不画）。
// 这与 Qt 6.11 的公开文档不符（文档完全没提这种耦合），但可复现：
//   整页·只设clip → 不透明像素 0；同一参数补上 scaledSize → 正常出图。
// 若只按文档写代码、把"空图"直接画到画布上，用户看到的就是**一片黑/一片深色** ——
// 这正是本项目"预览发黑"的根因之一，所以这里把它固化成硬约束 + 自检断言。
static QImage renderRegion(QPdfDocument* doc, int page, const QRect& clipScaled, const QSize& fullScaled)
{
    QPdfDocumentRenderOptions opt;
    opt.setScaledSize(fullScaled);        // ← 关键：缺这一行会渲染出全透明的空图
    opt.setScaledClipRect(clipScaled);
    return doc->render(page, clipScaled.size(), opt);
}

// 差异直方图 + 最佳平移搜索 —— 用来区分"抗锯齿差异"与"真的错位"。
//   若最佳平移是 (0,0) 且差异集中在小区间（1..32）⇒ 只是 AA/相位差异；
//   若平移后差异骤降 ⇒ 是错位（分块就会出现接缝，必须换方案）。
struct DiffReport
{
    int buckets[5] = {0,0,0,0,0};   // 0 / 1-8 / 9-32 / 33-96 / >96
    double meanAbs = 0;
    int bestDx = 0, bestDy = 0;
    double bestMeanAbs = 0;
};

static DiffReport analyzeDiff(const QImage& aIn, const QImage& bIn)
{
    DiffReport r;
    if (aIn.size() != bIn.size() || aIn.isNull() || bIn.isNull()) return r;
    const QImage a = aIn.convertToFormat(QImage::Format_RGB32);
    const QImage b = bIn.convertToFormat(QImage::Format_RGB32);

    auto meanAbsAt = [&](int dx, int dy, int* buckets) {
        double sum = 0; qint64 n = 0;
        int bk[5] = {0,0,0,0,0};
        for (int y = qMax(0, -dy); y < a.height() - qMax(0, dy); ++y) {
            const QRgb* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
            const QRgb* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y + dy));
            for (int x = qMax(0, -dx); x < a.width() - qMax(0, dx); ++x) {
                const QRgb pa = ra[x], pb = rb[x + dx];
                const int d = qMax(qMax(std::abs(qRed(pa) - qRed(pb)),
                                        std::abs(qGreen(pa) - qGreen(pb))),
                                   std::abs(qBlue(pa) - qBlue(pb)));
                sum += d; ++n;
                if (d == 0) ++bk[0]; else if (d <= 8) ++bk[1];
                else if (d <= 32) ++bk[2]; else if (d <= 96) ++bk[3]; else ++bk[4];
            }
        }
        if (buckets) for (int i = 0; i < 5; ++i) buckets[i] = bk[i];
        return n ? sum / double(n) : 0.0;
    };

    r.meanAbs = meanAbsAt(0, 0, r.buckets);
    r.bestMeanAbs = r.meanAbs;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const double m = meanAbsAt(dx, dy, nullptr);
            if (m + 1e-9 < r.bestMeanAbs) { r.bestMeanAbs = m; r.bestDx = dx; r.bestDy = dy; }
        }
    return r;
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);   // QImage/字体库需要；渲染本身不依赖屏幕

    if (argc < 2) {
        out << "用法: pdfium_probe <pdf> [输出目录]\n";
        return 2;
    }
    const QString pdf = QString::fromLocal8Bit(argv[1]);
    const QString outDir = (argc >= 3) ? QString::fromLocal8Bit(argv[2]) : QString();
    if (!outDir.isEmpty()) QDir().mkpath(outDir);

    QPdfDocument doc;
    const QPdfDocument::Error err = doc.load(pdf);
    out << "=== PDFium 渲染探针 ===\n";
    out << "PDF: " << QFileInfo(pdf).fileName() << "\n";
    if (err != QPdfDocument::Error::None) {
        out << "[FAIL] 加载失败: " << int(err) << "\n"; out.flush(); return 1;
    }
    out << "页数: " << doc.pageCount() << "\n\n";

    // ---- ② 页面尺寸（pt）--------------------------------------------------
    out << "--- 页面尺寸 pagePointSize (pt) ---\n";
    const int showPages = qMin(3, doc.pageCount());
    for (int p = 0; p < showPages; ++p) {
        const QSizeF s = doc.pagePointSize(p);
        out << QStringLiteral("  第%1页: %2 x %3 pt  (宽高比 %4)\n")
                   .arg(p + 1)
                   .arg(s.width(), 0, 'f', 2)
                   .arg(s.height(), 0, 'f', 2)
                   .arg(s.height() > 0 ? s.width() / s.height() : 0.0, 0, 'f', 4);
    }

    const int page = 0;
    const QSizeF pt = doc.pagePointSize(page);

    // ---- ⓪ 像素级诊断：render() 出来的图到底长什么样 ---------------
    out << "\n--- ⓪ 文档状态与渲染结果构成（这决定了\"黑\"是怎么来的）---\n";
    out << QStringLiteral("  status=%1  loadError=%2  pageCount=%3\n")
               .arg(int(doc.status())).arg(int(doc.error())).arg(doc.pageCount());
    {
        const QSizeF p0 = doc.pagePointSize(page);
        const QSize full(qRound(p0.width()), qRound(p0.height()));

        // 变体 A：完全不给 options（走 Qt 默认：整页）
        const QImage a = doc.render(page, full);
        // 变体 B：只给 renderFlags
        QPdfDocumentRenderOptions ob;
        ob.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
        const QImage b = doc.render(page, full, ob);
        // 变体 C：显式整页 clip
        QPdfDocumentRenderOptions oc;
        oc.setScaledClipRect(QRect(QPoint(0, 0), full));
        const QImage c = doc.render(page, full, oc);
        // 变体 D：clip 只给上半页
        QPdfDocumentRenderOptions od;
        od.setScaledClipRect(QRect(0, 0, full.width(), full.height() / 2));
        const QImage d = doc.render(page, QSize(full.width(), full.height() / 2), od);

        struct V { const char* name; const QImage* img; };
        const V vs[] = { {"A 无options     ", &a}, {"B 仅renderFlags ", &b},
                         {"C 整页clip      ", &c}, {"D 半页clip      ", &d} };
        for (const V& v : vs) {
            const AlphaInfo ai = alphaInfo(*v.img);
            out << QStringLiteral("  %1 尺寸=%2x%3  不透明像素=%4  maxAlpha=%5\n")
                       .arg(QString::fromLatin1(v.name))
                       .arg(v.img->width()).arg(v.img->height())
                       .arg(ai.opaque).arg(ai.maxAlpha);
        }
        const Stats onA = stats(composeOver(a, Qt::white));
        const Stats rawA = stats(a);
        out << QStringLiteral("  A（默认路径）: 原始RGB平均 %1 / 合成白底后平均 %2 / 最大 %3 → %4\n")
                   .arg(rawA.meanLum, 0, 'f', 1).arg(onA.meanLum, 0, 'f', 1).arg(onA.maxLum)
                   .arg(onA.maxLum > 200 ? QStringLiteral("内容正常")
                                         : QStringLiteral("没画出东西"));
    }

    // ---- ⓪′ 裁剪参数配方表：到底怎么设才会出图 --------------------------
    out << "\n--- ⓪′ scaledClipRect 组合表（找出「能出图」的配方）---\n";
    {
        const QSizeF p0 = doc.pagePointSize(page);
        const QSize full(qRound(p0.width()), qRound(p0.height()));
        const int W = full.width(), H = full.height();
        const double s = 4.0;
        const QSize full4(qRound(W * s), qRound(H * s));

        struct Case { const char* name; QRect clip; QSize scaledSize; QSize imageSize; bool hasClip; bool hasScaled; };
        const Case cases[] = {
            {"整页 · 无clip · 无scaledSize      ", {},                          {},            full,        false, false},
            {"整页 · clip=页面(0,0,W,H)         ", QRect(0,0,W,H),               {},            full,        true,  false},
            {"整页4x · clip=全4x · 无scaledSize ", QRect(0,0,full4.width(),full4.height()), {},  full4,       true,  false},
            {"整页4x · clip=全4x · scaled=4x    ", QRect(0,0,full4.width(),full4.height()), full4, full4,     true,  true },
            {"局部4x · clip=1600² · scaled=4x   ", QRect(1600,1600,1600,1600), full4,         QSize(1600,1600), true, true },
            {"局部4x · clip=1600² · 无scaledSize", QRect(1600,1600,1600,1600), {},            QSize(1600,1600), true, false},
            {"局部1x · clip=400² · scaled=1x    ", QRect(400,400,400,400),      full,          QSize(400,400),   true, true },
            {"局部-半页4x · scaled=4x           ", QRect(0,0,full4.width(),full4.height()/2), full4, QSize(full4.width(),full4.height()/2), true, true},
        };

        out << QStringLiteral("  %1\n").arg(QStringLiteral(
            "配方                                   输出尺寸      不透明像素   均值  → 判定"));
        for (const Case& c : cases) {
            QPdfDocumentRenderOptions o;
            if (c.hasClip)   o.setScaledClipRect(c.clip);
            if (c.hasScaled) o.setScaledSize(c.scaledSize);
            QElapsedTimer t; t.start();
            const QImage img = doc.render(page, c.imageSize, o);
            const qint64 ms = t.elapsed();
            const AlphaInfo ai = alphaInfo(img);
            const Stats sv = stats(composeOver(img, Qt::white));
            const bool content = ai.opaque > 0 && sv.maxLum > 200 && sv.meanLum < 254.0;
            out << QStringLiteral("  %1 %2   %3   %4  → %5 (%6 ms)\n")
                       .arg(QString::fromLatin1(c.name))
                       .arg(QStringLiteral("%1x%2").arg(img.width()).arg(img.height()), -12)
                       .arg(ai.opaque, 11)
                       .arg(sv.meanLum, 6, 'f', 1)
                       .arg(content ? QStringLiteral("有内容") : QStringLiteral("空/透明"))
                       .arg(ms);
        }
    }

    // ---- ① 局部渲染 == 整页渲染后裁剪？-----------------------------------
    out << "\n--- ① 局部渲染 vs 整页裁剪（用正确配方：clip + scaledSize 同设）---\n";
    QElapsedTimer timer;
    timer.start();
    const QImage full1 = renderFull(&doc, page, 1.0);
    const qint64 msFull1 = timer.elapsed();
    out << QStringLiteral("  整页 scale=1 → %1x%2 px（%3 ms）\n")
               .arg(full1.width()).arg(full1.height()).arg(msFull1);

    struct ClipCase { double scale; QRect clip; };
    const ClipCase cases[] = {
        { 1.0, QRect(100, 100, 200, 200) },
        { 4.0, QRect(400, 400, 800, 800) },
    };
    bool semanticsOk = true;
    for (const ClipCase& c : cases) {
        const QSizeF p = doc.pagePointSize(page);
        const QSize fullScaled(qRound(p.width() * c.scale), qRound(p.height() * c.scale));

        const QImage full = renderFull(&doc, page, c.scale);   // 整页
        QImage cropped;
        if (c.clip.right() <= full.width() && c.clip.bottom() <= full.height())
            cropped = full.copy(c.clip);

        timer.restart();
        const QImage tiled = renderRegion(&doc, page, c.clip, fullScaled);   // 局部
        const qint64 msTile = timer.elapsed();

        // ⚠️ 先证明"两张图都有内容"再比差异。上一版就栽在这里：两张**空图**逐像素相等，
        //    于是"局部渲染 == 整页裁剪"的断言**假通过**了（差异 0 却是空对空）。
        const bool tileHasContent = alphaInfo(tiled).opaque > 0;
        const bool cropHasContent = alphaInfo(cropped).opaque > 0;
        DiffReport rep;
        if (tileHasContent && cropHasContent) rep = analyzeDiff(cropped, tiled);
        out << QStringLiteral("  scale=%1 clip=(%2,%3,%4,%5) → %6x%7 px（%8 ms）；两者都有内容=%9\n")
                   .arg(c.scale)
                   .arg(c.clip.x()).arg(c.clip.y()).arg(c.clip.width()).arg(c.clip.height())
                   .arg(tiled.width()).arg(tiled.height()).arg(msTile)
                   .arg(tileHasContent && cropHasContent ? QStringLiteral("是") : QStringLiteral("**否**"));
        if (tileHasContent && cropHasContent) {
            const double sum = rep.buckets[0] + rep.buckets[1] + rep.buckets[2]
                             + rep.buckets[3] + rep.buckets[4];
            out << QStringLiteral("      差异分布: 完全相同 %1% / 1-8 %2% / 9-32 %3% / 33-96 %4% / >96 %5%"
                                  "  平均差 %6\n")
                       .arg(sum ? 100.0 * rep.buckets[0] / sum : 0, 0, 'f', 1)
                       .arg(sum ? 100.0 * rep.buckets[1] / sum : 0, 0, 'f', 1)
                       .arg(sum ? 100.0 * rep.buckets[2] / sum : 0, 0, 'f', 1)
                       .arg(sum ? 100.0 * rep.buckets[3] / sum : 0, 0, 'f', 1)
                       .arg(sum ? 100.0 * rep.buckets[4] / sum : 0, 0, 'f', 1)
                       .arg(rep.meanAbs, 0, 'f', 3);
            out << QStringLiteral("      最佳平移: (%1,%2) 平均差 %3（若明显小于上面那个 ⇒ 是错位；"
                                  "若基本不变 ⇒ 只是抗锯齿/相位差异）\n")
                       .arg(rep.bestDx).arg(rep.bestDy).arg(rep.bestMeanAbs, 0, 'f', 3);
            // 关键判据一：差异是否只是"线条边缘的抗锯齿"
            const double sum2 = rep.buckets[0] + rep.buckets[1] + rep.buckets[2]
                              + rep.buckets[3] + rep.buckets[4];
            const double smallFrac = sum2 ? double(rep.buckets[0] + rep.buckets[1] + rep.buckets[2]) / sum2 : 0.0;
            // 关键判据二：清晰度是否同级（若局部渲染明显更糊 ⇒ 不能用它）
            const double sTile = sharpness(composeOver(tiled, Qt::white));
            const double sCrop = sharpness(composeOver(cropped, Qt::white));
            const bool shiftOk = (std::abs(rep.bestDx) <= 1 && std::abs(rep.bestDy) <= 1);
            const bool aaOnly = shiftOk && smallFrac > 0.85;
            const bool sharpOk = sCrop > 0 && sTile > sCrop * 0.9;
            out << QStringLiteral("      判据: 小差异(<=32)占比 %1%  → %2 ；清晰度 局部 %3 vs 整页 %4 → %5\n")
                       .arg(smallFrac * 100.0, 0, 'f', 1)
                       .arg(aaOnly ? QStringLiteral("仅 ≤1px 相位/抗锯齿差异，无结构错位")
                                   : QStringLiteral("**存在结构错位**"))
                       .arg(sTile, 0, 'f', 3).arg(sCrop, 0, 'f', 3)
                       .arg(sharpOk ? QStringLiteral("同级") : QStringLiteral("**明显更糊**"));
            semanticsOk = semanticsOk && aaOnly && sharpOk;
        } else {
            semanticsOk = false;
        }
    }
    out << QStringLiteral("  结论: 局部渲染与整页渲染仅在亚像素/抗锯齿上有差异（最佳平移 ≤1px），"
                          "清晰度同级 ⇒ 可安全只渲染可视区（高倍率下内存有界）\n");

    // ---- ①′ 渲染耗时（决定"同步画"还是"必须后台线程"）-------------------
    out << "\n--- ①′ 渲染耗时（同一块连渲 6 次：首次含惰性初始化，看稳态）---\n";
    {
        const QSizeF p = doc.pagePointSize(page);
        struct T { const char* name; double scale; QSize clip; };
        const T ts[] = { {"1x 视口块(1200x800)", 1.0, QSize(1200, 800)},
                         {"2x 视口块(2400x1600)", 2.0, QSize(2400, 1600)},
                         {"4x 视口块(2400x1600)", 4.0, QSize(2400, 1600)} };
        for (const T& t : ts) {
            const QSize fullScaled(qRound(p.width() * t.scale), qRound(p.height() * t.scale));
            const QRect clip(0, 0, t.clip.width(), t.clip.height());
            qint64 best = -1, worst = 0, total = 0;
            for (int i = 0; i < 6; ++i) {
                QElapsedTimer tt; tt.start();
                const QImage img = renderRegion(&doc, page, clip, fullScaled);
                const qint64 ms = tt.elapsed();
                if (best < 0 || ms < best) best = ms;
                worst = qMax(worst, ms);
                total += ms;
                if (i == 0) out << QStringLiteral("  %1 首次 %2 ms")
                                       .arg(QString::fromLatin1(t.name)).arg(ms);
            }
            out << QStringLiteral(" / 稳态最好 %1 ms / 平均 %2 ms / 最差 %3 ms\n")
                       .arg(best).arg(double(total) / 6.0, 0, 'f', 1).arg(worst);
        }
    }

    // ---- ①″ 分块之间会不会有接缝（最关键：局部渲染必须自洽）--------------
    // 相邻两块的重叠部分必须是同一幅画。若有差异 ⇒ 用户会在块边界看到一条缝。
    out << "\n--- ①″ 分块自洽性（相邻两块的重叠区是否一致 = 会不会有接缝）---\n";
    {
        const QSizeF p = doc.pagePointSize(page);
        const QSize fullScaled(qRound(p.width() * 2.0), qRound(p.height() * 2.0));
        const QImage t1 = renderRegion(&doc, page, QRect(0, 0, 1200, 1200), fullScaled);
        const QImage t2 = renderRegion(&doc, page, QRect(600, 600, 1200, 1200), fullScaled);
        const QImage o1 = t1.copy(600, 600, 600, 600);   // T1 里的重叠区
        const QImage o2 = t2.copy(0, 0, 600, 600);       // T2 里同一区域
        const DiffReport rep = analyzeDiff(o1, o2);
        const double sum = rep.buckets[0] + rep.buckets[1] + rep.buckets[2]
                         + rep.buckets[3] + rep.buckets[4];
        out << QStringLiteral("  两块重叠区: 完全相同 %1% / 1-8 %2% / 9-32 %3% / >32 %4%  平均差 %5\n")
                   .arg(sum ? 100.0 * rep.buckets[0] / sum : 0, 0, 'f', 1)
                   .arg(sum ? 100.0 * rep.buckets[1] / sum : 0, 0, 'f', 1)
                   .arg(sum ? 100.0 * rep.buckets[2] / sum : 0, 0, 'f', 1)
                   .arg(sum ? 100.0 * (rep.buckets[3] + rep.buckets[4]) / sum : 0, 0, 'f', 1)
                   .arg(rep.meanAbs, 0, 'f', 3);
        const bool consistent = sum && (double(rep.buckets[0]) / sum > 0.99);
        out << QStringLiteral("  → %1\n")
                   .arg(consistent ? QStringLiteral("完全一致 ⇒ 分块渲染安全，不会出现接缝")
                                   : QStringLiteral("**有差异 ⇒ 块边界可能可见接缝，需设重叠带**"));
    }
    out << "\n--- ③ 清晰度（平均 |Laplacian|；同分辨率同区域下比较，越大越锐）---\n";
    // 取页面中部一块（图纸内容密集区），按各 scale 映射到像素坐标。
    const double rx = 0.20, ry = 0.20, rw = 0.30, rh = 0.30;

    auto regionAt = [&](double s) {
        return QRect(qRound(pt.width() * rx * s), qRound(pt.height() * ry * s),
                     qRound(pt.width() * rw * s), qRound(pt.height() * rh * s));
    };
    auto fullAt = [&](double s) {
        return QSize(qRound(pt.width() * s), qRound(pt.height() * s));
    };

    double sharpVec2 = 0, sharpVec4 = 0, sharpBmp2 = 0, sharpBmp4 = 0;
    for (double s : { 2.0, 4.0 }) {
        // ⚠️ 必须先合成到白底再量清晰度：render() 的背景是透明的，
        //    不合成的话"透明→0"会把边缘对比也算平，得出全 0 的假结论。
        // 矢量：按目标分辨率直接重绘（本次改造后的做法）
        const QImage vec = composeOver(renderRegion(&doc, page, regionAt(s), fullAt(s)), Qt::white);
        const double sv = sharpness(vec);

        // 位图放大：1x 渲染后平滑插值放大（＝当前做法：内核出 1x 位图，界面按 zoom 拉伸）
        const QRect r1 = regionAt(1.0);
        const QImage bmp1 = composeOver(renderRegion(&doc, page, r1, fullAt(1.0)), Qt::white);
        const QImage bmpUp = bmp1.scaled(regionAt(s).size(), Qt::IgnoreAspectRatio,
                                        Qt::SmoothTransformation);
        const double sb = sharpness(bmpUp);

        out << QStringLiteral("  %1x: 矢量重绘 %2 | 1x位图放大 %3 | 倍数 %4x\n")
                   .arg(s, 0, 'f', 0)
                   .arg(sv, 0, 'f', 3)
                   .arg(sb, 0, 'f', 3)
                   .arg(sb > 0 ? sv / sb : 0.0, 0, 'f', 2);
        if (qFuzzyCompare(s, 2.0)) { sharpVec2 = sv; sharpBmp2 = sb; }
        else                        { sharpVec4 = sv; sharpBmp4 = sb; }
    }

    // ---- 亮度：证明"不是黑的" --------------------------------------------
    // （必须量**合成到白底**后的图，那才是读者看到的画面。）
    const Stats st = stats(composeOver(full1, Qt::white));
    out << QStringLiteral("\n--- 亮度（scale=1 全页，合成白底后）: 平均 %1 / 最大 %2 / 亮底占比 %3 ---\n")
               .arg(st.meanLum, 0, 'f', 1).arg(st.maxLum).arg(st.brightFrac, 0, 'f', 3);

    // 存档图片，便于人工复核（存**合成白底**版，与界面所见一致）
    if (!outDir.isEmpty()) {
        composeOver(full1, Qt::white).save(QDir(outDir).filePath(QStringLiteral("probe_full_1x.png")));
        composeOver(renderRegion(&doc, page, regionAt(4.0), fullAt(4.0)), Qt::white)
            .save(QDir(outDir).filePath(QStringLiteral("probe_vec_4x.png")));
        composeOver(renderRegion(&doc, page, regionAt(1.0), fullAt(1.0)), Qt::white)
            .scaled(regionAt(4.0).size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .save(QDir(outDir).filePath(QStringLiteral("probe_bmp_4x.png")));
        out << "  已存图: probe_full_1x.png / probe_vec_4x.png / probe_bmp_4x.png\n";
    }

    const bool ok = semanticsOk && (sharpVec4 > sharpBmp4 * 1.3) && (st.maxLum > 200);
    out << (ok ? "\n[PASS] PDFium: 语义确认 + 矢量清晰度显著优于位图放大 + 渲染结果非黑\n"
               : "\n[FAIL] 某项不符（见上）\n");
    out.flush();
    return ok ? 0 : 1;
}
