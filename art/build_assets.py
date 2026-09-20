#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
美术资源后处理管线
------------------
把图像生成工具产出的 PNG（背景是"画出来的棋盘格"，不是真透明）
处理成游戏可用的 32 位 BGRA BMP（alpha 预乘，底部居中锚点）。

步骤：
  1. 从四边泛洪填充，识出背景（中性浅色 + 与种子色差 < thresh）
  2. 由内向外"推色"若干轮，消除边缘的浅色光晕残留
  3. 生成 alpha，并做 1px 羽化得到抗锯齿边
  4. 按内容包围盒裁切 -> 等比缩放到目标尺寸 -> 底部居中
  5. alpha 预乘（AlphaBlend 的 AC_SRC_ALPHA 要求）-> 写 BMP

阳光特殊处理：纯黑底 + 亮度转 alpha（保留发光渐变）
草坪特殊处理：缩放 + 烘焙棋盘格，输出不透明大图

加新资源的操作清单见项目根目录 PIPELINE.md。硬性规则：
  - 每加一株会开火的植物，必须同步生成配套子弹（bullet_*）
  - 子弹禁止走角色 grade()（会把火焰橙 / 末日紫拉成灰绿）
  - 子弹主体小，抠图腐蚀核要从 13px 降到 5px，否则核心会被吃光
