#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
产物 BMP 验收（构建完成后的最后一道闸）
---------------------------------------
必须手写解析 32 位 BMP，不能用 PIL：
  PIL 打开 32 位 BMP 后再 convert('RGBA') 会把 alpha 全变成 255，
  "透明背景"看着完全正常，实际上游戏里会画出一个黑方块。
  一度就是这么被骗过去的。

检查项：
  1. biBitCount == 32（AlphaBlend 取 alpha 的前提）
  2. biHeight > 0（自下而上，BMP 标准行序，save_bmp32 就是这么写的）
  3. alpha 是否真的存在（全 255 = 预乘/写盘环节丢了 alpha）
  4. 预乘是否成立：BGR <= alpha（AC_SRC_ALPHA 要求预乘，否则边缘发白）
  5. 透明度覆盖率：alpha>16 的占比，落在合理区间
  6. 四角 alpha 偏高时提示（可能是光晕/多画了一个对象的残留）

用法:
  python _verify_bmp.py                      # 验收全部 bullet_*.bmp
  python _verify_bmp.py bullet_pierce.bmp …  # 指定文件
"""
import os
import sys
import struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.abspath(os.path.join(HERE, '..', 'assets'))


def load_bmp32(path):
    b = open(path, 'rb').read()
    if b[:2] != b'BM':
        raise ValueError('不是 BMP: ' + path)
    off = struct.unpack_from('<I', b, 10)[0]
    w, h = struct.unpack_from('<ii', b, 18)
    bits = struct.unpack_from('<H', b, 28)[0]
    bottom_up = h > 0
    px = np.frombuffer(b, np.uint8, count=w * abs(h) * 4, offset=off)
    px = px.reshape(abs(h), w, 4)          # BGRA 顺序
    if bottom_up:
        px = px[::-1]                      # 统一成"第 0 行 = 画面顶部"
    return dict(w=w, h=abs(h), bits=bits, bottom_up=bottom_up, px=px)


def check(path):
    d = load_bmp32(path)
    px = d['px'].astype(np.int32)
    B, G, R, A = px[:, :, 0], px[:, :, 1], px[:, :, 2], px[:, :, 3]
    cov = float((A > 16).sum()) / A.size * 100.0
    corners = [int(A[0, 0]), int(A[0, -1]), int(A[-1, 0]), int(A[-1, -1])]
    # 预乘检查：只看不透明的实心像素，容差 2
    m = A > 16
    over = int((((B > A + 2) | (G > A + 2) | (R > A + 2)) & m).sum())
    allopaque = bool((A == 255).all())
    bad = []
    if d['bits'] != 32:
        bad.append('位数=%d' % d['bits'])
    if not d['bottom_up']:
        bad.append('行序非自下而上')
    if allopaque:
        bad.append('alpha 全255(丢了透明)')
    if over > A.size * 0.02:
        bad.append('预乘异常%dpx' % over)
    if not (6.0 <= cov <= 75.0):
        bad.append('覆盖率%.1f%%' % cov)
    if min(corners) > 200:
        bad.append('四角a=%s 全亮 疑似光晕残留' % corners)
    print('%-22s %3dx%-4d %2dbit 自下而上=%d 覆盖%5.1f%% 四角a=%-16s 预乘越界%4d  %s' % (
        os.path.basename(path), d['w'], d['h'], d['bits'], d['bottom_up'],
        cov, str(corners), over, 'OK' if not bad else '<<< ' + '; '.join(bad)))
    return not bad


if __name__ == '__main__':
    args = sys.argv[1:]
    if not args:
        args = sorted(f for f in os.listdir(ASSETS)
                      if f.startswith('bullet_') and f.endswith('.bmp'))
    ok = 0
    for p in args:
        if not os.path.isabs(p):
            p = os.path.join(ASSETS, p)
        ok += check(p)
    print('\n通过 %d / %d' % (ok, len(args)))
    sys.exit(0 if ok == len(args) else 1)
