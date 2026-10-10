using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace EGpdf2df.Engine
{
    /// <summary>
    /// Writes a multi-page PDF whose pages are the rendered (and red-annotated)
    /// drawing images. Each page embeds a JPEG via the standard /DCTDecode filter —
    /// no commercial or AGPL-incompatible PDF library required. The output is a real,
    /// openable PDF that preserves the original drawing visually and overlays only
    /// the red difference markers.
    /// </summary>
    public static class PdfFromImages
    {
        public static void Write(string path, List<(byte[] data, int w, int h)> pages)
        {
            var enc = Encoding.ASCII;
            using var ms = new MemoryStream();
            // PDF header — required for strict viewers (and MuPDF in the front-end).
            ms.Write(enc.GetBytes("%PDF-1.4\n%\u0080\u0081\u0082\u0083\n"));
            var offsets = new List<long> { 0 };   // 1-indexed by object number

            void WriteObj(int num, string body)
            {
                while (offsets.Count <= num) offsets.Add(0);
                offsets[num] = ms.Position;
                var bytes = enc.GetBytes($"{num} 0 obj\n{body}\nendobj\n");
                ms.Write(bytes, 0, bytes.Length);
            }
            void WriteImageObj(int num, byte[] jpeg, int w, int h)
            {
                while (offsets.Count <= num) offsets.Add(0);
                offsets[num] = ms.Position;
                var pre = enc.GetBytes($"{num} 0 obj\n<< /Type /XObject /Subtype /Image /Width {w} " +
                                       $"/Height {h} /ColorSpace /DeviceRGB /BitsPerComponent 8 " +
                                       $"/Filter /DCTDecode /Length {jpeg.Length} >>\nstream\n");
                ms.Write(pre, 0, pre.Length);
                ms.Write(jpeg, 0, jpeg.Length);
                var post = enc.GetBytes("\nendstream\nendobj\n");
                ms.Write(post, 0, post.Length);
            }

            // Object numbering & physical order (both ascending, the most parser-friendly
            // layout): 1 = catalog, 2 = /Pages tree, 3.. = per-page (page, content, image).
            // NOTE: the page objects MUST NOT reuse object 2 — that is the Pages node;
            // reusing it clobbers the xref and breaks the page tree (viewers fall back
            // to a default letter-size page and ignore our MediaBox). Writing objects in
            // ascending order avoids cross-parser quirks with out-of-order layouts.
            int cur = 3;
            WriteObj(1, "<< /Type /Catalog /Pages 2 0 R >>");   // object 1: catalog

            // Reserve object numbers up front so the /Pages Kids list is known before
            // the page objects are emitted (each page needs page+content+image = 3 nums).
            var pageObjs = new List<int>();
            foreach (var p in pages) { pageObjs.Add(cur); cur += 3; }
            WriteObj(2, $"<< /Type /Pages /Kids [ {string.Join(" 0 R ", pageObjs)} 0 R ] /Count {pageObjs.Count} >>");

            cur = 3;
            foreach (var p in pages)
            {
                int pageObj = cur++;
                int contentObj = cur++;
                int imageObj = cur++;
                var content = $"q {p.w} 0 0 {p.h} 0 0 cm /Im0 Do Q";
                WriteObj(contentObj, $"<< /Length {content.Length} >>\nstream\n{content}\nendstream");
                WriteImageObj(imageObj, p.data, p.w, p.h);
                WriteObj(pageObj, $"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {p.w} {p.h}] " +
                                  $"/CropBox [0 0 {p.w} {p.h}] " +
                                  $"/Resources << /XObject << /Im0 {imageObj} 0 R >> >> /Contents {contentObj} 0 R >>");
            }

            long xrefStart = ms.Position;
            int size = cur;   // objects 1..cur-1, plus the free object 0
            // IMPORTANT: every xref entry MUST be exactly 20 bytes with a STANDARD
            // 2-byte EOL (\r\n). A space+newline (" n \n") is NOT a valid EOL, and
            // MuPDF silently rejects the malformed table → "page tree load failed" →
            // it falls back to a default letter-sized page and ignores our MediaBox.
            var xref = new StringBuilder();
            xref.Append($"xref\r\n0 {size}\r\n0000000000 65535 f\r\n");
            for (int i = 1; i < size; i++)
                xref.Append($"{offsets[i]:0000000000} 00000 n\r\n");
            xref.Append($"trailer\r\n<< /Size {size} /Root 1 0 R >>\r\nstartxref\r\n{xrefStart}\r\n%%EOF\r\n");
            var xrefBytes = enc.GetBytes(xref.ToString());
            ms.Write(xrefBytes, 0, xrefBytes.Length);

            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.WriteAllBytes(path, ms.ToArray());
        }
    }
}
