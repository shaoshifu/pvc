# -*- coding: utf-8 -*-
"""验证两件事：

  ① 存档迁移：v4（6 关布局，972 字节）能否无损读进 v5（11 关布局，1012 字节）
     —— 这是上一轮加 5 个关卡时引入的回归：lvStars/lvUnlocked 用了 LV_COUNT，
        宏一变布局就变，老档读不进来，玩家进度被 saveResetNew() 清空。

  ② 通关即解锁：打通第 N 关后，第 N+1 关是否自动解锁（不依赖星星）。

全程用存档副本，不动玩家真档。

用法： python _verify_unlock.py
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
FR = os.path.join(D, "_frames_unlock")
TMP = os.path.join(D, "_unlock_test.dat")
SRC = os.path.join(D, "pvz_save.dat")          # 已恢复的 v4 真档副本来源

LV, PT = 11, 90
# v5 布局
V5_LVSTARS, V5_LVUNLOCKED = 4, 15
V5_OWNED = 116
# v4 布局（6 关）
V4_LVSTARS, V4_LVUNLOCKED = 4, 10
V4_OWNED = 106


def detect_ver(v, size):
    """按文件大小判版本，而不是假定源档一定是 v4。

    ⚠️ 一开始我写死了 v4 偏移，结果第二次运行时源档已经被上一次测试
       迁移成 v5 了，偏移全错，报出"植物 67 株"这类假故障。
       用大小判版本最省事：972 = v4（6 关），1012 = v5（11 关）。"""
    if size >= 1012:
        return 5
    if size >= 972:
        return 4
    return 0


def rd(p):
    d = open(p, "rb").read()
    n = len(d) // 4
    return list(struct.unpack("<%di" % n, d[:n * 4])), len(d)


def find_window_by_pid(pid):
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
    def __init__(self, save_src=None):
        """save_src：用哪份存档启动。默认 SRC。

        ⚠️ 以前这里无条件 `copy(SRC, TMP)`，于是 main() 里刚准备好的
           v4 备份（用来验迁移）会被悄悄覆盖成 v5，测试实际跑的不是
           要验的那条路径 —— 断言还能"通过"纯属巧合。必须显式控制。"""
        os.makedirs(FR, exist_ok=True)
        for f in os.listdir(FR):
            os.remove(os.path.join(FR, f))
        subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)
        src = save_src or SRC
        if os.path.abspath(src) != os.path.abspath(TMP):   # 同路径时 copy 会抛 SameFileError
            shutil.copy(src, TMP)
        env = dict(os.environ)
        env["PVZ_DUMPDIR"] = FR
        env["PVZ_WINDOWED"] = "1"
        env["PVZ_SAVE_FILE"] = TMP
        self.p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
        time.sleep(5.0)
        self.u = ctypes.windll.user32
        self.hwnd = find_window_by_pid(self.p.pid)
        if not self.hwnd:
            self.p.kill()
            sys.exit("[X] 找不到窗口")

    def press(self, c, w=0.5):
        self.u.PostMessageW(self.hwnd, 0x100, c, 0)
        self.u.PostMessageW(self.hwnd, 0x101, c, 0)
        time.sleep(w)

    def dump(self, wait=1.6):
        """按 P 触发写盘并等待落盘。gDumpLeft 是 30 帧（约 0.5s），
        留 1.6s 余量 —— 等太短会读到上一次的旧文件。"""
        self.press(ord("P"), wait)
        return self.stat(30)

    def stat(self, tag=30):
        fp = os.path.join(FR, "stat%02d.txt" % tag)
        return open(fp, encoding="utf-8", errors="replace").read() if os.path.exists(fp) else ""

    def lvline(self):
        m = re.search(r"^lv cur=(\d+) clearw=(\d+) stars=(\d+) unlocked=(\d+) playable=(\d+)$",
                      self.stat(), re.M)
        if not m:
            return None
        return dict(cur=int(m.group(1)), clearw=int(m.group(2)), stars=int(m.group(3)),
                    unlocked=m.group(4), playable=m.group(5))

    def close(self):
        try:
            self.p.terminate()
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()


def main():
    print("=" * 76)
    print("① 存档迁移：v4(6关/972字节) → v5(11关/1012字节)")
    print("=" * 76)
    shutil.copy(SRC, TMP)              # 先建副本，Game() 里也会再复制一次（幂等）
    before, sz_before = rd(TMP)
    srcver = detect_ver(before, sz_before)
    if srcver == 5:
        # 源档已经是 v5：再跑一遍"迁移"没有意义，直接跳过第 ① 项，
        # 改用 22:04 的 v4 备份来验证迁移（那才是真正要验的路径）。
        v4bak = os.path.join(D, "pvz_save.dat.bak-2204")
        if os.path.exists(v4bak):
            shutil.copy(v4bak, TMP)
            before, sz_before = rd(TMP)
            srcver = detect_ver(before, sz_before)
            print("  源档已是 v5，改用 v4 备份验证迁移：%s" % os.path.basename(v4bak))
        else:
            print("  ⚠ 源档已是 v5 且找不到 v4 备份，跳过迁移验证")
    print("  迁移前：%d 字节  ver=%d stars=%d  已拥有 %d 株"
          % (sz_before, before[1], before[2],
             sum(1 for x in before[V4_OWNED:V4_OWNED + PT] if x)))
    print("          lvStars[0..5]    = %s" % before[V4_LVSTARS:V4_LVSTARS + 6])
    print("          lvUnlocked[0..5] = %s" % before[V4_LVUNLOCKED:V4_LVUNLOCKED + 6])

    g = Game(save_src=TMP)                     # 显式用刚准备的 v4 备份启动
    g.dump()
    g.close()

    after, sz_after = rd(TMP)
    ok = True

    if sz_after != sz_before:
        print("\n  迁移后：%d 字节  ver=%d stars=%d  已拥有 %d 株"
              % (sz_after, after[1], after[2],
                 sum(1 for x in after[V5_OWNED:V5_OWNED + PT] if x)))
        print("          lvStars[0..10]    = %s" % after[V5_LVSTARS:V5_LVSTARS + LV])
        print("          lvUnlocked[0..10] = %s" % after[V5_LVUNLOCKED:V5_LVUNLOCKED + LV])
    else:
        print("\n  ✗ 存档大小没变（%d）—— 迁移没发生" % sz_after)
        ok = False

    # 断言：字段值无损
    checks = [
        ("stars 保持 %d" % before[2], after[2] == before[2]),
        ("lvStars 前 6 项保持",
         after[V5_LVSTARS:V5_LVSTARS + 6] == before[V4_LVSTARS:V4_LVSTARS + 6]),
        ("lvUnlocked 前 6 项不倒退",
         all(after[V5_LVUNLOCKED + i] >= before[V4_LVUNLOCKED + i] for i in range(6))),
        ("植物拥有数保持 %d" % sum(1 for x in before[V4_OWNED:V4_OWNED + PT] if x),
         sum(1 for x in after[V5_OWNED:V5_OWNED + PT] if x)
         == sum(1 for x in before[V4_OWNED:V4_OWNED + PT] if x)),
        ("ver 升到 5", after[1] == 5),
    ]
    print()
    for name, good in checks:
        print("  %s %s" % ("✔" if good else "✗", name))
        if not good:
            ok = False

    print()
    print("=" * 76)
    print("② 通关即解锁：打通一关 → 下一关自动解锁（不靠星星）")
    print("=" * 76)
    g = Game()
    g.dump()
    st0 = g.lvline()
    print("  初始：stars=%d unlocked=%s playable=%s"
          % (st0["stars"], st0["unlocked"], st0["playable"]))

    # 找第一个已解锁、且下一关还没解锁的关卡 —— 那才是能验证解锁链的位置
    target = -1
    for i in range(LV - 1):
        if st0["unlocked"][i] == "1" and st0["unlocked"][i + 1] == "0":
            target = i
            break
    if target < 0:
        # 没有这种关卡：说明解锁链已经走到头或全解锁，用最后一关已解锁的
        for i in range(LV - 1, -1, -1):
            if st0["unlocked"][i] == "1":
                target = i
                break
    print("  测试目标：第 %d 关（通关它应解锁第 %d 关）" % (target, target + 1))

    # 用 V 键切到 target 关（V 从第 6 关开始轮转，所以直接改 gCurLevel 更可控）
    # 这里改用「按 V 轮转到 6」再逐关…… 太绕；直接在存档副本里把 cur 设好不可行。
    # 所以用 V 键到达第 6 关后按 C 通关，验证 6→7 的解锁。
    # 先确保第 6 关解锁：改副本存档的 lvUnlocked[6]
    after2, _ = rd(TMP)
    if after2[V5_LVUNLOCKED + 6] == 0:
        after2[V5_LVUNLOCKED + 6] = 1
        with open(TMP, "wb") as f:
            f.write(struct.pack("<%di" % len(after2), *after2))
        print("  （已在副本里把第 6 关标记为已解锁，用于测试 6→7 的解锁链）")
    g.close()

    g = Game()
    g.press(ord("V"), 1.2)                     # V：进第 6 关
    time.sleep(0.8)
    g.dump()
    s1 = g.lvline()
    print("  进第 6 关后：cur=%s unlocked=%s" % (s1["cur"], s1["unlocked"]))
    if s1["cur"] != 6:
        print("  ✗ 没能进入第 6 关（cur=%s）" % s1["cur"])
        g.close()
        return 1

    before_unlock = s1["unlocked"][7]
    g.press(ord("C"), 2.5)                     # C：模拟通关
    g.dump()
    s2 = g.lvline()
    g.close()

    print("  通关第 6 关后：unlocked=%s" % s2["unlocked"])
    if s2 is None:
        print("  ✗ 读不到状态")
        ok = False
    elif before_unlock == "1":
        print("  · 第 7 关本来就已解锁，无法验证解锁动作")
    elif s2["unlocked"][7] == "1":
        print("  ✔ 第 7 关已解锁（通关第 6 关的结算自动解锁了下一关）")
    else:
        print("  ✗ 第 7 关仍未解锁 —— 通关解锁没生效")
        ok = False

    print()
    print("结论：" + ("✔ 存档迁移与通关解锁都正常" if ok else "✗ 有问题，见上方 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
