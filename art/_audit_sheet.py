# -*- coding: utf-8 -*-
"""把归档后的单位 BMP 贴到棋盘格上，做成大图供目视检查。

为什么必须目视：
  纯像素判据在这件事上反复失效 ——
    * 用"源图边框方差"判：P26 源图是径向渐变，离散 16.1，
      但泛洪抠完其实很干净；判据却一直在报它。
    * 用"图像最外圈 alpha"判：trim() 已经把图裁到内容包围盒，
      最外圈必然贴着角色本体 —— 100 张全报"边缘残留"，全是假阳性。
  真正要回答的问题是"角色背后/周围有没有一片粉紫色的雾"，
  这个只有把图放到棋盘格上、让半透明区域显出格纹才看得清。

棋盘格的意义：
  半透明像素会和格子混合 —— 格子线条能透过来 = 那里还有残留；
  完全透明处格子完全可见且干净；角色本体处格子被完全盖住。

用法：python art/_audit_sheet.py [plants|zombies|all]
      输出 art/sheet_plants.png / sheet_zombies.png
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DST = os.path.join(os.path.dirname(HERE), "assets")

CELL = 224          # 每格边长（含标签条）
PAD = 6


def checker(size=12, a=210, b=170):
    """中灰棋盘格：太亮会掩盖浅色残留，太暗会掩盖深色残留，中灰最平衡。"""
    y, x = np.mgrid[0:CELL, 0:CELL]
    c = np.where((((x // size) + (y // size)) % 2) == 0, a, b)
    return np.dstack([c, c, c]).astype(np.uint8)


def place(a, w, h, x0, y0, tile):
    """把 RGBA 按 alpha 合成到棋盘格上。"""
    sub = tile[y0:y0 + h, x0:x0 + w].astype(np.float32)
    al = a[:, :, 3:4].astype(np.float32) / 255.0
    out = a[:, :, :3].astype(np.float32) * al + sub * (1.0 - al)
    tile[y0:y0 + h, x0:x0 + w] = np.clip(out + 0.5, 0, 255).astype(np.uint8)


def main():
    which = (sys.argv[1] if len(sys.argv) > 1 else "all").lower()
    kinds = []
    if which in ("plants", "all"):
        # 第二批上线后植物从 50 涨到 100（与 C 侧 RG_PLANT_N 对齐）
        kinds.append(("rgplant", 100, "sheet_plants.png"))
    if which in ("zombies", "all"):
        kinds.append(("rgzombie", 50, "sheet_zombies.png"))

    for prefix, n, outname in kinds:
        cols = 10
        rows = (n + cols - 1) // cols
        W, H = cols * CELL, rows * CELL
        canvas = np.zeros((H, W, 3), np.uint8)
        base = checker()
        for r in range(rows):
            for c in range(cols):
                canvas[r * CELL:(r + 1) * CELL, c * CELL:(c + 1) * CELL] = base

        missing = []
        for i in range(n):
            p = os.path.join(DST, "%s_%02d.bmp" % (prefix, i))
            r, c = divmod(i, cols)
            ox, oy = c * CELL, r * CELL
            if not os.path.exists(p):
                missing.append(i)
                continue
            a, w, h = U.read_bmp32(p)
            k = min(1.0, (CELL - 2 * PAD) / float(max(w, h)))
            if k < 1.0:
                im = Image.fromarray(a, "RGBA").resize(
                    (max(1, int(w * k)), max(1, int(h * k))), Image.LANCZOS)
                a = np.array(im)
                w, h = a.shape[1], a.shape[0]
            # 水平居中、底边贴格底（和游戏里 spriteBlit 的贴地方式一致）
            place(a, w, h, ox + (CELL - w) // 2, oy + CELL - PAD - h, canvas)
            # 序号：左上角画个深色小方块+白字太麻烦，改为顺序阅读
        out = os.path.join(HERE, outname)
        Image.fromarray(canvas).save(out)
        print("%-20s %d 张 -> %s（%dx%d）%s" % (
            outname, n - len(missing), out, W, H,
            "  缺：%s" % missing if missing else ""))


if __name__ == "__main__":
    main()
