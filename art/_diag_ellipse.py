# -*- coding: utf-8 -*-
"""逐像素解剖底部 30% 区域：到底有多少像素是「洋红/紫红椭圆色」，alpha 分布如何。

前面 _fix_groundshadow.py 只认出 5/100 —— 猜测是椭圆 alpha 被键控压到 30 以下，
所以 `obj = al > 30` 看不见它。这里直接把底部区域按 (色相, alpha 分档) 统计。

用法： python art/_diag_ellipse.py P00 P03 P08 P11 P14 P26 P33 P43
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DST = os.path.join(ROOT, "assets")


def magentaish(rgb):
    """R 高 B 高 G 低 —— 洋红/紫红系（椭圆与背景同色族）。"""
    r = rgb[:, :, 0].astype(np.int32)
    g = rgb[:, :, 1].astype(np.int32)
    b = rgb[:, :, 2].astype(np.int32)
    return (r > g + 18) & (b > g + 18)


def show(key):
    pre = "rgplant_" if key[0] == "P" else "rgzombie_"
    p = os.path.join(DST, pre + key[1:] + ".bmp")
    if not os.path.exists(p):
        print("%-5s 缺" % key)
        return
    a, w, h = U.read_bmp32(p)
    al = a[:, :, 3]
    rgb = a[:, :, :3]
    mag = magentaish(rgb)

    print("=" * 78)
    print("%s  %dx%d" % (key, w, h))
    bands = [("全图", 0.0, 1.0), ("下30%", 0.70, 1.0), ("下15%", 0.85, 1.0), ("下8%", 0.92, 1.0)]
    for name, y0f, y1f in bands:
        y0, y1 = int(h * y0f), int(h * y1f)
        n = (y1 - y0) * w
        sub_al = al[y0:y1]
        sub_mag = mag[y0:y1]
        for lo, hi in ((1, 30), (30, 90), (90, 200), (200, 256)):
            m = (sub_al >= lo) & (sub_al < hi)
            if m.sum() == 0:
                continue
            mp = (m & sub_mag).sum()
            print("  %-6s a[%3d,%3d) 像素%6d  其中洋红%6d (%.0f%%)"
                  % (name, lo, hi, m.sum(), mp, 100.0 * mp / max(1, m.sum())))


if __name__ == "__main__":
    ks = [a.upper() for a in sys.argv[1:]] or ["P00", "P03", "P08", "P11", "P14", "P26", "P33", "P43"]
    for k in ks:
        show(k)
