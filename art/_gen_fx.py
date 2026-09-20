# -*- coding: utf-8 -*-
"""生成 8 枚档位徽章 + 6 张开牌特效（mmx CLI）。

为什么这两类素材不能用 `_gen_units.py` 那套：
  单位图是"一个角色"，徽章是"一枚对称符号"，特效是"一团光"。
  它们的提示词结构、抠图方式、入库尺寸都不一样，所以单独一条管线。

【两类素材、两套透明化方案】

① 徽章（badge_*.png）—— **洋红键控**，和单位图一样。
   徽章本体是实心图案，用与单位图完全相同的判据（背景色中位数 + 容差键控），
   复用 `_unitlib.matte_file`，抠图质量有既有保证。

② 特效（fx_*.png）—— **黑底 luma key**，这一步是本脚本的关键。
   发光特效的"透明"本质是"没光的地方"：在黑底上叠加时，
   像素越亮 = 光越强，越黑 = 越透明。所以：
       alpha = max(R,G,B)          （而非洋红键控的"背景色匹配"）
       RGB   = 原色 × alpha/255     （预乘，引擎用 AlphaBlend 要求预乘）
   为什么**不能**对特效用洋红键控：
     洋红键控会按"与背景色的距离"切硬边，而光效的边缘是渐隐的 ——
     切出来是一圈粉边 + 生硬的圆形边界，叠到游戏里就是"贴了张方图"。
     黑底 luma key 天然保留渐隐：越靠外的光越暗 → alpha 越低 → 自然消失。
   代价：黑底上的**暗色部分会一并变透明**。对发光特效这不是问题（暗色本来就不该看见），
   但如果哪张图生成出来的不是黑底（带彩色背景/星空/场景），luma key 会留下垃圾 ——
   所以入库前有一步"黑底纯度"检查，不合格的会打上 `!!非黑底` 标记要求重生成。

【命名规范】（与既有素材一致：全小写下划线，进 assets/）
  badge_rare.png … badge_primordial.png   档位徽章（对应 RGQ_NAME 下标 1..7）
  fx_rays / fx_shock / fx_spark / fx_pillar / fx_flare / fx_runes

用法：
    python art/_gen_fx.py                    # 全部 14 张
    python art/_gen_fx.py badge_primordial   # 指定
    python art/_gen_fx.py --build            # 只把已有出图入库，不重新生成
    python art/_gen_fx.py --jobs 2
"""
import argparse
import os
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, "gen_fx")          # mmx 原始出图
ASSETS = os.path.join(ROOT, "assets")

sys.path.insert(0, HERE)
import _unitlib as U                                                    # noqa: E402
import _build_units as B                                                # noqa: E402  (premultiply)

# ⚠️ 通道顺序：全链路统一 **RGBA**（= PIL 的 'RGBA' 模式）。
#    U.save_bmp32 内部再转 BGRA；引擎读图时只做一次 BGRA↔RGBA 交换。
#    如果这里传成 BGRA，游戏里会 R/B 互换（草坪变紫、红色变蓝）。


def save_png32(rgba, path):
    """写 32 位 RGBA PNG。

    与 Phase 1 的既有约定一致：PNG 里存的是**已预乘**的字节
    （PNG 格式本身不做 alpha 变换，引擎读回来就是原值）。
    所以调用前必须已经 premultiply 过，这里不再动字节。
    """
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    Image.fromarray(np.asarray(rgba, dtype=np.uint8), "RGBA").save(path, optimize=True)

MAX_PX = 256            # 徽章是 UI 图标，不需要 200px 那么大；特效要铺屏，给到 512
MAX_PX_FX = 512


# ------------------------------------------------------------------ 画风锚点
# 与单位图共用同一套画风词，保证"徽章 / 特效 / 角色"看起来是一套素材。
STYLE = ("cartoon 2D game asset, Plants vs Zombies art style, hand painted, "
         "vibrant saturated colors, bold clean dark outlines, glossy highlights, "
         "crisp readable silhouette, no text, no watermark, no border")

