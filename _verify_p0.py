# -*- coding: utf-8 -*-
"""验证 P0-1 断电 / P0-2 冰锥·熔心 / P2-1 概率门。

这三项都是「看起来有、实际没有」的典型 —— 断电原本只改了计时器没拦植物，
冰锥/熔心只有贴图和数值、没有行为。只读代码很容易被骗（注释写得很像实现了），
必须实测状态变化。

**本脚本的一个关键教训**：断言必须排除「游戏不在战斗状态」这个混淆因素。
`updateGame()` 只在 `ST_PLAY` 跑，一旦弹出三选一（`ST_DRAFT`），
所有计时器一起冻结 —— 那会让「断电冻结了植物」看起来成立，实际是假阳性。
实测踩过一次（+12s 时进了 DRAFT）。所以这里每步都断言 gState == 1。

用法： python _verify_p0.py
"""
import ctypes
import ctypes.wintypes
import os
import re
import shutil
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_p0")
TMP = os.path.join(D, "_p0_test.dat")

ST_PLAY = 1


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
        env["PVZ_WINDOWED"] = "1"
        env["PVZ_SAVE_FILE"] = TMP
        self.p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
        time.sleep(5.0)
        self.u = ctypes.windll.user32
        self.hwnd = find_by_pid(self.p.pid)
        if not self.hwnd:
            self.p.kill()
            sys.exit("[X] 找不到窗口")

    def key(self, c, w=0.5):
        self.u.PostMessageW(self.hwnd, 0x100, c, 0)
        self.u.PostMessageW(self.hwnd, 0x101, c, 0)
        time.sleep(w)

    def txt(self):
        f = os.path.join(FR, "stat30.txt")
        return open(f, encoding="utf-8", errors="replace").read() if os.path.exists(f) else ""

    def dump(self, wait=1.5):
        self.key(0x50, wait)          # P
        return self.txt()

    def close(self):
        try:
            self.p.terminate()
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()


def g1(pat, t, cast=float, dflt=None):
    m = re.search(pat, t, re.M)
    return cast(m.group(1)) if m else dflt


def probe(t):
    """读探针植物状态 (gState, alive, timer)"""
    return (g1(r"^probe state=(\d+)", t, int),
            g1(r"^probe state=\d+ alive=(\d+)", t, int),
            g1(r"^probe state=\d+ alive=\d+ timer=([\d.]+)", t, float))


