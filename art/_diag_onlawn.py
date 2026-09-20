# -*- coding: utf-8 -*-
"""在草坪底色上按【游戏里真实尺寸】合成，验证可读性。
pvz.c 里子弹是 spriteBlit(sc=0.62)，末日弹 0.85；草坪均值实测 RGB(96,132,43)。
这里据此缩放到实际显示尺寸，再算和草坪的亮度对比，并输出 ASCII。
"""
import os, sys, struct, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.path.join(HERE, '..', 'assets'))
LAWN = np.array([96, 132, 43], np.float32)     # RGB，实测

def load(p):
    b = open(p, 'rb').read()
    off = struct.unpack_from('<I', b, 10)[0]
    w, h = struct.unpack_from('<ii', b, 18)
    px = np.frombuffer(b, np.uint8, count=w*abs(h)*4, offset=off).reshape(abs(h), w, 4)
    if h > 0: px = px[::-1]
    a = px[:, :, 3:4].astype(np.float32) / 255.0
    rgb = px[:, :, [2, 1, 0]].astype(np.float32)     # 预乘的 RGB
    # 反预乘，拿到真实颜色
    un = np.where(a > 0.004, rgb / np.maximum(a, 0.004), 0.0)
    return np.clip(un, 0, 255), a

def lum(c):
    return 0.299*c[..., 0] + 0.587*c[..., 1] + 0.114*c[..., 2]

def onlawn(name, sc):
    rgb, a = load(os.path.join(OUT, name))
    h, w = a.shape[:2]
    nw, nh = max(1, int(round(w*sc))), max(1, int(round(h*sc)))
    from PIL import Image
    rgb_i = np.array(Image.fromarray(rgb.astype(np.uint8)).resize((nw, nh), Image.LANCZOS)).astype(np.float32)
    a_i = np.array(Image.fromarray((a[:, :, 0]*255).astype(np.uint8)).resize((nw, nh), Image.LANCZOS)).astype(np.float32)/255.0
    comp = LAWN[None, None, :]*(1-a_i[..., None]) + rgb_i*a_i[..., None]
    Ls = lum(rgb_i); Lc = lum(comp); Ll = lum(LAWN)
    m = a_i > 0.5
    if m.sum() == 0:
        print(name, '无实体像素'); return
    diff = np.abs(Lc - Ll)[m]
    visible = (diff > 25).mean()*100
    print('%-20s 显示 %2dx%-3d  主体亮度 %3.0f (p10 %3.0f / p90 %3.0f)  草坪 %3.0f  '
          '与草坪差>25 的像素 %4.1f%%  %s' % (
        name, nw, nh, Ls[m].mean(), np.percentile(Ls[m],10), np.percentile(Ls[m],90),
        Ll, visible, 'OK' if visible > 55 else '<<< 太接近草坪'))
    RAMP = ' .:-=+*#%@'
    for y in range(0, nh, max(1, nh//16)):
        line = ''
        for x in range(nw):
            v = a_i[y, x]
            line += RAMP[min(9, int(v*10))]
        print('     |'+line+'|')

if __name__ == '__main__':
    print('草坪 RGB=(96,132,43) 亮度=%.0f\n' % lum(LAWN))
    for n, sc in [('bullet_pierce.bmp', 0.62), ('bullet_ice.bmp', 0.62),
                  ('bullet_kernel.bmp', 0.62), ('bullet_cob.bmp', 0.62), ('bullet_doom.bmp', 0.85)]:
        onlawn(n, sc)
