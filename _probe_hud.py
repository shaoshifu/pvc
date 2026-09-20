# -*- coding: utf-8 -*-
"""验证截图管线 + 判定右下角状态栏是否可见。

粗粒度差分图：把 1000x650 分成 25x13 个 40x50 的块，打印每块的平均色，
对比菜单帧与战斗帧，确认 capture() 真的在工作。
"""
import subprocess, time, ctypes, os
import numpy as np
from PIL import Image

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
p = subprocess.Popen([os.path.join(D, "pvz.exe")], cwd=D)
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


def blockmap(a, tag):
    """25x13 块的均值色，打印成色块摘要"""
    bs = 40
    print("\n[%s] 粗糙块图（每块 40x50 的均值）：" % tag)
    for by in range(0, 650, 50):
        row = []
        for bx in range(0, 1000, 40):
            m = a[by:by + 50, bx:bx + 40].reshape(-1, 3).mean(0)
            row.append("%02x%02x%02x" % (int(m[0]), int(m[1]), int(m[2])))
        print("  y=%3d  " % by + " ".join(row))


print("\n########## 1) 初始（应为主菜单） ##########")
m0 = capture()
Image.fromarray(m0).save(os.path.join(D, "_probe_m0.png"))
blockmap(m0, "初始")

print("\n########## 2) 按 R 进战斗 ##########")
key(0x52)
time.sleep(2.5)
m1 = capture()
Image.fromarray(m1).save(os.path.join(D, "_probe_m1.png"))
blockmap(m1, "战斗")

d = np.abs(m0.astype(int) - m1.astype(int)).mean()
print("\n两帧全图平均差 = %.2f  （>3 说明 capture 真的在变，管线正常）" % d)
nd = int((np.abs(m0.astype(int) - m1.astype(int)).max(2) > 24).sum())
print("差异像素数 = %d / %d  (%.1f%%)" % (nd, W * H, 100.0 * nd / (W * H)))

# 面板可见性判定：模式条 814..992 x 96..116 是否整块实色
reg = m1[96:116, 814:992].astype(int).reshape(-1, 3)
cols, cnt = np.unique(reg, axis=0, return_counts=True)
print("\n战斗帧·模式条位置(814..992 x 96..116):")
print("  不同色数=%d  主色=%s 占比 %.1f%%" % (len(cols),
      tuple(int(v) for v in cols[np.argmax(cnt)]), 100.0 * cnt.max() / len(reg)))
print("  饱和度统计: 该区均值色 = %s" % (tuple(int(v) for v in reg.mean(0)),))

p.terminate()
try:
    p.wait(timeout=3)
except Exception:
    p.kill()
