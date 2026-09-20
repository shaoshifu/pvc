# -*- coding: utf-8 -*-
"""assets/*.bmp -> assets/*.png 或 *.jpg

为什么必须逐字节保留原值（这是本脚本唯一容易做错的地方）：
  · 这些 BMP 是 32 位 **BGRA 且 alpha 已预乘**（引擎用 AlphaBlend，它要求源是预乘的，
    见 pvz.c 里 spriteTint 的注释"RGB 按 alpha 预乘"）。
  · 如果按常规做法"反预乘 → 存 PNG(直通 alpha) → 加载时再预乘"，会**丢数据**：
    例如 A=1、RGB=200，反预乘得 200*255/1=51000，截到 255，再预乘回 255*1/255=1 ≠ 200。
    低 alpha 的描边像素会整片变暗。
  · PNG 本身**不校验 RGB ≤ A**，所以可以直接把预乘后的字节原样存进去，
    加载时只做一次 BGRA↔RGBA 的字节交换 —— 全程零误差。

所以本脚本只做两件事：① 交换 B/R 通道；② 压缩。不碰 alpha、不碰数值。

输出格式按"有没有透明"二分（规则确定，便于解释与复现）：
  · **全不透明** 的图（草坪 / 菜单背景这类整屏图，实测 21 张共 36 MB）
    → **JPEG q92**。PNG 对照片类纹理本来就压不动，换 JPEG 省 30 MB；
    q92 对这类有纹理的画面肉眼无差。想全部用 PNG 就加 --png-only。
  · **带透明** 的精灵 → **PNG**（必须无损，描边像素不能糊）。

用法：
    python art/_bmp2png.py            # 全部转换 + 自检
    python art/_bmp2png.py --verify    # 只自检（比对现有 PNG 与 BMP）
"""
import glob
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(os.path.dirname(HERE), "assets")


def read_bmp32(path):
    """返回 (w, h, bytes)——已是自上而下的 BGRA。"""
    b = open(path, "rb").read()
    if b[:2] != b"BM":
        raise ValueError("不是 BMP: %s" % path)
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bpp = struct.unpack_from("<H", b, 28)[0]
    if bpp != 32 or w <= 0 or h <= 0:
        raise ValueError("只处理 32bpp 正向 BMP: %s (%dx%d %dbpp)" % (path, w, h, bpp))
    if off + w * h * 4 > len(b):
        raise ValueError("数据长度不足: %s" % path)
    # BMP 自下而上；翻转成自上而下，与引擎的 DIB（biHeight 为负）一致
    rows = []
    for y in range(h - 1, -1, -1):
        rows.append(b[off + y * w * 4: off + y * w * 4 + w * 4])
    return w, h, b"".join(rows)


def to_img(bgra, w, h):
    """BGRA(预乘) -> RGBA(预乘，字节原样)。PNG 直接存；JPEG 前先还原成直通 alpha。"""
    return Image.frombytes("RGBA", (w, h), bgra, "raw", "BGRA")


def is_opaque(img):
    return img.getchannel("A").getextrema()[0] == 255


def convert(path, verify_only=False, png_only=False):
    w, h, bgra = read_bmp32(path)
    img = to_img(bgra, w, h)
    opaque = is_opaque(img)

    if opaque and not png_only:
        out = path[:-4] + ".jpg"
        if not verify_only:
            # 预乘在 A=255 时等于原色，所以直接转 RGB 存 JPEG，无信息损失以外的变化
            img.convert("RGB").save(out, "JPEG", quality=92, optimize=True, subsampling=0)
    else:
        out = path[:-4] + ".png"
        if not verify_only:
            img.save(out, "PNG", optimize=True, compress_level=9)

    # ⚠️ 必须删掉另一种扩展名：否则 .png 与 .jpg 同时存在，
    #    加载器按 .png 优先命中，体积优化白做（这个坑真踩过：
    #    中断留下的 795 个残留 PNG 让 JPEG 分支"看起来没生效"）。
    for other in (path[:-4] + ".png", path[:-4] + ".jpg"):
        if other != out and os.path.exists(other):
            os.remove(other)

    # 自检：把输出读回来，逐字节与 BMP 比对
    # JPEG 是有损的，只能查"非透明 + 尺寸"，不能查逐字节
    back = Image.open(out).convert("RGBA")
    if back.size != (w, h):
        return "尺寸不符 %sx%s" % back.size
    if out.endswith(".jpg"):
        # 有损格式：只保证尺寸与"仍然全不透明"
        if back.getchannel("A").getextrema()[0] != 255:
            return "★ JPEG 后出现了透明"
        return None
    rb = back.tobytes("raw", "BGRA")     # 换回 BGRA，与源同序
    if rb != bgra:
        n = sum(1 for i in range(0, min(len(rb), len(bgra)), 4) if rb[i:i + 4] != bgra[i:i + 4])
        return "★ 有 %d 个像素不一致" % n
    return None


def main():
    verify_only = "--verify" in sys.argv
    png_only = "--png-only" in sys.argv
    files = sorted(glob.glob(os.path.join(ASSETS, "*.bmp")))
    if not files:
        print("没有找到 BMP")
        return 1

    before = sum(os.path.getsize(f) for f in files)
    bad = 0
    for i, f in enumerate(files, 1):
        name = os.path.basename(f)
        try:
            err = convert(f, verify_only, png_only)
        except Exception as e:                       # noqa: BLE001
            err = "异常: %s" % e
        if err:
            bad += 1
            print("  ✗ %-28s %s" % (name, err))
        if i % 100 == 0:
            print("  ... %d/%d" % (i, len(files)))

    after = 0
    npng = njpg = 0
    for f in files:
        p1, p2 = f[:-4] + ".png", f[:-4] + ".jpg"
        if os.path.exists(p1):
            after += os.path.getsize(p1); npng += 1
        elif os.path.exists(p2):
            after += os.path.getsize(p2); njpg += 1
    print()
    print("  转换 %d 张，失败 %d 张" % (len(files), bad))
    print("  BMP 合计 %.1f MB  →  打包后 %.1f MB（省 %.0f%%）"
          % (before / 1048576.0, after / 1048576.0,
             100.0 * (1.0 - after / float(before or 1))))
    print("  分布：PNG %d 张 / JPEG %d 张" % (npng, njpg))
    print("  逐字节自检：%s" % ("全部通过 ✔" if bad == 0 else "有失败 ✗"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
