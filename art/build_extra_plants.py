"""Build 30 extra permanent-plant sprites (plant_51..plant_80).

The game loads 32-bit premultiplied-alpha BMPs from assets/.  These sprites are
procedural but hand-directed: every plant gets a distinct silhouette and motif,
so the new mechanics are recognizable before swapping in higher-end AI art.
"""
from __future__ import annotations

import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

from _unitlib import save_bmp32

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets"
CONTACT = Path(__file__).with_name("extra_plants_contact.png")
W, H, S = 160, 210, 3


def premultiply(rgba: np.ndarray) -> np.ndarray:
    a = rgba[:, :, 3:4].astype(np.float32) / 255.0
    out = rgba.copy().astype(np.float32)
    out[:, :, :3] *= a
    return np.clip(out, 0, 255).astype(np.uint8)


def shade(c, k):
    return tuple(max(0, min(255, int(v + (255 - v) * k if k >= 0 else v * (1 + k)))) for v in c)


def rgba(c, a=255):
    return (int(c[0]), int(c[1]), int(c[2]), int(a))


def sc(v):
    return int(round(v * S))


def box(b):
    x0, y0, x1, y1 = b
    if x1 < x0:
        x0, x1 = x1, x0
    if y1 < y0:
        y0, y1 = y1, y0
    return tuple(sc(v) for v in (x0, y0, x1, y1))


def line(d, pts, c, w=2):
    d.line([(sc(x), sc(y)) for x, y in pts], fill=rgba(c), width=sc(w), joint="curve")


def ellipse(d, b, c, outline=None, width=2):
    d.ellipse(box(b), fill=rgba(c), outline=rgba(outline) if outline else None, width=sc(width))


def rect(d, b, c, outline=None, width=2, r=0):
    if r:
        d.rounded_rectangle(box(b), radius=sc(r), fill=rgba(c),
                            outline=rgba(outline) if outline else None, width=sc(width))
    else:
        d.rectangle(box(b), fill=rgba(c), outline=rgba(outline) if outline else None, width=sc(width))


def poly(d, pts, c, outline=None):
    d.polygon([(sc(x), sc(y)) for x, y in pts], fill=rgba(c), outline=rgba(outline) if outline else None)


def star(d, cx, cy, r1, r2, n, c, outline=None, rot=-math.pi / 2):
    pts = []
    for i in range(n * 2):
        a = rot + math.pi * i / n
        r = r1 if i % 2 == 0 else r2
        pts.append((cx + math.cos(a) * r, cy + math.sin(a) * r))
    poly(d, pts, c, outline)


def glow_layer(base, draw_fn, blur=7, alpha=130):
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    draw_fn(d, alpha)
    base.alpha_composite(layer.filter(ImageFilter.GaussianBlur(sc(blur))))


SPECS = [
    ("泡泡莲", "bubble", (78, 181, 152), (160, 230, 235)),
    ("回声竹", "echo", (70, 175, 86), (230, 218, 92)),
    ("梦境菇", "dream", (112, 82, 184), (224, 150, 238)),
    ("孢子乐团", "spore", (88, 148, 95), (235, 190, 92)),
    ("符文仙人掌", "rune", (64, 168, 86), (94, 222, 158)),
    ("时钟芽", "clock", (102, 180, 95), (240, 202, 98)),
    ("镜蕨", "mirror", (66, 166, 130), (160, 238, 222)),
    ("彗星草", "comet", (74, 150, 216), (255, 210, 88)),
    ("蜜滴花", "honey", (226, 172, 58), (255, 236, 120)),
    ("特斯拉南瓜", "tesla", (226, 128, 56), (92, 236, 255)),
    ("泡泡炮", "cannon", (80, 168, 180), (190, 245, 255)),
    ("声波花", "sonic", (92, 172, 224), (236, 228, 120)),
    ("星云甜菜", "nebula", (118, 72, 178), (86, 228, 220)),
    ("灯笼瓜", "lantern", (234, 150, 58), (255, 226, 96)),
    ("珊瑚卫士", "coral", (238, 118, 110), (122, 220, 202)),
    ("齿轮花", "gear", (112, 154, 94), (205, 192, 154)),
    ("蜗牛菇", "snail", (120, 145, 102), (198, 222, 128)),
    ("棱镜柳", "prism", (92, 174, 154), (180, 244, 255)),
    ("纸鹤草", "crane", (120, 190, 120), (246, 246, 232)),
    ("季风芦苇", "monsoon", (72, 156, 170), (132, 218, 238)),
    ("烛芯芽", "candle", (214, 112, 62), (255, 220, 112)),
    ("石英玉米", "quartz", (210, 176, 72), (188, 238, 255)),
    ("铃兰钟", "bell", (112, 184, 116), (238, 226, 138)),
    ("涡旋萝卜", "vortex", (154, 102, 198), (118, 228, 246)),
    ("彩绘笔刷", "paint", (98, 176, 112), (248, 94, 152)),
    ("暮光兰", "dusk", (112, 84, 180), (255, 172, 120)),
    ("蒸汽辣椒", "steam", (220, 82, 58), (218, 234, 230)),
    ("轨道苔藓", "orbital", (82, 154, 114), (180, 220, 255)),
    ("催眠洋葱", "lullaby", (176, 126, 206), (236, 202, 142)),
    ("极光豌豆", "aurora", (78, 168, 220), (172, 255, 214)),
]


