# -*- coding: utf-8 -*-
"""子弹候选色彩诊断：量化"光晕残留"。
光晕的特征 = 大量中低 alpha + 高饱和度像素（抠图把彩色柔光当成实体留下了）。
干净精灵的特征 = alpha>200 占绝大多数，边缘过渡带很窄。
"""
import os, sys, importlib.util, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ba', os.path.join(HERE, 'build_assets.py'))
ba = importlib.util.module_from_spec(spec); spec.loader.exec_module(ba)

def diag(path, th=44):
    if not os.path.isabs(path): path = os.path.join(ba.RAW, path)
    r = ba.cutout(path, small=True); r = ba.trim(r); r = ba.fit_bottom(r, th)
    a = r[:, :, 3]; rgb = r[:, :, :3]
    n16 = (a > 16).sum(); n200 = (a > 200).sum(); nband = ((a > 40) & (a < 200)).sum()
    mx = rgb.max(axis=2); mn = rgb.min(axis=2)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1), 0)
    band = (a > 40) & (a < 200)
    print('%-26s alpha>16=%4d  >200=%4d (%.0f%%)  过渡带40~200=%3d (%.0f%%)  过渡带平均饱和=%.2f  主体平均RGB=%s' % (
        os.path.basename(path), n16, n200, 100.0*n200/max(n16,1), nband, 100.0*nband/max(n16,1),
        sat[band].mean() if nband else 0,
        np.round(rgb[a > 200].mean(axis=0)).astype(int) if n200 else '-'))

for p in sys.argv[1:]:
    diag(p)
