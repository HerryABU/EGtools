# -*- coding: utf-8 -*-
"""汇总 EGpdf2df 全本矢量比对结果 —— 按图号归属每个差异页。

输入：EGpdf2df 全本比对的 stdout 日志（含 JSON）+ **目标 PDF**（用于图号→页映射）。
输出：每个有差异图号的：差异区域数/类型/坐标 bbox、文字差异。

⚠️⚠️ 关键（实测踩坑）：`diffs[].page` 是**目标侧 0-based 页索引**，不是基准页！
    曾误用基准页映射，导致 P21（基准23/目标24）的 1090 处差异被归到基准24 的 P22——
    "页面配对错位"的又一变体。必须用目标 PDF 建图号映射。

用法：
  python summarize_full_diff.py <kernel_log> <target.pdf> --json out.json
"""
from __future__ import annotations
import sys, os, json, argparse, re
from collections import defaultdict
import pymupdf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sheetno import extract_from_pdf


def parse_kernel_json(log_text: str) -> dict:
    i = log_text.find("{")
    if i < 0:
        raise ValueError("日志里找不到 JSON 对象")
    obj, _ = json.JSONDecoder().raw_decode(log_text[i:])
    return obj


def page_to_sheetno(pdf_path: str) -> dict[int, str]:
    doc = pymupdf.open(pdf_path)
    m = {}
    for i, p in enumerate(doc):
        no, _ = extract_from_pdf(p)
        if no:
            m[i] = no
    doc.close()
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("target", help="目标 PDF（diffs[].page 是目标侧 0-based 页索引）")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    txt = open(args.log, encoding="utf-8", errors="ignore").read()
    data = parse_kernel_json(txt)
    summary = data.get("summary", {})
    diffs = data.get("diffs", [])
    alignments = data.get("alignments", [])
    p2s = page_to_sheetno(args.target)

    # 按基准页聚合差异
    by_page: dict[int, dict] = defaultdict(lambda: {"graphic": [], "text": []})
    for d in diffs:
        page = d.get("page", -1)
        typ = str(d.get("type", "")).lower()
        if "graphic" in typ or "vector" in typ:
            by_page[page]["graphic"].append(d)
        else:
            by_page[page]["text"].append(d)

    # 配对信息：从 alignments 拿 scale/dx/dy
    align_by_page = {a.get("page"): a for a in alignments}

    rows = []
    for page in sorted(by_page):
        g = by_page[page]["graphic"]
        t = by_page[page]["text"]
        no = p2s.get(page, f"(无图号 p{page})")
        bboxes = [x["bbox"] for x in g if x.get("bbox")]
        change_types = defaultdict(int)
        for x in g:
            change_types[str(x.get("changeType", "?"))] += 1
        a = align_by_page.get(page, {})
        rows.append({
            "sheet_no": no,
            "target_page_0based": page,
            "graphic_regions": len(g),
            "text_diffs": len(t),
            "change_types": dict(change_types),
            "align": {"scale": a.get("scale"), "dxPt": a.get("dxPt"), "dyPt": a.get("dyPt")},
            "sample_bboxes": bboxes[:10],
        })

    # 按图号汇总（同图号多页合并）
    by_sheet: dict[str, dict] = defaultdict(lambda: {"graphic": 0, "text": 0, "pages": []})
    for r in rows:
        s = by_sheet[r["sheet_no"]]
        s["graphic"] += r["graphic_regions"]
        s["text"] += r["text_diffs"]
        s["pages"].append(r["target_page_0based"])

    print(f"[summary] graphicDiffs={summary.get('graphicDiffs')} textDiffs={summary.get('textDiffs')} "
          f"matchedPages={len(summary.get('matchedPages', []))}")
    print(f"[pages] 有图形差异的目标页={len(rows)}  涉及图号={len(by_sheet)}")
    print("\n=== 图形差异按图号汇总（降序） ===")
    for no, s in sorted(by_sheet.items(), key=lambda kv: -kv[1]["graphic"]):
        print(f"  graphic={s['graphic']:>4} text={s['text']:>3}  {no:<16} 目标页={s['pages']}")

    if args.json:
        out = {
            "summary": summary,
            "pages": rows,
            "by_sheet": dict(by_sheet),
        }
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=2)
        print(f"\n[json] {args.json}")


if __name__ == "__main__":
    main()
