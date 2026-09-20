# -*- coding: utf-8 -*-
"""端到端：R 进战斗 → 模拟鼠标悬停 → PrintWindow 截图。
悬停位置1：第 1 张卡中心(180,50)   —— 应出现提示文字
悬停位置2：铲子右侧空白(800,50)    —— 修复前会显示"某个没上场植物"的提示，修复后应无
"""
import subprocess, time, ctypes, os
import numpy as np
from PIL import Image

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
p = subprocess.Popen([os.path.join(D, "pvz.exe")], cwd=D)
time.sleep(4)

u = ctypes.windll.user32
g = ctypes.windll.gdi32
hwnd = u.FindWindowW("PvZClassC", None)

class R(ctypes.Structure):
    _fields_ = [("L", ctypes.c_long), ("T", ctypes.c_long),
                ("R", ctypes.c_long), ("B", ctypes.c_long)]

rc = R(); u.GetClientRect(hwnd, ctypes.byref(rc))
W, H = rc.R, rc.B
print("hwnd =", hwnd, " client %dx%d" % (W, H))

u.PostMessageW(hwnd, 0x100, 0x52, 0)
u.PostMessageW(hwnd, 0x101, 0x52, 0)
time.sleep(3)

class BI(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]

def capture(hwnd, W, H):
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

def tooltip_text_pixels(a, x0, x1):
    """y 92..132 区域内近白/近亮文字像素（提示文字画在 y≈100）"""
    s = a[92:132, x0:x1].astype(int)
    bright = (s[:, :, 0] > 200) & (s[:, :, 1] > 195) & (s[:, :, 2] > 170)
    return int(bright.sum())

def movemouse(x, y):
    u.PostMessageW(hwnd, 0x200, 0, (y << 16) | x)   # WM_MOUSEMOVE

# --- 悬停 1：第 1 张卡 ---
movemouse(180, 50); time.sleep(0.8)
a1 = capture(hwnd, W, H)
n_card = tooltip_text_pixels(a1, 60, 340)
print("悬停第1张卡: 卡上方提示文字像素 = %d" % n_card)
Image.fromarray(a1).save(os.path.join(D, "_shot_hover_card.png"))

# --- 悬停 2：铲子右侧空白（修复前 bug 位）---
movemouse(800, 50); time.sleep(0.8)
a2 = capture(hwnd, W, H)
n_empty = tooltip_text_pixels(a2, 560, 995)
print("悬停空白区: 该区域提示文字像素 = %d" % n_empty)
Image.fromarray(a2).save(os.path.join(D, "_shot_hover_empty.png"))

# 顶栏主检查（用悬停空白那张：卡片价格/右侧残留）
wood = (np.abs(a2[2:88].astype(int) - np.array([104, 70, 42])).sum(2) < 60).mean()
print("顶栏木色占比 %.2f（战斗确认）" % wood)
s = a2[4:88].astype(int)
bright = (s[:, :, 0] > 185) & (s[:, :, 1] > 185) & (s[:, :, 2] > 170)
red = (s[:, :, 0] > 195) & (s[:, :, 1] < 175) & (s[:, :, 2] < 175)
xs = np.where((bright | red).any(0))[0]
bad = xs[(xs >= 573)]
print("顶栏铲子右侧(573..995)文字像素 = %d  落点=%s" % (bad.size, list(np.unique(bad))[:20] if bad.size else "无"))

print()
print("结论：",
      "悬停卡片有提示(%d px)，" % n_card,
      "悬停空白无提示(%d px)，" % n_empty,
      "顶栏右侧无残留(%d px)" % bad.size,
      "=> %s" % ("全部通过" if n_card > 20 and n_empty == 0 and bad.size == 0 else "存在问题"))

p.terminate()
try:
    p.wait(timeout=3)
except Exception:
    p.kill()