def draw_base(d, rnd, main, accent, kind):
    # ground shadow
    ellipse(d, (42, 174, 118, 196), (32, 55, 42), None)
    # stems and leaves
    line(d, [(80, 178), (76 + rnd.uniform(-5, 5), 132), (80 + rnd.uniform(-6, 6), 92)], shade(main, -0.28), 8)
    for side in (-1, 1):
        ellipse(d, (80 + side * 6, 134 + side * 3, 80 + side * 46, 162 + side * 3),
                shade(main, -0.06), shade(main, -0.42), 2)
    # body
    ellipse(d, (48, 58, 112, 130), main, shade(main, -0.42), 3)
    ellipse(d, (58, 66, 102, 111), shade(main, 0.22), None)
    ellipse(d, (68, 76, 92, 101), shade(accent, 0.06), None)

    # cute face keeps all units in the same PVZ-like family
    ellipse(d, (66, 94, 74, 104), (34, 42, 30), None)
    ellipse(d, (88, 94, 96, 104), (34, 42, 30), None)
    ellipse(d, (68, 95, 71, 98), (255, 255, 255), None)
    ellipse(d, (90, 95, 93, 98), (255, 255, 255), None)
    line(d, [(75, 113), (81, 117), (88, 113)], shade(main, -0.48), 2)


