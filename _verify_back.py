# -*- coding: utf-8 -*-
"""验证返回导航：按钮 / Esc / 右键 三条路都能从元界面回到主菜单。

背景：界面底部曾写着「右键 / Esc 返回」，但 Esc 的分支只清了卡片选中、
**并不返回** —— UI 承诺了做不到的事，玩家按 Esc 没反应就以为卡住了。
而且当时没有任何可见的返回按钮。这次修复加了可见按钮，并把三条路径
统一到 goBack()。

本脚本逐条验证，且**必须覆盖多个界面** —— 之前右键能返回、Esc 不能，
正是因为两条路径各写了一份。只测一个界面会漏掉"某些界面没接上"。

用法： python _verify_back.py
"""
import ctypes
import ctypes.wintypes
import os
import re
import shutil
import struct
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_back")
TMP = os.path.join(D, "_back_test.dat")

ST = dict(MENU=0, PLAY=1, PAUSE=2, WIN=3, LOSE=4, DRAFT=5,
          LEVELS=6, TALENTS=7, ACH=8, GACHA=9, LOADOUT=10, REWARD=11, DAILY=12)
ST_NAME = {v: k for k, v in ST.items()}

BACKBTN_X, BACKBTN_Y, BACKBTN_W, BACKBTN_H = 16.0, 14.0, 124.0, 36.0


def find_by_pid(pid):
    u = ctypes.windll.user32
    hit = []
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(h, _):
        wp = ctypes.c_ulong()
        u.GetWindowThreadProcessId(h, ctypes.byref(wp))
        if wp.value == pid:
            b = ctypes.create_unicode_buffer(256)
            u.GetClassNameW(h, b, 256)
            if b.value == "PvZClassC":
                hit.append(h)
        return True

    u.EnumWindows(proto(cb), 0)
    return hit[0] if hit else 0


class Game:
    def __init__(self):
        os.makedirs(FR, exist_ok=True)
        for f in os.listdir(FR):
            os.remove(os.path.join(FR, f))
        subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)
        shutil.copy(os.path.join(D, "pvz_save.dat"), TMP)
        env = dict(os.environ)
        env["PVZ_DUMPDIR"] = FR
        env["PVZ_GDI"] = "1"
        env["PVZ_WINDOWED"] = "1"
        env["PVZ_SAVE_FILE"] = TMP          # 存档隔离，别碰真档
        self.p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
        time.sleep(5.0)
        self.u = ctypes.windll.user32
        self.hwnd = find_by_pid(self.p.pid)
        if not self.hwnd:
            self.p.kill()
            sys.exit("[X] 找不到窗口")

    def state(self):
        sp = os.path.join(FR, "stat30.txt")
        if not os.path.exists(sp):
            return None
        t = open(sp, encoding="utf-8", errors="replace").read()
        m = re.search(r"^state=(\d+)", t, re.M)
        return int(m.group(1)) if m else None

    def dump(self, wait=1.4):
        self.key(0x50, wait)               # P
        return self.state()

    def key(self, c, w=0.5):
        self.u.PostMessageW(self.hwnd, 0x100, c, 0)
        self.u.PostMessageW(self.hwnd, 0x101, c, 0)
        time.sleep(w)

    def rclick(self, lx=500, ly=400):
        cr = ctypes.create_string_buffer(16)
        self.u.GetClientRect(self.hwnd, cr)
        cw, ch = struct.unpack("<ii", cr.raw[8:16])
        sc = min(cw / 1000.0, ch / 650.0)
        vw, vh = int(1000 * sc), int(650 * sc)
        ox, oy = (cw - vw) // 2, (ch - vh) // 2
        wx = ox + int(lx * vw / 1000.0)
        wy = oy + int(ly * vh / 650.0)
        lp = (wy << 16) | (wx & 0xFFFF)
        self.u.PostMessageW(self.hwnd, 0x204, 2, lp)      # WM_RBUTTONDOWN
        self.u.PostMessageW(self.hwnd, 0x205, 0, lp)      # WM_RBUTTONUP
        time.sleep(0.6)

    def lclick(self, lx, ly):
        cr = ctypes.create_string_buffer(16)
        self.u.GetClientRect(self.hwnd, cr)
        cw, ch = struct.unpack("<ii", cr.raw[8:16])
        sc = min(cw / 1000.0, ch / 650.0)
        vw, vh = int(1000 * sc), int(650 * sc)
        ox, oy = (cw - vw) // 2, (ch - vh) // 2
        wx = ox + int(lx * vw / 1000.0)
        wy = oy + int(ly * vh / 650.0)
        lp = (wy << 16) | (wx & 0xFFFF)
        self.u.PostMessageW(self.hwnd, 0x200, 0, lp)
        time.sleep(0.08)
        self.u.PostMessageW(self.hwnd, 0x201, 1, lp)
        self.u.PostMessageW(self.hwnd, 0x202, 0, lp)
        time.sleep(0.6)

    def close(self):
        try:
            self.p.terminate()
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()


