# -*- coding: utf-8 -*-
"""端到端验收：进战斗 -> 选卡种植物 -> 截图 -> 用像素检查底部状态条排版。

为什么要写这个：本环境模型看不了图片，肉眼看截图这条路是断的。
所以"改好了"不能靠感觉，必须让程序去数像素：
  1) 左侧 x 18..110 / y 604..650 应该出现"击杀 N"的描边文字像素；
  2) 右侧 x 860..995 / y 604..650 应该出现两行开关提示（两段分离的文字带）；
  3) 中间 x 830..870 应该是空的（这是修复前文字和波次条挤在一起的地方）；
  4) 波次条区域 x 178..822 / y 617..643 仍应存在（不能被文字压掉）。
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
print("hwnd = %s  client %dx%d" % (hwnd, W, H))


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


def click(x, y):
    u.PostMessageW(hwnd, 0x200, 0, (y << 16) | x)
    time.sleep(0.12)
    u.PostMessageW(hwnd, 0x201, 1, (y << 16) | x)
    time.sleep(0.06)
    u.PostMessageW(hwnd, 0x202, 0, (y << 16) | x)


def key(vk):
    u.PostMessageW(hwnd, 0x100, vk, 0)
    u.PostMessageW(hwnd, 0x101, vk, 0)
    time.sleep(0.25)


# ---- 进战斗 ----
key(0x52)                       # R
time.sleep(3.0)

# ---- 种几株向日葵，让画面里真有东西 ----
# 卡槽 1 中心 (180,46)；格子中心 = (62+col*96+48, 104+row*100+50)
click(180, 46)
time.sleep(0.4)
for (col, row) in [(0, 0), (1, 0), (1, 1), (2, 0)]:
    click(62 + col * 96 + 48, 104 + row * 100 + 50)
    time.sleep(0.55)

# ---- 等到有僵尸上场，击杀数不再恒为 0 ----
time.sleep(14.0)

a = capture()
Image.fromarray(a).save(os.path.join(D, "_shot_bottom_hud.png"))
Image.fromarray(a[596:650, 0:1000]).resize((1000 * 2, 54 * 2), Image.NEAREST).save(
    os.path.join(D, "_shot_bottom_bar_zoom.png"))
print("已保存 _shot_bottom_hud.png / _shot_bottom_bar_zoom.png")


def hit(a, box, rgb, tol):
    x0, y0, x1, y1 = box
    s = a[y0:y1, x0:x1].astype(int)
    d = np.abs(s - np.array(rgb)).sum(2)
    return (d < tol)


# pvz.c 里写死的颜色，只有这几处会用：
KILL_FILL = (232, 228, 202)     # 击杀文字填充
TOG_FILL = (178, 240, 150)      # 开关"开"时的绿色填充
OUTLINE = (20, 30, 20)          # 统一的深色描边

# 木色占比确认真的在战斗里（不是菜单）
wood = (np.abs(a[2:88].astype(int) - np.array([104, 70, 42])).sum(2) < 60).mean()
print("顶栏木色占比 %.2f  -> %s" % (wood, "战斗中" if wood > 0.3 else "不在战斗!"))

print()
print("=== A. 描边色(20,30,20)计数：目标窗口 vs 对照窗口 ===")
print("    （对照窗口取草坪上方同一 x 区间，那里肯定没有 HUD 文字）")
targets = [
    ("击杀(左)", (14, 610, 110, 646), (14, 560, 110, 596)),
    ("自动拾取(右)", (868, 603, 992, 631), (868, 555, 992, 583)),
    ("飘落特效(右)", (864, 623, 992, 651), (864, 575, 992, 603)),
    ("空档带(修复点)", (826, 604, 866, 650), (826, 556, 866, 602)),
    ("波次条中心", (400, 607, 600, 643), (400, 559, 600, 595)),
]
cal = {}
for name, box, ctrl in targets:
    t = int(hit(a, box, OUTLINE, 90).sum())
    c = int(hit(a, ctrl, OUTLINE, 90).sum())
    cal[name] = (t, c)
    ratio = (t / c) if c else float("inf")
    print("  %-16s 目标 %6d   对照 %6d   倍率 %s" % (name, t, c,
          ("%.1fx" % ratio) if c else "inf"))

print()
print("=== B. 右侧两行绿色填充(178,240,150)：逐行计数找两段 ===")
print("    阈值取 8（低于此值的是背景噪声，实测文字行峰值 33~39）")


def find_lines(mask, y0, thr=8, merge=3):
    """逐行计数找文字行；相邻 <=merge px 的空隙合并，避免一行被切成几段。"""
    rows = mask.sum(1)
    raw, inrun = [], False
    for i, v in enumerate(rows):
        if v >= thr and not inrun:
            inrun = True; st = i
        elif v < thr and inrun:
            inrun = False; raw.append([st, i])
    if inrun:
        raw.append([st, len(rows)])
    merged = []
    for r in raw:
        if merged and r[0] - merged[-1][1] <= merge:
            merged[-1][1] = r[1]
        else:
            merged.append(list(r))
    return [(y0 + a, y0 + b) for a, b in merged], rows


gm = hit(a, (864, 596, 996, 650), TOG_FILL, 45)
runs, rows = find_lines(gm, 596)
for st, en in runs:
    print("  绿色文字行 y %d..%d  高 %d px  峰值 %d" % (st, en, en - st, rows[st - 596:en - 596].max()))
print("  -> 检出 %d 行" % len(runs))

print()
print("=== C. 左侧击杀数填充(232,228,202) ===")
km = hit(a, (10, 606, 130, 650), KILL_FILL, 60)
kruns, krows = find_lines(km, 606, thr=3)
print("  击杀文字行:", kruns, " 像素 %d" % int(km.sum()))

print()
print("=== D. 判定 ===")
k_ok = int(km.sum()) > 25 and len(kruns) == 1
gap_ok = cal["空档带(修复点)"][0] < max(60, cal["空档带(修复点)"][1] * 1.6)
two_ok = len(runs) == 2
bar_ok = cal["波次条中心"][0] > 300
wood_ok = wood > 0.3

checks = [
    ("左侧 击杀 N 文字存在且只有一行", k_ok),
    ("右侧是两行独立文字(不是糊成一团)", two_ok),
    ("文字/波次条之间的空档是干净的(本次修复点)", gap_ok),
    ("波次进度条仍然存在", bar_ok),
    ("确实在战斗画面里", wood_ok),
]
ok = True
for name, passed in checks:
    print("  [%s] %s" % ("PASS" if passed else "FAIL", name))
    ok = ok and passed
print("\n===== 结论: %s =====" % ("全部通过" if ok else "存在未通过项"))

p.terminate()
try:
    p.wait(timeout=3)
except Exception:
    p.kill()
