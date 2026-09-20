# -*- coding: utf-8 -*-
"""生成 5 种机制子弹的对照图（contact sheet）。
本环境无法直接查看 32 位 alpha BMP，且 PIL 读 BMP 会丢 alpha，
因此这里手写 BMP 头解析 + 反预乘，再用 PIL 合成到草坪底色上。
输出：art/preview_mechanism_bullets.png
"""
import struct, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, '..', 'assets')

# 与 pvz.c 中的绘制参数保持一致
LAWN = (96, 132, 43)
BULLETS = [
    # (文件, 显示名, 机制, 游戏内缩放, 拖尾色 WPN_COL)
    ('bullet_pierce.bmp', 'pierce', '穿刺 · 多段穿透', 0.62, (116, 188, 246)),
    ('bullet_ice.bmp',    'ice',    '冰冻 · 减速',     0.62, (150, 220, 255)),
    ('bullet_kernel.bmp', 'kernel', '击退 · 冲击波',   0.62, (255, 214, 120)),
    ('bullet_cob.bmp',    'cob',    '爆炸 · 大威力',   0.62, (255, 150, 60)),
    ('bullet_doom.bmp',   'doom',   '追踪 · 跨行',     0.85, (230, 120, 210)),
]


def load_bgra32(path):
    """手写解析 32 位 BMP，返回非预乘 RGBA 数组（顶行优先）。"""
    b = open(path, 'rb').read()
    off = struct.unpack_from('<I', b, 10)[0]
    w, h = struct.unpack_from('<ii', b, 18)
    bits = struct.unpack_from('<H', b, 28)[0]
    assert bits == 32, '%s bits=%d' % (path, bits)
    px = np.frombuffer(b, np.uint8, count=w * abs(h) * 4, offset=off)
    px = px.reshape(abs(h), w, 4)          # BGRA
    if h > 0:                              # 正高度 = 自底向上，翻转
        px = px[::-1]
    bgr = px[:, :, :3].astype(np.float32)
    a = px[:, :, 3].astype(np.float32)
    # 反预乘：存储值 = 原值 * alpha/255  =>  原值 = 存储值 * 255/alpha
    scale = np.where(a > 0, 255.0 / np.maximum(a, 1.0), 0.0)[:, :, None]
    bgr = np.clip(bgr * scale, 0, 255)
    # 关键：BMP 存的是 BGRA，必须换回 RGBA，否则画面里红蓝互换
    # （曾经因此把青色的冰晶看成金色、橘色的爆炸看成蓝色）
    rgb = bgr[:, :, ::-1]
    out = np.concatenate([rgb, a[:, :, None]], axis=2).astype(np.uint8)
    return out


def composite(bullet_rgba, scale, canvas, ox, oy, trail=None):
    """把子弹按 scale 缩放到 canvas（lawn 底），可选在左侧画一条拖尾。"""
    h, w = bullet_rgba.shape[:2]
    nw, nh = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
    img = Image.fromarray(bullet_rgba, 'RGBA').resize((nw, nh), Image.NEAREST)
    arr = np.array(img).astype(np.float32)

    if trail is not None:
        # 拖尾：从弹体左侧延伸，模拟 WPN_COL 染色椭圆
        tw, th = max(6, nw), max(4, int(nh * 0.30))
        for i in range(tw):
            t = i / max(1, tw - 1)
            a = int(150 * (1 - t) ** 1.5)
            if a <= 0:
                continue
            x0 = ox - (tw - i)
            y0 = oy + (nh - th) // 2
            for yy in range(y0, y0 + th):
                for xx in range(x0, x0 + 1):
                    if 0 <= xx < canvas.shape[1] and 0 <= yy < canvas.shape[0]:
                        base = canvas[yy, xx].astype(np.float32)
                        col = np.array(trail, np.float32)
                        canvas[yy, xx] = (base * (1 - a / 255.0) + col * (a / 255.0)).astype(np.uint8)

    # alpha 合成
    for yy in range(nh):
        for xx in range(nw):
            a = arr[yy, xx, 3] / 255.0
            if a <= 0:
                continue
            ty, tx = oy + yy, ox + xx
            if not (0 <= ty < canvas.shape[0] and 0 <= tx < canvas.shape[1]):
                continue
            base = canvas[ty, tx].astype(np.float32)
            src = arr[yy, xx, :3]
            canvas[ty, tx] = (base * (1 - a) + src * a).astype(np.uint8)