def goto_menu(g):
    """确保回到主菜单：连按 Esc 直到 state==0（最多 5 次）"""
    for _ in range(5):
        if g.dump() == ST["MENU"]:
            return True
        g.key(0x1B, 0.4)
    return g.dump() == ST["MENU"]


def main():
    ok = True
    g = Game()

    # ---------- 0) 先确认起点在主菜单 ----------
    st = g.dump()
    print("启动状态：%s" % ST_NAME.get(st, st))
    if st != ST["MENU"]:
        print("✗ 启动不在主菜单，后续测试无意义")
        g.close()
        return 1

    # ---------- 逐界面 × 逐路径 ----------
    # 进入各元界面的方式：主菜单第一排按钮 + 每日营地
    ENTRIES = [
        ("选择关卡", (170, 353)),
        ("编组",     (385, 353)),
        ("天赋树",   (535, 353)),
        ("成就",     (685, 353)),
        ("抽卡",     (865, 353)),
        ("每日营地", (500, 419)),
    ]

    def reopen(lxly):
        """点主菜单按钮进界面"""
        goto_menu(g)
        g.lclick(*lxly)

    print()
    print("=" * 74)
    print("① 返回按钮（点左上角）")
    print("=" * 74)
    for name, pt in ENTRIES:
        reopen(pt)
        before = g.dump()
        if before == ST["MENU"]:
            print("  ✗ %-8s 没能进入该界面（点击坐标可能过时）" % name)
            ok = False
            continue
        g.lclick(BACKBTN_X + BACKBTN_W / 2, BACKBTN_Y + BACKBTN_H / 2)
        after = g.dump()
        good = after == ST["MENU"]
        print("  %s %-8s %s → %s" % ("✔" if good else "✗", name,
                                     ST_NAME.get(before, before), ST_NAME.get(after, after)))
        if not good:
            ok = False

    print()
    print("=" * 74)
    print("② Esc 键")
    print("=" * 74)
    for name, pt in ENTRIES:
        reopen(pt)
        before = g.dump()
        if before == ST["MENU"]:
            print("  ✗ %-8s 没能进入该界面" % name)
            ok = False
            continue
        g.key(0x1B, 0.6)                  # VK_ESCAPE
        after = g.dump()
        good = after == ST["MENU"]
        print("  %s %-8s %s → %s" % ("✔" if good else "✗", name,
                                     ST_NAME.get(before, before), ST_NAME.get(after, after)))
        if not good:
            ok = False

    print()
    print("=" * 74)
    print("③ 鼠标右键")
    print("=" * 74)
    for name, pt in ENTRIES:
        reopen(pt)
        before = g.dump()
        if before == ST["MENU"]:
            print("  ✗ %-8s 没能进入该界面" % name)
            ok = False
            continue
        g.rclick()
        after = g.dump()
        good = after == ST["MENU"]
        print("  %s %-8s %s → %s" % ("✔" if good else "✗", name,
                                     ST_NAME.get(before, before), ST_NAME.get(after, after)))
        if not good:
            ok = False

    print()
    print("=" * 74)
    print("④ Esc 在战斗中弹出暂停菜单，再按继续")
    print("=" * 74)
    goto_menu(g)
    g.key(0x70, 1.2)                      # F1 → 战斗
    st_play = g.dump()
    g.key(0x1B, 0.6)                      # Esc -> 暂停菜单
    st_after = g.dump()
    good1 = (st_play == ST["PLAY"] and st_after == ST["PAUSE"])
    print("  %s 战斗中按 Esc：PLAY → PAUSE" % ("✔" if good1 else "✗"))
    if not good1:
        ok = False

    st_pause = st_after
    g.key(0x1B, 0.6)                      # Esc 应该继续游戏
    st_resume = g.dump()
    good2 = (st_pause == ST["PAUSE"] and st_resume == ST["PLAY"])
    print("  %s 暂停中按 Esc：%s → %s（应恢复战斗）"
          % ("✔" if good2 else "✗", ST_NAME.get(st_pause, st_pause),
             ST_NAME.get(st_resume, st_resume)))
    if not good2:
        ok = False

    alive = (g.p.poll() is None)
    g.close()
    print()
    print("进程存活 = %s" % alive)
    print("结论：" + ("✔ 返回导航三条路径全部正常" if ok and alive
                     else "✗ 有问题，见上方 ✗"))
    return 0 if (ok and alive) else 1


if __name__ == "__main__":
    sys.exit(main())
