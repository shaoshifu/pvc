# -*- coding: utf-8 -*-
"""验证音效系统：触发点是否真的接到了 sfxPlay。

判据是 dumpStats 里的 `sfx slots=N/8` —— 音效槽一旦被 open 过就会一直是 used，
所以 N>0 直接证明"至少播过几种音效"，而不是"播了但听不见"。
这比录屏听声音可靠得多：无头环境本来也听不到。

场景用 X 键（6 株肉鸽植物 + 3 只僵尸），会自然产生：
  种植音（plantIt×6）、命中音（子弹打中）、僵尸死亡音（zombie_die）。

用法： python _verify_sfx.py
"""
import ctypes
import os
import re
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_sfx")


def find_window_by_pid(pid):
    """按 PID 精确找窗口（FindWindowW 会投给别的同名进程）。"""
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


def read_lines(tag):
    p = os.path.join(FR, "stat%02d.txt" % tag)
    if not os.path.exists(p):
        return []
    return open(p, encoding="utf-8", errors="replace").read().split("\n")


def grab(lines, key):
    for ln in lines:
        if ln.startswith(key):
            return ln
    return ""


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
    # 存档隔离：绝不读写玩家真档（此前漏了这行，测试会覆盖真存档）
    import shutil as _sh, os as _os
    _tmp_save = _os.path.join(D, "_test_%s.dat" % _os.path.basename(__file__))
    _real_save = _os.path.join(D, "pvz_save.dat")
    if _os.path.exists(_real_save):
        _sh.copy(_real_save, _tmp_save)
    env["PVZ_SAVE_FILE"] = _tmp_save
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

    press(0x70)          # F1 进战斗
    time.sleep(0.8)
    press(0x58)          # X：摆 6 株植物 + 3 只僵尸（会触发种植/命中/死亡音）

    # 采样两次：第一次看"种植音是否触发"，第二次看"战斗音是否触发"
    time.sleep(1.0)
    l1 = read_lines(16)
    time.sleep(3.0)
    l2 = read_lines(16)

    alive = (p.poll() is None)
    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()

    ok = True

    def slots(lines):
        m = re.search(r"slots=(\d+)/(\d+)", grab(lines, "sfx "))
        return (int(m.group(1)), int(m.group(2))) if m else (None, None)

    s1, tot = slots(l1)
    s2, _ = slots(l2)

    print("音效槽上限 = %s" % tot)
    if s1 is None or s2 is None:
        print("✗ 没有读到 sfx 状态行")
        return 1

    print("① 种植后（1.0s）：占用 %d 个槽" % s1)
    if s1 > 0:
        print("   -> 种植音已触发 ✔")
    else:
        print("   -> ✗ 一个音效都没播（种植音没接上？）")
        ok = False

    print("② 战斗后（4.0s）：占用 %d 个槽" % s2)
    if s2 > s1:
        print("   -> 战斗中又触发了新的音效类型（命中/死亡）✔")
    elif s2 > 0:
        print("   -> 有音效在播，但没有新增类型（可能僵尸已清完）")
    else:
        print("   -> ✗ 战斗阶段没有任何音效")
        ok = False

    print("\nBGM：%s" % (grab(l2, "bgm ") or "(未读到)"))
    print("进程存活 = %s" % alive)
    if not alive:
        ok = False
    print("\n结论：" + ("✔ 音效系统已接通" if ok else "✗ 音效系统有问题"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
