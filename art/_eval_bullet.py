#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
子弹候选图批量体检（只看数据，不需要肉眼）
-----------------------------------------
用途：mmx 出一批候选图后，先过一遍"和构建管线完全一致"的抠图步骤，
      把每张的 覆盖率 / 四角 alpha / 形状热力图 打出来，
      据此挑出「居中、不贴边、够粗」的那张再落到 bullet_xxx.jpg。

为什么需要它：生出来的图带光晕时，缩到 44px 会变成一坨绿斑，
而判断"光晕有没有被抠掉"在缩略图上肉眼根本看不出来。
这里用两个客观指标替代肉眼：
  1. 四角 alpha —— 只要不是 0，说明有残留贴到画面四角（光晕/渐变底）
  2. 覆盖率     —— alpha>16 的像素占比；正常子弹 15%~60%，<8% 太细看不清

用法：
  python _eval_bullet.py <目标高度> <图片1> [图片2 ...]
  python _eval_bullet.py 44 n2_needle_001.jpg n2_needle_002.jpg
"""
import os
import sys
import importlib.util
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ba', os.path.join(HERE, 'build_assets.py'))
ba = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ba)

RAMP = ' .:-=+*#%@'


def heatmap(rgba, cols=30):
    """把 alpha 通道降采样成 ASCII 灰度图，用来"看"形状（本模型无法直接看图）"""
    a = rgba[:, :, 3]
    h, w = a.shape
    rows = max(1, int(round(cols * h / float(w) * 0.5)))
    out = []
    for r in range(rows):
        line = ''
        for c in range(cols):
            y0, y1 = int(r * h / rows), max(int((r + 1) * h / rows), int(r * h / rows) + 1)
            x0, x1 = int(c * w / cols), max(int((c + 1) * w / cols), int(c * w / cols) + 1)
            v = a[y0:y1, x0:x1].mean() / 255.0
            line += RAMP[min(len(RAMP) - 1, int(v * len(RAMP)))]
        out.append(line)
    return out


def check(path, target_h, show=True):
    if not os.path.isabs(path):
        path = os.path.join(ba.RAW, path)
    name = os.path.basename(path)
    rgba = ba.cutout(path, small=True)
    rgba = ba.trim(rgba)
    rgba = ba.fit_bottom(rgba, target_h)
    a = rgba[:, :, 3]
    cov = float((a > 16).sum()) / a.size * 100.0
    corners = [int(a[0, 0]), int(a[0, -1]), int(a[-1, 0]), int(a[-1, -1])]
    h, w = a.shape
    print('%-28s %3dx%-4d 覆盖率 %5.1f%%  alpha>16 px=%d  四角a=%s  %s' % (
        name, w, h, cov, int((a > 16).sum()), corners,
        'OK' if (max(corners) == 0 and 8 <= cov <= 60) else '<<< 需复查'))
    if show:
        for ln in heatmap(rgba):
            print('      |' + ln + '|')
    return cov, corners


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    th = int(sys.argv[1])
    for p in sys.argv[2:]:
        check(p, th)
