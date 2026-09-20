# -*- coding: utf-8 -*-
"""脚部特写条：裁出每只角色底部 35%，放大并列，专门看"脚下有没有烘焙椭圆"。

为什么单独看脚：
  sheet_plants.png 上肉眼看每只脚下都有一片洋红椭圆，但用像素判据去量
  （底部宽度占比 + 底部颜色接近背景）只抓到 4 张 —— 因为那片椭圆是
  "比背景略深的洋红"，与出图背景色的距离处于中间地带，判据抓不稳。
  这种"判据抓不到、眼睛看得见"的东西，就该直接看。放大后：
    干净 = 角色脚/根/藤清晰收在地上，周围透出棋盘格
    有椭圆 = 脚下一整块洋红/粉紫色实心面
用法：python art/_audit_feetzoom.py [起始序号] [张数]
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

BOX = 260
PAD = 4


def checker(box=BOX, size=13, a=205, b=168):
    y, x = np.mgrid[0:box, 0:box]
    c = np.where((((x // size) + (y // size)) % 2) == 0, a, b)
    return np.dstack([c, c, c]).astype(np.uint8)


def main(prefix, start, n):
    cols = 8
    rows = (n + cols - 1) // cols
    W, H = cols * BOX, rows * BOX
    canvas = np.zeros((H, W, 3), np.uint8)
    base = checker()
    for r in range(rows):
        for c in range(cols):
            canvas[r * BOX:(r + 1) * BOX, c * BOX:(c + 1) * BOX] = base

    for i in range(n):
        idx = start + i
        p = os.path.join(DST, "%s_%02d.bmp" % (prefix, idx))
        if not os.path.exists(p):
            continue
        a, w, h = U.read_bmp32(p)
        cut = int(h * 0.65)                  # 只取下 35%
        a = a[cut:]
        h2 = a.shape[0]
        # 放大到宽度铺满格子
        k = (BOX - 2 * PAD) / float(w)
        im = Image.fromarray(a, "RGBA").resize(
            (max(1, int(round(w * k))), max(1, int(round(h2 * k)))), Image.NEAREST)
        a2 = np.array(im)
        hh, ww = a2.shape[:2]
        r, c = divmod(i, cols)
        ox, oy = c * BOX + (BOX - ww) // 2, r * BOX + BOX - hh
        al = a2[:, :, 3:4].astype(np.float32) / 255.0
        sub = canvas[oy:oy + hh, ox:ox + ww].astype(np.float32)
        canvas[oy:oy + hh, ox:ox + ww] = np.clip(
            a2[:, :, :3] * al + sub * (1 - al) + 0.5, 0, 255).astype(np.uint8)

    op = os.path.join(HERE, "feet_%s_%02d.png" % (prefix, start))
    Image.fromarray(canvas).save(op)
    print("-> %s  (%s_%02d..%02d, %dx%d)" % (op, prefix, start, start + n - 1, W, H))


if __name__ == "__main__":
    p = sys.argv[1] if len(sys.argv) > 1 else "rgplant"
    s = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    main(p, s, n)