"""
import os
import struct
import glob
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, 'raw')
OUT = os.path.abspath(os.path.join(HERE, '..', 'assets'))
os.makedirs(OUT, exist_ok=True)

# 时间戳 -> 资源名（生成顺序固定）
MAPPING = [
    ('06-05-24', 'peashooter'),
    ('06-06-33', 'sunflower'),
    ('06-06-51', 'wallnut'),
    ('06-07-11', 'potatomine'),
    ('06-07-31', 'snowpea'),
    ('06-07-50', 'repeater'),
    ('06-08-08', 'cherrybomb'),
    ('06-08-28', 'jalapeno'),
    ('06-08-48', 'zombie_normal'),
    ('06-09-15', 'zombie_cone'),
    ('06-09-35', 'zombie_bucket'),
    ('06-09-53', 'zombie_flag'),
    ('06-10-15', 'sun'),
    ('06-10-34', 'grass_old'),
    ('06-28-59', 'grass'),
    ('08-23-13', 'background'),
    ('08-23-39', 'decals'),
    ('08-18-45', 'zombie_normal_eat'),
    ('08-21-41', 'zombie_cone_eat'),
    ('08-22-08', 'zombie_bucket_eat'),
    ('08-22-35', 'zombie_flag_eat'),
    ('16-39-37', 'lawn_night'),
    ('16-39-58', 'lawn_water'),
    ('16-40-25', 'plant_pierce'),
    ('16-40-39', 'plant_splash'),
    ('16-40-56', 'plant_bounce'),
    ('16-41-17', 'plant_freeze'),
    ('16-41-34', 'plant_burn'),
    ('16-41-53', 'plant_doublestack'),
    ('16-42-12', 'plant_multilane'),
    ('16-42-31', 'plant_tall'),
    ('16-42-50', 'relic_gold'),
    ('16-43-07', 'relic_ghost'),
    ('16-43-25', 'relic_pumpkin'),
    ('18-19-29', 'plant_threepeater'),
    ('18-19-58', 'plant_spikeweed'),
    ('18-20-26', 'plant_magnet'),
    ('18-20-55', 'plant_kernelpult'),
    ('18-21-23', 'zombie_dancer'),
    ('18-21-51', 'zombie_zamboni'),
    ('18-22-19', 'zombie_balloon'),
    ('18-22-47', 'lawn_roof'),
    ('18-23-15', 'lawn_desert'),
    ('cardback', 'ui_cardback'),
    ('unlockburst', 'ui_burst'),
    ('zombie_vaulter', 'zombie_vaulter'),
    ('zombie_screendoor', 'zombie_screendoor'),
    ('zombie_football', 'zombie_football'),
    ('zombie_newspaper', 'zombie_newspaper'),
    ('zombie_digger', 'zombie_digger'),
    ('zombie_giant', 'zombie_giant'),
    ('hero_flame', 'hero_flame'),
    ('hero_ironnut', 'hero_ironnut'),
    ('hero_crystal', 'hero_crystal'),
    ('hero_thornvine', 'hero_thornvine'),
    ('hero_sungod', 'hero_sungod'),
    ('hero_frost', 'hero_frost'),
    ('hero_worldtree', 'hero_worldtree'),
    ('hero_doom', 'hero_doom'),
    ('starfruit', 'starfruit'), ('cactus', 'cactus'), ('splitpea', 'splitpea'),
    ('lightningreed', 'lightningreed'), ('bloomerang', 'bloomerang'),
    ('fume', 'fume'), ('laserbean', 'laserbean'), ('melonpult', 'melonpult'),
    ('wintermelon', 'wintermelon'), ('gloom', 'gloom'),
    ('goldmagnet', 'goldmagnet'), ('twinflower', 'twinflower'),
    ('marrow', 'marrow'), ('pumpkin', 'pumpkin'), ('garlic', 'garlic'),
    ('hypnoshroom', 'hypnoshroom'), ('iceshroom', 'iceshroom'),
    ('tanglekelp', 'tanglekelp'), ('cobcannon', 'cobcannon'), ('cattail', 'cattail'),
    ('bullet_pea', 'bullet_pea'), ('bullet_ice', 'bullet_ice'),
    ('bullet_kernel', 'bullet_kernel'), ('bullet_flame', 'bullet_flame'),
    ('bullet_crystal', 'bullet_crystal'), ('bullet_frost', 'bullet_frost'),
    ('bullet_doom', 'bullet_doom'),
    ('bullet_pierce', 'bullet_pierce'), ('bullet_splash', 'bullet_splash'),
    ('bullet_bounce', 'bullet_bounce'), ('bullet_burn', 'bullet_burn'),
    ('bullet_star', 'bullet_star'), ('bullet_spine', 'bullet_spine'),
    ('bullet_split', 'bullet_split'), ('bullet_arc', 'bullet_arc'),
    ('bullet_boomer', 'bullet_boomer'), ('bullet_fume', 'bullet_fume'),
    ('bullet_beam', 'bullet_beam'), ('bullet_melon', 'bullet_melon'),
    ('bullet_icemelon', 'bullet_icemelon'), ('bullet_gloom', 'bullet_gloom'),
    ('bullet_cob', 'bullet_cob'), ('bullet_dart', 'bullet_dart'),
]

# 目标高度（2 倍逻辑像素，因为渲染走 2x 超采样）
TARGET_H = {
    'peashooter': 200, 'sunflower': 200, 'wallnut': 194, 'potatomine': 140,
    'snowpea': 200, 'repeater': 202, 'cherrybomb': 178, 'jalapeno': 200,
    'zombie_normal': 200, 'zombie_cone': 200,
    'zombie_bucket': 200, 'zombie_flag': 200,
    'zombie_normal_eat': 200, 'zombie_cone_eat': 200,
    'zombie_bucket_eat': 200, 'zombie_flag_eat': 200,
    'plant_pierce': 200, 'plant_splash': 200, 'plant_bounce': 200,
    'plant_freeze': 200, 'plant_burn': 200,
    'plant_doublestack': 220, 'plant_multilane': 160, 'plant_tall': 280,
    'relic_gold': 96, 'relic_ghost': 96, 'relic_pumpkin': 200,
    'plant_threepeater': 200, 'plant_spikeweed': 120, 'plant_magnet': 170,
    'plant_kernelpult': 210,
    # 上一轮新增的 20 株植物：最初由 _export_new.py 以「默认 200」导出，
    # 但没登记进 TARGET_H —— 于是 MAPPING 里能 find_raw 到原图，走到这里就 KeyError，
    # 整条构建（含全部子弹）直接中断。这里补齐，顺带把导出权收回主管线。
    'starfruit': 200, 'cactus': 200, 'splitpea': 200, 'lightningreed': 190,
    'bloomerang': 200, 'fume': 200, 'laserbean': 200, 'melonpult': 210,
    'wintermelon': 210, 'gloom': 190, 'goldmagnet': 170, 'twinflower': 200,
    'marrow': 190, 'pumpkin': 200, 'garlic': 150, 'hypnoshroom': 180,
    'iceshroom': 180, 'tanglekelp': 150, 'cobcannon': 230, 'cattail': 210,
    'zombie_dancer': 220, 'zombie_zamboni': 190, 'zombie_balloon': 260,
    'ui_cardback': 320, 'ui_burst': 520,
    'zombie_vaulter': 210, 'zombie_screendoor': 205, 'zombie_football': 210,
    'zombie_newspaper': 205, 'zombie_digger': 200, 'zombie_giant': 330,
    # 神级植物：体型普遍比普通植物大一圈，稀有度越高越"重"
    'hero_flame': 215, 'hero_ironnut': 210,
    'hero_crystal': 240, 'hero_thornvine': 135,
    'hero_sungod': 260, 'hero_frost': 250,
    'hero_worldtree': 340, 'hero_doom': 320,
    # 子弹：统一 44px（= 22 逻辑像素），保证小尺寸下形状可读
    'bullet_pea': 44, 'bullet_ice': 44, 'bullet_kernel': 46,
    'bullet_flame': 48, 'bullet_crystal': 48, 'bullet_frost': 48, 'bullet_doom': 46,
    'bullet_pierce': 44, 'bullet_splash': 46, 'bullet_bounce': 44, 'bullet_burn': 46,
    'bullet_star': 44, 'bullet_spine': 44, 'bullet_split': 44, 'bullet_arc': 46,
    'bullet_boomer': 46, 'bullet_fume': 48, 'bullet_beam': 46, 'bullet_melon': 50,
    'bullet_icemelon': 50, 'bullet_gloom': 46, 'bullet_cob': 52, 'bullet_dart': 44,
    'sun': 116,
}

LAWN_W, LAWN_H = 1728, 1000      # 草坪纹理输出尺寸（= 864x500 逻辑 x 2）
BG_W, BG_H = 2000, 1300          # 背景图输出尺寸（= 1000x650 逻辑 x 2）
COLS, ROWS = 9, 5

# 只导出为独立资源的项；其余（grass/decals）在 build_lawn 里内部消化
EXPORT = set(TARGET_H.keys()) | {'lawn'}
NO_GRADE = {n for n in TARGET_H if n.startswith('bullet_')} | {'sun', 'ui_burst'}


# mmx CLI 只出 JPG，所以原始图目录要同时接受 png / jpg / jpeg / webp。
# 顺序即优先级：同名时 png 优先（png 无损，适合二次加工）。
RAW_EXTS = ('.png', '.jpg', '.jpeg', '.webp')


def _raw_list():
    out = []
    for ext in RAW_EXTS:
        out += glob.glob(os.path.join(RAW, '*' + ext))
    return out


def find_raw(key, required=True):
    # 精确文件名优先，避免 pumpkin 命中 relic_pumpkin、bullet_ice 命中 bullet_icemelon
    for ext in RAW_EXTS:
        exact = os.path.join(RAW, key + ext)
        if os.path.isfile(exact):
            return exact
    hits = [p for p in _raw_list()
            if os.path.splitext(os.path.basename(p))[0] == key or
               os.path.splitext(os.path.basename(p))[0].endswith('_' + key)]
    if not hits:
        hits = [p for p in _raw_list() if key in os.path.basename(p)]
    if not hits:
        if required:
            raise SystemExit('找不到原始图: ' + key)
        return None
    hits.sort(key=lambda p: (0 if os.path.basename(p).startswith(key) else 1, len(os.path.basename(p))))
    return hits[0]


def _detect_bg(arr):
    """识别背景类型。生成器在不同图上给的背景并不统一：
       有的是"画出来的棋盘格"(浅色两色)，有的直接是纯黑。"""
    h, w = arr.shape[:2]
    b = 24
    ring = np.concatenate([arr[:b, :].reshape(-1, 3), arr[-b:, :].reshape(-1, 3),
                           arr[:, :b].reshape(-1, 3), arr[:, -b:].reshape(-1, 3)])
    vals, counts = np.unique(ring, axis=0, return_counts=True)
    order = counts.argsort()[::-1]
    bg1 = vals[order[0]].astype(np.int32)
    if bg1.max() <= 24:                      # 纯黑底
        return 'black', [bg1]
    colors = [vals[i].astype(np.int32) for i in order[:min(2, len(order))]]
    lum = float(bg1.mean())
    if lum < 195:
        return 'gray', colors                # 中性灰：固定 >=195 阈值会整张漏判
    return 'light', colors


def _flood(mask, visited, seeds):
    """扫描线种子填充：把与 seeds 连通、mask 为真的像素标进 visited"""
    h, w = mask.shape
    stack = list(seeds)
    while stack:
        x, y = stack.pop()
        if visited[y, x] or not mask[y, x]:
            continue
        xl = x
        while xl > 0 and mask[y, xl - 1] and not visited[y, xl - 1]:
            xl -= 1
        xr = x
        while xr < w - 1 and mask[y, xr + 1] and not visited[y, xr + 1]:
            xr += 1
        visited[y, xl:xr + 1] = True
        for ny in (y - 1, y + 1):
            if 0 <= ny < h:
                seg = mask[ny, xl:xr + 1] & ~visited[ny, xl:xr + 1]
                idx = np.flatnonzero(seg)
                if idx.size:
                    br = np.flatnonzero(np.diff(idx) > 1)
                    st = np.concatenate(([0], br + 1))
                    en = np.concatenate((br, [idx.size - 1]))
                    for s, e in zip(st, en):
                        stack.append((xl + int(idx[s]), ny))


def flood_from_border(cand):
    """只保留「与画面边界相连」的候选像素（扫描线种子填充，O(段数)）

    为什么不用 ImageDraw.floodfill：Pillow 12.3.0 里它在 'L' 模式下是坏的——
    哪怕给一张 100x100 全 255 的图、thresh=0，填充后也毫无变化（实测）。
    """
    h, w = cand.shape
    seeds = []
    for x in np.flatnonzero(cand[0]):
        seeds.append((int(x), 0))
    for x in np.flatnonzero(cand[h - 1]):
        seeds.append((int(x), h - 1))
    for y in np.flatnonzero(cand[:, 0]):
        seeds.append((0, int(y)))
    for y in np.flatnonzero(cand[:, w - 1]):
        seeds.append((w - 1, int(y)))
    vis = np.zeros((h, w), bool)
    _flood(cand, vis, seeds)
    return vis


def kill_pockets(outside, cand, arr, kind, area_min=3000):
    """干掉被主体包围的「背景口袋」——典型是僵尸两腿之间 / 手臂与身体之间的空隙。

    这类区域颜色和背景一样，但没连到画面边界，泛洪抓不到，会变成纯白块贴在角色身上。
    判据用**面积**：口袋动辄几千像素，而角色内部的亮部（眼白、冰晶、金属高光）都很小。
    （试过按颜色判定，不稳——口袋里的棋盘格有时恰好整块都是白格。）
    """
    h, w = arr.shape[:2]
    bg = arr[outside].mean(axis=0).astype(np.float32) if outside.any() else np.array([255, 255, 255], np.float32)
    pocket = cand & ~outside
    vis = np.zeros((h, w), bool)
    rest = pocket & ~vis
    killed = 0
    while rest.any():
        k = int(np.argmax(rest))
        y0, x0 = divmod(k, w)
        comp = np.zeros((h, w), bool)
        _flood(pocket, comp, [(x0, y0)])
        vis |= comp
        area = int(comp.sum())
        if area >= area_min:
            mean_c = arr[comp].mean(axis=0).astype(np.float32)
            dist = float(np.abs(mean_c - bg).sum())
            if dist <= 90.0 or area >= 4 * area_min:
                outside |= comp
                killed += 1
        rest = pocket & ~vis
    return outside, killed


def cutout(path, push=10, feather=0.9, small=False):
    """抠图：识别背景 -> 边界连通域 -> 腐蚀取核心区 -> 推色 -> 边缘软键 alpha

    踩过的坑：
    1. 生成图里的"透明"其实是把棋盘格画成了像素（两色 228/253）。
    2. 不能用 PIL 的 ImageDraw.floodfill —— thresh 是**通道差值之和**，
       棋盘两色差 82，阈值设小了只吃掉一半，残留一圈浅色方块。
    3. 不同图的背景不统一，有的直接是纯黑。
    4. 图像本身在主体周围画了一圈柔光晕，必须用「核心区推色 + 边缘软键」处理掉。
    """
    arr = np.array(Image.open(path).convert('RGB'))
    h, w = arr.shape[:2]
    kind, colors = _detect_bg(arr)
    ai = arr.astype(np.int32)

    if kind == 'black':
        cand = arr.max(axis=2) <= 40
        softv = np.clip((arr.max(axis=2).astype(np.float32) - 6.0) / 20.0, 0, 1)
    else:
        mx16 = arr.max(axis=2).astype(np.int16)
        mn16 = arr.min(axis=2).astype(np.int16)
        # 棋盘格两色（228 / 253）都满足「中性 + 亮」，用这个规则一次覆盖，
        # 比取"出现最多的两种颜色"稳得多（近似白太多，前二往往是同一个色调）
        lum = float(colors[0].mean())
        cand = ((mx16 - mn16) <= 28) & (mn16 >= lum - 58) & (mn16 <= lum + 62)
        d = np.abs(ai - colors[0]).max(axis=2)
        softv = np.clip((d.astype(np.float32) - 14.0) / 38.0, 0, 1)

    # 只保留与画面边界相连的候选区 = 背景，再干掉被包围的背景口袋
    outside = flood_from_border(cand)
    pocket_min = 400 if small else 3000
    outside, _ = kill_pockets(outside, cand, arr, kind, area_min=pocket_min)
    inside = ~outside

    # 腐蚀出「核心区」：边缘一圈和背景混过色 / 有柔光晕，不能用来取色也不能算实体
    # 子弹等小主体用 5px，MinFilter(13) 会把整颗豌豆吃光
    erode = 5 if small else 13
    core = np.array(Image.fromarray((inside * 255).astype(np.uint8), 'L')
                    .filter(ImageFilter.MinFilter(erode))) > 127
    if small and int(core.sum()) < 80:
        core = inside.copy()

    # 由内向外推色，把核心区的颜色铺满整个画布
    col = arr.astype(np.float32)
    known = core.copy()
    for _ in range(push):
        acc = np.zeros_like(col)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            acc += np.roll(col, (dy, dx), (0, 1)) * np.roll(known, (dy, dx), (0, 1))[..., None]
            cnt += np.roll(known, (dy, dx), (0, 1))
        fill = (~known) & (cnt > 0)
        col[fill] = acc[fill] / cnt[fill][..., None]
        known = known | fill

    # alpha：核心区全不透明；核心到边界之间的窄带用软键（吃掉光晕但保住暗描边）
    alpha = np.zeros((h, w), np.float32)
    alpha[core] = 1.0
    band = inside & ~core
    alpha[band] = softv[band]
    alpha[outside] = 0.0
    a = Image.fromarray((alpha * 255).astype(np.uint8), 'L').filter(
        ImageFilter.GaussianBlur(feather))
    alpha = np.array(a).astype(np.float32) / 255.0
    alpha[core] = 1.0
    return np.dstack([col, alpha * 255.0])


def cutout_sun(path, gamma=0.85):
    """纯黑底：亮度直接当 alpha，天然就是预乘格式"""
    arr = np.array(Image.open(path).convert('RGB')).astype(np.float32)
    lum = arr.max(axis=2) / 255.0
    alpha = np.clip(lum, 0, 1) ** gamma
    # 去掉四角残留噪点
    alpha[alpha < 0.09] = 0.0     # 阈值别太低，否则四角残留微光会在精灵边界形成一个方形
    return np.dstack([arr, alpha * 255.0])


def trim(rgba, thresh=8):
    a = rgba[:, :, 3]
    ys, xs = np.where(a > thresh)
    if len(ys) == 0:
        return rgba
    return rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def fit_bottom(rgba, target_h):
    """等比缩放到目标高度，返回 (图像, 底部对齐说明)"""
    img = Image.fromarray(rgba.astype(np.uint8), 'RGBA')
    w, h = img.size
    scale = target_h / float(h)
    nw, nh = max(1, int(round(w * scale))), target_h
    img = img.resize((nw, nh), Image.LANCZOS)
    return np.array(img).astype(np.float32)


# ============================================================================
# 统一后处理：把每张角色资源的亮度/饱和度/对比度往全局基准拉一部分
# ============================================================================
# 为什么是"拉一部分"而不是拉平：
#   僵尸本来就该比植物灰、比植物冷（这是它们的角色特征），全拉平会丢掉特征。
#   但完全不拉，各图之间会像来自不同游戏 —— 实测僵尸亮度 155 / 植物 49~125、
#   僵尸饱和 0.18 / 植物 0.60，摆在一起就是"彩色贴纸 + 灰模"的割裂感。
#   所以取一个折中：朝基准靠拢 65%，保留 35% 的原有特征。
BASE_LUM      = 118.0     # 基准亮度（取植物平均值偏亮一点）
BASE_SAT      = 0.52      # 基准饱和度
BASE_CONTRAST = 145.0     # 基准对比度（亮度 5%~95% 分位差）
PULL          = 0.65      # 朝基准靠拢的比例

_LAST_GRADE = {}

def grade(rgba, name, log=False):
    """统一调色 + 光照方向校正。返回处理后的直通 RGBA（未预乘）。"""
    a = rgba.copy()
    al = a[:, :, 3]
    m = al > 40
    if m.sum() < 80:
        return a
    rgb = a[:, :, :3].astype(np.float32)
    px = rgb[m]
    lum = 0.299 * px[:, 0] + 0.587 * px[:, 1] + 0.114 * px[:, 2]
    cur_lum = float(lum.mean())
    mx = px.max(axis=1); mn = px.min(axis=1)
    cur_sat = float(((mx - mn) / np.maximum(mx, 1.0)).mean())
    cur_ctr = float(np.percentile(lum, 95) - np.percentile(lum, 5))

    kl = 1.0 + PULL * (BASE_LUM / max(cur_lum, 1.0) - 1.0)
    kl = min(max(kl, 0.55), 1.85)
    kc = 1.0 + PULL * (BASE_CONTRAST / max(cur_ctr, 1.0) - 1.0)
    kc = min(max(kc, 0.70), 1.70)
    ks = 1.0 + PULL * (BASE_SAT / max(cur_sat, 0.01) - 1.0)
    ks = min(max(ks, 0.80), 2.60)

    out = (rgb - 128.0) * kc + 128.0        # 围绕中灰做对比
    out *= kl                               # 亮度
    g = out[:, :, 0] * 0.299 + out[:, :, 1] * 0.587 + out[:, :, 2] * 0.114
    out = g[:, :, None] + (out - g[:, :, None]) * ks   # 饱和度
    out = np.clip(out, 0, 255)
    res = np.dstack([out, a[:, :, 3].astype(np.float32)])
    res[:, :, :3] = np.where(m[:, :, None], res[:, :, :3], 0.0)

    # ---- 光照方向校正：主光是左上，凡是"右下比左上更亮"的图做水平镜像 ----
    ys, xs = np.where(m)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    hh, ww = y1 - y0, x1 - x0
    # 注意：必须用与原图同形状的二维掩码，不能用 where() 得到的一维索引
    yy2 = np.arange(res.shape[0])[:, None]
    xx2 = np.arange(res.shape[1])[None, :]
    tlm = m & (yy2 < y0 + hh // 2) & (xx2 < x0 + ww // 2)
    brm = m & (yy2 >= y0 + hh // 2) & (xx2 >= x0 + ww // 2)
    if tlm.sum() > 30 and brm.sum() > 30:
        l2 = 0.299 * res[:, :, 0] + 0.587 * res[:, :, 1] + 0.114 * res[:, :, 2]
        d = float(l2[tlm].mean() - l2[brm].mean())
        if d < -3.0:                       # 光反了 -> 水平镜像扳回来
            res = res[:, ::-1, :].copy()
            flipped = True
        else:
            flipped = False
    else:
        d, flipped = 0.0, False

    if log:
        _LAST_GRADE[name] = (cur_lum, cur_sat, cur_ctr, kl, ks, kc, d, flipped)
        print('   grade %-20s 亮度%6.1f->%6.1f 饱和%.2f->%.2f 对比%5.1f->%5.1f %s'
              % (name, cur_lum, cur_lum * kl, cur_sat, min(cur_sat * ks, 1.0),
                 cur_ctr, cur_ctr * kc, '镜像校正光向' if flipped else ''))
    return res


def premultiply(rgba):
    """BGRA 预乘：RGB *= A/255（AlphaBlend 的 AC_SRC_ALPHA 要求）"""
    out = rgba.copy()
    a = out[:, :, 3:4] / 255.0
    out[:, :, :3] *= a
    # 预乘后必须 rgb <= alpha（逐通道）；越界说明哪步把 RGB 又写回去了
    if not (out[:, :, :3] <= out[:, :, 3:4] + 1.0).all():
        out[:, :, :3] = np.minimum(out[:, :, :3], out[:, :, 3:4])
    return out


def save_bmp32(path, rgba):
    """不预乘转换，调用方负责；这里只负责写 32 位 BGRA、自下而上的 BMP"""
    h, w = rgba.shape[:2]
    bgra = rgba[:, :, [2, 1, 0, 3]].astype(np.uint8)
    data = bgra[::-1].tobytes()
    hdr = b'BM' + struct.pack('<IHHI', 14 + 40 + len(data), 0, 0, 14 + 40)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 32, 0, len(data), 2835, 2835, 0, 0)
    with open(path, 'wb') as f:
        f.write(hdr + info + data)
    return w, h


def label_components(mask):
    """把二值掩膜拆成连通域列表（用同一套扫描线填充）"""
    h, w = mask.shape
    vis = np.zeros((h, w), bool)
    comps = []
    rest = mask & ~vis
    while rest.any():
        k = int(np.argmax(rest))
        y0, x0 = divmod(k, w)
        comp = np.zeros((h, w), bool)
        _flood(mask, comp, [(x0, y0)])
        comps.append(comp)
        vis |= comp
        rest = mask & ~vis
    return comps


def cut_decals_sheet(path, min_area=350):
    """把图集里的小物件一个个切出来（复用抠图管线 + 连通域拆分）"""
    rgba = cutout(path)
    solid = rgba[:, :, 3] > 128
    decals = []
    for comp in label_components(solid):
        if comp.sum() < min_area:
            continue
        ys, xs = np.where(comp)
        sub = rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
        decals.append(sub.astype(np.uint8))
    return decals


def make_fall_frames(rgba, name, angles=(-22, -45, -68, -85, -95)):
    """僵尸倒地：绕「脚底」旋转预渲染若干帧

    为什么这么干而不是去生成：绕脚底旋转出来的帧和行走姿态**完全同源**，
    配色/画风/比例绝对一致，而且零生成成本。倒地只持续 0.4 秒，
    玩家看到的是「倒下去」这个运动，不需要每帧都有独立美术。
    """
    im = Image.fromarray(rgba.astype(np.uint8), 'RGBA')
    h, w = im.size[1], im.size[0]
    pad = int(h * 1.25) + 4
    CW, CH = w + 2 * pad, h + pad
    feet = (pad + w // 2, h + pad // 2)          # 脚底在画布中的位置
    canvas = Image.new('RGBA', (CW, CH), (0, 0, 0, 0))
    canvas.paste(im, (pad, pad // 2))

    for i, ang in enumerate(angles):
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
        out = Image.new('RGBA', (nw, nh), (0, 0, 0, 0))
        out.paste(rot.crop((x0, y0, x1, y1)), (nw // 2 - fx, 0))
        a = np.array(out).astype(np.float32)
        a = premultiply(a)
        save_bmp32(os.path.join(OUT, '%s_dead%d.bmp' % (name, i)), a)


def paste_rgba(dst, src_rgb, src_a, cx, cy, scale, angle):
    """把带 alpha 的小图旋转缩放后合成到 dst（dst 是 HxWx3 float 数组）"""
    im = Image.fromarray(np.dstack([src_rgb, src_a]).astype(np.uint8), 'RGBA')
    nw = max(1, int(round(im.width * scale)))
    nh = max(1, int(round(im.height * scale)))
    im = im.resize((nw, nh), Image.LANCZOS)
    if angle:
        im = im.rotate(angle, resample=Image.BICUBIC, expand=True)
    a = np.array(im).astype(np.float32)

    H, W = dst.shape[:2]
    x0 = int(round(cx - a.shape[1] / 2.0))
    y0 = int(round(cy - a.shape[0] / 2.0))
    sx0, sy0 = max(0, -x0), max(0, -y0)
    x0, y0 = max(0, x0), max(0, y0)
    x1 = min(W, x0 + a.shape[1] - sx0)
    y1 = min(H, y0 + a.shape[0] - sy0)
    if x1 <= x0 or y1 <= y0:
        return
    patch = a[sy0:sy0 + (y1 - y0), sx0:sx0 + (x1 - x0)]
    al = (patch[:, :, 3:4] / 255.0)
    dst[y0:y1, x0:x1] = dst[y0:y1, x0:x1] * (1.0 - al) + patch[:, :, :3] * al


def strip_watermark(img):
    """抹掉生成图右下角的「AI 生成 / WORKBUDDY」水印。

    生成器把水印烧进了像素里，位置固定在右下角约 20% x 15% 的区域内。
    做法不是裁掉那块（会破坏构图），而是用「正上方同宽的一块」覆盖 ——
    这些底图都是可平铺纹理，纵向接续后纹理会自然衔接，看不出修补痕迹。
    """
    w, h = img.size
    bw, bh = int(w * 0.22), int(h * 0.17)
    if bw < 4 or bh < 4:
        return img
    img.paste(img.crop((w - bw, h - bh * 2, w, h - bh)), (w - bw, h - bh))
    return img


def build_lawn(path, decal_path=None):
    """草坪多层合成（离线烘焙，运行时零开销）

    层次：细草底纹 -> 大尺度色斑 -> 斜向割草条纹 -> 9x5 棋盘格
          -> 随机贴花（草簇/石子/小花）-> 边缘接触阴影 AO -> 暗角 -> 调色
    """
    from PIL import ImageEnhance

    # ---- L1 细草底纹：镜像平铺 2x2（天然无缝，比拉伸铺满更像草地）----
    src = Image.open(path).convert('RGB')
    w, h = src.size
    src = src.crop((0, 0, int(w * 0.82), int(h * 0.88)))      # 切掉右下角水印
    tile = src.resize((LAWN_W // 2, LAWN_H // 2), Image.LANCZOS)
    a = np.array(tile).astype(np.float32)
    row = np.concatenate([a, a[:, ::-1]], axis=1)
    base = np.concatenate([row, row[::-1, :]], axis=0)

    yy = np.arange(LAWN_H)[:, None].astype(np.float32)
    xx = np.arange(LAWN_W)[None, :].astype(np.float32)

    # ---- L1.5 高频草叶细节：真实草地近看有细碎的明暗颗粒，
    #      缺了这层，草坪会显得像塑料板（实测平均对比只有 23~31，角色是 130~200）----
    from PIL import ImageFilter
    hi = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8)).convert('L')
    hi = hi.filter(ImageFilter.UnsharpMask(radius=2, percent=190, threshold=1))
    hia = np.array(hi).astype(np.float32) - np.array(
        Image.fromarray(np.clip(base, 0, 255).astype(np.uint8)).convert('L')).astype(np.float32)
    # 高频部分只取 62%，避免噪点感
    base += (hia * 0.62)[:, :, None]

    # ---- L2 大尺度色斑：低频噪声轻微提亮/压暗，打散平铺的规律感 ----
    rng = np.random.default_rng(20260915)
    low = (rng.random((9, 16)) * 255).astype(np.uint8)
    low = np.array(Image.fromarray(low).resize((LAWN_W, LAWN_H), Image.BICUBIC))
    low = low.astype(np.float32) / 255.0
    low -= low.mean()
    base *= (1.0 + low * 0.20)[:, :, None]

    # ---- L3 斜向割草条纹：很淡，只为暗示「修剪过的草坪」----
    base *= (1.0 + 0.048 * np.sin((xx + yy) * (2.0 * 3.14159265 / 268.0)))[:, :, None]

    # ---- L4 9x5 棋盘格：给玩家格子感（明度差 9%）----
    cw, ch = LAWN_W // COLS, LAWN_H // ROWS
    for r in range(ROWS):
        for c in range(COLS):
            if (r + c) & 1:
                base[r * ch:(r + 1) * ch, c * cw:(c + 1) * cw] *= 0.915

    # ---- L5 方向光：左上偏暖提亮、右下偏冷压暗 ----
    # 必须和角色资源的光照方向一致（角色是左上暖主光 + 右下冷轮廓光），
    # 否则草坪和角色会像两个图层拼在一起。
    lx = 1.0 - xx / LAWN_W
    ly = 1.0 - yy / LAWN_H
    L = 0.45 * lx + 0.55 * ly                      # 1 = 左上, 0 = 右下
    lum = 0.855 + 0.28 * L                         # 原有 0.15 太弱，地面读不出光向
    base *= np.dstack([lum * (1.0 + 0.062 * L),    # 亮处偏暖
                       lum,
                       lum * (1.0 - 0.082 * L)])   # 暗处偏冷

    # ---- L5.5 树影斑驳：几块柔和的明暗斑，打破大面积均匀感 ----
    for _ in range(10):
        dcx = float(rng.random()) * LAWN_W
        dcy = float(rng.random()) * LAWN_H * 0.92
        dr = float(rng.uniform(170, 400))
        dd = ((xx - dcx) ** 2 + (yy - dcy) ** 2) / (dr * dr)
        k = float(rng.uniform(-0.060, 0.070))
        base *= (1.0 + k * np.exp(-dd))[:, :, None]

    # ---- L6 随机贴花 ----
    decals = []
    if decal_path and os.path.exists(decal_path):
        decals = cut_decals_sheet(decal_path)
        print('   贴花切出 %d 个' % len(decals))
        # 越靠草坪边缘越密（中间是主战区，少放以免干扰读图）
        # 尺寸必须很小：一格才 96x100 逻辑像素，贴花超过 ~14px 就会盖住战场
        for _ in range(90):
            d = decals[int(rng.integers(0, len(decals)))]
            cx = float(rng.random()) * LAWN_W
            cy = float(rng.random()) * LAWN_H
            edge = min(cx, cy, LAWN_W - cx, LAWN_H - cy) / (LAWN_H * 0.5)
            if rng.random() > (0.35 + (1.0 - min(edge, 1.0)) * 0.65):
                continue
            scale = float(rng.uniform(0.075, 0.155)) * (0.9 + min(edge, 1.0) * 0.35)
            shade = float(rng.uniform(0.90, 1.05))
            alpha = d[:, :, 3].astype(np.float32) * float(rng.uniform(0.75, 0.95))
            rgb = np.clip(d[:, :, :3].astype(np.float32) * shade, 0, 255)
            paste_rgba(base, rgb, alpha,
                       cx, cy, scale, float(rng.uniform(-180, 180)))

    # ---- L7 边界收口：土边 + 受光更亮的浅色草边 ----
    # 草坪硬切边会显得像贴上去的一块。收口做法：最外一条土色边（草坪像被围在花圃里），
    # 内侧一圈更亮更暖的草（边缘草受光多），再往外才是场景。
    d_edge = np.minimum(np.minimum(xx, yy), np.minimum(LAWN_W - 1 - xx, LAWN_H - 1 - yy))
    earth = np.clip((9.0 - d_edge) / 9.0, 0, 1)[:, :, None]
    base = base * (1.0 - earth) + np.array([104, 78, 50], np.float32) * earth

    fringe = (np.clip((46.0 - d_edge) / 46.0, 0, 1) *
              np.clip((d_edge - 6.0) / 12.0, 0, 1))
    base *= (1.0 + np.dstack([0.175 * fringe, 0.158 * fringe, 0.090 * fringe]))

    # 房屋方向的投影（左侧更暗）—— 替代原来四边均匀的 AO，
    # 因为有了草边收口后，四边再压暗会显得整块草坪是凹下去的
    base *= (0.845 + 0.155 * np.clip(xx / 130.0, 0, 1))[:, :, None]

    # 沿边界点缀鹅卵石（图集按栅格顺序切出，索引 1 就是石子）
    if len(decals) > 1:
        peb = decals[1]
        for along in range(26, LAWN_W - 26, 92):          # 上下边
            for py in (7.0, LAWN_H - 8.0):
                if rng.random() < 0.55:
                    shade = float(rng.uniform(0.92, 1.06))
                    rgb = np.clip(peb[:, :, :3].astype(np.float32) * shade, 0, 255)
                    paste_rgba(base, rgb, peb[:, :, 3].astype(np.float32) * 0.9,
                               along + float(rng.uniform(-15, 15)),
                               py + float(rng.uniform(-3, 3)),
                               float(rng.uniform(0.10, 0.16)),
                               float(rng.uniform(-40, 40)))
        for along in range(64, LAWN_H - 64, 92):          # 左右边
            for px in (7.0, LAWN_W - 8.0):
                if rng.random() < 0.55:
                    shade = float(rng.uniform(0.92, 1.06))
                    rgb = np.clip(peb[:, :, :3].astype(np.float32) * shade, 0, 255)
                    paste_rgba(base, rgb, peb[:, :, 3].astype(np.float32) * 0.9,
                               px + float(rng.uniform(-3, 3)),
                               along + float(rng.uniform(-15, 15)),
                               float(rng.uniform(0.10, 0.16)),
                               float(rng.uniform(-40, 40)))

    # ---- L8 暗角 ----
    nx = (xx - LAWN_W * 0.5) / (LAWN_W * 0.5)
    ny = (yy - LAWN_H * 0.5) / (LAWN_H * 0.5)
    base *= (1.0 - 0.055 * np.clip(nx * nx + ny * ny, 0, 1.35))[:, :, None]

    # ---- L9 调色：略微提饱和提亮，抵消上面各层的压暗 ----
    img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8))
    img = Image.fromarray(grade_env(img, target_contrast=64.0, target_lum=104.0))
    img = ImageEnhance.Color(img).enhance(1.06)
    img = ImageEnhance.Brightness(img).enhance(1.12)
    img = ImageEnhance.Contrast(img).enhance(1.03)
    out = np.dstack([np.array(img).astype(np.float32),
                     np.full((LAWN_H, LAWN_W), 255.0)])
    save_bmp32(os.path.join(OUT, 'lawn.bmp'), out)
    return LAWN_W, LAWN_H


def build_background(path):
    """场景背景图：抹掉右下角水印 -> 缩放到 2000x1300（2 倍逻辑），不透明"""
    img = Image.open(path).convert('RGB')
    w, h = img.size
    bw, bh = int(w * 0.075), int(h * 0.075)
    img.paste(img.crop((w - bw, h - bh * 2, w, h - bh)), (w - bw, h - bh))
    img = img.resize((BG_W, BG_H), Image.LANCZOS)
    out = np.dstack([np.array(img).astype(np.float32),
                     np.full((BG_H, BG_W), 255.0)])
    save_bmp32(os.path.join(OUT, 'background.bmp'), out)
    return BG_W, BG_H

def grade_env(img_arr, target_contrast=62.0, target_lum=None, pull=0.70):
    """环境（草坪/背景）的色调统一。

    环境的问题和角色不同：草坪平均对比只有 23~31（角色 130~200），
    导致角色像"贴"在地上。这里只提对比、并按需拉亮度，
    不碰色相 —— 夜间/沙漠的色相是关卡设计，不能被拉平。
    """
    a = np.array(img_arr).astype(np.float32)
    lum = 0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]
    cur_ctr = float(np.percentile(lum, 95) - np.percentile(lum, 5))
    cur_lum = float(lum.mean())
    kc = 1.0 + pull * (target_contrast / max(cur_ctr, 1.0) - 1.0)
    kc = min(max(kc, 0.80), 3.20)
    # 关键：对比必须围绕「图自身的均值」缩放，不能围绕 128 中灰。
    # 夜间草坪均值只有 9，围绕 128 缩放会把整张图推到负数再被 clip 成纯黑
    # （实测踩过：夜间草坪亮度 9 -> 0.2，整张全黑）。
    out = (a - cur_lum) * kc + cur_lum
    if target_lum:
        kl = 1.0 + pull * (target_lum / max(cur_lum, 1.0) - 1.0)
        out *= min(max(kl, 0.60), 1.70)
    return np.clip(out, 0, 255).astype(np.uint8)


def build_variant_lawn(path, name, brightness):
    """夜间 / 水中草坪：单张图直接缩放到 LAWN_W x LAWN_H，不烘焙棋盘格（运行时按模式判断）"""
    img = Image.open(path).convert('RGB')
    img = strip_watermark(img)                  # 抹掉右下角「AI 生成」水印
    img = img.resize((LAWN_W, LAWN_H), Image.LANCZOS)
    # 顺序很重要：先压暗再提对比。反过来的话，压暗会把刚提上去的对比又压回原样
    # （夜间草坪第一版就是栽在这：提完对比再 ×0.45，对比从 58 掉回 24）
    if brightness != 1.0:
        from PIL import ImageEnhance
        img = ImageEnhance.Brightness(img).enhance(brightness)
    img = Image.fromarray(grade_env(img, target_contrast=56.0, pull=0.85))
    a = np.array(img).astype(np.float32)
    out = np.dstack([a, np.full(a.shape[:2], 255.0)])
    save_bmp32(os.path.join(OUT, name + '.bmp'), out)
    return LAWN_W, LAWN_H


def main():
    sheet_cells = []
    # background / lawn_night / lawn_water 由专用函数处理，其余全部走通用抠图导出。
    # 注意：这里只能列"确切的 key"，绝不能用 startswith 前缀过滤 ——
    # 之前用 'plant_' / 'relic_' 前缀排除，把新增的 11 张武器/形态/货币资源全给跳过了。
    skip = {'grass', 'grass_old', 'decals', 'background', 'lawn_night', 'lawn_water',
            'lawn_roof', 'lawn_desert'}
    raw = {name: find_raw(key, required=False) for key, name in MAPPING}
    raw = {k:v for k,v in raw.items() if v}
    print('%-20s %-11s %s' % ('输出资源', '尺寸', '来源'))
    print('-' * 74)

    # 草坪（内部消化 grass + decals 两张图）
    w, h = build_lawn(raw['grass'], raw.get('decals'))
    print('%-20s %-11s %s + %s' % ('lawn.bmp', '%dx%d' % (w, h),
                                   os.path.basename(raw['grass']),
                                   os.path.basename(raw.get('decals', '-'))))
    # 场景背景
    w, h = build_background(raw['background'])
    print('%-20s %-11s %s' % ('background.bmp', '%dx%d' % (w, h),
                              os.path.basename(raw['background'])))
    # 关卡草坪变体
    for vname, vkey, factor in [('lawn_night', 'lawn_night', 0.62),
                                 ('lawn_water', 'lawn_water', 1.0),
                                 ('lawn_roof',  'lawn_roof', 1.0),
                                 ('lawn_desert','lawn_desert', 1.0)]:
        if vkey in raw:
            w, h = build_variant_lawn(raw[vkey], vname, factor)
            print('%-20s %-11s %s' % ('%s.bmp' % vname, '%dx%d' % (w, h),
                                       os.path.basename(raw[vkey])))

    for _, name in MAPPING:
        if name in skip:
            continue
        src = raw.get(name)
        if not src:
            print('skip missing', name)
            continue
        # 缺尺寸只跳过这一项，不能让它 KeyError 掉整条构建
        # （曾经因为漏登记 20 株植物的 TARGET_H，构建在 starfruit 处中断，
        #   后面所有子弹资源全部没产出，而且日志被缓冲成空文件，很难发现）
        if name not in TARGET_H:
            print('skip no-size', name)
            continue
        is_bullet = name.startswith('bullet_')
        if name == 'sun':
            rgba = cutout_sun(src)
        else:
            rgba = cutout(src, small=is_bullet)
        rgba = trim(rgba)
        rgba = fit_bottom(rgba, TARGET_H[name])
        # 角色走 grade() 统一基准；子弹/发光体颜色就是识别度，禁止拉灰
        if name not in NO_GRADE:
            rgba = grade(rgba, name, log=True)
        # 行走姿态额外派生一套「绕脚底旋转」的倒地帧（零生成成本，与行走同源）
        if name.startswith('zombie_') and not name.endswith('_eat'):
            make_fall_frames(rgba, name)
        rgba = premultiply(rgba)
        w, h = save_bmp32(os.path.join(OUT, name + '.bmp'), rgba)
        print('%-20s %-11s %s' % (name + '.bmp', '%dx%d' % (w, h), os.path.basename(src)))
        sheet_cells.append((name, rgba))

    # 拼版（棋盘底，方便看边缘有没有残留光晕）
    cell = 210
    cols = 5
    rows = (len(sheet_cells) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * cell, rows * cell), (60, 60, 66))
    for i, (name, rgba) in enumerate(sheet_cells):
        im = Image.fromarray(rgba.astype(np.uint8), 'RGBA')
        im.thumbnail((cell - 16, cell - 16), Image.LANCZOS)
        cx, cy = (i % cols) * cell, (i // cols) * cell
        for yy in range(0, cell, 16):
            for xx in range(0, cell, 16):
                if ((xx // 16) + (yy // 16)) & 1:
                    d = Image.new('RGB', (min(16, cell - xx), min(16, cell - yy)), (110, 110, 116))
                    sheet.paste(d, (cx + xx, cy + yy))
        sheet.paste(im, (cx + (cell - im.width) // 2, cy + (cell - im.height) // 2), im)
    sheet.save(os.path.join(HERE, 'contact_sheet.png'))
    print('\n拼版已输出: art/contact_sheet.png')


if __name__ == '__main__':
    main()
