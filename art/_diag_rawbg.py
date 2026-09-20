# -*- coding: utf-8 -*-
"""直接看原始图最下面几行的像素，判断"底部横条"是画出来的还是抠图伪影。"""
import os, sys, numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, 'raw')
for p in sys.argv[1:]:
    if not os.path.isabs(p): p = os.path.join(RAW, p)
    a = np.array(Image.open(p).convert('RGB'))
    h, w = a.shape[:2]
    print('== %s  %dx%d' % (os.path.basename(p), w, h))
    # 每行：整行均值 + 该行"非背景(偏离首行均值>40)"的像素个数
    ref = a[2:6].reshape(-1, 3).mean(axis=0)
    for y in list(range(0, 12)) + list(range(h - 14, h)):
        row = a[y]
        dev = np.abs(row.astype(np.int32) - ref).max(axis=1)
        n = int((dev > 40).sum())
        print('   y=%4d  均值=%s  偏离背景>40的像素=%4d (%.0f%%)' % (
            y, np.round(row.mean(axis=0)).astype(int), n, 100.0 * n / w))
