#pragma once

#include <QString>
#include <QRectF>
#include <QVector>

// A user-drawn rubber-band box, expressed in PDF points with the SAME
// convention the CLI kernel uses: origin TOP-LEFT, y grows DOWN, unit = PDF pt.
// Nothing about a specific drawing is hard-coded here — every box comes from
// the user at run time and is exported as JSON for the kernel.
struct Region
{
    int   page   = 0;
    QString kind;                 // global_table | detail_table | material_no_region
    QRectF rectPt;                // PDF points, top-left origin, y-down
};

// A character/word segment returned by the kernel, for overlay/debug.
struct WordBox
{
    QRectF rectPt;
    QString text;
    // Provenance from the kernel: "vector" (embedded PDF text) or "ocr"
    // (recovered by OCR for image-only regions). Empty if the kernel did not
    // report it (older responses / vector-only paths).
    QString source;
};