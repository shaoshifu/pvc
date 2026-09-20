# -*- coding: utf-8 -*-
"""把「边缘屏障泛洪」结果单独放大，和源图并排，用来最终判定。

用法：python art/_zoom_edged.py P26 [grad_thr] [tol]
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
BOX = 700
PAD = 8


def checker(box=BOX, size=18, a=205, b=168):
    y, x = np.mgrid[0:box, 0:box]
    c = np.where((((x // size) + (y // size)) % 2) == 0, a, b)
    return np.dstack([c, c, c]).astype(np.uint8)


def compose(rgba):
    h, w = rgba.shape[:2]
    k = (BOX - 2 * PAD) / float(max(h, w))
    im = Image.fromarray(rgba.astype(np.uint8), "RGBA").resize(
        (max(1, int(round(w * k))), max(1, int(round(h * k)))), Image.NEAREST)
    a = np.array(im)
    h, w = a.shape[:2]
    bg = checker()
    al = a[:, :, 3:4].astype(np.float32) / 255.0
    ox, oy = (BOX - w) // 2, (BOX - h) // 2
    sub = bg[oy:oy + h, ox:ox + w].astype(np.float32)
    bg[oy:oy + h, ox:ox + w] = np.clip(a[:, :, :3] * al + sub * (1 - al) + 0.5, 0, 255).astype(np.uint8)
    return bg


def main(key, gth, tol):
    jp = os.path.join(SRC, "u_%s.jpg" % key.lower())
    rgb = np.array(Image.open(jp).convert("RGB")).astype(np.float32)
    H, W = rgb.shape[:2]
    c, spread = U.border_color(rgb)
    rgb2, al0 = U.key_out(rgb, c)

    k2 = min(1.0, 320.0 / max(H, W))
    sw, sh = max(16, int(W * k2)), max(16, int(H * k2))
    small = np.array(Image.fromarray(rgb.astype(np.uint8)).resize((sw, sh), Image.BILINEAR)).astype(np.float32)
    grad = E.grad_mag(small)
    gm = E.flood_edged(small, grad, gth, tol, 1)
    gm_full = np.array(Image.fromarray((gm * 255).astype(np.uint8)).resize((W, H), Image.BILINEAR)).astype(np.float32) / 255.0 > 0.5
    al = np.where(gm_full, 0.0, al0)
    res = np.dstack([rgb2, al]).astype(np.uint8)

    raw = Image.open(jp).convert("RGB")
    rw, rh = raw.size
    kk = (BOX - 2 * PAD) / float(max(rw, rh))
    raw = np.array(raw.resize((int(rw * kk), int(rh * kk)), Image.LANCZOS))
    left = np.full((BOX, BOX, 3), 26, np.uint8)
    lh, lw = raw.shape[:2]
    left[(BOX - lh) // 2:(BOX - lh) // 2 + lh, (BOX - lw) // 2:(BOX - lw) // 2 + lw] = raw
    out = np.concatenate([left, np.full((BOX, 6, 3), 255, np.uint8), compose(res)], axis=1)
    op = os.path.join(HERE, "edged_%s.png" % key)
    Image.fromarray(out).save(op)
    print("%s grad_thr=%d tol=%d -> %s  泛洪背景占比=%.1f%%  不透明=%.1f%%"
          % (key, gth, tol, op, gm_full.mean() * 100, (al > 200).mean() * 100))


if __name__ == "__main__":
    a = sys.argv[1:]
    main((a[0] if a else "P26").upper(),
         int(a[1]) if len(a) > 1 else 26,
         int(a[2]) if len(a) > 2 else 26)
