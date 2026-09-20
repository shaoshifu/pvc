# -*- coding: utf-8 -*-
"""诊断分辨率与鼠标点击：一次性拿到客观数据，不靠猜。

回答三个问题：
  1. 屏幕多大、窗口实际开了多大、渲染了多少像素（分辨率到底提没提上去）
  2. 鼠标坐标换算的参数（gViewScale / 偏移）
  3. 点击右上角全屏按钮，到底有没有被判定命中

做法：启动诊断版，读 stat 里的 view/buf/blit 行；再用 PostMessage 发一个
真实的 WM_LBUTTONDOWN 到"全屏按钮"的窗口坐标，看是否切进全屏。

用法： python _diag_res.py
"""
import ctypes
import os
import re
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_diag")


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


def grab(txt, key):
    m = re.search(r"^%s(.*)$" % key, txt, re.M)
    return m.group(1).strip() if m else ""


def main():
    u = ctypes.windll.user32
    u.SetProcessDPIAware()
    sw = u.GetSystemMetrics(0)
    sh = u.GetSystemMetrics(1)
    print("屏幕分辨率 : %d x %d" % (sw, sh))

    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)

    r = subprocess.run([sys.executable, "_mk_framedump.py"], cwd=D,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("[X] 生成失败：" + (r.stderr or "")[-600:])

    env = dict(os.environ)
    env["PVZ_DUMPDIR"] = FR
    # 测试用窗口模式启动：一是便于读窗口尺寸，二是避免测试环境里
    # 默认全屏影响断言（PVZ_WINDOWED 是专门为此留的开关）
    env["PVZ_WINDOWED"] = "1"
    p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
    time.sleep(5.0)

    hwnd = find_window_by_pid(p.pid)
    if not hwnd:
        p.kill()
        sys.exit("[X] 找不到窗口")

    rc = ctypes.wintypes.RECT()
    u.GetClientRect(hwnd, ctypes.byref(rc))
    wr = ctypes.wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(wr))
    print("窗口外框   : %d x %d" % (wr.right - wr.left, wr.bottom - wr.top))
    print("客户区     : %d x %d" % (rc.right - rc.left, rc.bottom - rc.top))

    def press(c):
        u.PostMessageW(hwnd, 0x100, c, 0)
        u.PostMessageW(hwnd, 0x101, c, 0)

    press(0x70)          # F1 进战斗
    time.sleep(0.8)
    press(0x58)          # X 触发 stat16（诊断行只在这个 tag 下写盘）
    time.sleep(1.2)

    stat = os.path.join(FR, "stat16.txt")
    txt = open(stat, encoding="utf-8", errors="replace").read() if os.path.exists(stat) else ""
    print("\n--- 渲染诊断 ---")
    for k in ("view ", "buf ", "blit ", "perf ", "avg "):
        v = grab(txt, k)
        if v:
            print("  %-6s %s" % (k.strip(), v))

    # ---- 测试点击右上角全屏按钮 ----
    m = re.search(r"view (\d+)x(\d+)", txt)
    if not m:
        p.terminate()
        sys.exit("[X] 读不到 view 行")
    vw, vh = int(m.group(1)), int(m.group(2))

    m2 = re.search(r"blit dest=\d+x\d+ client=(\d+)x(\d+)", txt)
    cw, ch = (int(m2.group(1)), int(m2.group(2))) if m2 else (rc.right, rc.bottom)
    offx, offy = (cw - vw) // 2, (ch - vh) // 2
    k = vw / 1000.0

    # 全屏按钮：逻辑坐标 (956..992, 8..32) → 窗口坐标
    bx = offx + int((956 + 18) * k)
    by = offy + int((8 + 12) * k)
    print("\n--- 点击测试 ---")
    print("  换算参数 : view=%dx%d client=%dx%d off=%d,%d k=%.3f" % (vw, vh, cw, ch, offx, offy, k))
    print("  按钮窗口坐标 : (%d, %d)" % (bx, by))

    lp = (by << 16) | (bx & 0xFFFF)
    u.PostMessageW(hwnd, 0x0201, 1, lp)      # WM_LBUTTONDOWN
    u.PostMessageW(hwnd, 0x0202, 0, lp)      # WM_LBUTTONUP
    time.sleep(1.2)

    wr2 = ctypes.wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(wr2))
    w2 = wr2.right - wr2.left
    h2 = wr2.bottom - wr2.top
    full = (w2 >= sw - 4 and h2 >= sh - 4)
    print("  点击后窗口 : %d x %d  -> %s" % (w2, h2, "已切全屏 ✔" if full else "没切换 ✗"))

    alive = (p.poll() is None)
    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()
    print("\n进程存活 = %s" % alive)
    return 0


if __name__ == "__main__":
    import ctypes.wintypes
    sys.exit(main())
