# -*- coding: utf-8 -*-
"""诊断「贴地椭圆」的真实像素结构：逐行宽度 + alpha 剖面。

之前 _fix_groundshadow.py 用"独立连通域"判定，只认出 5/100，
但肉眼在 art/feet_rgplant_00.png 里几乎每张都能看到椭圆。
说明椭圆是**主体的横向突起**（alpha>30 与角色连成一片），
不是独立连通域。这里把底部 band 的逐行宽度打出来，用数据定阈值。

用法：
  python art/_diag_bottom.py P00 P03 P08 P11 P14 P26 P33 P43
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DST = os.path.join(ROOT, "assets")


def profile(a, thr=30):
    al = a[:, :, 3]
    obj = al > thr
    h, w = obj.shape
    rows = []
    for y in range(h):
        xs = np.where(obj[y])[0]
        if len(xs) == 0:
            rows.append((0, 0, 0, 0))
        else:
            rows.append((xs.max() - xs.min() + 1, len(xs), xs.min(), xs.max()))
    return rows


def show(key, thr=30):
    pre = "rgplant_" if key[0] == "P" else "rgzombie_"
    p = os.path.join(DST, pre + key[1:] + ".bmp")
    if not os.path.exists(p):
        print("%-5s 缺" % key)
        return
    a, w, h = U.read_bmp32(p)
    rows = profile(a, thr)
    widths = np.array([r[0] for r in rows], dtype=float)
    # 中上段（30%~65%高）作为"角色本体宽度"基准
    upper = widths[int(h * 0.30):int(h * 0.65)]
    base = float(np.median(upper[upper > 0])) if (upper > 0).any() else 1.0
    print("=" * 78)
    print("%s  %dx%d  上段中位宽=%.0f  最大宽=%.0f  最宽行=%d"
          % (key, w, h, base, widths.max(), int(np.argmax(widths))))
    print("  底部 40%% 逐行（y, 宽, 像素数, xmin..xmax）:")
    y0 = int(h * 0.60)
    step = max(1, (h - y0) // 22)
    for y in range(y0, h, step):
        wd, cnt, xa, xb = rows[y]
        flag = ""
        if wd > base * 1.30:
            flag = "  <== 宽于本体 %.2fx" % (wd / base)
        print("   y=%3d 宽=%4d 实心=%4d  x=%4d..%4d%s" % (y, wd, cnt, xa, xb, flag))
    print("   末行 y=%d 宽=%d" % (h - 1, rows[h - 1][0]))


if __name__ == "__main__":
    ks = [a.upper() for a in sys.argv[1:]] or ["P00", "P03", "P08", "P11", "P14", "P26", "P33", "P43"]
    for k in ks:
        show(k)