def main():
    row_h = 132
    top = 76
    W = 1140
    H = top + row_h * len(BULLETS) + 40
    canvas = np.zeros((H, W, 3), np.uint8)
    # 草坪底 + 明显格纹，方便看对比度
    canvas[:, :] = LAWN
    for gy in range(0, H, 44):
        canvas[gy:gy + 22, :] = (np.array(LAWN, np.float32) * 0.93).astype(np.uint8)
    for gx in range(0, W, 44):
        canvas[:, gx:gx + 22] = (canvas[:, gx:gx + 22].astype(np.float32) * 0.96).astype(np.uint8)

    try:
        f_big = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 26)
        f_mid = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 19)
        f_sm = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 16)
    except Exception:
        f_big = f_mid = f_sm = ImageFont.load_default()

    img = Image.fromarray(canvas, 'RGB')
    canvas = np.array(img)

    labels = []   # (x, y, text, font, color)
    for i, (fn, name, mech, scale, trail) in enumerate(BULLETS):
        y = top + i * row_h
        path = os.path.join(ASSETS, fn)
        rgba = load_bgra32(path)
        bh, bw = rgba.shape[:2]

        # 行分隔线
        canvas[y - 6:y - 4, 16:W - 16] = (255, 255, 255)

        # 左：真实尺寸 + 拖尾
        composite(rgba, scale, canvas, 150, y + (row_h - int(bh * scale)) // 2 - 8, trail=trail)

        # 中：4x 放大（贴地）
        composite(rgba, 4.0, canvas, 400, y + 12)

        # 右：验收数值
        solid = rgba[rgba[:, :, 3] > 200]
        cov = 100.0 * (rgba[:, :, 3] > 16).mean()
        lum = (0.299 * solid[:, 0] + 0.587 * solid[:, 1] + 0.114 * solid[:, 2]) if len(solid) else np.array([0.0])
        dk = 100.0 * (lum < 60).mean() if len(solid) else 0.0
        body = solid[:, :3].mean(axis=0) if len(solid) else np.zeros(3)
        col = tuple(int(min(255, v * 1.6)) for v in trail)
        labels.append((150, y + row_h - 34, '%s — %s' % (name, mech), f_mid, col))
        labels.append((640, y + 24, '%s.bmp   %dx%d  32bpp' % (name, bw, bh), f_sm, (255, 255, 255)))
        labels.append((640, y + 50, '覆盖率 %.1f%%  实心亮度 p50=%.0f  暗像素 %.0f%%' % (cov, np.percentile(lum, 50), dk),
                       f_sm, (255, 255, 255)))
        labels.append((640, y + 76, '主体 RGB [%d, %d, %d]' % tuple(int(v) for v in body), f_sm, (255, 255, 255)))

    # 所有合成完毕后再统一写字，避免写到已过期的小图上
    img = Image.fromarray(canvas, 'RGB')
    d = ImageDraw.Draw(img)
    d.text((24, 16), '机制子弹美术对照（mmx 生成 → 抠图 → 32 位 alpha BMP）',
           font=f_big, fill=(255, 255, 255))
    d.text((24, 50), '左：游戏内真实尺寸（含拖尾）    中：4x 放大看形体    右：验收数值',
           font=f_sm, fill=(232, 240, 220))
    for (x, yy, txt, fnt, cl) in labels:
        d.text((x, yy), txt, font=fnt, fill=cl)

    out = os.path.join(HERE, 'preview_mechanism_bullets.png')
    img.save(out)
    print('OK ->', out, img.size)


main()
