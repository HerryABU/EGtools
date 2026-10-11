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