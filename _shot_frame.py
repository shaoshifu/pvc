# -*- coding: utf-8 -*-
"""抓一张游戏内实拍帧并转成 PNG，用于交付展示。

与 _shot.py（悬停提示测试）不是一回事，故意用不同文件名。
按键与 _mk_framedump.py 一致：
  X(0x58)=6 株不同品质肉鸽植物 + 3 只肉鸽僵尸(tag16)
  Y(0x59)=铺满地块 + 强制主题(tag14)
  Z(0x5A)=三只肉鸽僵尸(tag13)
用法： python _shot_frame.py [X|Y|Z]
"""
import ctypes
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_shot")
KEY = {"X": (0x58, 16), "Y": (0x59, 14), "Z": (0x5A, 13),
       "D": (0x74, 5), "S": (0x75, 6)}   # D = 开局免费选卡, S = 货架（花阳光）


def to_png(bmp, png):
    with open(bmp, "rb") as f:
        b = f.read()
    off = int.from_bytes(b[10:14], "little")
    w = int.from_bytes(b[18:22], "little", signed=True)
    h = int.from_bytes(b[22:26], "little", signed=True)
    a = np.frombuffer(b[off:], np.uint8).reshape(abs(h), abs(w), 4)
    # biHeight > 0 = 自下而上存储，读出来要翻一下；< 0 = 已是顶行优先。
    # 无条件翻转会得到上下颠倒的画面（文字全倒过来），一定要判符号。
    if h > 0:
        a = a[::-1]
    Image.fromarray(a[:, :, :3][:, :, ::-1].copy()).save(png)
    return w, abs(h)


def main():
    tag_key = (sys.argv[1] if len(sys.argv) > 1 else "X").upper()
    code, tag = KEY.get(tag_key, KEY["X"])
    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)

    r = subprocess.run([sys.executable, "_mk_framedump.py"], cwd=D,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("[X] _dump 生成失败: " + (r.stderr or "")[-800:])

    env = dict(os.environ)
    env["PVZ_DUMPDIR"] = FR
    p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
    time.sleep(4.5)

    u = ctypes.windll.user32
    hwnd = u.FindWindowW("PvZClassC", None)
    if not hwnd:
        p.kill()
        sys.exit("[X] 找不到窗口")

    def press(c):
        u.PostMessageW(hwnd, 0x100, c, 0)
        u.PostMessageW(hwnd, 0x101, c, 0)

    press(0x70)                 # F1 -> 进 ST_PLAY
    time.sleep(0.8)
    press(code)
    time.sleep(2.5)             # 等动画收敛（太快会抓到翻牌中间态）

    src = None
    for t in (tag, tag - 1, tag - 2):
        cand = os.path.join(FR, "st%02d.bmp" % t)
        if os.path.exists(cand):
            src = cand
            break
    if src is None:
        p.kill()
        sys.exit("[X] 没有导出帧，目录内容：" + " ".join(sorted(os.listdir(FR))))

    out = os.path.join(D, "art", "shot_%s.png" % tag_key.lower())
    w, h = to_png(src, out)
    print("[OK] %s  %dx%d  <- %s" % (out, w, h, os.path.basename(src)))

    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()


if __name__ == "__main__":
    main()
