# -*- coding: utf-8 -*-
"""从**源图**出发验证椭圆检测：源图 → 抠图 → 修剪 → 检测 → 叠加图。

为什么不能直接拿 assets 里的 BMP 验证：
  assets 里的是**已经清过椭圆**的成品，再检测只能看到"还剩多少"，
  看不出"参数改动会不会误伤角色"。要看全貌必须回到 gen2 的源图重跑一遍前处理。

输出 art/sv_<KEY>.png：左=抠图后原样 | 右=被判定为椭圆的像素标红
用法： python art/_diag_srcvis.py Z00 Z03 Z07
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402
from _fix_groundshadow import fix as clean_ellipse       # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "gen2")


def to_rgba(a):
    f = a.astype(np.float32) / 255.0
    out = np.zeros_like(f)
    out[:, :, :3] = f[:, :, :3] * f[:, :, 3:4]
    out[:, :, 3] = f[:, :, 3]
    return (out * 255 + 0.5).astype(np.uint8)


def cell(a, box):
    h, w = a.shape[:2]
    im = Image.fromarray(to_rgba(a), "RGBA")
    yy, xx = np.mgrid[0:h, 0:w]
    dark = (((yy // 12) + (xx // 12)) % 2) == 0
    px = np.zeros((h, w, 4), np.uint8)
    px[~dark] = (185, 185, 185, 255)
    px[dark] = (220, 220, 220, 255)
    bg = Image.fromarray(px, "RGBA")
    bg.alpha_composite(im)
    s = (box - 12) / float(max(w, h))
    nw, nh = max(1, int(w * s)), max(1, int(h * s))
    big = bg.convert("RGB").resize((nw, nh), Image.LANCZOS)
    out = Image.new("RGB", (box, box), (245, 245, 245))
    out.paste(big, ((box - nw) // 2, box - nh - 6))
    return out


def show(key, box=520):
    src = os.path.join(SRC, "u_%s.jpg" % key.lower())
    if not os.path.exists(src):
        print("%-5s 缺源图" % key)
        return
    im, c, spread = U.matte_file(src)
    a = np.array(im)
    ys, xs = np.where(a[:, :, 3] > 0)
    a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    cleaned, st = clean_ellipse(a)
    diff = (a[:, :, 3] > 0) & (cleaned[:, :, 3] == 0)
    n = int(diff.sum())
    b = a.copy()
    b[diff] = (255, 40, 40, 255)
    strip = Image.new("RGB", (box * 2, box), (245, 245, 245))
    strip.paste(cell(a, box), (0, 0))
    strip.paste(cell(b, box), (box, 0))
    out = os.path.join(HERE, "sv_%s.png" % key)
    strip.save(out)
    desc = "-" if st is None else "%dx%d 高宽比%.2f" % (st["w"], st["h"], st["aspect"])
    print("%-5s 切 %5d px (%4.1f%%)  %s" % (key, n, 100.0 * n / a[:, :, 3].size, desc))


if __name__ == "__main__":
    for k in [a.upper() for a in sys.argv[1:]] or ["Z00"]:
        show(k)
