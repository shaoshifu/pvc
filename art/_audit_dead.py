# -*- coding: utf-8 -*-
"""审计僵尸倒地帧：洋红残留 + 透明度分布，并出一张肉眼复核表。

背景：倒地帧是拿存活帧绕脚底旋转派生的。椭圆清除如果做在旋转**之后**，
     椭圆会被转成一个斜的洋红色块 —— 既不贴底、泛洪也够不着。
     所以清除必须挂在 _build_units.build() 的流水线里（旋转之前）。
     本脚本用来证明"清干净了"。

输出 art/qa_dead.png（每只僵尸 5 帧连排，前 8 只）
用法： python art/_audit_dead.py [Z00 Z01 ...]
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DST = os.path.join(ROOT, "assets")

MAG = 0.02        # 洋红像素占比 > 该值 -> 提示可能残留


def mag_ratio(a):
    """量"椭圆色"残留占比。

    判据要比单纯的"洋红族"更严：僵尸常穿**蓝紫色**衣服，它同样满足
    R>G 且 B>G，会把数字抬到 20% 造成满屏误报（实测 Z01/Z03/Z07 全中）。
    椭圆色是"洋红被暗化"，特征是 R 与 B **数值接近**（P00 实测 RGB(73,19,81)，
    |R-B|=8），而蓝紫衣服的 B 明显高于 R（|R-B|≈40）。所以加一道 |R-B| 上限，
    并把统计限制在画面下半部（衣服不在最底部，椭圆在）。
    """
    h = a.shape[0]
    half = a[h // 2:]
    r = half[:, :, 0].astype(np.int32)
    g = half[:, :, 1].astype(np.int32)
    b = half[:, :, 2].astype(np.int32)
    solid = half[:, :, 3] > 60
    if solid.sum() == 0:
        return 0.0
    m = solid & (r > g + 18) & (b > g + 18) & (np.abs(r - b) < 34)
    return float(m.sum()) / float(solid.sum())


def to_rgba(a):
    f = a.astype(np.float32) / 255.0
    out = np.zeros_like(f)
    out[:, :, :3] = f[:, :, :3] * f[:, :, 3:4]
    out[:, :, 3] = f[:, :, 3]
    return (out * 255 + 0.5).astype(np.uint8)


def cell(a, box):
    h, w = a.shape[:2]
    im = Image.fromarray(to_rgba(a), "RGBA")
    yy, xx = np.mgrid[0:h, 0:w]
    dark = (((yy // 10) + (xx // 10)) % 2) == 0
    px = np.zeros((h, w, 4), np.uint8)
    px[~dark] = (185, 185, 185, 255)
    px[dark] = (220, 220, 220, 255)
    bg = Image.fromarray(px, "RGBA")
    bg.alpha_composite(im)
    s = (box - 8) / float(max(w, h))
    big = bg.convert("RGB").resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
    out = Image.new("RGB", (box, box), (245, 245, 245))
    out.paste(big, ((box - big.size[0]) // 2, (box - big.size[1]) // 2))
    return out


def audit(keys):
    worst = []
    for k in keys:
        ratios = []
        for i in range(5):
            p = os.path.join(DST, "rgzombie_%s_dead%d.bmp" % (k[1:], i))
            if not os.path.exists(p):
                ratios.append(None)
                continue
            a, w, h = U.read_bmp32(p)
            ratios.append(mag_ratio(a))
        bad = [r for r in ratios if r is not None and r > MAG]
        tag = "洋红残留" if bad else "干净"
        print("%-5s %s  5帧洋红占比 %s"
              % (k, tag, " ".join("-" if r is None else "%.1f%%" % (r * 100) for r in ratios)))
        if bad:
            worst.append(k)
    print("\n有洋红残留：%d / %d %s" % (len(worst), len(keys), " ".join(worst)))
    return worst


def sheet(keys, out, box=200):
    """每只僵尸占一行，5 帧横排 —— 一行看完一只的整个倒地过程。"""
    img = Image.new("RGB", (box * 5, box * len(keys)), (245, 245, 245))
    for r, k in enumerate(keys):
        for i in range(5):
            p = os.path.join(DST, "rgzombie_%s_dead%d.bmp" % (k[1:], i))
            if not os.path.exists(p):
                continue
            a, w, h = U.read_bmp32(p)
            img.paste(cell(a, box), (i * box, r * box))
    img.save(out)
    print("-> %s (%dx%d)" % (os.path.basename(out), img.size[0], img.size[1]))


if __name__ == "__main__":
    ks = [a.upper() for a in sys.argv[1:]] or ["Z%02d" % i for i in range(8)]
    audit(ks)
    # 复核图最多画 10 只 —— 每只一行 5 帧，再多就超过肉眼能扫的长度了
    sheet(ks[:10], os.path.join(HERE, "qa_dead.png"))
