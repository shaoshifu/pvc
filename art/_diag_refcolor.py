# -*- coding: utf-8 -*-
"""用「底部参考色相似度 + 洋红族」双闸门泛洪切椭圆。

观察：椭圆不是固定色，而是**每张图各自**把纯洋红 #FF00FF 暗化后的同族色
（P00 实测 RGB(73,19,81)）。所以判据 = 「和画面最底部那几行的代表色有多像」。

流程：
  1. 取最底部 3 行里 alpha>AL 的像素，中位数 = 参考色 ref
  2. dist(px) = 各通道欧氏距离
  3. walk = (dist < TOL) & (alpha > AL) & 洋红族(R>G+18 且 B>G+18)
     ↑ 洋红族这道闸门是关键：P41 角色本体是棕橙色，虽然暗部
       与椭圆参考色距离在 TOL 内，但不满足 B>G+18，被挡住。
  4. 从底边 3 行的 walk 像素 4 邻域泛洪
  5. bbox 高/宽 < ASPECT_MAX 才算椭圆（双保险）

用法： python art/_diag_refcolor.py P00 P03 P08 P11 P14 P26 P29 P33 P37 P41 P43 --verbose
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DST = os.path.join(ROOT, "assets")

AL = 24
TOL = 60.0
ASPECT_MAX = 0.40


def magenta_family(a):
    r = a[:, :, 0].astype(np.int32)
    g = a[:, :, 1].astype(np.int32)
    b = a[:, :, 2].astype(np.int32)
    return (r > g + 18) & (b > g + 18)


def ref_flood(a, tol=TOL, verbose=False, need_mag=True):
    h, w = a.shape[:2]
    al = a[:, :, 3].astype(np.float32)
    rgb = a[:, :, :3].astype(np.float32)
    solid = al > AL

    band = slice(max(0, h - 3), h)
    m = solid[band]
    if m.sum() < 4:
        return np.zeros((h, w), bool), None
    ref = np.median(rgb[band][m], axis=0)

    d = np.sqrt(((rgb - ref) ** 2).sum(axis=2))
    walk = solid & (d < tol)
    if need_mag:
        walk &= magenta_family(a)

    seed = np.zeros((h, w), bool)
    seed[max(0, h - 3):] = walk[max(0, h - 3):]
    if seed.sum() == 0:
        return np.zeros((h, w), bool), None

    cur = seed.copy()
    for _ in range(4000):
        n = cur.copy()
        n[1:, :] |= cur[:-1, :]
        n[:-1, :] |= cur[1:, :]
        n[:, 1:] |= cur[:, :-1]
        n[:, :-1] |= cur[:, 1:]
        n &= walk
        if np.array_equal(n, cur):
            break
        cur = n

    ys, xs = np.where(cur)
    if len(xs) == 0:
        return np.zeros((h, w), bool), None
    cw, ch = xs.max() - xs.min() + 1, ys.max() - ys.min() + 1
    asp = ch / float(cw)
    ok = asp < ASPECT_MAX
    st = (ref, cw, ch, asp, int(len(xs)), ok)
    if verbose:
        print("   ref=RGB(%d,%d,%d)  bbox %dx%d (y=%d..%d) 高宽比%.3f 面积%d -> %s"
              % (ref[0], ref[1], ref[2], cw, ch, ys.min(), ys.max(), asp, len(xs),
                 "椭圆" if ok else "放弃(太竖)"))
    return (cur if ok else np.zeros((h, w), bool)), st


if __name__ == "__main__":
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    verbose = "--verbose" in sys.argv
    nomag = "--nomag" in sys.argv
    ks = [a.upper() for a in argv] or ["P00", "P03", "P08", "P11", "P14", "P26", "P29", "P33", "P37", "P41", "P43"]
    for k in ks:
        pre = "rgplant_" if k[0] == "P" else "rgzombie_"
        p = os.path.join(DST, pre + k[1:] + ".bmp")
        if not os.path.exists(p):
            print("%-5s 缺" % k)
            continue
        a, w, h = U.read_bmp32(p)
        m, st = ref_flood(a, verbose=verbose, need_mag=not nomag)
        n = int(m.sum())
        print("%-5s %dx%d  切出 %4d px (%.1f%%)  %s"
              % (k, w, h, n, 100.0 * n / (w * h), "" if st else "无种子/放弃"))