def motif(d, kind, main, accent, rnd):
    dark = shade(main, -0.48)
    pale = shade(accent, 0.35)
    if kind == "bubble":
        for x, y, r in [(46, 58, 10), (112, 70, 9), (100, 44, 7), (62, 36, 6)]:
            ellipse(d, (x - r, y - r, x + r, y + r), shade(accent, 0.12), shade(accent, -0.22), 2)
            ellipse(d, (x - r * .35, y - r * .35, x + r * .05, y + r * .05), (255, 255, 255), None)
    elif kind == "echo":
        for k in range(3):
            line(d, [(101 + k * 7, 74 - k * 6), (128 + k * 5, 58 - k * 10)], accent, 3)
        rect(d, (54, 56, 68, 124), shade(main, -0.18), dark, 3, 6)
        line(d, [(61, 61), (61, 118)], shade(accent, 0.05), 2)
    elif kind == "dream":
        ellipse(d, (42, 42, 118, 82), shade(accent, -0.08), dark, 3)
        for x in (58, 80, 102):
            ellipse(d, (x - 6, 52, x + 6, 64), pale, None)
        star(d, 112, 42, 7, 3, 5, pale, None)
    elif kind == "spore":
        for a in range(0, 360, 45):
            x = 80 + math.cos(math.radians(a)) * 42
            y = 92 + math.sin(math.radians(a)) * 34
            ellipse(d, (x - 7, y - 7, x + 7, y + 7), accent, dark, 2)
        line(d, [(46, 126), (114, 126)], shade(accent, -0.12), 5)
    elif kind == "rune":
        for a in range(0, 360, 60):
            x = 80 + math.cos(math.radians(a)) * 35
            y = 92 + math.sin(math.radians(a)) * 35
            line(d, [(80, 92), (x, y)], accent, 2)
        star(d, 80, 54, 13, 6, 6, accent, dark)
    elif kind == "clock":
        ellipse(d, (52, 48, 108, 104), shade(accent, 0.02), dark, 4)
        line(d, [(80, 76), (80, 56)], dark, 4)
        line(d, [(80, 76), (96, 84)], dark, 4)
        ellipse(d, (75, 71, 85, 81), pale, None)
    elif kind == "mirror":
        poly(d, [(80, 42), (108, 76), (80, 112), (52, 76)], shade(accent, 0.05), dark)
        poly(d, [(80, 50), (99, 76), (80, 104), (61, 76)], (210, 255, 242), None)
        line(d, [(66, 88), (96, 58)], (255, 255, 255), 3)
    elif kind == "comet":
        star(d, 94, 58, 19, 7, 5, accent, dark)
        for k in range(4):
            line(d, [(88 - k * 5, 65 + k * 9), (36 - k * 6, 94 + k * 3)], shade(accent, -0.08), 4 - k % 2)
    elif kind == "honey":
        for x, y in [(62, 56), (82, 46), (102, 57), (72, 76), (94, 78)]:
            poly(d, [(x, y - 10), (x + 9, y - 5), (x + 9, y + 5), (x, y + 10), (x - 9, y + 5), (x - 9, y - 5)], accent, dark)
        ellipse(d, (70, 102, 92, 130), shade(accent, 0.18), None)
    elif kind == "tesla":
        rect(d, (45, 68, 115, 135), shade(main, -0.05), dark, 10, 18)
        for x in (62, 80, 98):
            line(d, [(x, 54), (x - 8, 76), (x + 8, 96), (x, 120)], accent, 4)
    elif kind == "cannon":
        rect(d, (74, 66, 124, 98), shade(main, -0.18), dark, 4, 16)
        ellipse(d, (106, 58, 140, 106), shade(accent, -0.08), dark, 4)
        ellipse(d, (116, 68, 134, 96), (32, 68, 76), None)
    elif kind == "sonic":
        ellipse(d, (52, 54, 108, 110), shade(main, 0.08), dark, 3)
        for r in (20, 34, 48):
            d.arc(box((80 - r, 76 - r, 80 + r, 76 + r)), -35, 35, fill=rgba(accent), width=sc(3))
    elif kind == "nebula":
        ellipse(d, (42, 46, 118, 122), shade(main, -0.05), dark, 3)
        for a in (0, 35, -35):
            d.arc(box((36, 54, 124, 116)), 180 + a, 360 + a, fill=rgba(accent), width=sc(4))
        star(d, 82, 78, 7, 3, 5, pale)
    elif kind == "lantern":
        rect(d, (56, 48, 104, 112), shade(accent, 0.06), dark, 8, 18)
        line(d, [(62, 48), (80, 32), (98, 48)], dark, 4)
        ellipse(d, (68, 68, 92, 100), (255, 238, 120), None)
    elif kind == "coral":
        for x in (62, 80, 98):
            line(d, [(x, 128), (x + rnd.choice([-10, 10]), 78), (x + rnd.choice([-18, 18]), 52)], accent, 7)
        for x, y in [(47, 72), (112, 82), (86, 48)]:
            ellipse(d, (x - 8, y - 8, x + 8, y + 8), shade(accent, 0.16), dark, 2)
    elif kind == "gear":
        star(d, 82, 78, 34, 25, 12, accent, dark)
        ellipse(d, (67, 63, 97, 93), (58, 70, 58), None)
    elif kind == "snail":
        ellipse(d, (52, 64, 112, 124), shade(main, 0.04), dark, 3)
        for r in (28, 18, 9):
            d.arc(box((82 - r, 91 - r, 82 + r, 91 + r)), 20, 325, fill=rgba(accent), width=sc(3))
        line(d, [(46, 72), (32, 50)], accent, 2)
        line(d, [(54, 70), (48, 45)], accent, 2)
    elif kind == "prism":
        poly(d, [(80, 36), (114, 78), (80, 124), (46, 78)], (210, 255, 246), dark)
        for a, c in zip([-25, 0, 25], [(255, 120, 120), (120, 220, 255), (255, 230, 120)]):
            line(d, [(80, 80), (126, 80 + a)], c, 3)
    elif kind == "crane":
        poly(d, [(44, 82), (80, 52), (116, 82), (86, 78), (80, 118), (74, 78)], pale, dark)
        poly(d, [(80, 52), (92, 38), (90, 58)], accent, dark)
    elif kind == "monsoon":
        for k in range(4):
            line(d, [(36, 68 + k * 14), (116, 54 + k * 10)], shade(accent, 0.08), 4)
        rect(d, (70, 46, 88, 128), shade(main, -0.1), dark, 3, 9)
    elif kind == "candle":
        rect(d, (60, 54, 100, 122), (245, 218, 142), dark, 4, 12)
        star(d, 80, 43, 19, 8, 5, accent, shade(accent, -0.4))
        ellipse(d, (72, 72, 88, 118), shade(accent, 0.28), None)
    elif kind == "quartz":
        for pts in [[(70, 116), (55, 66), (76, 38), (88, 96)], [(86, 118), (84, 54), (106, 36), (108, 106)], [(58, 122), (38, 82), (54, 58), (74, 112)]]:
            poly(d, pts, shade(accent, 0.08), dark)
    elif kind == "bell":
        rect(d, (58, 58, 102, 112), shade(accent, 0.04), dark, 4, 18)
        ellipse(d, (52, 100, 108, 126), shade(accent, -0.04), dark, 3)
        ellipse(d, (74, 114, 86, 126), dark, None)
        line(d, [(80, 44), (80, 58)], dark, 4)
    elif kind == "vortex":
        ellipse(d, (42, 46, 118, 124), shade(main, -0.03), dark, 3)
        for r in (40, 30, 20, 10):
            d.arc(box((80 - r, 86 - r, 80 + r, 86 + r)), 25 + r, 300 + r, fill=rgba(accent), width=sc(4))
    elif kind == "paint":
        line(d, [(54, 122), (102, 50)], (100, 58, 30), 8)
        ellipse(d, (94, 40, 122, 68), accent, dark, 3)
        for x, c in [(56, (255, 78, 120)), (76, (255, 220, 78)), (98, (78, 200, 255))]:
            ellipse(d, (x - 8, 120, x + 8, 136), c, None)
    elif kind == "dusk":
        for a in range(0, 360, 60):
            x = 80 + math.cos(math.radians(a)) * 30
            y = 78 + math.sin(math.radians(a)) * 22
            ellipse(d, (x - 15, y - 10, x + 15, y + 10), shade(accent, -0.04), dark, 2)
        ellipse(d, (64, 62, 96, 96), shade(main, 0.16), None)
    elif kind == "steam":
        rect(d, (56, 58, 104, 126), main, dark, 4, 24)
        star(d, 80, 46, 18, 7, 5, accent, shade(accent, -0.42))
        for x in (50, 80, 110):
            d.arc(box((x - 12, 28, x + 14, 72)), 80, 260, fill=rgba(pale), width=sc(3))
    elif kind == "orbital":
        ellipse(d, (52, 70, 108, 126), shade(main, -0.02), dark, 3)
        d.arc(box((36, 38, 124, 126)), 200, 520, fill=rgba(accent), width=sc(4))
        star(d, 116, 48, 10, 4, 5, pale)
    elif kind == "lullaby":
        rect(d, (54, 50, 106, 126), main, dark, 6, 24)
        for k in range(3):
            line(d, [(106 + k * 7, 60 - k * 7), (122 + k * 8, 42 - k * 9)], accent, 3)
        ellipse(d, (66, 76, 94, 104), shade(accent, 0.10), None)
    elif kind == "aurora":
        ellipse(d, (50, 62, 112, 120), main, dark, 3)
        for k, c in enumerate([(130, 255, 230), (120, 180, 255), (230, 140, 255)]):
            line(d, [(46, 52 + k * 11), (120, 38 + k * 17)], c, 4)
        ellipse(d, (92, 78, 122, 100), shade(accent, 0.08), dark, 3)


