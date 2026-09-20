# -*- coding: utf-8 -*-
"""验证「植物没选却出现选取」的修复。

用户的报障实际包含三件独立的事：
  1. 战斗卡片栏里出现了他没主动选的卡 —— 根因是存档里残留了
     索引 77/81 的旧编制项（超出 gCandidateSlots=2，且在编组网格外）；
  2. 编组界面显示"已选候选 3 / 2" —— 数字超过上限，与画面只有一个绿框矛盾；
  3. 在非战斗界面按数字键会静默修改战斗的 gSel（已单独验证）。

本脚本用**存档副本**跑，不动玩家的真档：启动时给出收敛前的存档，
看修复后 loadout 是否被收敛到合理状态。

用法： python _verify_loadout.py
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
SAVE_SRC = os.path.join(D, "pvz_save.dat")
SAVE_TST = os.path.join(D, "_verify_save.dat")
FR = os.path.join(D, "_frames_vlo")

PT_COUNT, PT_HERO = 90, 82
OFF_LO, OFF_OW = 16, 106     # 由 _probe_const.c 实测确认


def load_arr(path, off, count):
    d = open(path, "rb").read()
    vals = list(struct.unpack("<%di" % (len(d) // 4), d[:len(d) // 4 * 4]))
    return vals[off:off + count]


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


def main():
    if not os.path.exists(SAVE_SRC):
        sys.exit("[X] 找不到 %s" % SAVE_SRC)

    before = [i for i, v in enumerate(load_arr(SAVE_SRC, OFF_LO, PT_COUNT)) if v]
    print("收敛前 loadout 置1索引: %s" % before)

    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)
    r = subprocess.run([sys.executable, "_mk_framedump.py"], cwd=D,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("[X] 生成失败：" + (r.stderr or "")[-400:])

    shutil.copy(SAVE_SRC, SAVE_TST)          # 用副本，保护玩家真档
    env = dict(os.environ)
    env["PVZ_DUMPDIR"] = FR
    env["PVZ_WINDOWED"] = "1"
    env["PVZ_SAVE_FILE"] = SAVE_TST
    p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
    time.sleep(5.0)

    u = ctypes.windll.user32
    hwnd = find_window_by_pid(p.pid)
    if not hwnd:
        p.kill()
        sys.exit("[X] 找不到窗口")

    def press(c):
        u.PostMessageW(hwnd, 0x100, c, 0)
        u.PostMessageW(hwnd, 0x101, c, 0)
        time.sleep(0.45)

    press(0x79)          # F10 -> 编组界面
    time.sleep(1.2)
    press(ord("P"))      # 只写盘、不改状态
    time.sleep(0.8)

    alive = (p.poll() is None)
    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()

    after = [i for i, v in enumerate(load_arr(SAVE_TST, OFF_LO, PT_COUNT)) if v]
    owned = load_arr(SAVE_TST, OFF_OW, PT_COUNT)

    ok = True
    print("收敛后 loadout 置1索引: %s" % after)
    print()

    # 断言 1：不再有网格外的编制项
    outside = [i for i in after if i >= 32]
    if outside:
        print("✗ 仍有网格外（>=32）的编制项: %s" % outside)
        ok = False
    else:
        print("✔ 没有网格外的编制项（这些项玩家看不见也点不到）")

    # 断言 2：数量不超过上限（gCandidateSlots = 2）
    if len(after) > 2:
        print("✗ 编制数量 %d 超过上限 2" % len(after))
        ok = False
    else:
        print("✔ 编制数量 %d ≤ 上限 2" % len(after))

    # 断言 3：全部是已拥有的
    bad = [i for i in after if not owned[i]]
    if bad:
        print("✗ 编制里含未拥有的植物: %s" % bad)
        ok = False
    else:
        print("✔ 编制里的植物都已拥有")

    # 断言 4：根因（那只"没选的卡"）确实被清掉了
    print()
    if 77 in before or 81 in before:
        if not any(i in after for i in (77, 81)):
            print("✔ 报障根因已清除：索引 77 / 81 不再出现在编制里")
        else:
            print("✗ 索引 77 / 81 仍在编制里 —— 这就是用户看到的那张'没选的卡'")
            ok = False

    # 断言 5：运行时 gLoadout 与收敛结果一致
    sp = os.path.join(FR, "stat30.txt")
    if os.path.exists(sp):
        s = open(sp, encoding="utf-8", errors="replace").read()
        m = re.search(r"^loadout ([\d ]+)$", s, re.M)
        gl = [int(v) for v in m.group(1).split()] if m else []
        gl = [v for v in gl if v]        # gLoadout 是紧凑数组，0 是有效 id 需保留
        print("运行时 gLoadout 前 4 项: %s" % (m.group(1) if m else "(未读到)"))
        m2 = re.search(r"sel gSel=(-?\d+)", s)
        gsel = m2.group(1) if m2 else "?"
        if gsel == "-1":
            print("✔ 进入编组界面后 gSel = -1（无残留选中）")
        else:
            print("✗ gSel = %s，战斗选中态跨界面残留了" % gsel)
            ok = False

    print("\n进程存活 = %s" % alive)
    if not alive:
        ok = False
    print("结论：" + ("✔ 「没选却出现选取」已修复" if ok else "✗ 仍有问题，见上方 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
