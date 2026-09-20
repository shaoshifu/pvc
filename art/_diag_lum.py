# -*- coding: utf-8 -*-
"""亮度分布诊断：暗灰子弹在绿草坪上看不清，需要看主体亮度分位。"""
import os, sys, importlib.util, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ba', os.path.join(HERE, 'build_assets.py'))
ba = importlib.util.module_from_spec(spec); spec.loader.exec_module(ba)
def lum(path, th=44):
    if not os.path.isabs(path): path = os.path.join(ba.RAW, path)
    r = ba.cutout(path, small=True); r = ba.trim(r); r = ba.fit_bottom(r, th)
    a = r[:, :, 3]; m = a > 200
    if m.sum() == 0: m = a > 16
    L = r[:, :, :3][m].mean(axis=1)
    q = np.percentile(L, [10, 50, 90])
    dark = (L < 60).mean() * 100
    lite = (L > 190).mean() * 100
    print('%-26s 亮度 p10/p50/p90 = %3.0f / %3.0f / %3.0f   暗(<60)=%3.0f%%  亮(>190)=%3.0f%%' % (
        os.path.basename(path), q[0], q[1], q[2], dark, lite))
for p in sys.argv[1:]: lum(p)
