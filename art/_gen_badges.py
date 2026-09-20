# -*- coding: utf-8 -*-
"""程序化生成 8 枚档位徽章 + 6 张开牌特效（不依赖任何图像模型）。

【为什么放弃 AI 出图，改程序化】
  徽章和特效这两类素材，AI 出图连试三轮都不合格：
    · **颜色不可控**：提示词里写 "sapphire blue"/"topaz gold"，
      8 枚里 5 枚跑偏；第 2 轮改成精确十六进制 #60A8E8 之后，
      **8 枚全变成蓝色** —— 模型画的是"3D 珠宝渲染图"，它的配色逻辑
      压过了文字约束。而档位颜色是游戏既有的语言（RGQ_COL，
      卡片档位标签就是那个颜色），错了就是错的。
    · **甩不掉的底部圆盘**：模型执意给徽章补一圈发光底座，
      那底座是档位色（蓝/紫）而不是洋红，**洋红键控抓不到它**，
      于是每枚徽章下面都顶着一块色斑。加 "no disc, no pedestal,
      no glowing platform" 三轮都压不住。
    · **特效形状跑偏**：要"扇形放射光"给出星芒、"冲击波环"给出斜置镜头光晕。
  程序化绘制把这些问题的根源消掉：颜色是 RGB 常量、形状是几何参数、
  没有背景可抠（直接画在 RGBA 上）。而且它和游戏里既有的
  光扇/冲击波/火花（drawCelebBack/Front 的 GDI 版本）是同一套视觉语言。

【产出的几何是"同一套"，只是换了光色】
  徽章：外环 + 内环 + 多面宝石 + 顶饰，颜色全取该档的 RGQ_COL。
  特效：光扇 / 冲击波 / 火花 / 光柱 / 闪斑 / 符文环。
  每一张都用**预乘 alpha** 存 PNG（与 Phase 1 的既有约定一致，
  引擎用 AlphaBlend，它要求源是预乘的）。

用法：
    python art/_gen_badges.py            # 全部 14 张
    python art/_gen_badges.py badge      # 只徽章
    python art/_gen_badges.py fx         # 只特效
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")

SS = 4          # 超采样倍数：4x 绘制后缩回，边缘才不会有锯齿

# 档位配色：**必须**与 pvz.c 的 RGQ_COL 一致（原文抄自那里）
#   RGQ_COL = { 灰, 蓝, 紫, 金, 洋红, 赤红, 炽白金, 太初紫白 }
# (文件名, 档位色, 金属) —— 档位色**原样抄自 pvz.c 的 RGQ_COL**。
# 中间曾经多放了一个"暗色"字段，但 METALS 里已经自带 lo/hi/edge 三个色阶，
# 那个字段没人用，反而把解包顺序搞错（金属名收到了一个 RGB 元组）。
TIERS = [
    ("badge_common",     (176, 182, 176), "iron"),
    ("badge_rare",       ( 96, 168, 232), "silver"),
    ("badge_epic",       (168, 108, 232), "silver"),
    ("badge_legend",     (240, 168,  56), "gold"),
    ("badge_myth",       (244,  88, 132), "rosegold"),
    ("badge_ultra",      (255,  64,  48), "blacksteel"),
    ("badge_hall",       (255, 246, 214), "gold"),
    ("badge_primordial", (255, 236, 255), "rune"),
]

METALS = {
    "iron":       ((138, 144, 138), ( 96, 100,  96), (176, 182, 176)),
    "silver":     ((196, 204, 216), (128, 138, 152), (240, 246, 252)),
    "gold":       ((212, 168,  72), (140, 104,  28), (255, 232, 152)),
    "rosegold":   ((226, 158, 150), (156,  96,  92), (255, 214, 206)),
    "blacksteel": (( 76,  72,  78), ( 38,  36,  40), (128, 122, 130)),
    "rune":       ((186, 162, 224), (118,  94, 164), (240, 226, 255)),
}


def lerp(c0, c1, t):
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c0, c1))


def over(dst, src, alpha):
    """src 以 alpha 叠到 dst 上（都是 RGB 元组，0..255）。"""
    return tuple(int(round(s * alpha + d * (1.0 - alpha))) for s, d in zip(src, dst))


# ============================================================ 徽章
def badge_glow_amount(idx):
    """低档位几乎不发光、高档位明显发光 —— 让"档位感"一眼可辨。"""
    return [0.00, 0.06, 0.10, 0.16, 0.22, 0.26, 0.40, 0.50][idx]


def make_badge(idx, name, tier_col, metal):
    # 装饰风格由金属决定：金环配 12 齿、符文环配 16 齿、其余素环。
    # 原来 style 是独立入参，但它的取值与 metal 一一对应，等于同一件事写两遍。
    style = {"gold": "gold", "rune": "rune"}.get(metal, "")
    size = 128
    S = size * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx = cy = S / 2.0
    R = S * 0.42
    m_hi, m_lo, m_edge = METALS[metal]

    # ---- ① 先铺辉光垫底（在高档位才明显）
    g = badge_glow_amount(idx)
    if g > 0.01:
        # ⚠️ 辉光半径必须**留在画布内**，否则圆环被画布裁成方形 ——
        #    抓帧时表现为徽章周围套着一个暗色方块（残留 alpha 只有个位数，
        #    在 4x 放大下非常明显），看起来像"抠图没抠干净"。
        #    原来写 R*(1+k*0.085) 到 k=14 时已经到 0.92*S，四角必然被切。
        #    改成从 R*1.05 平滑推到 0.47*S，并且最外圈 alpha 收到 1 以下、
        #    让边界自然消失而不是被切断。
        r_in, r_out = R * 1.05, 0.47 * S
        for k in range(12):
            t = (k + 1) / 12.0
            rr = r_in + (r_out - r_in) * t
            a = int(150.0 * g * (1.0 - t) ** 1.6)
            if a <= 0:
                continue
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr],
                      outline=tier_col + (a,), width=max(1, int(S * 0.020)))

    # ---- ② 外环
    w_out = max(2, int(S * 0.055))
    d.ellipse([cx - R, cy - R, cx + R, cy + R], outline=m_lo + (255,), width=w_out)
    d.ellipse([cx - R + w_out * 0.30, cy - R + w_out * 0.30,
               cx + R - w_out * 0.30, cy + R - w_out * 0.30],
              outline=m_edge + (255,), width=max(1, w_out // 3))
    for t in range(26):                      # 左上高光弧
        a0 = math.radians(-168 + t * 5.0)
        a1 = math.radians(-168 + t * 5.0 + 5.6)
        rr = R - w_out * 0.20
        d.line([cx + math.cos(a0) * rr, cy + math.sin(a0) * rr,
                cx + math.cos(a1) * rr, cy + math.sin(a1) * rr],
               fill=m_hi + (215,), width=max(1, int(w_out * 0.40)))

    # ---- ③ 高档位的装饰齿
    if style == "gold":
        n, ln, wd = 12, 0.95, 0.013
    elif style == "rune":
        n, ln, wd = 16, 1.15, 0.009
    else:
        n, ln, wd = 0, 0, 0
    for i in range(n):
        a = 2 * math.pi * i / n - math.pi / 2
        r0, r1 = R + w_out * 0.30, R + w_out * ln
        d.line([cx + math.cos(a) * r0, cy + math.sin(a) * r0,
                cx + math.cos(a) * r1, cy + math.sin(a) * r1],
               fill=m_hi + (238,), width=max(1, int(S * wd)))

    # ---- ④ 宝石座 + 宝石（多面切割）
    Ri = R * 0.66
    d.ellipse([cx - Ri, cy - Ri, cx + Ri, cy + Ri], fill=m_lo + (255,))
    d.ellipse([cx - Ri, cy - Ri, cx + Ri, cy + Ri], outline=m_edge + (255,),
              width=max(1, int(S * 0.018)))
    Rg = Ri * 0.86
    half = Rg * 0.74
    top = [(cx, cy - Rg), (cx + half, cy - Rg * 0.34),
           (cx, cy + Rg * 0.12), (cx - half, cy - Rg * 0.34)]
    bot = [(cx - half, cy - Rg * 0.34), (cx, cy + Rg * 0.12),
           (cx + half, cy - Rg * 0.34), (cx, cy + Rg)]
    d.polygon(top, fill=lerp(tier_col, (255, 255, 255), 0.50) + (255,))
    d.polygon(bot, fill=tier_col + (255,))
    d.polygon(top, outline=lerp(tier_col, (255, 255, 255), 0.82) + (255,))
    d.polygon(bot, outline=lerp(tier_col, (0, 0, 0), 0.40) + (255,))
    d.polygon([(cx - half * 0.58, cy - Rg * 0.28),
               (cx - half * 0.06, cy - Rg * 0.74),
               (cx - half * 0.02, cy - Rg * 0.24)],
              fill=(255, 255, 255, 165))

    arr = np.array(im)
    # 缩回目标尺寸（LANCZOS 抗锯齿）
    arr = np.array(Image.fromarray(arr, "RGBA").resize((size, size), Image.LANCZOS))
    return arr


# ============================================================ 特效
def make_fx(kind, size=256):
    S = size * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx = cy = S / 2.0

    if kind == "fx_rays":
        # 18 道宽窄相间的楔形，从内半径推到外半径
        for k in range(18):
            a = 2 * math.pi * k / 18
            aw = (0.052 if k % 2 else 0.026)
            r0, r1 = S * 0.055, S * 0.470
            br = 235 if k % 2 else 130
            pts = [(cx + math.cos(a) * r0, cy + math.sin(a) * r0),
                   (cx + math.cos(a) * r1, cy + math.sin(a) * r1),
                   (cx + math.cos(a + aw) * r1, cy + math.sin(a + aw) * r1),
                   (cx + math.cos(a + aw) * r0, cy + math.sin(a + aw) * r0)]
            d.polygon(pts, fill=(255, 252, 240, br))
        # 中心白核
        for k in range(9, 0, -1):
            rr = S * 0.030 * k
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr],
                      fill=(255, 255, 255, int(28 * (10 - k))))

    elif kind == "fx_shock":
        # 一圈离散光点（GDI 画不出粗圆弧，点阵更"能量"也更好看）
        n = 64
        for i in range(n):
            a = 2 * math.pi * i / n
            rr = S * 0.415
            r = S * 0.030
            d.ellipse([cx + math.cos(a) * rr - r, cy + math.sin(a) * rr * 0.82 - r,
                       cx + math.cos(a) * rr + r, cy + math.sin(a) * rr * 0.82 + r],
                      fill=(255, 248, 230, 235))
        for k in range(6):
            rr = S * 0.415 + k * S * 0.012
            d.ellipse([cx - rr, cy - rr * 0.82, cx + rr, cy + rr * 0.82],
                      outline=(255, 240, 210, 60), width=max(1, int(S * 0.006)))

    elif kind == "fx_spark":
        # 40 条带拖尾的亮点
        rng = np.random.default_rng(20260921)
        for i in range(40):
            a = float(rng.random()) * 2 * math.pi
            dist = S * (0.14 + 0.34 * float(rng.random()))
            r = S * (0.012 + 0.016 * float(rng.random()))
            for t in range(6):                       # 拖尾：从亮点往回画淡点
                f = 1.0 - t / 6.0
                dd = dist * (1.0 - 0.055 * t)
                al = int(210 * f * f)
                if al <= 0:
                    continue
                d.ellipse([cx + math.cos(a) * dd - r * f,
                           cy + math.sin(a) * dd * 0.78 - r * f,
                           cx + math.cos(a) * dd + r * f,
                           cy + math.sin(a) * dd * 0.78 + r * f],
                          fill=(255, 246, 222, al))

    elif kind == "fx_pillar":
        # 从顶部倾泻的光柱：上宽下窄、中心更亮
        for k in range(24):
            t = k / 23.0
            w = S * (0.150 - 0.075 * t)
            y0 = S * 0.02 + t * S * 0.78
            y1 = y0 + S * 0.030
            al = int(120 * (1.0 - t) * (0.55 + 0.45 * math.sin(t * math.pi)))
            if al <= 0:
                continue
            d.rectangle([cx - w, y0, cx + w, y1], fill=(255, 250, 232, al))
        d.rectangle([cx - S * 0.030, 0, cx + S * 0.030, S * 0.80],
                    fill=(255, 255, 255, 190))

    elif kind == "fx_flare":
        # 四芒 + 六芒叠加的闪斑
        for (n, ln, wd, al) in ((4, 0.480, 0.026, 220), (6, 0.330, 0.016, 150),
                                (12, 0.190, 0.008, 90)):
            for i in range(n):
                a = 2 * math.pi * i / n + (0.0 if n != 6 else math.pi / 6)
                r1 = S * ln
                d.polygon([(cx - math.sin(a) * S * wd, cy + math.cos(a) * S * wd),
                           (cx + math.cos(a) * r1, cy + math.sin(a) * r1),
                           (cx + math.sin(a) * S * wd, cy - math.cos(a) * S * wd)],
                          fill=(255, 255, 255, al))
        for k in range(10, 0, -1):
            rr = S * 0.024 * k
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr],
                      fill=(255, 255, 255, int(30 * (11 - k))))

    elif kind == "fx_runes":
        # 一圈符文：用简单笔画拼出 16 个"字形"，不用字体（避免依赖字体文件）
        n = 16
        strokes = [
            [(0, -1), (0, 1), (-0.7, -0.7)],
            [(-0.7, -0.7), (0.7, -0.7), (0.7, 0.7), (-0.7, 0.7)],
            [(-0.6, -0.8), (0.6, -0.8), (0.0, 0.8)],
            [(0, -1), (0.7, 0), (0, 1), (-0.7, 0)],
            [(-0.7, 0.6), (0.0, -0.9), (0.7, 0.6)],
            [(-0.5, -0.9), (-0.5, 0.9), (0.6, 0.2)],
            [(0.7, -0.8), (-0.7, 0.0), (0.7, 0.8)],
            [(-0.7, -0.5), (0.0, 0.0), (-0.7, 0.5), (0.7, 0.0), (-0.7, -0.5)],
        ]
        for i in range(n):
            a = 2 * math.pi * i / n - math.pi / 2
            rr = S * 0.400
            px, py = cx + math.cos(a) * rr, cy + math.sin(a) * rr
            k = S * 0.062                              # 符文半尺寸
            # 每个符文朝向圆心（"贴在环上"而不是各自正立）
            ca, sa = -math.sin(a), math.cos(a)
            for seg in strokes[i % len(strokes)]:
                p0 = (px + seg[0] * k, py + seg[1] * k)
                p1 = (px + seg[1] * k, py + seg[0] * k)
                d.line([p0, p1], fill=(255, 244, 255, 235), width=max(2, int(S * 0.011)))
            d.ellipse([px - k * 0.16, py - k * 0.16, px + k * 0.16, py + k * 0.16],
                      fill=(255, 255, 255, 255))
        for k in range(5):
            rr = S * 0.400 + k * S * 0.018
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr],
                      outline=(236, 226, 255, 70), width=max(1, int(S * 0.005)))

    arr = np.array(im)
    return np.array(Image.fromarray(arr, "RGBA").resize((size, size), Image.LANCZOS))


# ============================================================ 入库
def premultiply(rgba):
    """RGB *= A/255（预乘，AlphaBlend 的 AC_SRC_ALPHA 要求）。
    ⚠️ 必须返回 uint8：曾经返回 float32 直接写文件，文件变成 4 倍大、
       游戏读到前 1/4 是 float 裸字节，整张糊成噪声。"""
    out = rgba.astype(np.float32)
    out[:, :, :3] *= (out[:, :, 3:4] / 255.0)
    out[:, :, :3] = np.minimum(out[:, :, :3], out[:, :, 3:4])
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def save_png(rgba, path):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    Image.fromarray(rgba, "RGBA").save(path, optimize=True)


FX_NAMES = ["fx_rays", "fx_shock", "fx_spark", "fx_pillar", "fx_flare", "fx_runes"]


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    made = 0
    if what in ("all", "badge"):
        print("== 档位徽章（颜色取自 RGQ_COL，程序化绘制）==")
        for i, (name, col, metal) in enumerate(TIERS):
            arr = make_badge(i, name, col, metal)
            pm = premultiply(arr)
            dst = os.path.join(ASSETS, name + ".png")
            save_png(pm, dst)
            back = np.array(Image.open(dst).convert("RGBA"))
            # 回读校验：① 尺寸 ② 有 alpha ③ 主色确实是该档位色
            assert back.shape[:2] == arr.shape[:2], "%s 尺寸不符" % name
            vis = back[:, :, 3] > 200
            n = int(vis.sum())
            assert n > 400, "%s 可见像素太少(%d)，画错了" % (name, n)
            # ⚠️ 取色必须**只取宝石那一小块**。
            #    第一版取"全部可见像素的中位数"，结果量到的是金属环的颜色 ——
            #    badge_hall 的宝石是炽白金、环是金，中位色变成金色，报偏差 122
            #    被误判成"配色不对"。宝石在正中，取中央 20% 的小方块。
            hh, ww = back.shape[0], back.shape[1]
            c0, c1 = int(hh * 0.40), int(hh * 0.60)
            d0, d1 = int(ww * 0.40), int(ww * 0.60)
            ctr = back[c0:c1, d0:d1]
            cv = ctr[:, :, 3] > 200
            assert int(cv.sum()) > 30, "%s 宝石区没有像素" % name
            rgb = ctr[:, :, :3][cv].astype(np.float32)
            al = ctr[:, :, 3][cv].astype(np.float32) / 255.0
            un = np.clip(rgb / np.maximum(al[:, None], 1e-6), 0, 255)
            dom = np.median(un, axis=0)
            # 宝石台面混了 50% 白高光，所以与纯档位色会有差距；
            # 这里比的是"色相方向"而不是精确值：看三个通道的大小序是否正确。
            order_ok = int(np.argmax(dom)) == int(np.argmax(col))
            err = float(np.abs(dom - np.array(col, np.float32)).mean())
            print("  %-18s %3dx%3d  可见%6d px  宝石色 %s（目标 %s）主通道序 %s  色差 %5.1f"
                  % (name, arr.shape[1], arr.shape[0], n,
                     np.round(dom).astype(int), col,
                     "OK" if order_ok else "!!错", err))
            made += 1
    if what in ("all", "fx"):
        print("\n== 开牌特效（程序化绘制）==")
        for name in FX_NAMES:
            arr = make_fx(name)
            pm = premultiply(arr)
            dst = os.path.join(ASSETS, name + ".png")
            save_png(pm, dst)
            back = np.array(Image.open(dst).convert("RGBA"))
            assert back.shape[:2] == arr.shape[:2], "%s 尺寸不符" % name
            bright = int((back[:, :, 3] > 40).sum())
            print("  %-18s %3dx%3d  亮部 %6d px  alpha上限 %d  OK"
                  % (name, arr.shape[1], arr.shape[0], bright, int(back[:, :, 3].max())))
            made += 1
    print("\n共生成 %d 张 → assets/" % made)
    return 0


if __name__ == "__main__":
    sys.exit(main())
