# -*- coding: utf-8 -*-
"""驱动 _dump.exe 走遍 13 个界面，把每个界面的 gFrameDC 真帧读回来分析。

输出的 BMP 是 32bpp 顶向下、54 字节头，直接 numpy 读。
"""
import os
import subprocess
import sys
import time
import ctypes
import numpy as np

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
FR = os.path.join(D, "_frames")
VK = {"play": 0x70, "pause": 0x71, "win": 0x72, "lose": 0x73, "draft": 0x74,
      "levels": 0x75, "talents": 0x76, "ach": 0x77, "gacha": 0x78,
      "loadout": 0x79, "reward": 0x7B, "daily": 0x24, "menu": 0x23}
TAG = {"play": 1, "pause": 2, "win": 3, "lose": 4, "draft": 5, "levels": 6,
       "talents": 7, "ach": 8, "gacha": 9, "loadout": 10, "reward": 11,
       "daily": 12, "menu": 0}


def load(path):
    with open(path, "rb") as f:
        b = f.read()
    off = int.from_bytes(b[10:14], "little")
    w = int.from_bytes(b[18:22], "little", signed=True)
    h = int.from_bytes(b[22:26], "little", signed=True)
    a = np.frombuffer(b[off:], np.uint8).reshape(abs(h), abs(w), 4)
    return a[:, :, :3][:, :, ::-1].copy()          # BGRA -> RGB


def blockmap(a, x0, y0, x1, y1, bs=20, tag=""):
    print("    [%s] (%d,%d)-(%d,%d) 每块 %dpx 均值色：" % (tag, x0, y0, x1, y1, bs))
    for y in range(y0, y1, bs):
        row = []
        for x in range(x0, x1, bs):
            m = a[y:y + bs, x:x + bs].reshape(-1, 3).mean(0)
            row.append("%02x%02x%02x" % (int(m[0]), int(m[1]), int(m[2])))
        print("      y=%3d  " % y + " ".join(row))


def uniqshare(a, x0, y0, x1, y1):
    s = a[y0:y1, x0:x1].reshape(-1, 3)
    cols, cnt = np.unique(s, axis=0, return_counts=True)
    k = int(np.argmax(cnt))
    return len(cols), tuple(int(v) for v in cols[k]), cnt[k] / len(s)


def main():
    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)

    env = dict(os.environ)
    env["PVZ_DUMPDIR"] = FR
    env["PVZ_GDI"] = "1"       # 诊断版要读取 gFrameDC；正式版默认走 GPU
    p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
    time.sleep(4.5)

    u = ctypes.windll.user32
    hwnd = u.FindWindowW("PvZClassC", None)
    print("hwnd =", hwnd)

    def key(code):
        u.PostMessageW(hwnd, 0x100, code, 0)
        u.PostMessageW(hwnd, 0x101, code, 0)

    order = ["menu", "play", "pause", "win", "lose", "draft", "levels",
             "talents", "ach", "gacha", "loadout", "reward", "daily"]
    for st in order:
        key(VK[st])
        time.sleep(1.4)
        path = os.path.join(FR, "st%02d.bmp" % TAG[st])
        if not os.path.exists(path):
            print("\n### %-8s  ✗ 没有导出文件" % st)
            continue
        a = load(path)
        n, top, share = uniqshare(a, 812, 94, 995, 165)
        print("\n### %-8s 帧 %dx%d | 面板区(812..995 x 94..165): 色数=%d 主色=%s 占比%.1f%%"
              % (st, a.shape[1], a.shape[0], n, top, share * 100))
        blockmap(a, 800, 88, 1000, 168, 20, st)

    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()


if __name__ == "__main__":
    main()