def draw_sprite(idx, spec):
    name, kind, main, accent = spec
    rnd = random.Random(4300 + idx)
    im = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    glow_layer(im, lambda gd, a: ellipse(gd, (43, 40, 117, 138), accent, None), 9, 120)
    d = ImageDraw.Draw(im)
    draw_base(d, rnd, main, accent, kind)
    motif(d, kind, main, accent, rnd)
    # Final crisp outline accents.
    d.ellipse(box((48, 58, 112, 130)), fill=None, outline=rgba(shade(main, -0.55)), width=sc(2))
    im = im.resize((W, H), Image.Resampling.LANCZOS)
    arr = np.asarray(im, dtype=np.uint8)
    save_bmp32(premultiply(arr), str(OUT / f"plant_{idx:02d}.bmp"))
    return im, name


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    thumbs = []
    for offset, spec in enumerate(SPECS, start=51):
        thumbs.append(draw_sprite(offset, spec))
    sheet_w, sheet_h = 6 * 150, 5 * 190
    sheet = Image.new("RGBA", (sheet_w, sheet_h), (20, 32, 24, 255))
    for i, (im, name) in enumerate(thumbs):
        x = (i % 6) * 150 + 18
        y = (i // 6) * 190 + 8
        sheet.alpha_composite(im, (x, y))
    sheet.save(CONTACT)
    print(f"built {len(thumbs)} sprites: plant_51.bmp..plant_80.bmp")
    print(CONTACT)


if __name__ == "__main__":
    main()
