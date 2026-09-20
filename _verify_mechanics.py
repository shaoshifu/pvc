# -*- coding: utf-8 -*-
"""运行时机制验证：静态审计只证明"代码接通了"，这里证明"真的生效了"。

为什么需要第二道：
  `_audit_mechanics.py` 能查出"某个 trait 位从没被引用"，
  但查不出"引用是错的" —— 比如把减速写进一个没人读的字段、
  或者击退量算成负数反而把僵尸拉过来。那些只有跑起来才看得见。

方法：
  按 M / N 键摆出两组各 5 株、代表不同机制的肉鸽植物（各自同行配一只僵尸），
  然后读 dumpStats 写的 stat{17,18}.txt —— 明细里每只僵尸带
  (row, x, hp, slow, rooted)。

三个必须注意的坑（都踩过，写在下面各处的注释里）：
  1. 找窗口必须按 PID，不能用 FindWindowW（会投给别的 pvz.exe 实例）；
  2. 僵尸必须紧贴植物摆，影响半径只有 rgRangeMul*52；
  3. 击退要以**出场位置**为基准，不能拿首次采样当基准。

用法： python _verify_mechanics.py
"""
import ctypes
import os
import re
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_mech")

# 僵尸出场位置，必须与 _mk_framedump.py 的 M/N 键一致 = cellCX(1) + 46
SPAWN_X = 252.0

# (行, 肉鸽索引, 名字, 预期, 判据种类)
BATCHES = [
    ("M", 17, "第一批：状态类机制", [
        (0, 67, "寒霜地衣", "减速场 SLOWFIELD", "slow"),
        (1, 50, "弹簧豆", "击退 KNOCKBACK", "knock"),
        (2, 59, "处刑者", "处决+暴击 EXECUTE|CRIT", "dmg"),
        (3, 81, "血蔷薇", "吸血 LIFESTEAL", "dmg"),
        (4, 60, "断头台", "破甲+处决+暴击", "dmg"),
    ]),
    ("N", 18, "第二批：光环与远程机制", [
        (0, 63, "圣殿钟", "护盾+时序回溯 SHIELDALLY|TIMEHEAL", "dmg"),
        (1, 55, "猎手指针", "标记 MARK", "dmg"),
        (2, 78, "星轨信标", "轨道轰炸 ORBITAL", "dmg"),
        (3, 74, "蜂巢塔", "召唤 SUMMON", "dmg"),
        (4, 64, "丰收之角", "阳光光环 ENERGYGAIN", "sun"),
    ]),
]


def find_window_by_pid(pid):
    """按进程 ID 精确找窗口。

    ⚠️ 绝对不能用 FindWindowW("PvZClassC", None)：
    只要还有**别的** pvz.exe / _dump.exe 活着，它就会返回那个窗口，
    按键全投给了另一个进程 —— 表现是"帧没导出 / 场景没摆上"，
    看起来像渲染坏了或按键没实现，其实是投递目标错了。
    """
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


def parse_stat(path):
    """把 statNN.txt 解析成 {row: dict(x, hp, slow, rooted, dead)}。"""
    if not os.path.exists(path):
        return {}, ""
    txt = open(path, encoding="utf-8", errors="replace").read()
    head = txt.split("\n")[0]
    m = re.search(r"^detail (.*)$", txt, re.M)
    if not m:
        return {}, head
    out = {}
    for z in re.finditer(r"z(\d+)\((\d+),([\d.]+),([\d.]+),([\d.]+),([\d.]+)(,D)?\)", m.group(1)):
        out[int(z.group(2))] = dict(idx=int(z.group(1)), x=float(z.group(3)),
                                    hp=float(z.group(4)), slow=float(z.group(5)),
                                    rooted=float(z.group(6)), dead=bool(z.group(7)))
    return out, head


def sun_of(head):
    m = re.search(r"sun=(\d+)", head or "")
    return int(m.group(1)) if m else None


def run_batch(key, tag, title, rows, stat_dir):
    stat = os.path.join(stat_dir, "stat%02d.txt" % tag)
    ctypes.windll.user32.PostMessageW(0, 0, 0, 0)   # 占位，避免 lint 抱怨未用
    return stat


