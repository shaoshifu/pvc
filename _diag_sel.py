# -*- coding: utf-8 -*-
"""排查「植物没选却出现选取」：实测各条进入战斗路径下 gSel 的值。

gSel 是战斗中的卡片选中项（值为植物 id，未选中时必须是 -1）。
卡片高亮的判据是 `gSel == selId`，所以只要 gSel 在进入战斗时不是 -1，
就会出现"没人点过、卡片却亮着"。

测四条路径：
  A. 直接 F1 进战斗（不重置）        —— 看 gSel 初值
  B. 从选关界面点关卡进战斗（走 resetGame）
  C. 在菜单界面按数字键后再进战斗    —— 验证键盘分支缺状态判断的 bug
  D. 进战斗后点击卡片 → 读 gSel      —— 确认正常选中路径

用法： python _diag_sel.py
"""
import ctypes
import ctypes.wintypes
import os
import re
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_sel")


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


def read_stat(tag=30):
    p = os.path.join(FR, "stat%02d.txt" % tag)
    if not os.path.exists(p):
        return {}
    txt = open(p, encoding="utf-8", errors="replace").read()
    out = {}
    for key, pat in (("state", r"state=(\d+)"),
                     ("gSel", r"gSel=(-?\d+)"),
                     ("shovel", r"shovel=(\d+)"),
                     ("N", r"N=(\d+)"),
                     ("bonus", r"bonus=(\d+)"),
                     ("rg", r"rg=(\d+)"),
                     ("sun", r"sun=(\d+)"),
                     ("cd0", r"cd0=([\d.]+)"),
                     ("cd1", r"cd1=([\d.]+)")):
        m = re.search(pat, txt)
        out[key] = m.group(1) if m else "?"
    m = re.search(r"^loadout ([\d ]+)$", txt, re.M)
    out["loadout"] = m.group(1).split() if m else []
    return out


class Game:
    def __init__(self):
        os.makedirs(FR, exist_ok=True)
        for f in os.listdir(FR):
            os.remove(os.path.join(FR, f))
        subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)
        r = subprocess.run([sys.executable, "_mk_framedump.py"], cwd=D,
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("[X] 生成失败：" + (r.stderr or "")[-400:])
        env = dict(os.environ)
        env["PVZ_DUMPDIR"] = FR
        env["PVZ_WINDOWED"] = "1"
        self.p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
        time.sleep(5.0)
        self.u = ctypes.windll.user32
        self.u.SetProcessDPIAware()
        self.hwnd = find_window_by_pid(self.p.pid)
        if not self.hwnd:
            self.p.kill()
            sys.exit("[X] 找不到窗口")

    def key(self, c):
        self.u.PostMessageW(self.hwnd, 0x100, c, 0)
        self.u.PostMessageW(self.hwnd, 0x101, c, 0)
        time.sleep(0.35)

    def click(self, lx, ly):
        """点逻辑坐标（1000×650 那套），自动换算到当前窗口客户区"""
        rc = ctypes.wintypes.RECT()
        self.u.GetClientRect(self.hwnd, ctypes.byref(rc))
        cw, ch = rc.right, rc.bottom
        sc = min(cw / 1000.0, ch / 650.0)
        vw, vh = int(1000 * sc), int(650 * sc)
        ox, oy = (cw - vw) // 2, (ch - vh) // 2
        wx = ox + int(lx * vw / 1000.0)
        wy = oy + int(ly * vh / 650.0)
        lp = (wy << 16) | (wx & 0xFFFF)
        self.u.PostMessageW(self.hwnd, 0x200, 0, lp)
        time.sleep(0.12)
        self.u.PostMessageW(self.hwnd, 0x201, 1, lp)
        self.u.PostMessageW(self.hwnd, 0x202, 0, lp)
        time.sleep(0.5)

    def dump(self):
        """按 P 只写盘、不改状态，然后读回"""
        self.key(ord('P'))
        time.sleep(0.5)
        return read_stat(30)

    def close(self):
        try:
            self.p.terminate()
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()


def fmt(s):
    if not s:
        return "?"
    return ("state=%s gSel=%s shovel=%s sun=%s | N=%s bonus=%s rg=%s "
            "loadout=[%s] cd0=%s cd1=%s"
            % (s.get("state"), s.get("gSel"), s.get("shovel"), s.get("sun"),
               s.get("N"), s.get("bonus"), s.get("rg"),
               ",".join(s.get("loadout") or []), s.get("cd0"), s.get("cd1")))


def main():
    print("=" * 78)
    print("路径 A：直接 F1 进战斗（不经过 resetGame）")
    g = Game()
    g.key(0x70)                      # F1
    time.sleep(0.8)
    print("   ", fmt(g.dump()))

    print()
    print("路径 C：在【非战斗界面】按数字键 1 / 2（键盘分支是否误设 gSel）")
    g = Game()
    print("    启动后（应停在菜单）:", fmt(g.dump()))
    before = g.dump()
    g.key(ord('1'))
    g.key(ord('2'))
    time.sleep(0.3)
    after = g.dump()
    print("    按数字键后       :", fmt(after))
    if before.get("state") != "1":
        if after.get("gSel") == "-1":
            print("    ✔ 非战斗界面按数字键不再污染 gSel（保持 -1）")
        else:
            print("    ✗ BUG 仍在：按数字键把 gSel 改成了 %s" % after.get("gSel"))
    g.close()

    print()
    print("=" * 78)
    print("路径 B：选关界面点第 0 关进战斗（走 resetGame）")
    g = Game()
    g.key(0x73)                      # F4? 用 F6 进选关更稳
    g.key(0x75)                      # F6 -> ST_LEVELS
    time.sleep(0.5)
    before = g.dump()
    print("    进关前:", fmt(before))
    # 选关界面第 0 关的卡片位置：点左上角那张
    g.click(180, 200)
    time.sleep(1.0)
    after = g.dump()
    print("    进关后:", fmt(after))
    if after.get("gSel") not in ("-1", "?"):
        print("    ⚠️ gSel 非 -1！这就是「没选却出现选取」")
    g.close()

    print()
    print("=" * 78)
    print("路径 D：战斗中点击卡片（正常选中路径）")
    g = Game()
    g.key(0x70)                      # F1
    time.sleep(0.5)
    g.click(185, 45)                 # 第一张卡
    time.sleep(0.3)
    d = g.dump()
    print("    点第1张卡:", fmt(d))
    if d.get("gSel") == "0":
        print("    ✔ 战斗中选卡正常（gSel 变成第一张卡的植物 id）")
    else:
        print("    ✗ 战斗中选卡没生效（gSel=%s）" % d.get("gSel"))

    print()
    print("路径 E：选中卡片后切到非战斗界面（选中态应被作废）")
    g.key(0x1B)                      # ESC
    time.sleep(0.4)
    e = g.dump()
    print("    ESC 之后:", fmt(e))
    if e.get("gSel") == "-1":
        print("    ✔ 切界面后选中态已作废")
    else:
        print("    ✗ 选中态跨界面残留了（gSel=%s）" % e.get("gSel"))
    g.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
