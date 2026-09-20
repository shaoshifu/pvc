# -*- coding: utf-8 -*-
"""把 mmx 出的 6 张光效统一成"暖白"色调，再入库到 assets/。

【为什么要这一步】
  mmx 出的六张形状都对，但**色调各不一样**：光扇偏蓝、符文环偏粉、
  星芒偏冷，而游戏里的庆典是按**档位色**推进的（殿堂炽白金、始祖太初紫白）。
  六种色调混在一起，叠到同一个庆典里会互相打架（蓝光配紫白彩幕 = 脏）。

  统一成中性暖白之后，光效在视觉上就是"纯光"，
  任何档位色叠上去都协调 —— 这是整套 VFX 素材的通行做法。

【做法：保形状、去色相】
  光效的"形状"全在 alpha 通道里（黑底 luma key 已经把亮度搬成 alpha，
  见 art/_gen_fx.py 的说明），所以：
    ① alpha 原样保留
    ② RGB 换成"从每像素亮度推出来的暖白"，而不是原色
       —— 直接去饱和会得到灰白，缺少发光感；暖白（R>G>B 微差）更像光
    ③ 重新按 alpha 预乘（AlphaBlend 要求）
  这样六张不但色调一致，亮度层级也一致（不会一张刺眼一张发灰）。

用法：
    python art/_unify_fx.py            # 从 assets/ 读、处理后写回
    python art/_unify_fx.py --dry      # 只看每张的色调统计，不写
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")

# 暖白：R 略高、B 略低。差值刻意做得很小 —— 大了就变成"黄光"，
# 与"太初紫白"这种冷色档位放一起会显脏。
WARM = np.array([255.0, 250.0, 240.0], np.float32)

NAMES = ["fx_rays", "fx_shock", "fx_spark", "fx_pillar", "fx_flare", "fx_runes"]


def premultiply(rgba):
    """RGB *= A/255，返回 uint8（AlphaBlend 的 AC_SRC_ALPHA 要求预乘）。"""
    out = rgba.astype(np.float32)
    out[:, :, :3] *= (out[:, :, 3:4] / 255.0)
    out[:, :, :3] = np.minimum(out[:, :, :3], out[:, :, 3:4])
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def unify(path, dry=False):
    im = Image.open(path)
    if im.mode != "RGBA":
        return path, False, "不是 RGBA（%s）" % im.mode
    a = np.array(im).astype(np.float32)
    al = a[:, :, 3]
    # 反预乘拿回"未乘的真彩色"，用它算亮度
    af = np.maximum(al[:, :, None], 1e-6) / 255.0
    lin = np.clip(a[:, :, :3] / af, 0, 255)
    lum = lin.max(axis=2) / 255.0                 # 0..1 的每像素光强
    # 记录原始色相（用作统计），然后整体换成暖白
    r, g, b = lin[:, :, 0], lin[:, :, 1], lin[:, :, 2]
    was = "偏蓝" if (b.mean() > r.mean() + 4) else ("偏粉" if (r.mean() > g.mean() + 8) else "中性")
    # 亮点稍微提升：让"中心"更亮、"边缘"更快衰减，发光感更足
    lum = np.clip(lum ** 1.15 * 1.06, 0.0, 1.0)
    out = np.zeros_like(a)
    out[:, :, 3] = al
    for c in range(3):
        out[:, :, c] = lum * WARM[c]
    if dry:
        return path, True, "原色调 %-4s → 暖白（可见 px %d，峰值 alpha %d）" % (
            was, int((al > 8).sum()), int(al.max()))
    Image.fromarray(premultiply(out), "RGBA").save(path, optimize=True)
    back = np.array(Image.open(path).convert("RGBA"))
    assert back.shape == a.shape, "写回后尺寸变了"
    return path, True, "原色调 %-4s → 暖白（可见 px %d，峰值 alpha %d）" % (
        was, int((al > 8).sum()), int(al.max()))


def main():
    dry = "--dry" in sys.argv
    print("== 统一光效色调（暖白）==")
    ok = 0
    for n in NAMES:
        p = os.path.join(ASSETS, n + ".png")
        if not os.path.exists(p):
            print("  %-12s 缺文件" % n)
            continue
        _, good, msg = unify(p, dry)
        print("  %-12s %s %s" % (n, "OK " if good else "X  ", msg))
        ok += good
    print("\n%s %d / %d" % ("检查" if dry else "处理", ok, len(NAMES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
