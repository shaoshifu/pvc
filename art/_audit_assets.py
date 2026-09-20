# -*- coding: utf-8 -*-
"""
资源审计：把 assets/ 下的植物/僵尸 BMP 拼成总览图，并输出统计，
用来一次性发现「空图 / 单色 / 全透明 / 尺寸异常」的问题资源。

用法：C:/Python314/python.exe art/_audit_assets.py
"""
import os, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, '..', 'assets')

PLANTS = [
    "sunflower", "peashooter", "wallnut", "potatomine",
    "snowpea", "repeater", "cherrybomb", "jalapeno",
    "plant_threepeater", "plant_spikeweed", "plant_magnet", "plant_kernelpult",
    "starfruit", "cactus", "splitpea", "lightningreed", "bloomerang",
    "fume", "laserbean", "melonpult", "wintermelon", "gloom",
    "goldmagnet", "twinflower", "marrow", "pumpkin", "garlic",
    "hypnoshroom", "iceshroom", "tanglekelp", "cobcannon", "cattail",
    "hero_flame", "hero_ironnut", "hero_crystal", "hero_thornvine",
    "hero_sungod", "hero_frost", "hero_worldtree", "hero_doom",
]
ZOMBIES = [
    "zombie_normal", "zombie_cone", "zombie_bucket", "zombie_flag",
    "zombie_dancer", "zombie_zamboni", "zombie_balloon",
    "zombie_vaulter", "zombie_screendoor", "zombie_football",
    "zombie_newspaper", "zombie_digger", "zombie_giant",
]


def checker(w, h, s=12):
    """灰白棋盘格背景（用于看 alpha）"""
    img = Image.new('RGBA', (w, h), (255, 255, 255, 255))
    px = img.load()
    for y in range(h):
        for x in range(w):
            if ((x // s) + (y // s)) % 2:
                px[x, y] = (208, 208, 208, 255)
    return img


def load(name):
    p = os.path.join(ASSETS, name + '.bmp')
    if not os.path.exists(p):
        return None, None
    im = Image.open(p).convert('RGBA')
    return im, p


def stats(im):
    """返回 (alpha覆盖率, 平均亮度, 包围盒)"""
    a = im.getchannel('A')
    bbox = a.getbbox()
    hist = a.histogram()
    total = im.width * im.height
    opaque = sum(hist[16:])           # alpha > 16 视为有效
    rgb = im.convert('RGB')
    # 只统计有效区域亮度
    if bbox:
        crop = rgb.crop(bbox)
        gray = crop.convert('L')
        h2 = gray.histogram()
        n = sum(h2)
        avg = sum(i * c for i, c in enumerate(h2)) / max(1, n)
    else:
        avg = 0
    return opaque / total, avg, bbox


def sheet(names, out, cols=8, cell=150, label=True):
    rows = (len(names) + cols - 1) // cols
    pad, top = 8, 20
    W = cols * (cell + pad) + pad
    H = rows * (cell + pad + top) + pad
    canvas = Image.new('RGBA', (W, H), (250, 250, 250, 255))

    from PIL import ImageDraw, ImageFont
    d = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 13)
    except Exception:
        font = ImageFont.load_default()

    report = []
    for i, name in enumerate(names):
        r, c = divmod(i, cols)
        x0 = pad + c * (cell + pad)
        y0 = pad + r * (cell + pad + top) + top

        im, path = load(name)
        if im is None:
            report.append((name, 'MISSING', '', '', ''))
            d.rectangle([x0, y0, x0 + cell, y0 + cell], fill=(255, 220, 220, 255))
            d.text((x0 + 4, y0 + 4), name + ' MISSING', fill=(180, 0, 0, 255), font=font)
            continue

        cov, avg, bbox = stats(im)
        flag = ''
        if cov < 0.02:
            flag = 'EMPTY'
        elif avg > 250 or avg < 12:
            flag = 'FLAT?'
        report.append((name, f'{im.width}x{im.height}', f'{cov*100:.1f}%',
                       f'{avg:.0f}', flag))

        bg = checker(cell, cell)
        sc = min(cell / max(1, im.width), cell / max(1, im.height))
        tw, th = max(1, int(im.width * sc)), max(1, int(im.height * sc))
        thumb = im.resize((tw, th), Image.LANCZOS)
        bg.alpha_composite(thumb, ((cell - tw) // 2, (cell - th) // 2))
        canvas.alpha_composite(bg, (x0, y0))
        d.rectangle([x0, y0, x0 + cell, y0 + cell], outline=(180, 180, 180, 255))
        d.text((x0 + 3, y0 - 17), name, fill=(20, 20, 20, 255), font=font)

    canvas.convert('RGB').save(out, quality=92)
    return report


def main():
    print('=' * 78)
    print('植物资源审计 (%d 个)' % len(PLANTS))
    print('=' * 78)
    rep = sheet(PLANTS, os.path.join(HERE, 'audit_plants.png'))
    print('%-22s %-12s %-9s %-7s %s' % ('资源名', '尺寸', 'alpha', '亮度', '标记'))
    print('-' * 78)
    bad = 0
    for name, size, cov, avg, flag in rep:
        if flag:
            bad += 1
        print('%-22s %-12s %-9s %-7s %s' % (name, size, cov, avg, flag))
    print('-' * 78)
    print('问题资源: %d / %d' % (bad, len(PLANTS)))

    print()
    print('=' * 78)
    print('僵尸资源审计 (%d 个)' % len(ZOMBIES))
    print('=' * 78)
    rep2 = sheet(ZOMBIES, os.path.join(HERE, 'audit_zombies.png'), cols=7)
    print('%-22s %-12s %-9s %-7s %s' % ('资源名', '尺寸', 'alpha', '亮度', '标记'))
    print('-' * 78)
    bad2 = 0
    for name, size, cov, avg, flag in rep2:
        if flag:
            bad2 += 1
        print('%-22s %-12s %-9s %-7s %s' % (name, size, cov, avg, flag))
    print('-' * 78)
    print('问题资源: %d / %d' % (bad2, len(ZOMBIES)))
    print()
    print('总览图: art/audit_plants.png, art/audit_zombies.png')


if __name__ == '__main__':
    main()
