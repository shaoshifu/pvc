# -*- coding: utf-8 -*-
"""验证五个新关卡：能不能进、规则有没有真的装上。

为什么要专门验这个：关卡数据（名字/波数/倍率）加错了编译不会报错，
规则挂钩写错了也只是"没效果"—— 都属于静默失败。必须逐关进一次、
把规则状态读出来对照，否则很可能交付一个"看起来有 11 关、
实际后 5 关和第 5 关一模一样"的假成果。

用法： python _verify_levels.py
"""
import ctypes
import ctypes.wintypes
import os
import re
import struct
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_lvver")
SAVE = os.path.join(D, "_lvver_save.dat")

# 期望：关卡号 -> (规则 id, 规则名, 断言函数)
EXPECT = {
    6:  (1, "冰面击退 + 薄冰格", lambda d: d["thinice"] == 6),
    7:  (2, "岩浆裂隙",         lambda d: d["rifts"] == 3),
    8:  (3, "迷雾视野",         lambda d: d["fogcol"] in (5, 7)),
    9:  (4, "供电连链",         lambda d: d["kind"] == 4),
    10: (5, "三阶段终局",       lambda d: d["phase"] >= 1),
}


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


def parse(txt):
    d = {}
    m = re.search(r"rule kind=(\d+) level=(\d+) phase=(\d+) thinice=(\d+) "
                  r"rifts=(\d+) heat=([\d.]+) fogcol=(\d+) grava=([\d.]+)", txt)
    if m:
        d = dict(kind=int(m.group(1)), level=int(m.group(2)), phase=int(m.group(3)),
                 thinice=int(m.group(4)), rifts=int(m.group(5)),
                 heat=float(m.group(6)), fogcol=int(m.group(7)),
                 grava=float(m.group(8)))
    return d


def main():
    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)
    r = subprocess.run([sys.executable, "_mk_framedump.py"], cwd=D,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("[X] 生成失败：" + (r.stderr or "")[-400:])

    # 用一个"全解锁"的存档，省得被 starReq 拦住
    if not os.path.exists(SAVE):
        import shutil
        src = os.path.join(D, "pvz_save.dat")
        shutil.copy(src, SAVE)
    # 把星星改高，让新关卡全部可进
    raw = bytearray(open(SAVE, "rb").read())
    struct.pack_into("<i", raw, 8, 999)          # stars 字段（magic,ver,stars,coins）
    open(SAVE, "wb").write(bytes(raw))

    env = dict(os.environ)
    env["PVZ_DUMPDIR"] = FR
    env["PVZ_WINDOWED"] = "1"
    env["PVZ_SAVE_FILE"] = SAVE

    p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
    time.sleep(5.0)
    u = ctypes.windll.user32
    hwnd = find_by_pid(p.pid)
    if not hwnd:
        p.kill()
        sys.exit("[X] 找不到窗口")

    def press(c):
        u.PostMessageW(hwnd, 0x100, c, 0)
        u.PostMessageW(hwnd, 0x101, c, 0)
        time.sleep(0.4)

    def click(lx, ly):
        cr = ctypes.create_string_buffer(16)
        u.GetClientRect(hwnd, cr)
        cw, ch = struct.unpack("<ii", cr.raw[8:16])
        sc = min(cw / 1000.0, ch / 650.0)
        vw, vh = int(1000 * sc), int(650 * sc)
        ox, oy = (cw - vw) // 2, (ch - vh) // 2
        wx = ox + int(lx * vw / 1000.0)
        wy = oy + int(ly * vh / 650.0)
        lp = (wy << 16) | (wx & 0xFFFF)
        u.PostMessageW(hwnd, 0x200, 0, lp)
        time.sleep(0.08)
        u.PostMessageW(hwnd, 0x201, 1, lp)
        u.PostMessageW(hwnd, 0x202, 0, lp)
        time.sleep(0.55)

    LV_CARD_W, LV_CARD_H = 292.0, 96.0
    LV_X0, LV_Y0, GX, GY = 58.0, 108.0, 12.0, 12.0

    def level_xy(i):
        return (LV_X0 + (i % 3) * (LV_CARD_W + GX) + LV_CARD_W * 0.5,
                LV_Y0 + (i // 3) * (LV_CARD_H + GY) + LV_CARD_H * 0.5)

    print("按 V 依次进入第 6~10 关，读规则状态\n")
    ok = True
    for lv in range(6, 11):
        press(ord("V"))               # 直接切到下一个新关卡（绕开 UI 点击）
        time.sleep(1.2)
        press(ord("P"))               # 只写盘、不改状态
        time.sleep(0.7)
        sp = os.path.join(FR, "stat30.txt")
        d = parse(open(sp, encoding="utf-8", errors="replace").read()) if os.path.exists(sp) else {}
        want_kind, name, check = EXPECT[lv]
        if not d:
            print("  ✗ 关卡 %d（%s）：读不到规则状态" % (lv, name))
            ok = False
            continue
        if d["level"] != lv:
            print("  ✗ 关卡 %d（%s）：实际进入了关卡 %d" % (lv, name, d["level"]))
            ok = False
            continue
        good = (d["kind"] == want_kind) and check(d)
        print("  %s 关卡 %-2d %-16s rule=%d(期望%d) phase=%d 薄冰=%d 裂隙=%d "
              "迷雾列=%d 热浪=%.2f"
              % ("✔" if good else "✗", lv, name, d["kind"], want_kind,
                 d["phase"], d["thinice"], d["rifts"], d["fogcol"], d["heat"]))
        if not good:
            ok = False

    alive = (p.poll() is None)
    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()
    print("\n进程存活 = %s" % alive)
    print("结论：" + ("✔ 五个新关卡全部可进，规则各自生效" if ok and alive
                     else "✗ 有问题，见上方 ✗"))
    return 0 if (ok and alive) else 1


if __name__ == "__main__":
    sys.exit(main())
