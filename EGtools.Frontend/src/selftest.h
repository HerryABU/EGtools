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