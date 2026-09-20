# -*- coding: utf-8 -*-
"""验证鼠标点击在各分辨率/窗口模式下都能正确命中。

为什么必须单独测：分辨率改造后，鼠标坐标要经过
「窗口像素 → 逻辑坐标」的换算（windowToGame 用 gViewScale / gViewOff*）。
这一步一旦算错，表现就是用户说的"按钮按不出来" —— 而画面看起来完全正常。

覆盖三种情况：
  A. 默认（全屏启动）
  B. 窗口模式（PVZ_WINDOWED=1）
每个都点「开始」按钮（逻辑坐标 x 40..300, y 330..376），
成功的判据是状态切到选关界面（stat 里的 state 变化）。

用法： python _verify_click.py
"""
import ctypes
import ctypes.wintypes
import os
import re
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))


def find_window_by_pid(pid):
    u = ctypes.windll.user32
    found = []
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(hwnd, _):
        wpid = ctypes.c_ulong()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid:
            buf = ctypes.create_unicode_buffer(256)
            u.GetClassNameW(hwnd, buf, 256)
            if buf.value == "PvZClassC":
                found.append(hwnd)
        return True

    u.EnumWindows(proto(cb), 0)
    return found[0] if found else 0


def run_case(name, extra_env, logical_pt):
    """logical_pt: 要点击的**逻辑坐标**（0~1000 / 0~650 那套）"""
    fr = os.path.join(D, "_frames_click_%s" % name)
    os.makedirs(fr, exist_ok=True)
    for f in os.listdir(fr):
        os.remove(os.path.join(fr, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)

    r = subprocess.run([sys.executable, "_mk_framedump.py"], cwd=D,
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        return None, "生成失败：" + (r.stderr or "")[-300:]

    env = dict(os.environ)
    env["PVZ_DUMPDIR"] = fr
    env["PVZ_GDI"] = "1"       # 截帧工具读取的是 GDI 的 1x 帧
    env.update(extra_env)
    p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
    time.sleep(5.0)

    u = ctypes.windll.user32
    u.SetProcessDPIAware()
    hwnd = find_window_by_pid(p.pid)
    if not hwnd:
        p.kill()
        return None, "找不到窗口"

    rc = ctypes.wintypes.RECT()
    u.GetClientRect(hwnd, ctypes.byref(rc))
    cw, ch = rc.right, rc.bottom

    def press(c):
        u.PostMessageW(hwnd, 0x100, c, 0)
        u.PostMessageW(hwnd, 0x101, c, 0)

    # 先按 P 记录「点击前」的状态（不改游戏状态，只写盘）
    press(ord('P'))
    time.sleep(0.6)
    before = ""
    fp = os.path.join(fr, "stat30.txt")
    if os.path.exists(fp):
        before = open(fp, encoding="utf-8", errors="replace").read()
    m0 = re.search(r"state=(\d+)", before)
    st_before = m0.group(1) if m0 else "?"

    # 按当前窗口算出「逻辑坐标 → 窗口坐标」，与游戏内 windowToGame 互逆
    sc = min(cw / 1000.0, ch / 650.0)
    vw, vh = int(1000 * sc + 0.5), int(650 * sc + 0.5)
    offx, offy = (cw - vw) // 2, (ch - vh) // 2
    wx = offx + int(logical_pt[0] * vw / 1000.0)
    wy = offy + int(logical_pt[1] * vh / 650.0)

    lp = (wy << 16) | (wx & 0xFFFF)
    u.PostMessageW(hwnd, 0x0200, 0, lp)      # WM_MOUSEMOVE
    time.sleep(0.15)
    u.PostMessageW(hwnd, 0x0201, 1, lp)      # WM_LBUTTONDOWN
    u.PostMessageW(hwnd, 0x0202, 0, lp)      # WM_LBUTTONUP
    time.sleep(1.0)

    # 再按 P 记录「点击后」的状态
    press(ord('P'))
    time.sleep(0.6)
    after = ""
    if os.path.exists(fp):
        after = open(fp, encoding="utf-8", errors="replace").read()

    alive = (p.poll() is None)
    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()

    m1 = re.search(r"state=(\d+)", after)
    mm = re.search(r"mouse=(-?\d+),(-?\d+)", after)
    return dict(client=(cw, ch), view=(vw, vh), off=(offx, offy),
                click=(wx, wy), st_before=st_before,
                st_after=(m1.group(1) if m1 else "?"),
                mouse=(mm.group(1) + "," + mm.group(2)) if mm else "?",
                alive=alive), None


def main():
    # 菜单「开始」按钮：逻辑坐标约 (170, 353)
    for name, env in (("full", {}), ("win", {"PVZ_WINDOWED": "1"})):
        info, err = run_case(name, env, (170, 353))
        label = "默认(全屏)" if name == "full" else "窗口模式"
        if err:
            print("✗ %-10s %s" % (label, err))
            continue
        # ST_MENU = 0，点「开始」后应变成 ST_LEVELS
        hit = info["st_after"] != info["st_before"]
        print("%s %-10s 客户区=%dx%d view=%dx%d 点击=%s 鼠标逻辑坐标=%s 状态 %s→%s"
              % ("✔" if hit else "✗", label,
                 info["client"][0], info["client"][1],
                 info["view"][0], info["view"][1], info["click"], info["mouse"],
                 info["st_before"], info["st_after"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