# 颜色必须**精确**给十六进制值：只写 "sapphire blue" 这类词，模型会按自己的
# 审美选色 —— 实测 8 枚里有 5 枚跑偏（稀有出成橙金、传说出成蓝），
# 而档位颜色是游戏里既有的语言（RGQ_COL），必须严格对齐。
# 另外必须显式禁掉"底部圆盘"：模型给徽章补了一圈发光底座，
# 那底座是档位色（蓝/紫）而不是粉色，**洋红键控抓不到它**，
# 结果每个徽章下面都顶着一块色斑。
BADGE_STYLE = (STYLE + ", ornate heraldic badge, perfectly symmetrical, "
               "a faceted gem set in the center of a metal filigree ring, "
               "viewed straight on, flat 2D icon, game UI rank emblem, "
               "on a completely flat solid pure magenta #FF00FF background, "
               "uniform magenta with no gradient, no drop shadow, "
               "no ground shadow, no ellipse, no disc, no pedestal, "
               "no flat base, no glowing platform, no circular glow behind it, "
               "the badge floats alone with nothing beneath it")

# ⚠️ 特效提示词的三条硬教训（第一轮 6 张里 4 张报废，全踩在这上面）：
#   ① **绝对不要出现 flare / lens / light beam / spark of light 这类词**：
#      它们会把模型推向"电影镜头光晕"（lens flare）—— 出来的是斜置的
#      蓝白光斑，而不是"从中心放射的光扇"。第一轮的 rays/shock/spark
#      三张全是这个产物。
#   ② **要描述几何，不要描述氛围**：写"24 根等距锥形辐条"能出光扇；
#      写"一束放射的光"只能看运气。点数、形状、对称性都要给死。
#   ③ **显式要求"平视、正面、完全对称、居中"**：否则模型会加透视、
#      把图形斜放。第一轮的 shock 就是一个斜放的椭圆光晕。
#   保留可用的两张（fx_flare 的四芒星、fx_runes 的符文环）的写法作为范本：
#   它们之所以成功，正是因为描述里给的是"四芒星""一圈 16 个符文"这样的几何。
FX_STYLE = ("game VFX sprite element, flat 2D vector art, perfectly centered, "
            "front view, no perspective, completely symmetrical, "
            "single isolated graphic element, no scene, no scenery, "
            "no lens flare, no camera flare, no bokeh, no light leak, "
            "no text, no watermark, no border, no frame, "
            "pure solid black background filling the whole canvas, "
            "the black background stays completely empty and untouched")

# 档位 → (徽章名, 中心宝石颜色词, 附加修饰)
# 档位 → (徽章名, 中心宝石的**精确颜色**, 环体修饰)
# 宝石色严格取自 pvz.c 的 RGQ_COL（也就是卡片档位标签的颜色）——
# 徽章和文字标签必须是同一个颜色语言，否则玩家会以为是两个系统的两种等级。
BADGES = [
    ("badge_common",     "a dull plain grey iron gem, colour #B0B6B0",
     "a simple worn iron ring, humble and plain"),
    ("badge_rare",       "a brilliant sapphire gem, colour #60A8E8",
     "a polished silver ring with small sapphire inlays"),
    ("badge_epic",       "a radiant amethyst gem, colour #A86CE8",
     "an ornate ring with arcane purple engravings"),
    ("badge_legend",     "a glowing topaz gem, colour #F0A838",
     "a heavy gold ring set with small amber gems"),
    ("badge_myth",       "a blazing magenta gem, colour #F45884",
     "a rose-gold ring with petals of pink light"),
    ("badge_ultra",      "a molten crimson gem, colour #FF4030",
     "a blackened steel ring with red-hot cracks and embers"),
    ("badge_hall",       "a blinding white-gold gem, colour #FFF6D6",
     "a regal golden laurel ring, rays of white light"),
    ("badge_primordial", "a pure white-violet gem, colour #FFECFF",
     "an ancient floating ring of white runes, halo of violet light"),
]

