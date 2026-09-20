# -*- coding: utf-8 -*-
"""把 mmx 出的肉鸽单位图入库成游戏素材。

流程：art/gen2/u_p00.jpg
        -> 通用按键控抠图（_unitlib.matte_file，自动识别背景色）
        -> 裁掉全透明边（省内存，也让 spriteLoad 更快）
        -> 只需要时缩到尺寸上限（绝不放大）
        -> 预乘 alpha（uint8！）
        -> assets/rgplant_00.bmp / assets/rgzombie_00.bmp
        -> 僵尸额外派生 5 张倒地帧 rgzombie_00_dead0..4.bmp

为什么只裁不加画布：
  C 侧的 drawPlantEntity / drawZombie 用 spriteBlit(dc, sp, x, y, 1.0f, 1.0f, -1)
  贴图 —— 它按 **原始像素尺寸 / SS** 算显示大小。所以这里只保证像素尺寸 <= 上限，
  不放大也不缩到固定画布（缩小会丢掉出图的细节，而那正是不满"粗糙"换来的东西）。
  画法上 spriteBlit 把贴图底边对齐到单位脚底，出图是"脚在最下"，天然对应。

⚠️ 两个曾经踩过的坑（都验证过）：
  1. 预乘 alpha 必须是 **uint8**。曾经 premultiply() 返回 float32 就直接交给
     save_bmp32()，tobytes() 按 4 字节/元素写出去 —— 文件是应有大小的 4 倍，
     游戏读到的前 1/4 是 float 的裸字节，整张图糊成噪声。
     这里统一在最后 astype(np.uint8)，并且写完立刻回读校验。
  2. BMP 必须自下而上（biHeight = +h），spriteLoad() 拒绝 h <= 0。
     save_bmp32() 已经处理，不要绕过它自己拼头。

用法：
    python art/_build_units.py P00 Z24       # 指定
    python art/_build_units.py               # 全部
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                    # noqa: E402
from _fix_groundshadow import fix as fix_ground_shadow  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "gen2")
DST = os.path.join(ROOT, "assets")

MAX_PX = 200                 # 植物：1 逻辑格 (96x100) 在 2 倍超采样下的资源上限
MAX_LONG = 210               # 僵尸会比格子高一点（如巨人），放宽到接近 1 格半高

# 倒地帧的旋转角，和 build_assets.make_fall_frames 保持一致 ——
# 肉鸽僵尸和基座僵尸的倒地观感必须是同一套语言。
FALL_ANGLES = (-22, -45, -68, -85, -95)


def premultiply(rgba):
    """RGB *= A/255，返回 **uint8**（AlphaBlend 的 AC_SRC_ALPHA 要求预乘）。"""
    out = rgba.astype(np.float32)
    a = out[:, :, 3:4] / 255.0
    out[:, :, :3] *= a
    # 预乘后必须 rgb <= alpha（逐通道）；越界说明哪步把 RGB 又写回去了
    out[:, :, :3] = np.minimum(out[:, :, :3], out[:, :, 3:4])
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def trim(rgba, thr=0):
    """裁到 alpha 的非零包围盒（thr=0 表示连极淡的辉光也保留）。"""
    a = rgba[:, :, 3]
    ys, xs = np.where(a > thr)
    if len(xs) == 0:
        return None
    return rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def limit(rgba, max_long):
    """只在超限时缩小，绝不放大。"""
    h, w = rgba.shape[:2]
    k = max_long / float(max(h, w))
    if k >= 1.0:
        return rgba
    im = Image.fromarray(rgba, "RGBA").resize(
        (max(1, int(round(w * k))), max(1, int(round(h * k)))), Image.LANCZOS)
    return np.array(im)


def verify(path, want_w, want_h):
    """回读刚写的 BMP，确认尺寸与字节数都对得上。

    这一步不是形式主义：float32 直写的 bug 就是"文件能写出来、尺寸也对不上"
    才没被发现。入库是 100 张的批处理，必须在源头拦住。
    """
    b = open(path, "rb").read()
    w = int.from_bytes(b[18:22], "little")
    h = int.from_bytes(b[22:26], "little")
    off = int.from_bytes(b[10:14], "little")
    if w != want_w or h != want_h:
        return "尺寸不符 %dx%d != %dx%d" % (w, h, want_w, want_h)
    if h <= 0:
        return "biHeight=%d（游戏会拒绝载入）" % h
    if len(b) != off + w * h * 4:
        return "字节数不符 实际%d 应为%d" % (len(b), off + w * h * 4)
    return None


def make_fall_frames(rgba, name):
    """僵尸倒地帧：绕「脚底」旋转预渲染。

    和 build_assets.make_fall_frames 同一个思路 —— 绕脚底旋转出来的帧
    与行走姿态**完全同源**，配色/画风/比例绝对一致，零生成成本。
    倒地只持续 0.42 秒（DEATH_DUR），看到的是"倒下去"这个运动，
    不需要每帧独立美术。
    """
    im = Image.fromarray(rgba, "RGBA")
    w, h = im.size
    pad = int(h * 1.25) + 4
    CW, CH = w + 2 * pad, h + pad
    feet = (pad + w // 2, h + pad // 2)          # 脚底在画布中的位置
    canvas = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    canvas.paste(im, (pad, pad // 2))

    made = 0
    for i, ang in enumerate(FALL_ANGLES):
        rot = canvas.rotate(ang, resample=Image.BICUBIC, center=feet)
        arr = np.array(rot)
        ys, xs = np.where(arr[:, :, 3] > 0)
        if len(ys) == 0:
            continue
        x0, x1 = int(xs.min()), int(xs.max()) + 1
        y0, y1 = int(ys.min()), int(ys.max()) + 1
        fx, fy = feet[0] - x0, feet[1] - y0       # 脚底在裁剪块内的位置
        nw = max(fx, (x1 - x0) - fx) * 2 + 4
        nh = fy + 3
        out = Image.new("RGBA", (nw, nh), (0, 0, 0, 0))
        out.paste(rot.crop((x0, y0, x1, y1)), (nw // 2 - fx, 0))
        U.save_bmp32(premultiply(np.array(out)), os.path.join(DST, "%s_dead%d.bmp" % (name, i)))
        made += 1
    return made


def build(key, with_dead=True):
    src = os.path.join(SRC, "u_%s.jpg" % key.lower())
    if not os.path.exists(src):
        return key, False, "缺源图"
    im, c, spread = U.matte_file(src)
    arr = np.array(im)
    arr = trim(arr)
    if arr is None:
        return key, False, "抠完什么都没有"
    # 贴地椭圆清除必须发生在 make_fall_frames 之前 —— 这是本文件里最关键的顺序约束。
    # 倒地帧是拿这里的 arr 绕脚底旋转出来的：如果先旋转再清，椭圆会被转成一个
    # **斜的洋红色块**，而且不再贴底，"从底边取参考色"的前提直接失效。
    # 挂进流水线后，存活帧与 5 张倒地帧一次同步干净。
    arr, st = fix_ground_shadow(arr)          # st is None = 本图没有贴地椭圆
    if st is not None:
        ys, xs = np.where(arr[:, :, 3] > 0)
        if len(xs) == 0:
            return key, False, "清完椭圆后什么都没了"
        arr = arr[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    cap = MAX_LONG if key[0] == "Z" else MAX_PX
    arr = limit(arr, cap)
    name = ("rgplant_%s" % key[1:]) if key[0] == "P" else ("rgzombie_%s" % key[1:])
    dst = os.path.join(DST, name + ".bmp")
    U.save_bmp32(premultiply(arr), dst)

    err = verify(dst, arr.shape[1], arr.shape[0])
    if err:
        return key, False, "%s.bmp 写入校验失败：%s" % (name, err)

    dead = 0
    if key[0] == "Z" and with_dead:
        dead = make_fall_frames(arr, name)

    a = arr[:, :, 3]
    return key, True, ("%s.bmp %dx%d 不透明%.0f%% 倒地%d帧 背景=%s 离散%.1f%s%s"
                       % (name, arr.shape[1], arr.shape[0],
                          (a > 200).mean() * 100, dead, c.round(0).astype(int),
                          spread, "  !!背景不纯" if spread > 12 else "",
                          "  椭圆已清" if st is not None else ""))


def main():
    keys = sys.argv[1:] or (["P%02d" % i for i in range(50)] +
                            ["Z%02d" % i for i in range(50)])
    ok = bad = 0
    dirty = []
    for k in keys:
        k, good, msg = build(k)
        if good:
            ok += 1
            if "背景不纯" in msg:
                dirty.append(k)
        else:
            bad += 1
        print("  %s %s %s" % (k, "OK " if good else "X  ", msg))
    print("\n入库 %d / %d，失败 %d" % (ok, len(keys), bad))
    if dirty:
        print("背景不纯（建议重生成）：%s" % " ".join(dirty))


if __name__ == "__main__":
    main()
