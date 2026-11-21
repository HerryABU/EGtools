# -*- coding: utf-8 -*-
"""图号提取：严禁正则，纯字符状态机 + 确定坐标裁剪；OCR 仅作可选辅助。

坐标来源（实测三份 PDF 完全一致）：
    标题栏图号锚点 x0≈1517.8, y0≈984.1 (pt)，页尺寸 1683.78x1190.55 (A1 横版)。
图号形态（字符状态机判定，非正则）：
    形如 408.101.P01 —— 点分三段：段1/段2 全数字，段3 为 'P'+数字。
"""
from __future__ import annotations

import pymupdf

# 标题栏图号裁剪窗口（pt）。锚点 + 容差：x[1508,1600] y[978,1002]
TITLE_BOX = (1508.0, 978.0, 1600.0, 1002.0)
# 只有 A1 横版主图页才有此标题栏坐标；A3 封面/A0 总图跳过
A1_W_MIN, A1_W_MAX = 1600.0, 1750.0


def looks_like_sheet_no(s: str) -> bool:
    """字符状态机：判定 '408.101.P01' 形态，严禁正则。
    规则：恰好两个 '.' 分成三段；前两段每段至少1位且全为数字；
          第三段首字符 'P'，其后至少1位且全为数字。"""
    segs: list[list[str]] = [[]]
    dot_count = 0
    for ch in s:
        if ch == '.':
            dot_count += 1
            if dot_count > 2:
                return False
            segs.append([])
        else:
            segs[-1].append(ch)
    if dot_count != 2 or len(segs) != 3:
        return False
    if not segs[0] or not segs[1]:
        return False
    if not all(c.isdigit() for c in segs[0]):
        return False
    if not all(c.isdigit() for c in segs[1]):
        return False
    seg3 = segs[2]
    if len(seg3) < 2 or seg3[0] != 'P':
        return False
    return all(c.isdigit() for c in seg3[1:])


def _order_chars(words: list[tuple]) -> str:
    """把裁剪窗口内的字/词按阅读顺序拼成一行：先按 y 居中行分组，再按 x 排序。
    应对旧版里图号被拆成逐字符且倒序存入内容流的情况。"""
    if not words:
        return ""
    # 按 y 中心聚成若干行（容差 6pt）
    rows: list[list[tuple]] = []
    for w in sorted(words, key=lambda w: (w[1] + w[3]) / 2):
        cy = (w[1] + w[3]) / 2
        placed = False
        for row in rows:
            if abs((row[0][1] + row[0][3]) / 2 - cy) <= 6.0:
                row.append(w)
                placed = True
                break
        if not placed:
            rows.append([w])
    # 每行按 x0 从小到大拼（图号从左往右读：408.101.P01）
    row_strings = []
    for row in rows:
        row.sort(key=lambda w: w[0])
        row_strings.append(''.join(w[4] for w in row))
    # 取唯一一行（标题栏图号独占一行）；若多行，取最长的
    return max(row_strings, key=len) if row_strings else ""


def extract_from_pdf(page, use_ocr: bool = False, ocr_lang: str = 'eng') -> tuple[str | None, str]:
    """从单页提取标题栏图号。返回 (图号 or None, 诊断说明)。
    page: pymupdf.Page（坐标基于未旋转的 0 度页面）。"""
    r = page.rect
    if not (A1_W_MIN <= r.width <= A1_W_MAX) or page.rotation != 0:
        return None, f"skip: page size {r.width:.0f}x{r.height:.0f} rot={page.rotation}"
    x0, y0, x1, y1 = TITLE_BOX
    words = [w for w in page.get_text("words")
             if x0 <= (w[0] + w[2]) / 2 <= x1 and y0 <= (w[1] + w[3]) / 2 <= y1]
    s = _order_chars(words)
    diag = f"text='{s}' words={len(words)}"
    if looks_like_sheet_no(s):
        return s, diag + " -> OK(text)"

    # 可选 OCR 辅助：仅当环境装了 tesseract 才启用（当前环境无，留接口）
    if use_ocr:
        try:
            import pytesseract
            from PIL import Image
            clip = pymupdf.Rect(x0, y0, x1, y1)
            pix = page.get_pixmap(clip=clip, matrix=pymupdf.Matrix(4, 4))
            img = Image.frombytes("RGB", (pix.width, pix.height), pix.samples)
            ocr_s = pytesseract.image_to_string(img, lang=ocr_lang, config="--psm 7").strip()
            diag += f" ocr='{ocr_s}'"
            if looks_like_sheet_no(ocr_s):
                return ocr_s, diag + " -> OK(ocr)"
        except Exception as e:  # OCR 不可用时静默降级到文本层结果
            diag += f" ocr_unavailable={type(e).__name__}"
    return None, diag + " -> FAIL"
