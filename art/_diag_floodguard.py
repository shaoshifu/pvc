# -*- coding: utf-8 -*-
"""对照实验：matte_file 里那道 `al < 200` 保护判据，是不是把粉雾留下了？

推断（待证）：
  flood_bg 判定「从边框连通可达」= 背景。P26 的粉雾中心是 (237,61,187)，
  到边框中位色 (168,8,126) 的距离约 106 > hi(100) —— 于是 key_out 给出 alpha=255。
  matte_file 里写的是：
        al = np.where(bg & (al < 200), 0, al)
  意思是「泛洪说是背景，且键控也偏透明」才清零。粉雾的键控 alpha 是 255，
  所以即使泛洪已经把它吃进背景集合，这一条也会把它**原样留下**。

  这道判据本意是保护「贴着背景色的角色暗部」，但角色暗部根本不在泛洪集合里
  —— 泛洪在角色硬边处就停了，进不去。所以这道保护其实是在防一个不存在的情况。

本脚本量化三种策略：
  A. 现行   al = where(bg & (al<200), 0, al)
  B. 直接清 al = where(bg, 0, al)
  C. 软化   al = where(bg, al*0.0 与 key 结果取 min 的过渡, al)  —— 见下

度量：
  * 洋红残留 px：整图 (r-g>60)&(b-g>40)&(a>40) 的像素数（粉雾/粉点）
  * 主体面积：alpha>200 的像素数（判 C 是否把角色啃掉）
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "gen2")


def magenta_px(true, al):
    r, g, b = true[:, :, 0], true[:, :, 1], true[:, :, 2]
    m = (r - g > 60) & (b - g > 40) & (al > 40)
    return int(m.sum())


def run(key):
    jp = os.path.join(SRC, "u_%s.jpg" % key.lower())
    a = np.array(__import__("PIL.Image", fromlist=["Image"]).open(jp).convert("RGB")).astype(np.float32)
    c, spread = U.border_color(a)
    rgb2, al0 = U.key_out(a, c)
    bg = U.flood_bg(a)

    res = {}
    res["A 现行(保护<200)"] = np.where(bg & (al0 < 200), 0.0, al0)
    res["B 泛洪即清"] = np.where(bg, 0.0, al0)
    # C：泛洪区域内做一个 0.35 的底，靠键控 alpha 决定过渡，避免边界硬切
    res["C 软化(清九成)"] = np.where(bg, al0 * 0.10, al0)

    print("\n=== %s  源背景=%s 离散%.1f   泛洪判为背景=%.1f%% ===" % (
        key, c.round(0).astype(int), spread, bg.mean() * 100))
    print("%-18s %10s %10s %10s" % ("策略", "洋红残留px", "不透明px", "全清px"))
    base = None
    for name, al in res.items():
        true = np.clip(rgb2, 0, 255)
        op = int((al > 200).sum())
        cl = int((al < 20).sum())
        m = magenta_px(true, al)
        if base is None:
            base = op
        print("%-18s %10d %10d %10d" % (name, m, op, cl))
    return


if __name__ == "__main__":
    for k in (sys.argv[1:] or ["P26", "P43", "P00", "P05"]):
        run(k.upper())
