# -*- coding: utf-8 -*-
"""深入矢量差异比对 —— 由查重结果驱动，调用项目 EGpdf2df 内核做逐图元比对。

链路：
  1. 用 drawing_dedup 的指纹结果找出"同一图号、工程图有改动"的页对（两侧页索引按图号精确定位）；
  2. 对每个差异图号，构造 EGpdf2df 请求，用 --page-mode pin 把两侧页钉死在图号对应页；
     ⚠️ 必须用图号定位两侧页 —— 旧版/新版同图号页码会错开（如 P21 旧=23 新=24），
        按页码直接比会配错页（标题栏图号都会对不上）。
  3. 内核做纯矢量比对（几何标定 + 共线覆盖匹配 + 第三态变更识别 + 区域聚类），输出标注 PDF + JSON；
  4. 汇总每个图号的：图形差异区域数/类型/坐标、文字差异、管线号一一对应结果。

用法：
  python vector_diff.py <旧版.pdf> <新版.pdf> [--kernel EXE] [--sim-threshold 0.97] \
      [--out-dir DIR] [--sheet-nos P21,P23] [--json out.json]

依赖：pymupdf（图号定位）；EGpdf2df.exe（矢量比对内核，随 EGtools 安装或 DIST/）。
"""
from __future__ import annotations
import sys, os, json, argparse, subprocess
from collections import defaultdict
import pymupdf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sheetno import extract_from_pdf
from drawing_dedup import segs_from_page, fingerprint, cosine


def find_kernel(explicit: str | None) -> str:
    if explicit and os.path.isfile(explicit):
        return explicit
    cands = [
        r"C:\Program Files\EGtools\EGpdf2df\EGpdf2df.exe",
        os.path.join(HERE, "..", "..", "DIST", "EGpdf2df", "EGpdf2df.exe"),
        os.path.join(HERE, "..", "..", "DIST-arm64", "EGpdf2df", "EGpdf2df.exe"),
    ]
    for c in cands:
        c = os.path.abspath(c)
        if os.path.isfile(c):
            return c
    raise FileNotFoundError("找不到 EGpdf2df.exe，请用 --kernel 指定路径")


def index_by_sheetno(pdf_path: str) -> dict[str, list[int]]:
    doc = pymupdf.open(pdf_path)
    g: dict[str, list[int]] = defaultdict(list)
    for i, p in enumerate(doc):
        no, _ = extract_from_pdf(p)
        if no:
            g[no].append(i)
    doc.close()
    return g


def fp_at(pdf_path: str, page_idx: int) -> list[float]:
    doc = pymupdf.open(pdf_path)
    fp = fingerprint(segs_from_page(doc[page_idx]))
    doc.close()
    return fp


def parse_kernel_json(log_text: str) -> dict:
    """内核 stdout = 日志(可能 GBK 乱码) + 一个 JSON 对象 + 尾部。用 raw_decode 取 JSON。"""
    i = log_text.find("{")
    if i < 0:
        return {}
    s = log_text[i:]
    try:
        obj, _ = json.JSONDecoder().raw_decode(s)
        return obj
    except Exception:
        return {}


