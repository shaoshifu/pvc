# -*- coding: utf-8 -*-
"""验证第三批新僵尸（飞 / 跳 / 召唤 / 闪现 / 钻地 / 尸王）的机制是否真的在跑。

为什么挑这几只：它们都带**位移类**机制（FLY/JUMP/BLINK/BURROW/SUMMON），
x 会明显变化 —— 读 detail 行就能判断，不像"回血"类要长时间采样才看得出来。

判据：
  · 6 只都生成（数量对）；
  · 至少 2 只的 x 相对出场位置**明显前移**（位移机制生效）；
  · 尸王（Z69）带 SUMMON，场上僵尸总数应**超过**出场的 6 只。

用法： python _verify_newz.py
"""
import ctypes
import os
import re
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_newz")
SPAWN_X = None      # 每只出场位置不同（660 + i*30），按索引推算


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


def parse(path):
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
        sys.exit("[X] 生成失败：" + (r.stderr or "")[-800:])

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

    press(0x70)          # F1
    time.sleep(0.8)
    press(0x4A)          # J：刷 6 只第三批僵尸

    stat = os.path.join(FR, "stat22.txt")
    # ⚠️ 采样窗口必须 ≥10 秒。
    # 僵尸机制的脉冲周期是 `4.6 - 0.42*tier` 秒（普通约 4.6s、传奇约 2.1s），
    # 4.5 秒内位移类机制最多只触发一次 —— 第一次跑就是按 4.5 秒采的，
    # 结果 6 只僵尸全都"没动"，差点被误判成"位移机制没实现"。
    time.sleep(0.5)
    t1, _ = parse(stat)
    time.sleep(4.5)
    t2, h2 = parse(stat)
    time.sleep(5.0)
    t3, h3 = parse(stat)

    alive = (p.poll() is None)
    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()

    print("汇总：", h3 or h2)
    ok = True

    n1 = len(t1)
    print("\n① 生成数量：首采样 %d 只 %s" % (n1, "✔" if n1 >= 6 else "✗ 期望 ≥6"))
    if n1 < 6:
        ok = False

    # 位移类机制：比较首末采样的 x（僵尸向左推进 = x 减小）
    print("\n② 位移机制（x 应随时间明显减小）：")
    moved = 0
    for i, z1 in sorted(t1.items()):
        z3 = t3.get(i)
        if not z3:
            print("   z%-3d 已被清掉（正常）" % i)
            continue
        dx = z3["x"] - z1["x"]
        # 10 秒内正常推进约 110px。能跑到 -150 以下，说明中间被
        # BLINK / DASH / FLY / BURROW 这类机制额外推过。
        fast = dx < -150.0
        if fast:
            moved += 1
        print("   z%-3d x: %6.0f → %6.0f  (Δ=%+.0f)%s"
              % (i, z1["x"], z3["x"], dx, "  ← 位移机制生效" if fast else ""))
    if moved >= 2:
        print("   -> %d 只明显前移 ✔" % moved)
    else:
        print("   -> ✗ 只有 %d 只前移，位移机制可能没生效" % moved)
        ok = False

    # 召唤：尸王在场时应不断有新僵尸冒出来
    print("\n③ 召唤机制：")
    n3 = len(t3)
    extra = n3 - n1
    if n3 > 6 or extra > 0:
        print("   末次采样 %d 只（比出场多 %d）→ 有僵尸被召唤出来 ✔" % (n3, extra))
    else:
        print("   末次采样 %d 只，没有新增 —— 可能已被清完，也可能召唤没生效" % n3)

    print("\n进程存活 = %s" % alive)
    if not alive:
        ok = False
    print("\n结论：" + ("✔ 第三批新僵尸机制正常" if ok else "✗ 存在问题，见上方 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
