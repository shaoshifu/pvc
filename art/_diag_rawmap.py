# -*- coding: utf-8 -*-
"""把原图的"离背景距离"渲染成 ASCII 图 —— 没有视觉输入时用这个"看"画了什么。
字符越靠后 = 越偏离纯背景（越可能是实体）；空白 = 背景。
"""
import os, sys, numpy as np
from PIL import Image
RAMPS = ' .:-=+*#%@'
def show(p, cols=64):
    if not os.path.isabs(p): p = os.path.join('raw', p)
    a = np.array(Image.open(p).convert('RGB')).astype(np.int32)
    h, w = a.shape[:2]
    bg = np.median(a[:8].reshape(-1, 3), axis=0)
    dev = np.abs(a - bg).max(axis=2).astype(np.float32)
    rows = max(1, int(cols * h / w * 0.5))
    print('== %s  背景=%s' % (os.path.basename(p), bg.astype(int)))
    for r in range(rows):
        line = ''
        for c in range(cols):
            y0, y1 = int(r*h/rows), max(int((r+1)*h/rows), int(r*h/rows)+1)
            x0, x1 = int(c*w/cols), max(int((c+1)*w/cols), int(c*w/cols)+1)
            v = dev[y0:y1, x0:x1].max() / 255.0
            line += RAMPS[min(len(RAMPS)-1, int(v*len(RAMPS)))]
        print('  |'+line+'|')
for p in sys.argv[1:]: show(p)
