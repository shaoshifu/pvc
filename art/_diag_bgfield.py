# -*- coding: utf-8 -*-
"""试验：冻结参考色的「背景色场 + 受限剥离」。

要解决的两处残留（P26 目视确认）：
  ① 贴着角色轮廓的粉色光晕 —— 模型把背景色烘焙成了角色的边缘辉光
  ② 零散粉色颗粒 —— 每个颗粒自身边缘构成屏障，泛洪进不去内部

为什么原来的泛洪停在轮廓处：
  参考色 ref 是「已确定背景的 3x3 加权模糊」，而 known 会随着波前不断加入
  新判定为背景的像素。走到角色边缘时，ref 已经被角色自身的颜色污染，
  于是"和 ref 的差 < tol" 一直成立 —— 波前要么停下（屏障挡住），
  要么冲进角色内部（无屏障时把整只蘑菇吃掉，实测不透明 40%→11%）。

正解（本脚本）：把参考色**冻结**。
  1. mask0 = 带边缘屏障的泛洪（保角色，不可越过轮廓）
  2. 用 mask0 里的背景色做一次「向外推演」，得到覆盖全图的背景色场 ref_f
     —— 推演过程中只扩散、不吸收新源，所以角色区域得到的是
     "该处背景本该是什么颜色"的合理外插。
  3. 从 mask0 出发做受限剥离：只往「颜色接近 ref_f」的方向爬，
     步数上限 R 圈。光晕和颗粒的颜色≈背景 → 被吃掉；
     角色本体颜色远离背景 → 爬不进去。

量化输出：泛洪占比 / 不透明占比 / 洋红残留px
用法：python art/_diag_bgfield.py P26 P43 P00 P05 P33
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
WORK = 320


def bg_field(small, mask, iters=40):
    """把 mask 内的颜色向外推演成覆盖全图的背景色场（冻结源，只扩散）。"""
    known = np.where(mask[:, :, None], small, 0.0)
    wgt = mask.astype(np.float32)[:, :, None]
    for _ in range(iters):
        if wgt.min() > 0:
            break
        k = U._blur3(known)
        w = U._blur3(wgt)
        # 注意 wgt 是 (h,w,1)：写 `w > 1e-3` 会和 2 维掩膜广播成 (h,w,w)，直接报错。
        # 只填充「自己还没值、但邻居已经有值」的像素。
        fill = (wgt[:, :, 0] <= 1e-3) & (w[:, :, 0] > 1e-3)
        if not fill.any():
            break
        known = np.where(fill[:, :, None], k / np.maximum(w, 1e-3), known)
        wgt = np.where(fill[:, :, None], 1.0, wgt)
    return known / np.maximum(wgt, 1e-3)


def grow(m):
    o = m.copy()
    o[1:, :] |= m[:-1, :]
    o[:-1, :] |= m[1:, :]
    o[:, 1:] |= m[:, :-1]
    o[:, :-1] |= m[:, 1:]
    return o


def peel(small, ref, mask0, tol=34.0, rounds=26):
    """从 mask0 出发，只向「颜色接近 ref」的地方爬。"""
    mask = mask0.copy()
    for _ in range(rounds):
        cand = grow(mask) & ~mask
        if not cand.any():
            break
        d = np.sqrt(((small - ref) ** 2).sum(axis=2))
        add = cand & (d < tol)
        if not add.any():
            break
        mask |= add
    return mask


def small_of(rgb, k=None):
    H, W = rgb.shape[:2]
    k = k or min(1.0, float(WORK) / max(H, W))
    sw, sh = max(16, int(W * k)), max(16, int(H * k))
    return np.array(Image.fromarray(rgb.astype(np.uint8)).resize((sw, sh), Image.BILINEAR)).astype(np.float32)


def up(mask, W, H):
    return np.array(Image.fromarray((mask * 255).astype(np.uint8)).resize(
        (W, H), Image.BILINEAR)).astype(np.float32) / 255.0 > 0.5


def evaluate(key, gth=26, tol_flood=26, tol_peel=34.0, rounds=26, save=False):
    jp = os.path.join(SRC, "u_%s.jpg" % key.lower())
    rgb = np.array(Image.open(jp).convert("RGB")).astype(np.float32)
    H, W = rgb.shape[:2]
    c, spread = U.border_color(rgb)
    rgb2, al0 = U.key_out(rgb, c)

    small = small_of(rgb)
    grad = E.grad_mag(small)
    m0 = E.flood_edged(small, grad, gth, tol_flood, 1)
    ref = bg_field(small, m0)
    mp = peel(small, ref, m0, tol_peel, rounds)
    mask_full = up(mp, W, H)

    al = np.where(mask_full, 0.0, al0)
    t = np.clip(rgb2, 0, 255)
    r, g, b = t[:, :, 0], t[:, :, 1], t[:, :, 2]
    mag = int(((r - g > 60) & (b - g > 40) & (al > 40)).sum())
    op = int((al > 200).sum())
    print("%-6s 离散%5.1f  泛洪%5.1f%%  剥离后背景%5.1f%%  不透明%5.1f%%  洋红残留%7d" % (
        key, spread, m0.mean() * 100, mp.mean() * 100, op * 100.0 / (W * H), mag))

    if save:
        res = np.dstack([rgb2, al]).astype(np.uint8)
        im = Image.fromarray(res, "RGBA")
        im.thumbnail((460, 460), Image.NEAREST)
        a = np.array(im)
        h, w = a.shape[:2]
        y, x = np.mgrid[0:520, 0:520]
        ck = np.where((((x // 14) + (y // 14)) % 2) == 0, 205, 168)
        bgc = np.dstack([ck, ck, ck]).astype(np.uint8)
        ox, oy = (520 - w) // 2, (520 - h) // 2
        alr = a[:, :, 3:4].astype(np.float32) / 255.0
        sub = bgc[oy:oy + h, ox:ox + w].astype(np.float32)
        bgc[oy:oy + h, ox:ox + w] = np.clip(a[:, :, :3] * alr + sub * (1 - alr) + 0.5, 0, 255).astype(np.uint8)
        out = os.path.join(HERE, "peel_%s.png" % key)
        Image.fromarray(bgc).save(out)
        print("      -> %s" % out)


if __name__ == "__main__":
    for k in (sys.argv[1:] or ["P26", "P43", "P00", "P05", "P33"]):
        evaluate(k.upper(), save=True)
