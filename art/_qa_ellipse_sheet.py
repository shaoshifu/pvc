# -*- coding: utf-8 -*-
"""椭圆检测的批量肉眼复核表：50 张一图，被判定为椭圆的像素标红。

为什么需要：
  逐个开 rv_XX.png 太慢，而且人眼容易漏。
  把所有单元的**底部 32%** 按同一倍率放大平铺成一张大图，
  红色即"将被清除"的区域 —— 一眼扫过去就能发现误伤（红色爬到角色身上）
  或漏判（脚下还留着洋红圈却没标红）。

输出： art/qa_ell_plants.png / art/qa_ell_zombies.png
用法： python art/_qa_ellipse_sheet.py
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402
from _fix_groundshadow import detect as detect_ellipse   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DST = os.path.join(ROOT, "assets")

CELL = 200         # 每格边长
COLS = 10
CROP = 0.34        # 只取底部 34%


def to_rgba(a):
    f = a.astype(np.float32) / 255.0
    out = np.zeros_like(f)
    out[:, :, :3] = f[:, :, :3] * f[:, :, 3:4]
    out[:, :, 3] = f[:, :, 3]
    return (out * 255 + 0.5).astype(np.uint8)


def cell_for(key, tint=True):
    pre = "rgplant_" if key[0] == "P" else "rgzombie_"
    p = os.path.join(DST, pre + key[1:] + ".bmp")
    if not os.path.exists(p):
        return Image.new("RGB", (CELL, CELL), (60, 60, 60)), 0
    a, w, h = U.read_bmp32(p)
    kill, _ = detect_ellipse(a)
    n = int(kill.sum())
    if tint:
        b = a.copy()
        b[kill] = (255, 0, 0, 255)
    else:
        b = a
    y0 = int(h * (1.0 - CROP))
    sub = b[y0:]
    im = Image.fromarray(to_rgba(sub), "RGBA")
    sh, sw = sub.shape[:2]
    yy, xx = np.mgrid[0:sh, 0:sw]
    dark = (((yy // 10) + (xx // 10)) % 2) == 0
    px = np.zeros((sh, sw, 4), np.uint8)
    px[~dark] = (185, 185, 185, 255)
    px[dark] = (220, 220, 220, 255)
    bg = Image.fromarray(px, "RGBA")
    bg.alpha_composite(im)
    # 等比例放大填满 CELL
    s = CELL / float(max(sw, sh))
    nw, nh = max(1, int(sw * s)), max(1, int(sh * s))
    big = bg.convert("RGB").resize((nw, nh), Image.NEAREST)
    out = Image.new("RGB", (CELL, CELL), (245, 245, 245))
    out.paste(big, ((CELL - nw) // 2, CELL - nh))
    return out, n


def sheet(prefix, keys, out):
    rows = (len(keys) + COLS - 1) // COLS
    W, H = CELL * COLS, CELL * rows
    img = Image.new("RGB", (W, H), (245, 245, 245))
    tot = 0
    for i, k in enumerate(keys):
        c, n = cell_for(k)
        tot += 1 if n > 0 else 0
        img.paste(c, ((i % COLS) * CELL, (i // COLS) * CELL))
    img.save(out)
    print("%s  %d x %d  有椭圆 %d/%d" % (os.path.basename(out), W, H, tot, len(keys)))


if __name__ == "__main__":
    sheet("P", ["P%02d" % i for i in range(50)], os.path.join(HERE, "qa_ell_plants.png"))
    sheet("Z", ["Z%02d" % i for i in range(50)], os.path.join(HERE, "qa_ell_zombies.png"))