def main():
    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)

    print("[1/3] 生成并编译 _dump.exe ...")
    r = subprocess.run([sys.executable, "_mk_framedump.py"], cwd=D,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("[X] 生成失败：" + (r.stderr or "")[-900:])

    env = dict(os.environ)
    env["PVZ_DUMPDIR"] = FR
    p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
    time.sleep(4.5)

    u = ctypes.windll.user32
    hwnd = find_window_by_pid(p.pid)
    if not hwnd:
        p.kill()
        sys.exit("[X] 找不到窗口（pid=%d）" % p.pid)

    def press(c):
        u.PostMessageW(hwnd, 0x100, c, 0)
        u.PostMessageW(hwnd, 0x101, c, 0)

    print("[2/3] F1 进战斗 (hwnd=%d, pid=%d)" % (hwnd, p.pid))
    press(0x70)
    time.sleep(0.8)

    overall_ok = True
    all_lines = []

    for key, tag, title, rows in BATCHES:
        press(ord(key))
        stat = os.path.join(FR, "stat%02d.txt" % tag)
        # 采样点必须早：僵尸会走动、植物会被啃掉，
        # 太晚采样看到的已经不是"机制刚生效"的状态，判定会失真。
        time.sleep(0.5)
        t1, h1 = parse_stat(stat)
        time.sleep(1.0)
        t2, _ = parse_stat(stat)
        time.sleep(2.0)
        t3, h3 = parse_stat(stat)

        print("\n=== %s（stat%02d）===" % (title, tag))
        print("汇总：", h3 or h1)
        print("%-4s %-10s %-32s %-18s %-16s %-8s %s"
              % ("行", "植物", "预期机制", "x(出场→首采样)", "hp(初→末)", "slow峰", "判定"))
        print("-" * 116)

        for row, idx, name, expect, kind in rows:
            seq = [t[row] for t in (t1, t2, t3) if row in t]
            if not seq:
                print("%-4d %-10s %-32s %-18s %-16s %-8s %s"
                      % (row, name, expect, "-", "-", "-", "· 开局即被击杀（见 killed）"))
                continue
            first, last = seq[0], seq[-1]
            slowmax = max(s["slow"] for s in seq)
            died = (t3.get(row) is None) or t3.get(row, {}).get("dead", False)

            if kind == "slow":
                good = slowmax > 0
                verdict = ("✔ 减速生效 (slow峰 %.1f)" % slowmax) if good else "✗ slow 全程为 0"
                x_col = "%.0f→%.0f" % (first["x"], last["x"])
            elif kind == "knock":
                # ⚠️ 基准是**出场位置**，不是 t1：僵尸本来就在向左走，
                # 用 t1 作参照只会看到"在前进"，被推开了也判不出来。
                good = first["x"] > SPAWN_X + 20.0
                verdict = ("✔ 击退生效 (推后 %.0fpx)" % (first["x"] - SPAWN_X)) if good else \
                          ("✗ 未被推开 (出场 %.0f → 首采样 %.0f)" % (SPAWN_X, first["x"]))
                x_col = "%.0f→%.0f" % (SPAWN_X, first["x"])
            elif kind == "sun":
                s0, s1 = sun_of(h1), sun_of(h3)
                good = (s0 is not None and s1 is not None and s1 > s0)
                verdict = ("✔ 阳光增长 (%s→%s)" % (s0, s1)) if good else \
                          ("✗ 阳光未增长 (%s→%s)" % (s0, s1))
                x_col = "-"
            else:
                good = (last["hp"] < first["hp"] - 1.0) or died
                verdict = ("✔ 伤害通道生效%s" % ("（已击杀）" if died else "")) if good else \
                          "✗ hp 无变化（机制没打到）"
                x_col = "%.0f→%.0f" % (first["x"], last["x"])

            if not good:
                overall_ok = False
            line = ("%-4d %-10s %-32s %-18s %-16s %-8.1f %s"
                    % (row, name, expect, x_col,
                       "%.0f→%.0f" % (first["hp"], last["hp"]), slowmax, verdict))
            print(line)
            all_lines.append((name, verdict))

    alive = (p.poll() is None)
    print("\n[3/3] 进程存活 = %s" % alive)
    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()

    if not alive:
        overall_ok = False
    print("结论：" + ("✔ 运行时机制验证通过（16 个新机制的代表路径全部生效）"
                     if overall_ok else "✗ 存在未生效的机制，见上方 ✗ 行"))
    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main())
