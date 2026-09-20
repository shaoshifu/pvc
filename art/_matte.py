# -*- coding: utf-8 -*-
"""把 mmx 生成的洋红底 JPG 抠成 32 位带 alpha 的 BMP。

为什么不用网上的"一键去背景"：
  1. 图像生成模型给的"纯洋红底"边缘一定有半透明过渡像素，
     直接按阈值二值化会留一圈硬锯齿 + 洋红描边（俗称紫边）。
  2. 这里用「洋红程度」当连续 alpha，再对残留做 despill，
     边缘就是自然过渡，缩放到游戏尺寸后看不出紫边。

输出尺寸按游戏需要：世界层是 2 倍超采样，所以 1 格 (96x100) 要存 192x200。

用法：
    python art/_matte.py art/gen2/th_scorch.jpg assets/tile_scorch.bmp 192 200
"""
import os
import sys

import numpy as np
from PIL import Image


def matte(img, size):
    a = np.array(img.convert("RGB")).astype(np.float32)
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]

    # 洋红程度：纯洋红 (255,0,255) 上 min(r,b)-g = 255
    over = np.minimum(r, b) - g
    alpha = 1.0 - np.clip(over / 110.0, 0.0, 1.0)

    # despill：把半透明边缘残留的洋红压掉，否则缩小后会有一圈紫边
    spill = np.clip(over, 0.0, None)
    r2 = r - spill * 0.92
    b2 = b - spill * 0.92

    rgb = np.stack([r2, g, b2], -1).clip(0, 255).astype(np.uint8)
    al = (alpha * 255.0).clip(0, 255).astype(np.uint8)

    out = np.dstack([rgb, al])
    im = Image.fromarray(out, "RGBA")
    if size:
        im = im.resize(size, Image.LANCZOS)
    return im


def save_bmp32(im, path):
    """写 32 位 BMP。

    ⚠️ 必须是【自下而上】（biHeight 为正数）+ 行序反转。
    游戏的 spriteLoad() 会拒绝 h <= 0 的文件（它按自下而上读、再自己翻转），
    写成顶向下（负高度）会静默加载失败 —— 表现是"贴图没生效"，很难查。
    """
    import struct
    a = np.array(im)                      # RGBA
    h, w = a.shape[:2]
    bgra = a[::-1, :, [2, 1, 0, 3]]       # 翻转行序 + RGBA -> BGRA
    hdr = struct.pack("<2sIHHI", b"BM", 14 + 40 + w * h * 4, 0, 0, 14 + 40)
    ih = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0,
                     w * h * 4, 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(ih)
        f.write(bgra.tobytes())


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else None
    h = int(sys.argv[4]) if len(sys.argv) > 4 else None
    size = (w, h) if (w and h) else None
    im = matte(Image.open(src), size)
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    save_bmp32(im, dst)
    a = np.array(im)[:, :, 3]
    print("%s -> %s  %dx%d  透明占比 %.1f%%"
          % (os.path.basename(src), dst, im.width, im.height,
             (a == 0).mean() * 100))
    return 0


if __name__ == "__main__":
    sys.exit(main())
