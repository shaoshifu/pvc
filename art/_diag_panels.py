# -*- coding: utf-8 -*-
"""并排对比多种抠图策略，肉眼定夺。

面板：源图 | 纯键控 | 键控+泛洪(带保护) | 键控+泛洪(全清) | 边缘屏障泛洪
用法：python art/_diag_panels.py P26
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402
import _diag_edgemask as E                               # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "gen2")
BOX = 460
PAD = 6


def checker(box=BOX, size=14, a=200, b=165):
    y, x = np.mgrid[0:box, 0:box]
    c = np.where((((x // size) + (y // size)) % 2) == 0, a, b)
    return np.dstack([c, c, c]).astype(np.uint8)


def compose(rgba, box=BOX):
    h, w = rgba.shape[:2]
    k = (box - 2 * PAD) / float(max(h, w))
    im = Image.fromarray(rgba.astype(np.uint8), "RGBA")
    im = im.resize((max(1, int(round(w * k))), max(1, int(round(h * k)))),
                   Image.LANCZOS if k < 1 else Image.NEAREST)
    a = np.array(im)
    h, w = a.shape[:2]
    bg = checker(box)
    al = a[:, :, 3:4].astype(np.float32) / 255.0
    ox, oy = (box - w) // 2, (box - h) // 2
    sub = bg[oy:oy + h, ox:ox + w].astype(np.float32)
    bg[oy:oy + h, ox:ox + w] = np.clip(a[:, :, :3] * al + sub * (1 - al) + 0.5, 0, 255).astype(np.uint8)
    return bg


def label_strip(text, w, h=26):
    """用 PIL 画条白底黑字，标出每个面板是什么。"""
    from PIL import ImageDraw
    box = Image.new("RGB", (w, h), (255, 255, 255))
    ImageDraw.Draw(box).text((6, 6), text, fill=(0, 0, 0))
    return np.array(box)


def main(key):
    jp = os.path.join(SRC, "u_%s.jpg" % key.lower())
    rgb = np.array(Image.open(jp).convert("RGB")).astype(np.float32)
    c, spread = U.border_color(rgb)
    rgb2, al0 = U.key_out(rgb, c)
    H, W = rgb.shape[:2]

    bgf = U.flood_bg(rgb)
    variants = []
    variants.append(("源图", np.dstack([rgb, np.full((H, W), 255, np.uint8)])))
    variants.append(("纯键控", np.dstack([rgb2, al0]).astype(np.uint8)))
    variants.append(("泛洪(带保护)", np.dstack([rgb2, np.where(bgf & (al0 < 200), 0.0, al0)]).astype(np.uint8)))
    variants.append(("泛洪(全清)", np.dstack([rgb2, np.where(bgf, 0.0, al0)]).astype(np.uint8)))

    # 边缘屏障版
    k2 = min(1.0, 320.0 / max(H, W))
    sw, sh = max(16, int(W * k2)), max(16, int(H * k2))
    small = np.array(Image.fromarray(rgb.astype(np.uint8)).resize((sw, sh), Image.BILINEAR)).astype(np.float32)
    grad = E.grad_mag(small)
    gm = E.flood_edged(small, grad, 26, 26, 1)
    gm_full = np.array(Image.fromarray((gm * 255).astype(np.uint8)).resize((W, H), Image.BILINEAR)).astype(np.float32) / 255.0 > 0.5
    variants.append(("边缘屏障(26,26)", np.dstack([rgb2, np.where(gm_full, 0.0, al0)]).astype(np.uint8)))

    panels = [compose(v) for _, v in variants]
    strips = [label_strip(t, BOX) for t, _ in variants]
    row = np.concatenate(panels, axis=1)
    lab = np.concatenate(strips, axis=1)
    out = np.concatenate([lab, row], axis=0)
    op = os.path.join(HERE, "panels_%s.png" % key)
    Image.fromarray(out).save(op)
    print("%s -> %s  (源背景=%s 离散%.1f)" % (key, op, c.round(0).astype(int), spread))


if __name__ == "__main__":
    for k in (sys.argv[1:] or ["P26"]):
        main(k.upper())