# 特效名 → 视觉描述（全部改成"几何规格书"式写法，见上面 FX_STYLE 的三条教训）
FX = [
    ("fx_rays",
     "a sun emblem made of exactly 24 straight tapered spokes radiating outward "
     "from a small round core, every spoke the same length, evenly spaced around "
     "the full 360 degrees, alternating one long spoke and one short spoke, "
     "pale warm white light, graphic and flat"),
    ("fx_shock",
     "one single ring made of exactly 48 evenly spaced round dots, like a pearl "
     "necklace seen from directly above, all dots the same size, forming a "
     "perfect circle, concentric with the canvas center, nothing inside the ring, "
     "pale warm white"),
    ("fx_spark",
     "a firework starburst of exactly 60 short tapered streaks pointing straight "
     "outward from the exact center, every streak the same length and same width, "
     "evenly spaced around the full 360 degrees, bright white cores with soft "
     "edges"),
    ("fx_pillar",
     "a straight vertical tapered beam of light, wider at the top edge of the "
     "canvas and narrowing to a point near the bottom edge, exactly symmetrical "
     "about the vertical center line, bright white core with a pale warm halo, "
     "nothing else in frame"),
    ("fx_flare",
     "a four pointed star flare with a small blinding white round core and exactly "
     "four long straight tapering spikes, one up one down one left one right, "
     "plus four shorter diagonal spikes, everything symmetrical"),
    ("fx_runes",
     "a flat ring of exactly 16 glowing angular rune glyphs arranged in a perfect "
     "circle, all glyphs the same size and evenly spaced, each glyph a simple "
     "straight-line symbol of light, glowing white with a violet halo"),
]

def _throttle(state):
    with state["lock"]:
        gap = time.time() - state["last"][0]
        if gap < state["gap"][0]:
            time.sleep(state["gap"][0] - gap)
        state["last"][0] = time.time()


def _run_once(prompt, out):
    cmd = ["mmx", "image", "generate", "--prompt", prompt,
           "--aspect-ratio", "1:1", "--out", out]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, shell=True)
    ok = os.path.exists(out) and os.path.getsize(out) > 12000
    return ok, ((r.stdout or "") + (r.stderr or ""))


def build_prompt(kind, key):
    if kind == "badge":
        for n, gem, ring in BADGES:
            if n == key:
                return "%s, %s, %s" % (BADGE_STYLE, gem, ring)
        raise KeyError(key)
    for n, desc in FX:
        if n == key:
            return "%s, %s" % (FX_STYLE, desc)
    raise KeyError(key)


def gen(key, state, tries=3):
    kind = "badge" if key.startswith("badge_") else "fx"
    out = os.path.join(OUT, "%s.jpg" % key)
    if os.path.exists(out) and os.path.getsize(out) > 12000:
        return key, True, "已存在，跳过"
    p = build_prompt(kind, key)
    assert len(p) < 1500, "提示词 %d 字符，超过 mmx 的 1500 上限" % len(p)
    msg = ""
    for _ in range(tries):
        _throttle(state)
        ok, msg = _run_once(p, out)
        if ok:
            return key, True, "%.0f KB" % (os.path.getsize(out) / 1024.0)
        if "sensitivity filter" in msg or "RPM" in msg:
            with state["lock"]:
                state["gap"][0] = min(state["gap"][0] * 2.0, 20.0)
            time.sleep(state["gap"][0])
            continue
        break
    return key, False, msg[-260:].replace("\n", " ")


# ------------------------------------------------------------------ 入库
def luma_key(arr):
    """黑底 → alpha = max(R,G,B)，RGB 按 alpha 预乘。发光特效的透明化标准做法。

    为什么用 max 而不是加权 luma(0.299R+0.587G+0.114B)：
      纯蓝/纯紫的亮光 luma 只有 0.11~0.30，会被压得几乎全透明；
      而 max 取"最亮的那一个通道"，任何颜色的光都能保住强度。
      光效不看人眼感知权重，只看"这里有多少光"。
    """
    a = arr.astype(np.float32)
    a[:, :, 3] = a[:, :, :3].max(axis=2)           # 0..255
    # RGB 保持原色；预乘交给 B.premultiply（入库那一步统一做）
    return a.astype(np.uint8)


def black_purity(path):
    """黑底纯度：边框一圈的均值。越低越干净（纯黑=0）。返回 None 表示不可读。"""
    try:
        a = np.array(Image.open(path).convert("RGB")).astype(np.float32)
    except Exception:                                                    # noqa: BLE001
        return None
    ring = 8
    px = np.concatenate([a[:ring].reshape(-1, 3), a[-ring:].reshape(-1, 3),
                         a[:, :ring].reshape(-1, 3), a[:, -ring:].reshape(-1, 3)])
    return float(px.mean())


