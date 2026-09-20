# -*- coding: utf-8 -*-
"""把生成的美术素材转换成引擎能直接读的 BMP。

引擎要求（来自 spriteLoadName 的实现）：
  · 32 位 BMP，带 alpha
  · 草坪地表 1728×1000（= 9 列 × 192, 5 行 × 200）
  · 角色方图，高度统一到现有精灵量级

AI 出图是 JPG（无 alpha、尺寸任意），所以这里做三件事：
  1. 草坪：直接缩放到 1728×1000，不透明
  2. 角色：抠掉纯色背景 → 缩放 → 转 32 位带 alpha 的 BMP
  3. 统一去掉 AI 图里常见的四角水印/签名痕迹（裁边）

用法： python _install_art.py
"""
import os
import sys

import numpy as np
from PIL import Image

D = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(D, "assets_new")
DST = os.path.join(D, "assets")

LAWN_W, LAWN_H = 1728, 1000

# (源目录, 源文件, 目标 BMP 名, 类型)
JOBS = [
    # ---- 草坪地表：缩放到引擎尺寸 ----
    ("lv6_ice",     "lawn_ice",        "lawn_ice",        "lawn"),
    ("lv7_magma",   "lawn_magma",      "lawn_magma",      "lawn"),
    ("lv8_void",    "lawn_void",       "lawn_void",       "lawn"),
    ("lv9_circuit", "lawn_circuit",    "lawn_circuit",    "lawn"),
    ("lv10_astral", "lawn_astral",     "lawn_astral",     "lawn"),
    # 始祖主题草坪：只在始祖级出现后才会被 drawBackground 用到，
    # 缺图会自动退回程序化草坪，所以它不是"必须存在"的资源。
    ("primordial",  "lawn_primordial", "lawn_primordial", "lawn"),
    # ---- 道具 / 特效 ----
    ("lv6_ice",     "hazard_thin_ice", "hazard_thin_ice", "prop"),
    ("lv7_magma",   "hazard_rift",     "hazard_rift",     "prop"),
    ("lv9_circuit", "node_power",      "node_power",      "prop"),
    ("lv10_astral", "fx_meteor",       "fx_meteor",       "prop"),
    ("lv6_ice",     "fx_snowstorm",    "fx_snowstorm",    "wide"),
    ("lv7_magma",   "fx_ember",        "fx_ember",        "wide"),
    ("lv8_void",    "fx_fog",          "fx_fog",          "wide"),
    # ---- 角色 ----
    ("lv6_ice",     "zombie_frostfang", "zombie_frostfang", "char"),
    ("lv6_ice",     "zombie_glacier",   "zombie_glacier",   "char"),
    ("lv7_magma",   "zombie_magmaw",    "zombie_magmaw",    "char"),
    ("lv7_magma",   "zombie_ashwalker", "zombie_ashwalker", "char"),
    ("lv8_void",    "zombie_phantom",   "zombie_phantom",   "char"),
    ("lv8_void",    "zombie_blinker",   "zombie_blinker",   "char"),
    ("lv9_circuit", "zombie_cogwork",   "zombie_cogwork",   "char"),
    ("lv9_circuit", "zombie_saboteur",  "zombie_saboteur",  "char"),
    ("lv10_astral", "boss_apostle",     "boss_apostle",     "char"),
]

# 现有僵尸精灵的高度基准：取两张现成的量一下，新角色对齐同一量级
CHAR_H = 128


def find_src(sub, name):
    d = os.path.join(SRC, sub)
    for ext in (".jpg", ".jpeg", ".png", ".webp"):
        p = os.path.join(d, name + ext)
        if os.path.exists(p):
            return p
    return None


def trim_watermark(im):
    """AI 出图常在角落留签名。裁掉 2% 边缘，成本极低。"""
    w, h = im.size
    dx, dy = int(w * 0.02), int(h * 0.02)
    return im.crop((dx, dy, w - dx, h - dy))


def chroma_key(im):
    """抠掉纯色背景**和角色脚下的绿色投影**。

    ⚠️ 不能用「与四角背景色的色距」做阈值 —— 实测会漏掉脚底的椭圆投影：
       背景 [176,250,173]（浅绿），投影 [87,172,92]（中绿），
       两者色距很大所以投影被判成"实体"，进游戏后僵尸脚下就挂着一块绿饼。

    这里改用「绿色主导 + 边界洪水填充」：
      1. 绿色主导 = G 明显高于 R 和 B 的最大值（背景和投影都满足）
      2. 从画面四条边向内洪水填充，只有**与边界连通**的绿色区域才抠掉
    第 2 步是关键：僵尸身上也可能有绿色（比如毒藤色的装饰），
    但它们不与边界连通，所以会被保留 —— 纯按颜色抠会把角色一起挖空。
    """
    from collections import deque
    rgb = np.array(im.convert("RGB")).astype(np.int16)
    h, w, _ = rgb.shape
    g = rgb[:, :, 1] - np.maximum(rgb[:, :, 0], rgb[:, :, 2])
    greenish = g > 32

    vis = np.zeros((h, w), bool)
    dq = deque()
    for x in range(w):
        for y in (0, h - 1):
            if greenish[y, x] and not vis[y, x]:
                vis[y, x] = True; dq.append((y, x))
    for y in range(h):
        for x in (0, w - 1):
            if greenish[y, x] and not vis[y, x]:
                vis[y, x] = True; dq.append((y, x))
    while dq:
        y, x = dq.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < h and 0 <= nx < w and greenish[ny, nx] and not vis[ny, nx]:
                vis[ny, nx] = True
                dq.append((ny, nx))

    alpha = np.where(vis, 0, 255).astype(np.uint8)
    rgba = np.dstack([np.array(im.convert("RGB")), alpha])
    return Image.fromarray(rgba, "RGBA"), vis


