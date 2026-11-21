# -*- coding: utf-8 -*-
"""图纸工程图查重 —— 按图号分组，比对工程图矢量指纹。

设计（严格遵守项目铁律）：
  · 图号：严禁正则，字符状态机 + 确定坐标裁剪（见 sheetno.py）；OCR 仅可选辅助。
  · 工程图部分：剔除标题栏区域(x>1300,y>930)后的矢量线画（get_drawings 的 'l'/'re'/'c'）。
  · 指纹：等价复用 EGtools VectorGeomDiff.Fingerprint —— 48×32 占据网格，线段采样，
          按自身包围盒归一化，L2 归一化；相似度 = 余弦点积。纯矢量，非像素、非 OCR。
  · 查重语义：
      (a) PDF 内部：同一图号出现在多页 ⇒ 比对这些页的工程图指纹，高相似 = 重复页。
      (b) 跨 PDF：同一图号在不同 PDF 中的工程图指纹比对，高相似 = 跨版本重复(未改动)。

用法：
  python drawing_dedup.py <pdf1> <pdf2> ... [--threshold 0.97] [--json out.json]
"""
from __future__ import annotations
import sys, math, json, argparse, os
from collections import defaultdict
import pymupdf

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sheetno import extract_from_pdf, A1_W_MIN, A1_W_MAX

GW, GH = 48, 32
TITLE_EXCLUDE = (1300.0, 930.0)   # x>1300 且 y>930 视为标题栏，从工程图指纹中剔除


def segs_from_page(page):
    """提取工程图矢量线段 (x0,y0,x1,y1)。剔除标题栏区。"""
    xc, yc = TITLE_EXCLUDE
    segs = []
    for d in page.get_drawings():
        for it in d["items"]:
            op = it[0]
            if op == "l":
                a, b = it[1], it[2]
                p = (a.x, a.y, b.x, b.y)
            elif op == "re":
                r = it[1]
                x0, y0, x1, y1 = r.x0, r.y0, r.x1, r.y1
                p = (x0, y0, x1, y0)   # 用矩形上边代表，避免方向歧义
            elif op == "c":
                a, b, c, dd = it[1], it[2], it[3], it[4]
                p = (a.x, a.y, dd.x, dd.y)   # 贝塞尔首尾连线近似
            else:
                continue
            # 剔除标题栏：两端点都在标题栏区则丢弃
            if p[0] > xc and p[1] > yc and p[2] > xc and p[3] > yc:
                continue
            if math.hypot(p[2]-p[0], p[3]-p[1]) < 0.5:
                continue
            segs.append(p)
    return segs


def fingerprint(segs):
    """等价 VectorGeomDiff.Fingerprint：48×32 占据网格 + L2。"""
    g = [0.0] * (GW * GH)
    if not segs:
        return g
    x0 = min(min(s[0], s[2]) for s in segs)
    x1 = max(max(s[0], s[2]) for s in segs)
    y0 = min(min(s[1], s[3]) for s in segs)
    y1 = max(max(s[1], s[3]) for s in segs)
    w, h = max(1e-6, x1-x0), max(1e-6, y1-y0)
    for ax, ay, bx, by in segs:
        length = math.hypot(bx-ax, by-ay)
        n = max(1, min(24, math.ceil(length / 4.0)))
        for i in range(n+1):
            t = i / n
            px, py = ax + (bx-ax)*t, ay + (by-ay)*t
            cx = int((px-x0)/w*GW); cx = 0 if cx<0 else (GW-1 if cx>=GW else cx)
            cy = int((py-y0)/h*GH); cy = 0 if cy<0 else (GH-1 if cy>=GH else cy)
            g[cy*GW+cx] += 1
    norm = math.sqrt(sum(v*v for v in g))
    if norm > 1e-12:
        g = [v/norm for v in g]
    return g


def cosine(a, b):
    return sum(x*y for x, y in zip(a, b))


def collect(pdf_path):
    """返回 [(file, page_idx, sheet_no, fp, seg_count), ...] + 统计。"""
    doc = pymupdf.open(pdf_path)
    fname = pdf_path.rsplit("/", 1)[-1]
    rows = []
    skipped = 0
    for i, p in enumerate(doc):
        no, diag = extract_from_pdf(p)
        if no is None:
            skipped += 1
            continue
        segs = segs_from_page(p)
        rows.append((fname, i, no, fingerprint(segs), len(segs)))
    doc.close()
    return rows, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdfs", nargs="+")
    ap.add_argument("--threshold", type=float, default=0.97)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    all_rows = []
    for pdf in args.pdfs:
        rows, skipped = collect(pdf)
        print(f"[load] {pdf.rsplit('/',1)[-1]}: {len(rows)} pages with sheet-no, {skipped} skipped")
        all_rows += rows

    # 按图号分组
    groups = defaultdict(list)
    for row in all_rows:
        groups[row[2]].append(row)

    # (a) 组内查重：同图号多次出现
    internal_dups = []
    for no, items in groups.items():
        for a in range(len(items)):
            for b in range(a+1, len(items)):
                ra, rb = items[a], items[b]
                sim = cosine(ra[3], rb[3])
                # 同文件内同图号，或跨文件同图号
                if ra[0] == rb[0]:
                    kind = "PDF内部重复页"
                else:
                    kind = "跨PDF同图号"
                internal_dups.append({
                    "sheet_no": no, "kind": kind,
                    "a": f"{ra[0]}#p{ra[1]}", "b": f"{rb[0]}#p{rb[1]}",
                    "sim": round(sim, 4), "seg_a": ra[4], "seg_b": rb[4],
                    "duplicate": sim >= args.threshold,
                })

    internal_dups.sort(key=lambda d: -d["sim"])
    dups = [d for d in internal_dups if d["duplicate"]]

    print(f"\n[result] 图号分组数={len(groups)} 比对对数={len(internal_dups)} "
          f"重复对(>={args.threshold})={len(dups)}")
    print("\n=== 重复清单(按相似度降序) ===")
    for d in dups[:60]:
        print(f"  {d['sim']:.4f}  {d['kind']:<12} {d['sheet_no']:<16} {d['a']} <-> {d['b']}  seg {d['seg_a']}/{d['seg_b']}")

    print("\n=== 相似度最低的 10 对(同图号但工程图差异最大) ===")
    for d in internal_dups[-10:]:
        print(f"  {d['sim']:.4f}  {d['kind']:<12} {d['sheet_no']:<16} {d['a']} <-> {d['b']}  seg {d['seg_a']}/{d['seg_b']}")

    if args.json:
        out = {
            "threshold": args.threshold,
            "pages": len(all_rows),
            "groups": len(groups),
            "pairs": len(internal_dups),
            "duplicates": len(dups),
            "all_pairs": internal_dups,
        }
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=2)
        print(f"\n[json] {args.json}")


if __name__ == "__main__":
    main()
