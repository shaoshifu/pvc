# -*- coding: utf-8 -*-
"""跑 _hudtest.exe：检查 PRE / POST 两个标记块是否出现在屏幕上。"""
import subprocess, time, ctypes, os
import numpy as np
from PIL import Image

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
try:
    subprocess.run(["taskkill", "/F", "/IM", "pvz.exe"], capture_output=True)
except Exception:
    pass

p = subprocess.Popen([os.path.join(D, "_hudtest.exe")], cwd=D)
time.sleep(4.5)

u = ctypes.windll.user32
g = ctypes.windll.gdi32
hwnd = u.FindWindowW("PvZClassC", None)
print("hwnd =", hwnd)


class RC(ctypes.Structure):
    _fields_ = [("L", ctypes.c_long), ("T", ctypes.c_long),
                ("R", ctypes.c_long), ("B", ctypes.c_long)]


rc = RC(); u.GetClientRect(hwnd, ctypes.byref(rc))
W, H = rc.R, rc.B
print("client %dx%d" % (W, H))


class BI(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]


def capture():
    hdcwin = u.GetDC(hwnd)
    mem = g.CreateCompatibleDC(hdcwin)
    bmp = g.CreateCompatibleBitmap(hdcwin, W, H)
    old = g.SelectObject(mem, bmp)
    u.PrintWindow(hwnd, mem, 3)
    bi = BI(40, W, -H, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(W * H * 4)
    g.GetDIBits(mem, bmp, 0, H, buf, ctypes.byref(bi), 0)
    g.SelectObject(mem, old)
    g.DeleteObject(bmp); g.DeleteDC(mem); u.ReleaseDC(hwnd, hdcwin)
    a = np.frombuffer(buf, np.uint8).reshape(H, W, 4)
    return a[:, :, :3][:, :, ::-1].copy()


def key(code):
    u.PostMessageW(hwnd, 0x100, code, 0)
    u.PostMessageW(hwnd, 0x101, code, 0)


def magenta(a, x0, y0, x1, y1):
    s = a[y0:y1, x0:x1].astype(int)
    return int(((s[:, :, 0] > 200) & (s[:, :, 1] < 60) & (s[:, :, 2] > 200)).sum())


def green(a, x0, y0, x1, y1):
    s = a[y0:y1, x0:x1].astype(int)
    return int(((s[:, :, 0] < 60) & (s[:, :, 1] > 200) & (s[:, :, 2] < 60)).sum())


for tag, do_key in (("主菜单", False), ("战斗(ST_PLAY)", True)):
    if do_key:
        key(0x52)
        time.sleep(2.5)
    a = capture()
    Image.fromarray(a).save(os.path.join(D, "_hudtest_%s.png" % ("menu" if not do_key else "play")))
    mp = magenta(a, 822, 202, 978, 222)
    gp = green(a, 822, 252, 978, 272)
    print("\n[%s]" % tag)
    print("  PRE  块(StretchBlt 之前, 品红) 命中像素 = %5d  %s" % (mp, "可见 ✗异常" if mp > 20 else "不可见 ✓符合预期"))
    print("  POST 块(StretchBlt 之后, 纯绿) 命中像素 = %5d  %s" % (gp, "可见 ✓符合预期" if gp > 20 else "不可见 ✗管线有问题"))

# 顺带看面板区实际是什么
a = capture()
reg = a[94:165, 812:995].astype(int).reshape(-1, 3)
print("\n面板区(812..995 x 94..165) 均值色 = %s（若是模式条实色块，应接近单一实色）"
      % (tuple(int(v) for v in reg.mean(0)),))

p.terminate()
try:
    p.wait(timeout=3)
except Exception:
    p.kill()
