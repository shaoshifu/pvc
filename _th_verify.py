# -*- coding: utf-8 -*-
"""动态地形 + 事件主题 的运行时验证。

验证点：
  A. 按 Y 铺满地块 + 强制炼狱主题 → 草坪上应出现地形色块与主题染色
  B. 按 U 刷一只神级僵尸 → 主题应被「自然触发」（不靠强制调用）
  C. 进程全程存活
"""
import os
import subprocess
import sys
import time
import ctypes
import numpy as np

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
FR = os.path.join(D, "_frames_th")

LAWN = (62, 104, 926, 604)   # LAWN_X, LAWN_Y, +9*96, +5*100


def load(path):
    with open(path, "rb") as f:
        b = f.read()
    off = int.from_bytes(b[10:14], "little")
    w = int.from_bytes(b[18:22], "little", signed=True)
    h = int.from_bytes(b[22:26], "little", signed=True)
    a = np.frombuffer(b[off:], np.uint8).reshape(abs(h), abs(w), 4)
    return a[:, :, :3][:, :, ::-1].copy()


def wait_file(path, timeout=6.0):
    """等诊断版把帧写出来；渲染有节拍，固定 sleep 不可靠。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(path):
            return True
        time.sleep(0.2)
    return False


def lawn_std(a):
    x0, y0, x1, y1 = LAWN
    return float(a[y0:y1, x0:x1].reshape(-1, 3).std(1).mean())


def main():
    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)

    print("[1/3] 生成 _dump.c 并编译 ...")
    r = subprocess.run([sys.executable, "_mk_framedump.py"], cwd=D,
                       capture_output=True, text=True)
    print((r.stdout or "")[-400:])
    if r.returncode != 0:
        print((r.stderr or "")[-2000:])
        sys.exit("[X] _dump 生成失败")

    env = dict(os.environ)
    env["PVZ_DUMPDIR"] = FR
    p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)

    u = ctypes.windll.user32

    # ⚠️ 不能用 FindWindowW("PvZClassC", None)：如果有别的 _dump.exe / pvz.exe
    # 还活着，它会返回【那个】窗口，于是按键全投给别的进程，
    # 表现成"帧没导出"，很容易误判成渲染坏了。
    # 必须按 PID 精确匹配自己刚启动的这个进程。
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def find_by_pid(pid):
        hit = []

        def cb(h, l):
            wpid = ctypes.c_ulong()
            u.GetWindowThreadProcessId(h, ctypes.byref(wpid))
            if wpid.value == pid:
                buf = ctypes.create_unicode_buffer(256)
                u.GetClassNameW(h, buf, 256)
                if buf.value == "PvZClassC":
                    hit.append(h)
            return True

        u.EnumWindows(WNDENUMPROC(cb), 0)
        return hit[0] if hit else 0

    hwnd = 0
    for _ in range(50):
        time.sleep(0.2)
        if p.poll() is not None:
            sys.exit("[X] _dump.exe 提前退出，returncode=%s" % p.returncode)
        hwnd = find_by_pid(p.pid)
        if hwnd:
            break
    if not hwnd:
        p.kill()
        sys.exit("[X] 找不到属于本进程的窗口 (pid=%d)" % p.pid)
    print("    窗口 pid=%d hwnd=%d" % (p.pid, hwnd))

    # PostMessage 返回 0 表示投递失败（句柄无效）——必须校验，
    # 否则会静默丢失按键，表现成"帧没导出"，误判为渲染坏了。
    def key(code):
        ok1 = u.PostMessageW(hwnd, 0x100, code, 0)
        ok2 = u.PostMessageW(hwnd, 0x101, code, 0)
        if not ok1 or not ok2:
            print("    [warn] PostMessage 失败 code=0x%02X ok=%s/%s" % (code, ok1, ok2))
        return bool(ok1)

    key(0x23)                           # END -> 先回 ST_MENU，确保状态可预期
    time.sleep(0.6)

    ok = True

    # 先抓一张「干净草坪」作为基线
    key(0x70)                     # F1 -> ST_PLAY
    p01 = os.path.join(FR, "st01.bmp")
    if not wait_file(p01):
        p.kill()
        sys.exit("[X] 基线帧 st01.bmp 未导出")
    base = load(p01)
    base_std = lawn_std(base)
    print("### 基线（未加地形）草坪 std = %.2f" % base_std)

    # A. 地块 + 强制主题
    key(0x59)                     # 'Y'
    f14 = os.path.join(FR, "st14.bmp")
    if not wait_file(f14):
        print("### 地形主题  ✗ 没有导出帧"); ok = False
    else:
        a = load(f14)
        x0, y0, x1, y1 = LAWN
        d = np.abs(a[y0:y1, x0:x1].astype(int) -
                   base[y0:y1, x0:x1].astype(int)).sum(2)
        changed = float((d > 45).mean()) * 100.0
        print("### 地形主题  与基线相比，草坪有 %.1f%% 的像素被改写" % changed)
        if changed > 25.0:
            print("    -> 地块叠加大范围生效  ✔")
        else:
            print("    -> 地块叠加覆盖不足  ✗"); ok = False
        m = a[y0:y1, x0:x1].reshape(-1, 3).mean(0)
        print("    草坪均值色 = (%.0f, %.0f, %.0f)" % tuple(m))
        if m[0] > m[1] > m[2]:
            print("    -> R>G>B，符合炼狱暖色染色  ✔")
        else:
            print("    -> 未见暖色染色  ✗"); ok = False

    # B. 神级僵尸自然触发主题
    key(0x55)                     # 'U'
    f15 = os.path.join(FR, "st15.bmp")
    if not wait_file(f15):
        print("### 自然触发  ✗ 没有导出帧"); ok = False
    else:
        b = load(f15)
        x0, y0, x1, y1 = LAWN
        mb = b[y0:y1, x0:x1].reshape(-1, 3).mean(0)
        print("### 自然触发  草坪均值色 = (%.0f, %.0f, %.0f)" % tuple(mb))
        db = float(np.abs(mb.astype(int) - np.array([100, 160, 74])).sum())
        print("    与普通草坪基准色距离 = %.0f" % db)
        if db > 60:
            print("    -> 背景已被主题改写  ✔")
        else:
            print("    -> 未见主题变化  ✗"); ok = False

    alive = (p.poll() is None)
    print("### 进程存活 =", alive)
    if not alive:
        ok = False

    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()

    print("[3/3] " + ("全部通过 ✔" if ok else "存在问题 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
