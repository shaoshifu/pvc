# -*- coding: utf-8 -*-
"""连通域体检：一张合格的子弹素材必须是【恰好一个】主体。
mmx 常偷偷多画一个东西（倒影 / 地面横条 / 第二根针），
这种图抠完会变成一个很宽的横向条，进游戏就是异常像素。
这里直接数连通域，把"多对象"从"需要人眼判断"变成可断言的指标。

用法: python _diag_objects.py 44 n4_needle_001.jpg ...
      python _diag_objects.py --raw        # 只查原始图，快速筛候选
"""
import os, sys, importlib.util, numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ba', os.path.join(HERE, 'build_assets.py'))
ba = importlib.util.module_from_spec(spec); spec.loader.exec_module(ba)


def components(mask):
    """四连通计数，返回 [(面积, (y0,y1,x0,x1))] 按面积降序"""
    m = mask.copy()
    h, w = m.shape
    out = []
    for y in range(h):
        for x in range(w):
            if not m[y, x]:
                continue
            # 用 build_assets 里现成的泛洪（它已经处理过边界/递归问题）
            comp = ba.flood_from_border(np.pad(m, 1)) if False else None
            # 自己写个栈式泛洪，避免依赖
            stack = [(y, x)]
            m[y, x] = False
            n = 0
            ys = [y]; xs = [x]
            while stack:
                cy, cx = stack.pop()
                n += 1
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < h and 0 <= nx < w and m[ny, nx]:
                        m[ny, nx] = False
                        stack.append((ny, nx))
                        ys.append(ny); xs.append(nx)
            out.append((n, (min(ys), max(ys), min(xs), max(xs))))
    out.sort(reverse=True)
    return out


def raw_check(path):
    if not os.path.isabs(path):
        path = os.path.join(HERE, 'raw', path)
    a = np.array(Image.open(path).convert('RGB')).astype(np.int32)
    bg = np.median(a[:8].reshape(-1, 3), axis=0)
    dev = np.abs(a - bg).max(axis=2)
    comps = components(dev > 70)
    total = comps[0][0] if comps else 0
    big = [c for c in comps if c[0] > total * 0.02]
    print('%-24s 主体块数=%d  最大=%d  占比>2%%的块=%d %s' % (
        os.path.basename(path), len(comps), total, len(big),
        'OK' if len(big) == 1 else '<<< 多对象: ' + str([c[0] for c in big[:5]])))
    return len(big) == 1


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--raw']
    if '--raw' in sys.argv or not args:
        cands = sorted([f for f in os.listdir(os.path.join(HERE, 'raw'))
                        if f.lower().startswith(('n2_needle', 'n3_needle', 'n4_needle', 'n_needle', 'bullet_pierce'))])
        for c in cands:
            raw_check(c)
    else:
        th = int(args[0])
        for p in args[1:]:
            raw_check(p)
