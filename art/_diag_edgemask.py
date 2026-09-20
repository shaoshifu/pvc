# -*- coding: utf-8 -*-
"""试验：带「边缘屏障」的泛洪，能不能既吃掉渐变背景、又不啃角色。

为什么之前两种做法都不行：
  * 只按「到邻近背景色的差 < tol」泛洪：渐变背景要求 tol 放宽
    （P26 的 spread=16.1 ⇒ tol=35），但角色自身的平滑渐变（蘑菇帽
    从深蓝到浅蓝也是连续过渡）同样满足 —— 泛洪顺着它走进角色内部，
    整只蘑菇被吃掉（实测不透明 40% -> 11%）。
  * 加「且键控也偏透明」的保护：把背景里的亮粉雾留了下来（见 _diag_floodguard）。

正解：把「边缘」当成屏障。
  角色轮廓在图像上是一道**色彩突变**（梯度大），背景渐变是**低频**（梯度小）。
  所以：先算梯度幅值图，把超过阈值的像素标成屏障，再在非屏障区域泛洪。
  屏障是闭合曲线 → 泛洪从边框进不去角色内部，而背景渐变一路畅通。

本脚本扫描几组 (grad_thr, tol) 看效果，并给出量化：
  洋红残留px（越低越好） / 不透明px（越接近源图主体越好） / 角色bbox是否完整
用法：python art/_diag_edgemask.py P26 P43 P00 P05 P33
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "gen2")
WORK = 320


def grad_mag(small):
    """归一化最大梯度幅值（每通道分别求梯度取最大）。"""
    g = np.zeros(small.shape[:2], np.float32)
    for ch in range(3):
        gy, gx = np.gradient(small[:, :, ch])
        g = np.maximum(g, np.sqrt(gx * gx + gy * gy))
    return g


def dilate(m, it=1):
    for _ in range(it):
        o = m.copy()
        o[1:, :] |= m[:-1, :]
        o[:-1, :] |= m[1:, :]
        o[:, 1:] |= m[:, :-1]
        o[:, :-1] |= m[:, 1:]
        m = o
    return m


def flood_edged(small, grad, grad_thr, tol, dil=1):
    """在非屏障区域从边框泛洪，参考色跟随渐变。"""
    h, w = small.shape[:2]
    barrier = dilate(grad > grad_thr, dil)
    free = ~barrier

    def grow(m):
        o = m.copy()
        o[1:, :] |= m[:-1, :]
        o[:-1, :] |= m[1:, :]
        o[:, 1:] |= m[:, :-1]
        o[:, :-1] |= m[:, 1:]
        return o

    mask = np.zeros((h, w), bool)
    mask[0, :] = mask[-1, :] = mask[:, 0] = mask[:, -1] = True
    mask &= free
    known = np.where(mask[:, :, None], small, 0.0)
    wgt = mask.astype(np.float32)[:, :, None]

    for _ in range(max(h, w)):
        cand = grow(mask) & ~mask & free
        if not cand.any():
            break
        ref = U._blur3(known) / np.maximum(U._blur3(wgt), 1e-3)
        d = np.sqrt(((small - ref) ** 2).sum(axis=2))
        add = cand & (d < tol)
        if not add.any():
            break
        mask |= add
        known[add] = small[add]
        wgt[add] = 1.0
    return mask


def eval_key(key, grad_thr, tol, dil=1):
    jp = os.path.join(SRC, "u_%s.jpg" % key.lower())
    rgb = np.array(Image.open(jp).convert("RGB")).astype(np.float32)
    H, W = rgb.shape[:2]
    k = min(1.0, float(WORK) / max(H, W))
    sw, sh = max(16, int(W * k)), max(16, int(H * k))
    small = np.array(Image.fromarray(rgb.astype(np.uint8)).resize((sw, sh), Image.BILINEAR)).astype(np.float32)

    grad = grad_mag(small)
    bg = flood_edged(small, grad, grad_thr, tol, dil)
    bg_full = np.array(Image.fromarray((bg * 255).astype(np.uint8)).resize(
        (W, H), Image.BILINEAR)).astype(np.float32) / 255.0 > 0.5

    c, spread = U.border_color(rgb)
    rgb2, al0 = U.key_out(rgb, c)
    al = np.where(bg_full, 0.0, al0)

    r, g, b = np.clip(rgb2, 0, 255)[:, :, 0], np.clip(rgb2, 0, 255)[:, :, 1], np.clip(rgb2, 0, 255)[:, :, 2]
    mag = int(((r - g > 60) & (b - g > 40) & (al > 40)).sum())
    op = int((al > 200).sum())
    # 主体包围盒覆盖率：alpha>200 的像素落在最大连通域 bbox 里的比例
    ys, xs = np.where(al > 200)
    if len(xs) == 0:
        return mag, 0, 0, 0.0
    bw, bh = xs.max() - xs.min() + 1, ys.max() - ys.min() + 1
    cover = op / float(bw * bh)
    return mag, op, op * 100.0 / (W * H), cover


if __name__ == "__main__":
    keys = [k.upper() for k in (sys.argv[1:] or ["P26", "P43", "P00", "P05", "P33"])]
    print("%-6s %-9s %-7s %9s %9s %8s %7s" % ("key", "grad_thr", "tol", "洋红px", "不透明px", "占比%", "bbox填充"))
    for key in keys:
        base = None
        # 基准：只看键控（不泛洪），作为"角色完整"的参考
        jp = os.path.join(SRC, "u_%s.jpg" % key.lower())
        rgb = np.array(Image.open(jp).convert("RGB")).astype(np.float32)
        c, spread = U.border_color(rgb)
        _, al0 = U.key_out(rgb, c)
        base = int((al0 > 200).sum())
        print("%-6s %-9s %-7s %9s %9d  <-纯键控基准" % (key, "-", "-", "-", base))
        for gth in (18, 26, 36):
            for tol in (16, 26):
                mag, op, pct, cover = eval_key(key, gth, tol)
                print("%-6s %-9d %-7d %9d %9d %8.1f %7.2f" % (key, gth, tol, mag, op, pct, cover))
        print()
