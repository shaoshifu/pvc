#!/usr/bin/env python
# -*- coding: utf-8 -*-
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_assets as ba

names = [
 'starfruit','cactus','splitpea','lightningreed','bloomerang','fume','laserbean',
 'melonpult','wintermelon','gloom','goldmagnet','twinflower','marrow','pumpkin',
 'garlic','hypnoshroom','iceshroom','tanglekelp','cobcannon','cattail',
 'bullet_star','bullet_spine','bullet_split','bullet_arc','bullet_boomer','bullet_fume',
 'bullet_beam','bullet_melon','bullet_icemelon','bullet_gloom','bullet_cob','bullet_dart'
]
ok = 0
for name in names:
    src = ba.find_raw(name, required=False)
    if not src:
        print('MISS', name); continue
    is_bullet = name.startswith('bullet_')
    try:
        rgba = ba.cutout(src, small=is_bullet)
        rgba = ba.trim(rgba)
        th = ba.TARGET_H.get(name, 200 if not is_bullet else 44)
        rgba = ba.fit_bottom(rgba, th)
        if name not in ba.NO_GRADE:
            rgba = ba.grade(rgba, name, log=False)
        rgba = ba.premultiply(rgba)
        w, h = ba.save_bmp32(os.path.join(ba.OUT, name + '.bmp'), rgba)
        print('OK', name, '%dx%d' % (w, h))
        ok += 1
    except Exception as e:
        print('ERR', name, e)
print('exported', ok)
