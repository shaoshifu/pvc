# -*- coding: utf-8 -*-
"""把「参考色泛洪」切出的区域画成红色叠加图，肉眼复核是否误伤角色。

输出 art/rv_<KEY>.png：源图 | 清除后（被清像素标红）
用法： python art/_diag_refvis.py P00 P03 P14 P33 P41 P43
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _diag_refcolor import ref_flood                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DST = os.path.join(ROOT, "assets")


def to_rgba(a):
    f = a.astype(np.float32) / 255.0
    rgb = f[:, :, :3] * f[:, :, 3:4]
    out = np.zeros_like(f)
    out[:, :, :3] = rgb
    out[:, :, 3] = f[:, :, 3]
    return (out * 255 + 0.5).astype(np.uint8)


def on_checker(a, box):
    h, w = a.shape[:2]
    im = Image.fromarray(to_rgba(a), "RGBA")
    yy, xx = np.mgrid[0:h, 0:w]
    dark = (((yy // 12) + (xx // 12)) % 2) == 0
    px = np.zeros((h, w, 4), np.uint8)
    px[~dark] = (170, 170, 170, 255)
    px[dark] = (210, 210, 210, 255)
    cb = Image.fromarray(px, "RGBA")
    cb.alpha_composite(im)
    cell = Image.new("RGBA", (box, box), (245, 245, 245, 255))
    cell.alpha_composite(cb, ((box - w) // 2, box - h - 8))
    return cell


def show(key, tol=60.0):
    pre = "rgplant_" if key[0] == "P" else "rgzombie_"
    p = os.path.join(DST, pre + key[1:] + ".bmp")
    if not os.path.exists(p):
        print("%-5s 缺" % key)
        return
    a, w, h = U.read_bmp32(p)
    kill, st = ref_flood(a, tol=tol)
    n = int(kill.sum())

    b = a.copy()
    b[kill] = (255, 0, 0, 255)
    box = max(w, h) + 16
    c1 = on_checker(a, box)
    c2 = on_checker(b, box)
    strip = Image.new("RGB", (box * 2, box), (245, 245, 245))
    strip.paste(c1.convert("RGB"), (0, 0))
    strip.paste(c2.convert("RGB"), (box, 0))
    out = os.path.join(HERE, "rv_%s.png" % key)
    strip.save(out)
    print("%-5s 切 %4dpx (%.1f%%) -> %s" % (key, n, 100.0 * n / (w * h), os.path.basename(out)))


if __name__ == "__main__":
    ks = [a.upper() for a in sys.argv[1:]] or ["P00"]
    for k in ks:
        show(k)
