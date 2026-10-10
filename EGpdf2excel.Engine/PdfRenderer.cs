using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using MuPDFCore;

namespace EGpdf2excel.Engine
{
    /// <summary>
    /// Rasterises a single PDF page to a 24-bit BMP for the Qt front-end to display.
    /// The Qt shell owns NO PDF logic: it asks this CLI to paint a page, receives an
    /// image plus the page's point size, and maps rubber-band boxes into PDF points
    /// with its own transform. This keeps the 3-layer split (Qt shell / CLI kernel).
    ///
    /// A JSON summary is printed to stdout so the front-end can compute the exact
    /// pixel<->point mapping (and knows the render scale) without guessing.
    /// </summary>
    public static class PdfRenderer
    {
        public class RenderInfo
        {
            public int Page { get; set; }
            public double PageWidthPt { get; set; }   // page width  in PDF points
            public double PageHeightPt { get; set; }  // page height in PDF points
            public int ImageWidthPx { get; set; }
            public int ImageHeightPx { get; set; }
            public double Scale { get; set; }
        }

        /// <summary>Render one page to a BMP file. Returns the JSON summary string.</summary>
        public static string RenderPage(string pdfPath, int page, double scale, string outPath, Action<string>? log = null)
        {
            void L(string s) => (log ?? Console.Error.WriteLine)(s);
            if (!File.Exists(pdfPath)) throw new FileNotFoundException("pdf not found", pdfPath);

            using var ctx = new MuPDFContext(512u * 1024 * 1024);
            // MuPDFCore cannot open some non-ASCII paths directly; stage via a temp copy.
            string? tmp = null;
            string open = pdfPath;
            if (pdfPath.Any(c => c > 127))
            {
                tmp = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N") + ".pdf");
                File.Copy(pdfPath, tmp, true);
                open = tmp;
            }
            try
            {
                using var doc = new MuPDFDocument(ctx, open);
                int nPages = doc.Pages.Count;
                if (page < 0 || page >= nPages)
                    throw new ArgumentOutOfRangeException("page", $"page {page} out of range (0..{nPages - 1})");

                var bounds = doc.Pages[page].Bounds;
                double pwPt = bounds.X1 - bounds.X0;
                double phPt = bounds.Y1 - bounds.Y0;

                // Render to RGBA (top-down). Derive the EXACT pixel size from the byte
                // length (MuPDF rounds each axis independently) to avoid mismatches.
                byte[] bytes = doc.Render(page, scale, PixelFormats.RGBA, false);
                int totalPx = bytes.Length / 4;
                (int w, int h) = PixelDims(totalPx, pwPt, phPt, scale);

                WriteBmp24(bytes, w, h, outPath);

                var info = new RenderInfo
                {
                    Page = page,
                    PageWidthPt = Math.Round(pwPt, 3),
                    PageHeightPt = Math.Round(phPt, 3),
                    ImageWidthPx = w,
                    ImageHeightPx = h,
                    Scale = scale
                };
                L($"Rendered page {page} ({w}x{h}px, {pwPt:F1}x{phPt:F1}pt) -> {outPath}");
                return JsonSerializer.Serialize(info, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
            }
            finally { if (tmp != null) { try { File.Delete(tmp); } catch { } } }
        }

        /// <summary>Write top-down RGBA bytes as a bottom-up 24-bit BMP (rows padded to 4 bytes).</summary>
        static void WriteBmp24(byte[] rgba, int w, int h, string path)
        {
            int rowSize = ((w * 3) + 3) & ~3;         // 24bpp rows are 4-byte aligned
            int pixelDataSize = rowSize * h;
            const int headerSize = 14 + 40;            // BITMAPFILEHEADER + BITMAPINFOHEADER
            int fileSize = headerSize + pixelDataSize;

            string? dir = Path.GetDirectoryName(Path.GetFullPath(path));
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

            using var fs = new FileStream(path, FileMode.Create, FileAccess.Write);
            using var wtr = new BinaryWriter(fs);

            // BITMAPFILEHEADER
            wtr.Write((byte)'B'); wtr.Write((byte)'M');
            wtr.Write(fileSize);
            wtr.Write((short)0); wtr.Write((short)0);
            wtr.Write(headerSize);
            // BITMAPINFOHEADER (height positive => bottom-up rows)
            wtr.Write(40);
            wtr.Write(w);
            wtr.Write(h);
            wtr.Write((short)1);                       // planes
            wtr.Write((short)24);                      // bits per pixel
            wtr.Write(0);                              // BI_RGB
            wtr.Write(pixelDataSize);
            wtr.Write(2835); wtr.Write(2835);         // ~72 DPI
            wtr.Write(0); wtr.Write(0);

            // Pixel data: bottom-up, BGR
            var row = new byte[rowSize];
            for (int y = h - 1; y >= 0; y--)
            {
                Array.Clear(row, 0, rowSize);
                int src = y * w * 4;
                for (int x = 0; x < w; x++)
                {
                    byte r = rgba[src + x * 4 + 0];
                    byte g = rgba[src + x * 4 + 1];
                    byte b = rgba[src + x * 4 + 2];
                    row[x * 3 + 0] = b;                 // BGR
                    row[x * 3 + 1] = g;
                    row[x * 3 + 2] = r;
                }
                wtr.Write(row);
            }
        }

        /// <summary>
        /// Derive the true (w,h) of a whole-page RGBA render from its byte length.
        /// MuPDF rounds each axis independently, so we search widths within ±2px of the
        /// Math.Round estimate for one that divides totalPx; the matching h is then exact
        /// (w*h == totalPx). This keeps the BMP writer's row stride consistent with the
        /// actual pixel buffer.
        /// </summary>
        static (int w, int h) PixelDims(int totalPx, double wPt, double hPt, double scale)
        {
            int wEst = Math.Max(1, (int)Math.Round(wPt * scale));
            int bestW = wEst, bestErr = int.MaxValue;
            for (int dw = -2; dw <= 2; dw++)
            {
                int w = wEst + dw;
                if (w <= 0) continue;
                if (totalPx % w == 0)
                {
                    int err = Math.Abs(w - wEst);
                    if (err < bestErr) { bestErr = err; bestW = w; }
                }
            }
            if (bestErr == int.MaxValue) bestW = wEst;
            int h = Math.Max(1, totalPx / bestW);
            return (bestW, h);
        }
    }
}