def fit_char(im):
    """抠图 → 裁到实体边界 → 等比缩放到统一高度。"""
    rgba, bg = chroma_key(im)
    a = np.array(rgba)
    ys, xs = np.where(a[:, :, 3] > 12)
    if len(xs) == 0:
        return None
    pad = 4
    x0, x1 = max(0, xs.min() - pad), min(a.shape[1], xs.max() + pad)
    y0, y1 = max(0, ys.min() - pad), min(a.shape[0], ys.max() + pad)
    cropped = rgba.crop((x0, y0, x1, y1))
    w, h = cropped.size
    nw = max(1, int(round(w * CHAR_H / h)))
    return cropped.resize((nw, CHAR_H), Image.LANCZOS)


def save_bmp(img, path):
    """手写 32 位 BGRA BMP。

    ⚠️ 不能用 PIL 的 img.save(path, "BMP") —— 它对 RGBA 会**丢掉 alpha 通道**
    （落成 24 位），结果就是抠好的图变成带绿底的不透明方块。
    实测症状：读回来透明比 0.0%、边缘 alpha 全是 255。
    引擎读的是 32 位带 alpha 的 BMP（与 spriteTint 里 CreateDIBSection 的
    32bpp 格式一致），所以这里按 Win32 的 BI_RGB + 32bpp 手写。
    """
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    w, h = img.size
    a = np.array(img)                       # RGBA
    # BMP 是 BGRA 顺序，且自下而上
    bgra = np.dstack([a[:, :, 2], a[:, :, 1], a[:, :, 0], a[:, :, 3]])
    bgra = bgra[::-1]                       # 垂直翻转
    data = bgra.tobytes()

    # BITMAPFILEHEADER(14) + BITMAPINFOHEADER(40)
    off_bits = 14 + 40
    fh = b"BM" + (off_bits + len(data)).to_bytes(4, "little") + \
         b"\x00\x00\x00\x00" + off_bits.to_bytes(4, "little")
    ih = (40).to_bytes(4, "little") + w.to_bytes(4, "little", signed=True) + \
         h.to_bytes(4, "little", signed=True) + \
         (1).to_bytes(2, "little") + (32).to_bytes(2, "little") + \
         (0).to_bytes(4, "little") + len(data).to_bytes(4, "little") + \
         (2835).to_bytes(4, "little") + (2835).to_bytes(4, "little") + \
         (0).to_bytes(4, "little") + (0).to_bytes(4, "little")
    with open(path, "wb") as f:
        f.write(fh + ih + data)


def main():
    if not os.path.isdir(DST):
        sys.exit("[X] 找不到 assets 目录")
    ok = fail = 0
    for sub, name, out, kind in JOBS:
        src = find_src(sub, name)
        if not src:
            print("  ✗ 源文件缺失 %s/%s" % (sub, name))
            fail += 1
            continue
        im = trim_watermark(Image.open(src))
        dst = os.path.join(DST, out + ".bmp")
        try:
            if kind == "lawn":
                save_bmp(im.resize((LAWN_W, LAWN_H), Image.LANCZOS), dst)
                note = "%dx%d" % (LAWN_W, LAWN_H)
            elif kind == "wide":
                # 特效层保持比例，宽度对齐草坪
                w, h = im.size
                nh = int(round(h * LAWN_W / w))
                save_bmp(im.resize((LAWN_W, nh), Image.LANCZOS), dst)
                note = "%dx%d" % (LAWN_W, nh)
            elif kind == "prop":
                save_bmp(im.resize((192, 192), Image.LANCZOS), dst)
                note = "192x192"
            else:                                   # char
                c = fit_char(im)
                if c is None:
                    print("  ✗ 抠图失败（找不到实体）%s" % name)
                    fail += 1
                    continue
                save_bmp(c, dst)
                note = "%dx%d" % c.size
            print("  ✔ %-18s %-10s %s" % (out, note, os.path.basename(src)))
            ok += 1
        except Exception as e:
            print("  ✗ %-18s %s" % (out, e))
            fail += 1
    print("\n完成：成功 %d / 失败 %d" % (ok, fail))
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
