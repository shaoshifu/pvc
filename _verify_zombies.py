# -*- coding: utf-8 -*-
"""僵尸侧运行时验证：肉鸽僵尸能否正常生成、承载品质倍率、推进并被击杀。

M/N 两批验证只覆盖了**植物侧**的机制。僵尸侧要回答的是另外三个问题：
  1. 肉鸽僵尸能否正常生成（数量与品质分布对不对）；
  2. 品质倍率有没有真的作用到血量上（肉鸽僵尸应明显比普通僵尸厚）；
  3. 它们能否正常向左推进（x 随时间减小）并被击杀。

场景用 _mk_framedump.py 的 Z 键：Z49 终极奇葩（传奇）+ Z42 + Z7，都是普通基座，
所以"推进"这件事不受基座差异干扰。

用法： python _verify_zombies.py
"""
import ctypes
import os
import re
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_zm")

# 普通僵尸（ZT_NORMAL）的基准血量，用于对比肉鸽倍率是否生效
BASE_HP = 200.0


def find_window_by_pid(pid):
    """按 PID 精确找窗口（理由见 _verify_mechanics.py 里的同名函数）。"""
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
    if not os.path.exists(path):
        return {}, ""
    txt = open(path, encoding="utf-8", errors="replace").read()
    head = txt.split("\n")[0]
    m = re.search(r"^detail (.*)$", txt, re.M)
    out = {}
    if m:
        for z in re.finditer(r"z(\d+)\((\d+),([\d.]+),([\d.]+),([\d.]+),([\d.]+)(,D)?\)",
                             m.group(1)):
            out[int(z.group(1))] = dict(row=int(z.group(2)), x=float(z.group(3)),
                                        hp=float(z.group(4)), dead=bool(z.group(7)))
    return out, head


def main():
    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)

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

    press(0x70)                     # F1 进战斗
    time.sleep(0.8)
    press(0x5A)                     # Z：刷 3 只肉鸽僵尸

    stat = os.path.join(FR, "stat13.txt")
    time.sleep(0.6)
    t1, _ = parse_stat(stat)
    time.sleep(1.4)
    t2, _ = parse_stat(stat)
    time.sleep(2.0)
    t3, h3 = parse_stat(stat)

    alive = (p.poll() is None)
    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()

    print("汇总：", h3)
    print()
    ok = True

    # --- 1. 生成数量 ---
    # ⚠️ 不能要求"首采样恰好 3 只"：Z 键刷的三只里有脆的，
    # 可能在 0.6 秒内就被植物/小推车清掉了（实测 killed=1）。
    # 正确判据是「刷出来的总数 = 存活 + 已击杀」。
    m = re.search(r"killed=(\d+)", h3 or "")
    killed = int(m.group(1)) if m else 0
    born = len(t1) + killed
    print("① 肉鸽僵尸生成：存活 %d + 已击杀 %d = %d 只 %s"
          % (len(t1), killed, born, "✔" if born >= 3 else "✗ 期望 3 只"))
    if born < 3:
        ok = False

    # --- 2. 品质倍率是否作用到血量 ---
    # Z49 是传奇（rgZombieHpMul ≈ 8.56），Z7 是史诗，都该明显高于基准 200。
    # 只对普通基座的僵尸比倍率。Z49 用的是 ZT_GIANT 基座 ——
    # 它基准血量本来就是普通僵尸的十几倍，还带 RZ2_ALL（含成长/自愈），
    # 实测血量能到 6.5 万。拿它去比"是否 > 1.5 倍"毫无意义，
    # 第一版就是这么写的，于是报出一个看着像 bug 的 327 倍。
    print("\n② 品质血量倍率（比对普通基座；巨人基座单独标注）：")
    checked = 0
    # 只把首采样仍存活且血量为正的单位纳入倍率比较；
    # detail 会保留 active=1 的死亡动画对象，hp=0 的对象不是倍率失败。
    for idx, z in sorted(t1.items()):
        if z["dead"] or z["hp"] <= 0.0:
            print("   z%-3d 已进入死亡动画（hp=%.0f），不参与倍率比对" % (idx, z["hp"]))
            continue
        if z["hp"] > 5000:
            print("   z%-3d 血量 %8.0f  ← 巨人基座×传奇倍率×成长封顶，不参与倍率比对"
                  % (idx, z["hp"]))
            continue
        mul = z["hp"] / BASE_HP
        checked += 1
        good = mul > 1.5
        if not good:
            ok = False
        print("   z%-3d 血量 %6.0f = 普通僵尸的 %4.2f 倍  %s"
              % (idx, z["hp"], mul, "✔" if good else "✗ 倍率未生效"))
    if checked == 0:
        print("   ✗ 没有可比对的普通基座僵尸")
        ok = False

    # --- 3. 推进与击杀 ---
    print("\n③ 行为：")
    moved = 0
    for idx, z1 in t1.items():
        z2 = t2.get(idx)
        if z2 and z2["x"] < z1["x"] - 5.0:
            moved += 1
    print("   推进（x 减小）：%d / %d 只 %s" % (moved, len(t1),
                                            "✔" if moved >= max(1, len(t1) - 1) else "✗"))
    if moved < max(1, len(t1) - 1):
        ok = False

    survived = len(t3)
    print("   存活到末次采样：%d 只（被植物/小推车清掉的属于正常）%s"
          % (survived, "✔" if survived < len(t1) else "· 全部存活（本场景未交战）"))

    print("\n进程存活 = %s" % alive)
    if not alive:
        ok = False
    print("\n结论：" + ("✔ 僵尸侧运行时验证通过" if ok else "✗ 僵尸侧存在问题"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
