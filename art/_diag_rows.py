# -*- coding: utf-8 -*-
"""打印「源图抠图后」逐行的洋红占比，看扫带为什么提前中断。

用法： python art/_diag_rows.py Z00
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402
from _fix_groundshadow import magenta_family, AL, ROW_MAG_RATIO   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "gen2")


def main(key):
    src = os.path.join(SRC, "u_%s.jpg" % key.lower())
    im, c, spread = U.matte_file(src)
    a = np.array(im)
    ys, xs = np.where(a[:, :, 3] > 0)
    a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    h, w = a.shape[:2]
    solid = a[:, :, 3] > AL
    mag = solid & magenta_family(a)
    print("%s %dx%d  中线=%d" % (key, w, h, h // 2))
    print("  y     实体   洋红   占比  洋红跨度 跨/宽  (占比达标 | 跨度达标)")
    for y in range(int(h * 0.5), h):
        ns = int(solid[y].sum())
        nm = int(mag[y].sum())
        r = (nm / float(ns)) if ns else 0.0
        xs_m = np.where(mag[y])[0]
        span = int(xs_m.max() - xs_m.min() + 1) if len(xs_m) else 0
        sr = span / float(w)
        ok1 = ns and r >= ROW_MAG_RATIO
        ok2 = sr >= 0.55
        tag = ("占比 " if ok1 else "     ") + ("| 跨度" if ok2 else "|     ")
        if ns == 0:
            tag = "(空)"
        print("  %4d  %5d  %5d  %5.2f  %6d  %5.2f  %s" % (y, ns, nm, r, span, sr, tag))


if __name__ == "__main__":
    for k in [a.upper() for a in sys.argv[1:]] or ["Z00"]:
        main(k)
