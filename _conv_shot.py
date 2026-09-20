# -*- coding: utf-8 -*-
from PIL import Image
import os
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_frames_loadout1")
for tag in ("11", "10", "01"):
    p = os.path.join(D, "st%s.bmp" % tag)
    im = Image.open(p).convert("RGB")
    W, H = im.size
    print("st%s.bmp %dx%d" % (tag, W, H))
    im.resize((W // 2, H // 2), Image.LANCZOS).save(os.path.join(D, "p%s_full.png" % tag))
    if tag in ("11", "10"):
        im.crop((0, int(H * 0.86), W, int(H * 0.99))).save(os.path.join(D, "p%s_text.png" % tag))
    else:
        im.crop((0, 0, int(W * 0.72), int(H * 0.30))).save(os.path.join(D, "p%s_top.png" % tag))
print("ok")