def build(key, force=False):
    src = os.path.join(OUT, "%s.jpg" % key)
    if not os.path.exists(src):
        return key, False, "缺源图"
    kind = "badge" if key.startswith("badge_") else "fx"

    if kind == "badge":
        im, c, spread = U.matte_file(src)
        arr = np.array(im)
        # 与 _strip_pinkhalo 同样的第二判据：贴剪影的亮品红也要清掉。
        from scipy import ndimage as ndi
        f = arr.astype(np.float32)
        r, g, b = f[:, :, 0], f[:, :, 1], f[:, :, 2]
        mx, mn = f[:, :, :3].max(2), f[:, :, :3].min(2)
        pink = (g < r - 20) & (g < b - 20) & ((mx - mn) > 50)
        al = arr[:, :, 3].copy()
        lab, _n = ndi.label(pink, structure=np.ones((3, 3), bool))
        nb = ndi.binary_dilation(al == 0, structure=np.ones((3, 3), bool))
        touch = set(np.unique(lab[nb & pink]).tolist()) - {0}
        halo = np.isin(lab, list(touch)) & (mx > 150) if touch else np.zeros_like(pink)
        kill = pink & (al > 0) & ((al < 250) | halo)
        al[kill] = 0
        arr[:, :, 3] = al
        maxpx, extra = MAX_PX, "背景=%s 清粉%d" % (np.round(c).astype(int), int(kill.sum()))
    else:
        pur = black_purity(src)
        if pur is None:
            return key, False, "源图不可读"
        arr = luma_key(np.array(Image.open(src).convert("RGBA")))
        maxpx = MAX_PX_FX
        extra = "黑底纯度=%.1f%s" % (pur, "  !!非黑底" if pur > 24 else "")

    ys, xs = np.where(arr[:, :, 3] > 0)
    if len(xs) == 0:
        return key, False, "透明化之后什么都没了"
    arr = arr[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    # 只缩不放（放大出糊边）；上限以内的原样保留
    h, w = arr.shape[0], arr.shape[1]
    if max(h, w) > maxpx:
        sc = maxpx / float(max(h, w))
        nw, nh = max(1, int(round(w * sc))), max(1, int(round(h * sc)))
        arr = np.array(Image.fromarray(arr, "RGBA").resize((nw, nh), Image.LANCZOS))

    os.makedirs(ASSETS, exist_ok=True)
    dst = os.path.join(ASSETS, key + ".png")
    amax = int(arr[:, :, 3].max())
    save_png32(B.premultiply(arr.astype(np.uint8)), dst)
    # 回读校验：写出来的尺寸 / 是否真是 PNG / alpha 是否还在
    back = Image.open(dst)
    assert back.size == (arr.shape[1], arr.shape[0]), "PNG 尺寸不符"
    assert int(np.array(back)[:, :, 3].max()) == amax, "PNG 的 alpha 被改写"
    return key, True, "%s.png %dx%d alpha上限%d %s" % (
        key, arr.shape[1], arr.shape[0], amax, extra)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keys", nargs="*")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--build", action="store_true", help="只入库，不重新生成")
    ap.add_argument("--rebuild", action="store_true", help="入库时强制覆盖")
    args = ap.parse_args()

    allkeys = [n for n, _, _ in BADGES] + [n for n, _ in FX]
    keys = args.keys or allkeys
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(ASSETS, exist_ok=True)

    if not args.build:
        state = {"lock": threading.Lock(), "last": [0.0], "gap": [2.0]}
        print("生成 %d 张（并发 %d）..." % (len(keys), args.jobs))
        bad = []
        done = [0]
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(gen, k, state): k for k in keys}
            for f in as_completed(futs):
                try:
                    k, ok, msg = f.result()
                except Exception as e:                                   # noqa: BLE001
                    k, ok, msg = futs[f], False, repr(e)[:160]
                done[0] += 1
                print("  [%2d/%2d] %-18s %s %s" % (done[0], len(keys), k, "OK " if ok else "X  ", msg))
                if not ok:
                    bad.append(k)
        print("\n出图 %d / %d" % (len(keys) - len(bad), len(keys)))

    print("\n入库到 assets/ ...")
    ok = 0
    for k in keys:
        k, good, msg = build(k)
        print("  %-18s %s %s" % (k, "OK " if good else "X  ", msg))
        ok += good
    print("\n入库 %d / %d" % (ok, len(keys)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
