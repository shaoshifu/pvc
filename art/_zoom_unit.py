# -*- coding: utf-8 -*-
"""放大单张（原图 + 归档后），并排对比，供目视判断残留。

用法：python art/_zoom_unit.py P26 [P00 Z43 ...]
输出 art/zoom_<key>.png —— 左：mmx 原图，右：抠图后贴在棋盘格上
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "gen2")
DST = os.path.join(ROOT, "assets")

BOX = 720       # 输出边长
PAD = 8


def checker(size=16, a=215, b=175, box=BOX):
    y, x = np.mgrid[0:box, 0:box]
    c = np.where((((x // size) + (y // size)) % 2) == 0, a, b)
    return np.dstack([c, c, c]).astype(np.uint8)


def fit(a, box, up=True):
    """装进 box 见方。up=True 时也放大 —— 检查残留必须看原像素级细节，
    归档后的 BMP 只有 ~200px，按 1:1 画出来太小，看不清边缘。"""
    h, w = a.shape[:2]
    k = (box - 2 * PAD) / float(max(h, w))
    if (k < 1.0) or (up and k > 1.0):
        a = np.array(Image.fromarray(a.astype(np.uint8), "RGBA").resize(
            (max(1, int(round(w * k))), max(1, int(round(h * k)))),
            Image.LANCZOS if k < 1.0 else Image.NEAREST))
    return a


def on_checker(a, box):
    a = fit(a, box)
    h, w = a.shape[:2]
    bg = checker(box=box)
    al = a[:, :, 3:4].astype(np.float32) / 255.0
    ox, oy = (box - w) // 2, box - PAD - h
    sub = bg[oy:oy + h, ox:ox + w].astype(np.float32)
    out = a[:, :, :3].astype(np.float32) * al + sub * (1.0 - al)
    bg[oy:oy + h, ox:ox + w] = np.clip(out + 0.5, 0, 255).astype(np.uint8)
    return bg


def make(key):
    jp = os.path.join(SRC, "u_%s.jpg" % key.lower())
    bp = os.path.join(DST, (("rgplant_%s" % key[1:]) if key[0] == "P"
                            else ("rgzombie_%s" % key[1:])) + ".bmp")
    if not os.path.exists(jp):
        print("  %s 缺源图" % key)
        return
    raw = Image.open(jp).convert("RGB")
    rw, rh = raw.size
    k = (BOX - 2 * PAD) / float(max(rw, rh))
    raw = raw.resize((int(rw * k), int(rh * k)), Image.LANCZOS)
    ra = np.dstack([np.array(raw), np.full((raw.size[1], raw.size[0]), 255, np.uint8)])
    left = np.full((BOX, BOX, 3), 30, np.uint8)
    lh, lw = ra.shape[:2]
    left[(BOX - lh) // 2:(BOX - lh) // 2 + lh, (BOX - lw) // 2:(BOX - lw) // 2 + lw] = ra[:, :, :3]
    right = on_checker(U.read_bmp32(bp)[0], BOX)
    div = np.full((BOX, 6, 3), 255, np.uint8)
    out = np.concatenate([left, div, right], axis=1)
    op = os.path.join(HERE, "zoom_%s.png" % key)
    Image.fromarray(out).save(op)
    c, spread = U.border_color(np.array(Image.open(jp).convert("RGB")).astype(np.float32))
    print("  %s -> %s   源图边框色=%s 离散=%.1f" % (key, op, c.round(0).astype(int), spread))


if __name__ == "__main__":
    for k in (sys.argv[1:] or ["P26"]):
        make(k.upper())
