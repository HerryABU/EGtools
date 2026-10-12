#pragma once

#include <QString>

// Headless QA: drives the real code path (CLI render -> coordinate transform ->
// region JSON -> CLI extract) under the offscreen Qt platform, then prints a
// PASS/FAIL line. Returns 0 on success, non-zero on failure.
int runSelfTest(const QString& cliDir, const QString& pdfPath);

// Pure coordinate-mapping QA: verifies the px<->pt transform (the heart of the
// "no hard-coded coordinates" guarantee) WITHOUT spawning any child process,
// so it runs even where QProcess is restricted.
int runMathTest();

// Render diagnosis: renders one page through the kernel fusion layer and reports
// image size / format / luminance histogram, so a "black page" can be pinned to
// either the render step or the display step instead of being guessed at.
int runRenderTest(const QString& cliDir, const QString& pdfPath, int page);

// 连续滚动视图自检：版面（堆叠/留白/居中/缩放）+ 命中判定 + 真实文档的懒渲染。
// 版面部分完全不依赖内核，任何环境都能跑；给了 PDF 还会验证"滚动到某页能渲染出来"。
int runContinuousTest(const QString& cliDir, const QString& pdfPath);

// 批量提取并发自检：同一 PDF 重复 N 次、并发度 >1 跑一遍，验证并发路径的
// 线程安全与计数正确（跨线程信号不丢、成败数准、确实产出文件）。
int runBatchTest(const QString& cliDir, const QString& pdfPath, int jobs, int concurrency);

// 布局审计：离屏构造主窗口，打印窗口最小尺寸 + 控件树几何 + 各关键区域宽度。
// 用来把"横向太长"从主观观感变成可比较的数字（尤其是 minimumSizeHint —— 它才是
// 决定"窗口能不能变窄"的东西）。不依赖内核与 PDF。
int runLayoutDump();

// 画布绘制自检：渲染一页 → 放进真实的画布壳 → grab() 抓成位图 → 统计像素。
// 这是唯一能抓住"画布全黑"这类故障的测试：渲染成功 ≠ 画出来了。
// （GPU 壳在无头环境不可用时会自动用光栅壳；两边都走同一套 PdfCanvas::paint。）
int runPaintTest(const QString& cliDir, const QString& pdfPath);

// 端到端界面绘制自检：构造**真实主窗口**、打开 PDF、抓画布控件位图并统计像素。
// 与 --test-paint 的区别在于画布处于真实嵌套（QScrollArea ⊂ QStackedWidget ⊂ QSplitter）——
// "渲染正常但界面全黑"这类故障只在这种真实处境下才复现得出来。
int runUiPaintTest(const QString& cliDir, const QString& pdfPath);

// 截图取证：保存真实主窗口与整屏 PNG（人工/多模态复核用，配色与排版的问题说不清时看图）。
int runScreenshot(const QString& cliDir, const QString& pdfPath, const QString& outPngBase);

// 「图纸比对」页交互自检：差异清单填充 / 筛选 / 跳到某条差异（不依赖真实比对，秒级）。
int runDiffUiTest();

// 面板自检：侧栏宽度预设 / 折叠展开 / "折叠后顶部页签接管导航" / 结果面板折叠。
int runPanelsTest();

// 主题自检：深浅主题是否**真实生效**（用首页当探针 —— 它原先把浅色写死在代码里）。
int runThemeTest();

// 矢量渲染自检：断言"按目标分辨率重绘 / 明显更锐（含与旧内核位图路径对比）/ 不黑"，
// 并核对 PDFium 与内核的页面 pt 尺寸一致（否则框选与词盒会整体偏移）。
int runVectorRenderTest(const QString& cliDir, const QString& pdfPath);