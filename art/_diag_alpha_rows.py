# -*- coding: utf-8 -*-
"""逐行看 cutout 输出的 alpha，定位"底部横条"从哪来。"""
import os, sys, importlib.util, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ba', os.path.join(HERE, 'build_assets.py'))
ba = importlib.util.module_from_spec(spec); spec.loader.exec_module(ba)
p = os.path.join(ba.RAW, sys.argv[1])
r = ba.cutout(p, small=True)
a = r[:, :, 3]
h, w = a.shape
print('抠图输出 %dx%d' % (w, h))
ys, xs = np.where(a > 16)
print('内容包围盒 x:%d~%d  y:%d~%d' % (xs.min(), xs.max(), ys.min(), ys.max()))
print('每行 alpha>16 像素数（只打有内容的行）:')
for y in range(h):
    n = int((a[y] > 16).sum())
    if n:
        print('  y=%4d  n=%4d (%.1f%%)' % (y, n, 100.0 * n / w))