def run_pair(kernel: str, base_pdf: str, tgt_pdf: str, base_idx: int, tgt_idx: int,
             out_pdf: str, timeout: int = 600) -> dict:
    """跑一次完整矢量比对。

    ⚠️⚠️ 关键（实测踩坑）：必须用**默认 filter 模式**，不能用 pin。
       - `filter`（默认）：先把两本 PDF **按真图号全本配对**，再按 -p 的基准页(0-based)
         筛出该页对应的目标页 —— 即使两版同图号页码错开（P21 旧=23 新=24）也能配对正确。
       - `pin`（--page-mode pin）：把页序按索引钉死（给 p 就比 基准p ↔ 目标p），
         同图号页码错开时**必配错页**（实测 -p 23 pin 比成了 基准24↔目标24，图号对不上）。
       - `--json 契约` 模式加 -p 会走全本 175 页配对、极慢；用 CLI 位置参数形式。
    -p 传基准侧页 base_idx（0-based），内核按图号找到正确的目标页。
    """
    cmd = [kernel, base_pdf, tgt_pdf,
           "-p", str(base_idx),
           "-o", out_pdf, "--graphic-method", "vector"]
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    env["EGTOOLS_NO_COLOR"] = "1"
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="ignore", env=env, timeout=timeout)
    data = parse_kernel_json(proc.stdout)
    return {
        "returncode": proc.returncode,
        "data": data,
        "stdout_tail": proc.stdout[-1500:],
        "stderr_tail": proc.stderr[-800:],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline")
    ap.add_argument("target")
    ap.add_argument("--kernel", default=None)
    ap.add_argument("--sim-threshold", type=float, default=0.97,
                    help="指纹相似度低于此值判定为有改动（需深入比对）")
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--sheet-nos", default=None, help="只比对指定图号，逗号分隔")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    kernel = find_kernel(args.kernel)
    out_dir = args.out_dir or os.path.join(HERE, "vector_diff_out")
    os.makedirs(out_dir, exist_ok=True)
    print(f"[kernel] {kernel}")
    print(f"[out] {out_dir}")

    bmap = index_by_sheetno(args.baseline)
    tmap = index_by_sheetno(args.target)
    common = set(bmap) & set(tmap)
    print(f"[sheets] baseline={len(bmap)} target={len(tmap)} common={len(common)}")

    if args.sheet_nos:
        wanted = [s.strip() for s in args.sheet_nos.split(",") if s.strip()]
        common = common & set(wanted)

    # 若显式指定图号，跳过指纹筛选直接全部深入比对（省去对 160+ 图号算指纹的耗时）
    explicit = bool(args.sheet_nos)
    changed = []
    for no in sorted(common):
        bi, ti = bmap[no][0], tmap[no][0]
        if explicit:
            sim = -1.0  # 显式指定：不做指纹预筛（-1 = 未测）
            changed.append((no, sim, bi, ti))
        else:
            sim = cosine(fp_at(args.baseline, bi), fp_at(args.target, ti))
            if sim < args.sim_threshold:
                changed.append((no, sim, bi, ti))
    print(f"[changed] {len(changed)} 图号需深入矢量比对"
          + ("" if explicit else f"（指纹 < {args.sim_threshold}）"))

    results = []
    for no, sim, bi, ti in changed:
        out_pdf = os.path.join(out_dir, f"diff_{no.replace('.', '_')}.pdf")
        print(f"\n[run] {no}  sim={sim:.4f}  base#p{bi} -> tgt#p{ti}")
        try:
            r = run_pair(kernel, args.baseline, args.target, bi, ti, out_pdf)
            data = r["data"]
            summary = data.get("summary", {})
            diffs = data.get("diffs", [])
            gdiffs = [d for d in diffs if d.get("type") in ("graphic", "graphicDiff", "vector")]
            bboxes = [d.get("bbox") for d in gdiffs if d.get("bbox")]
            entry = {
                "sheet_no": no, "fingerprint_sim": round(sim, 4),
                "base_page": bi, "target_page": ti,
                "graphic_diff_regions": summary.get("graphicDiffs"),
                "text_diffs": summary.get("textDiffs"),
                "matched_pages": summary.get("matchedPages", []),
                "diff_pdf": out_pdf if os.path.isfile(out_pdf) else None,
                "diff_pdf_size": os.path.getsize(out_pdf) if os.path.isfile(out_pdf) else 0,
                "sample_bboxes": bboxes[:12],
                "alignments": data.get("alignments", []),
                "pipe_match": data.get("pipeMatch", {}),
                "returncode": r["returncode"],
            }
            print(f"   graphic={entry['graphic_diff_regions']} text={entry['text_diffs']} "
                  f"regions_bbox={len(bboxes)} pdf={'OK' if entry['diff_pdf'] else 'MISS'}")
        except Exception as e:
            entry = {"sheet_no": no, "fingerprint_sim": round(sim, 4),
                     "base_page": bi, "target_page": ti, "error": f"{type(e).__name__}: {e}"}
            print(f"   ERROR {e}")
        results.append(entry)

    out = {
        "baseline": args.baseline, "target": args.target,
        "sim_threshold": args.sim_threshold,
        "kernel": kernel,
        "changed_count": len(changed),
        "results": results,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=2)
        print(f"\n[json] {args.json}")


if __name__ == "__main__":
    main()
