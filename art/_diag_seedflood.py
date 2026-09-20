# -*- coding: utf-8 -*-
"""验证「从画面底边泛洪洋红系像素」能否干净切出贴地椭圆。

为什么换思路：
  art/_diag_ascii.py 的逐像素图证明，椭圆与角色在 alpha>30 掩码里是**同一个连通域**
  （P00 底带 y=185..197 是 RGB(73,19,81) 的实心洋红带，x=34..172，
   与上方 y<=182 的蓝色球体直接相连），所以任何"找独立连通域"的判定都不会命中。

新思路：
  椭圆必然**触底**，且必然是**洋红系**（R>G+18 且 B>G+18）——
  这是模型给纯洋红背景补阴影时留下的同色族暗化。
  于是从底边两行取洋红系像素做种子，4 邻域泛洪，限定只能走洋红系且 alpha>30。
  角色的蓝色/绿色/棕色本体不在洋红族内，天然是屏障。
  最后用 bbox 形状做保险：高/宽 必须 < ASPECT，否则判定为"角色本体恰好是洋红系"，放弃。

用法： python art/_diag_seedflood.py P00 P03 P14 P26 [--verbose]
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DST = os.path.join(ROOT, "assets")

AL = 30
ASPECT_MAX = 0.55


def magenta(a):
    r = a[:, :, 0].astype(np.int32)
    g = a[:, :, 1].astype(np.int32)
    b = a[:, :, 2].astype(np.int32)
    return (r > g + 18) & (b > g + 18)


def seed_flood(a, verbose=False):
    h, w = a.shape[:2]
    al = a[:, :, 3]
    mag = magenta(a)
    walk = (al > AL) & mag
    seed = np.zeros((h, w), bool)
    seed[h - 2:] = walk[h - 2:]
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
    if verbose:
        print("   底泛洪 bbox %dx%d (x=%d..%d y=%d..%d) 高宽比%.3f 面积%d -> %s"
              % (cw, ch, xs.min(), xs.max(), ys.min(), ys.max(), asp, len(xs),
                 "判定为椭圆" if ok else "放弃（太竖，疑为角色本体）"))
    return (cur if ok else np.zeros((h, w), bool)), (cw, ch, asp, len(xs))


if __name__ == "__main__":
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    verbose = "--verbose" in sys.argv
    ks = [a.upper() for a in argv] or ["P00", "P03", "P08", "P11", "P14", "P26", "P33", "P43"]
    for k in ks:
        pre = "rgplant_" if k[0] == "P" else "rgzombie_"
        p = os.path.join(DST, pre + k[1:] + ".bmp")
        if not os.path.exists(p):
            print("%-5s 缺" % k)
            continue
        a, w, h = U.read_bmp32(p)
        m, st = seed_flood(a, verbose)
        n = int(m.sum())
        print("%-5s %dx%d  底泛洪 %d px (%.1f%%)  %s"
              % (k, w, h, n, 100.0 * n / (w * h), "" if st else "无种子"))