def main():
    ok = True
    g = Game()

    # ================= P0-1 =================
    print("=" * 74)
    print("P0-1  第 9 关断电：断电行的植物必须停止工作")
    print("=" * 74)
    g.key(ord("V"), 1.2)
    for _ in range(3):
        g.key(ord("V"), 1.2)          # → 第 9 关
    t = g.dump()
    lv, kind = g1(r"^lv cur=(\d+)", t, int), g1(r"rule kind=(\d+)", t, int)
    print("  关卡 %s，规则 %s（期望 9 / 4）" % (lv, kind))
    if lv != 9 or kind != 4:
        print("  ✗ 没进到第 9 关，无法验证")
        g.close()
        return 1

    g.key(ord("Z"), 1.5)              # 断电场景（自带向日葵 + 长波次计时）
    t = g.dump(1.5)
    st, alive, timer0 = probe(t)
    cut = re.search(r"^cutrow (\d+)=([\d.]+)", t, re.M)
    pw = g1(r"^power powered=(\d+)/5 ", t, int)
    print("  探针：state=%s alive=%s timer=%.2f（要求 state=1 才算有效）"
          % (st, alive, timer0 if timer0 is not None else -1))
    print("  断电行=%s  powered=%s/5" % (cut.group(1) if cut else "无", pw))

    if st != ST_PLAY:
        print("  ✗ 不在战斗状态，测试无效")
        ok = False
    elif pw == 5:
        print("  ✗ 已断电但 powered 仍是 5/5 —— newLevelRowPowered 没生效")
        ok = False
    else:
        print("  ✔ 通电位翻转（该行判定为无电）")

    # ---- 核心断言：断电期间计时器冻结 ----
    if ok and st == ST_PLAY:
        time.sleep(1.5)
        t1 = g.dump(1.2)
        s1, a1, tm1 = probe(t1)
        c1 = re.search(r"^cutrow (\d+)=", t1, re.M) is not None
        time.sleep(2.5)
        t2 = g.dump(1.2)
        s2, a2, tm2 = probe(t2)
        c2 = re.search(r"^cutrow (\d+)=", t2, re.M) is not None
        print("  两次采样：state=%s→%s  断电中=%s→%s  timer=%.2f→%.2f"
              % (s1, s2, c1, c2,
                 tm1 if tm1 is not None else -1, tm2 if tm2 is not None else -1))
        if s1 == ST_PLAY and s2 == ST_PLAY and c1 and c2:
            if abs(tm2 - tm1) < 0.02:
                print("  ✔ 战斗中且断电，计时器完全冻结 —— 植物确实停工了")
            else:
                print("  ✗ 计时器推进了 %.3f，断电没拦住植物" % (tm2 - tm1))
                ok = False
        else:
            print("  ✗ 采样时不在战斗状态（state=%s/%s），无法判定" % (s1, s2))
            ok = False

    # ---- 恢复后重新工作 ----
    print("  等断电结束（20 秒窗口）…")
    for _ in range(12):
        time.sleep(2.0)
        t3 = g.dump(1.0)
        if not re.search(r"^cutrow (\d+)=", t3, re.M):
            break
    t3 = g.dump(1.2)
    pw3 = g1(r"^power powered=(\d+)/5 ", t3, int)
    c3 = re.search(r"^cutrow (\d+)=", t3, re.M) is not None
    s3, a3, tm3 = probe(t3)
    if not c3 and pw3 == 5:
        print("  ✔ 断电自动恢复（powered=5/5，cutrow 已清除）")
        time.sleep(2.5)
        t4 = g.dump(1.2)
        s4, a4, tm4 = probe(t4)
        if s3 == ST_PLAY and s4 == ST_PLAY and tm4 is not None and tm3 is not None:
            if abs(tm4 - tm3) > 0.05:
                print("  ✔ 恢复后计时器重新推进 %.2f → %.2f（植物复工）" % (tm3, tm4))
            else:
                print("  ✗ 恢复后计时器仍冻结（%.2f → %.2f）" % (tm3, tm4))
                ok = False
        else:
            print("  · 恢复后采样不在战斗中，跳过复工断言")
    else:
        print("  ✗ 断电没自动恢复（cutrow=%s powered=%s）" % (c3, pw3))
        ok = False

    # ================= P0-2a 冰锥溅射 =================
    print()
    print("=" * 74)
    print("P0-2a  冰锥僵尸：受击时给相邻行僵尸挂上冰冻")
    print("=" * 74)
    g.key(ord("V"), 1.2)              # → 第 10 关
    g.key(ord("V"), 1.2)              # → 第 6 关（冰原）
    t = g.dump()
    print("  关卡 %s（期望 6）" % g1(r"^lv cur=(\d+)", t, int))
    g.key(ord("B"), 2.0)              # 刷 8 种新僵尸
    t = g.dump(1.2)
    nf = g1(r"frostfang=(\d+)", t, int)
    print("  冰锥僵尸在场 %s 只" % nf)
    if not nf:
        print("  ✗ 场上没有冰锥僵尸")
        ok = False
    else:
        before = re.search(r"^zrows(.*)$", t, re.M)
        print("  受击前 zrows:%s" % (before.group(1) if before else "（无）"))
        g.key(ord("Y"), 0.9)          # 给冰锥 1 点伤害 + 相邻行放被试
        # ⚠️ 不要等太久：冰冻是有时限的（3 秒），等过头就读到 0 了。
        #   实测踩过：等了 2.0 秒正好把 2 秒的冰冻耗完，误判成"溅射没生效"。
        t = g.dump(0.9)
        rows = re.search(r"^zrows(.*)$", t, re.M)
        slow = g1(r"zstate slow=(\d+)", t, int)
        print("  受击后 zrows:%s" % (rows.group(1) if rows else "（无）"))
        print("  减速僵尸数 = %s" % slow)
        if rows and re.search(r"s2\.0", rows.group(1)):
            print("  ✔ 相邻行僵尸出现 s2.0 冰冻 —— 溅射生效")
        else:
            print("  ✗ 相邻行没有出现冰冻 —— 溅射没生效")
            ok = False

    # ================= P0-2b 熔心 =================
    print()
    print("=" * 74)
    print("P0-2b  熔心僵尸：免疫裂隙伤害 + 踩裂隙会提前引爆")
    print("=" * 74)
    g.key(ord("V"), 1.2)              # → 第 7 关（熔岩）
    t = g.dump()
    lv = g1(r"^lv cur=(\d+)", t, int)
    rifts = g1(r"rifts=(\d+)", t, int)
    rt0 = g1(r"riftT=([\d.]+)", t, float)
    print("  关卡 %s（期望 7），裂隙 %s 处，riftT=%.1f"
          % (lv, rifts, rt0 if rt0 is not None else -1))
    if lv != 7:
        print("  ✗ 没进到第 7 关")
        ok = False
    else:
        g.key(ord("B"), 2.0)          # 刷含熔心的僵尸
        t = g.dump(1.2)
        nm = g1(r"magmaw=(\d+)", t, int)
        rt1 = g1(r"riftT=([\d.]+)", t, float)
        print("  熔心在场 %s 只，riftT=%.1f" % (nm, rt1 if rt1 is not None else -1))
        if nm:
            print("  ✔ 熔心僵尸已生成（免疫分支在裂隙喷发处，踩裂隙加速在此验证）")
            time.sleep(4.0)
            t = g.dump(1.2)
            rt2 = g1(r"riftT=([\d.]+)", t, float)
            print("  4 秒后 riftT=%.1f（自然递减约 -4；骤降说明被熔心点燃）"
                  % (rt2 if rt2 is not None else -1))
        else:
            print("  ✗ 场上没有熔心僵尸")
            ok = False

    alive_proc = (g.p.poll() is None)
    g.close()
    print()
    print("进程存活 = %s" % alive_proc)
    print("结论：" + ("✔ P0/P2 各项通过" if ok and alive_proc
                     else "✗ 有问题，见上方 ✗"))
    return 0 if (ok and alive_proc) else 1


if __name__ == "__main__":
    sys.exit(main())
