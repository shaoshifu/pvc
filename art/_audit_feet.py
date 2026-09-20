# -*- coding: utf-8 -*-
"""检查「脚下烘焙椭圆阴影」—— 一个疑似影响全部 100 张的系统性缺陷。

线索：art/sheet_plants.png / sheet_zombies.png 上，几乎**每一只**角色脚下
      都有一片洋红/粉紫的椭圆。而 STYLE 里明确写了
      "no ground shadow, no drop shadow, no ellipse under the character"。
      模型没听（这类否定项本来就常被忽略），而这片椭圆是"比背景略深的洋红"，
      正好落在键控的 lo..hi 过渡区里 —— 于是被留成了半透明椭圆。

为什么必须修：
  游戏里 drawPlantEntity / drawZombie 已经会自己画三层接触影 + 品质色光垫
  （见 drawContactShadow 与 RGQ_COL 那两段）。贴图再自带一片洋红椭圆，
  叠上去就是"脚下挂了圈粉红"，而且洋红和草地绿完全冲突，一眼假。

判据（本脚本）：
  取底部若干行，量「alpha>40 的横向宽度」。角色脚/根是细的，
  烘焙椭圆却是整个底座 —— 若底部宽度接近整体最大宽度，且底部中心
  颜色偏向出图背景色，即可判定存在椭圆。

用法：python art/_audit_feet.py
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


def check(path, jpg):
    a, w, h = U.read_bmp32(path)
    al = a[:, :, 3].astype(np.float32)
    a3 = np.maximum(al[:, :, None] / 255.0, 1e-3)
    true = np.clip(a[:, :, :3].astype(np.float32) / a3, 0, 255)

    c, spread = U.border_color(
        np.array(__import__("PIL.Image", fromlist=["Image"]).open(jpg).convert("RGB")).astype(np.float32))

    rows = al > 40
    maxw = int(rows.sum(1).max()) if rows.any() else 0
    if maxw == 0:
        return None
    # 底部 6 行
    bw = int(rows[-6:].sum(1).max())
    # 底部区域里"接近背景色"的像素占比（椭圆特征）
    foot = true[-8:]
    m = (al[-8:] > 40)
    if m.sum() == 0:
        return None
    d = np.sqrt(((foot - c) ** 2).sum(2))
    near_bg = float((m & (d < 120)).sum()) / float(m.sum())

    return dict(w=w, h=h, maxw=maxw, bottom_w=bw,
                ratio=bw / float(maxw), near_bg=near_bg, spread=spread)


def main():
    keys = (["P%02d" % i for i in range(50)] + ["Z%02d" % i for i in range(50)])
    bad = []
    print("%-6s %7s %7s %7s %8s %8s" % ("key", "整体宽", "底部宽", "占比", "底近背景", "源离散"))
    for k in keys:
        pre = "rgplant_" if k[0] == "P" else "rgzombie_"
        p = os.path.join(DST, pre + k[1:] + ".bmp")
        jpg = os.path.join(SRC, "u_%s.jpg" % k.lower())
        if not os.path.exists(p) or not os.path.exists(jpg):
            print("%-6s 缺" % k)
            continue
        r = check(p, jpg)
        if r is None:
            print("%-6s 空" % k)
            continue
        flag = ""
        if r["ratio"] > 0.80 and r["near_bg"] > 0.18:
            flag = "  << 疑似烘焙椭圆"
            bad.append(k)
        print("%-6s %7d %7d %7.2f %8.2f %8.1f%s" % (
            k, r["maxw"], r["bottom_w"], r["ratio"], r["near_bg"], r["spread"], flag))
    print("\n疑似脚下椭圆：%d 张  %s" % (len(bad), " ".join(bad)))


if __name__ == "__main__":
    main()
