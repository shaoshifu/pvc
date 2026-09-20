# -*- coding: utf-8 -*-
"""把底部区域画成 ASCII 色族图，肉眼定位「贴地椭圆」到底是哪些像素。

字符表（按 RGB 粗分色族，只看 alpha>40 的像素）：
    . = 透明(alpha<=40)      # = 深色(明度<60)
    M = 洋红系(R>G+20 且 B>G+20)
    R = 红/橙系(R>G+20 且 B<=G+20)      Y = 黄系(R>B+30 且 G>B+30 且 明度>120)
    G = 绿系(G>R+15 且 G>B+15)          B = 蓝/青系(B>R+15)
    w = 高亮(明度>210 且饱和度低)        ? = 其它（棕/灰）

用法： python art/_diag_ascii.py P00 60
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DST = os.path.join(ROOT, "assets")


def classify(a):
    r = a[:, :, 0].astype(np.int32)
    g = a[:, :, 1].astype(np.int32)
    b = a[:, :, 2].astype(np.int32)
    al = a[:, :, 3].astype(np.int32)
    lum = (r * 299 + g * 587 + b * 114) // 1000
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    sat = (mx - mn)

    ch = np.full(a.shape[:2], "?", dtype="<U1")
    ch[(al <= 40)] = "."
    m = al > 40
    ch[m & (sat < 40) & (lum > 210)] = "w"
    ch[m & (lum < 60)] = "#"
    ch[m & (r > g + 20) & (b > g + 20)] = "M"
    ch[m & (r > g + 20) & (b <= g + 20)] = "R"
    ch[m & (g > r + 15) & (g > b + 15)] = "G"
    ch[m & (b > r + 15)] = "B"
    ch[m & (r > b + 30) & (g > b + 30) & (lum > 120)] = "Y"
    return ch


def show(key, rows=48):
    pre = "rgplant_" if key[0] == "P" else "rgzombie_"
    p = os.path.join(DST, pre + key[1:] + ".bmp")
    if not os.path.exists(p):
        print("%-5s 缺" % key)
        return
    a, w, h = U.read_bmp32(p)
    ch = classify(a)
    y0 = int(h * 0.42)
    sub = ch[y0:]
    sh, sw = sub.shape
    # 横向不抽稀（宽度本来就不大），纵向抽稀到 rows
    ys = np.linspace(0, sh - 1, min(rows, sh)).astype(int)
    print("=" * (sw + 12))
    print("%s  %dx%d   显示 y=%d..%d（下 58%%）" % (key, w, h, y0, h - 1))
    for i, y in enumerate(ys):
        line = "".join(sub[y])
        print("%4d %s" % (y0 + y, line))
    # 顺带把底部 12 行的代表色打出来
    print("  底部 10 行代表色（每 20 像素取样）：")
    for y in range(max(0, h - 10), h):
        xs = np.where(a[y, :, 3] > 40)[0]
        if len(xs) == 0:
            print("   y=%3d  --" % y)
            continue
        samples = []
        for x in xs[::max(1, len(xs) // 8)]:
            samples.append("(%3d,%3d,%3d)a%3d" % (a[y, x, 0], a[y, x, 1], a[y, x, 2], a[y, x, 3]))
        print("   y=%3d  %s" % (y, " ".join(samples)))


if __name__ == "__main__":
    args = sys.argv[1:]
    rows = 48
    if args and args[-1].isdigit():
        rows = int(args.pop())
    ks = [a.upper() for a in args] or ["P00"]
    for k in ks:
        show(k, rows)
