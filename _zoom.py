# -*- coding: utf-8 -*-
"""把自导真帧的缺陷区域裁剪 + 放大，用于人眼复核。"""
import os
import sys
from PIL import Image, ImageDraw

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
FR = os.path.join(D, "_frames2")

# (标签, 帧文件, (x0,y0,x1,y1), 放大倍数)
JOBS = [
    ("A_ST_DRAFT_tooltip",  "st05.bmp", (20, 80, 360, 245), 3),
    ("B_ST_LOADOUT_dup",    "st10.bmp", (250, 68, 760, 112), 3),
    ("C_ST_REWARD_dup",     "st11.bmp", (330, 38, 780, 100), 3),
    ("D_ST_ACH_bleed",      "st08.bmp", (20, 605, 420, 650), 3),
    ("E_ST_LOADOUT_bleed",  "st10.bmp", (300, 605, 700, 650), 3),
    ("F_ST_MENU_ok",        "st00.bmp", (0, 0, 1000, 650), 1),
    ("G_ST_PLAY_ok",        "st01.bmp", (0, 0, 1000, 650), 1),
]

for tag, fn, box, z in JOBS:
    p = os.path.join(FR, fn)
    if not os.path.exists(p):
        print("skip", tag); continue
    im = Image.open(p).convert("RGB")
    c = im.crop(box)
    c = c.resize((c.width * z, c.height * z), Image.NEAREST)
    d = ImageDraw.Draw(c)
    d.rectangle([0, 0, c.width - 1, c.height - 1], outline=(255, 0, 255), width=1)
    out = os.path.join(D, "_zoom_%s.png" % tag)
    c.save(out)
    print("%-24s %s  %dx%d" % (tag, out, c.width, c.height))
