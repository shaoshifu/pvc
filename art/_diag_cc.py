# -*- coding: utf-8 -*-
"""诊断：抠图后的 alpha 连通域分布 —— 用来判断"残留的粉雾"是不是独立块。

动机：P26 目视确认角色背后留有一片亮粉色雾 + 零散粉点，
      而它们显然没被泛洪吃掉（泛洪只吃"从边框连通可达"的背景）。
      若这些雾是**独立连通域**，就能用「保留主体连通域」干净地剔掉；
      若它们在像素级和角色相连，就得换色相判据。
      这个脚本就是回答这个问题的。
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "gen2")
DST = os.path.join(ROOT, "assets")


def label_cc(mask, max_iter=3000):
    """纯 numpy 连通域标记：迭代最小标签传播。"""
    h, w = mask.shape
    BIG = h * w
    lab = np.where(mask, np.arange(BIG).reshape(h, w), BIG).astype(np.int64)
    for _ in range(max_iter):
        old = lab
        n = lab.copy()
        n[1:, :] = np.minimum(n[1:, :], lab[:-1, :])
        n[:-1, :] = np.minimum(n[:-1, :], lab[1:, :])
        n[:, 1:] = np.minimum(n[:, 1:], lab[:, :-1])
        n[:, :-1] = np.minimum(n[:, :-1], lab[:, 1:])
        lab = np.where(mask, n, BIG)
        if np.array_equal(lab, old):
            break
    return lab


def diag(key, alpha_thr=40):
    jp = os.path.join(SRC, "u_%s.jpg" % key.lower())
    bp = os.path.join(DST, ("rgplant_%s" % key[1:]) if key[0] == "P"
                      else ("rgzombie_%s" % key[1:]))
    bp += ".bmp"

    a, w, h = U.read_bmp32(bp)
    al = a[:, :, 3]
    a3 = np.maximum(al[:, :, None] / 255.0, 1e-3)
    true = np.clip(a[:, :, :3].astype(np.float32) / a3, 0, 255)

    c, spread = U.border_color(
        np.array(__import__("PIL.Image", fromlist=["Image"]).open(jp).convert("RGB")).astype(np.float32))

    mask = al > alpha_thr
    lab = label_cc(mask)
    ids, counts = np.unique(lab[mask], return_counts=True)
    order = np.argsort(-counts)
    tot = int(mask.sum())

    print("\n=== %s  %dx%d  源图背景=%s 离散%.1f ===" % (key, w, h, c.round(0).astype(int), spread))
    print("alpha>%d 像素 %d（%.1f%%）  连通域 %d 个" % (alpha_thr, tot, tot * 100.0 / (w * h), len(ids)))
    print("%-6s %-8s %-7s %-10s %-22s %s" % ("#", "面积", "占比", "bbox", "平均RGB", "到背景色距"))
    for i in order[:8]:
        lid = ids[i]
        m = (lab == lid)
        ys, xs = np.where(m)
        mc = true[m].mean(0)
        d = float(np.sqrt(((mc - c) ** 2).sum()))
        print("%-6d %-8d %-6.1f%% (%3d,%3d)-(%3d,%3d) (%5.0f,%5.0f,%5.0f)  %s%.0f" % (
            i, counts[i], counts[i] * 100.0 / tot,
            xs.min(), ys.min(), xs.max(), ys.max(), mc[0], mc[1], mc[2],
            "近背景! " if d < 90 else "", d))


if __name__ == "__main__":
    for k in (sys.argv[1:] or ["P26"]):
        diag(k.upper())
