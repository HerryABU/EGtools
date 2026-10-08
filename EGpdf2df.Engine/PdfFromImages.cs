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

            int cur = 1;
            WriteObj(cur++, "<< /Type /Catalog /Pages 2 0 R >>");   // object 1: catalog

            var pageObjs = new List<int>();
            foreach (var p in pages)
            {
                int pageObj = cur++;
                int contentObj = cur++;
                int imageObj = cur++;
                pageObjs.Add(pageObj);
                var content = $"q {p.w} 0 0 {p.h} 0 0 cm /Im0 Do Q";
                WriteObj(contentObj, $"<< /Length {content.Length} >>\nstream\n{content}\nendstream");
                WriteImageObj(imageObj, p.data, p.w, p.h);
                WriteObj(pageObj, $"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {p.w} {p.h}] " +
                                  $"/Resources << /XObject << /Im0 {imageObj} 0 R >> >> /Contents {contentObj} 0 R >>");
            }
            // object 2: pages (written last so its Kids list is complete & no other
            // objects are embedded inside its body).
            WriteObj(2, $"<< /Type /Pages /Kids [ {string.Join(" ", pageObjs)} ] /Count {pageObjs.Count} >>");

            long xrefStart = ms.Position;
            int size = cur;   // objects 1..cur-1, plus the free object 0
            var xref = new StringBuilder();
            xref.Append($"xref\n0 {size}\n0000000000 65535 f \n");
            for (int i = 1; i < size; i++)
                xref.Append($"{offsets[i]:0000000000} 00000 n \n");
            xref.Append($"trailer\n<< /Size {size} /Root 1 0 R >>\nstartxref\n{xrefStart}\n%%EOF\n");
            var xrefBytes = enc.GetBytes(xref.ToString());
            ms.Write(xrefBytes, 0, xrefBytes.Length);

            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.WriteAllBytes(path, ms.ToArray());
        }
    }
}
