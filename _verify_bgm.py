# -*- coding: utf-8 -*-
"""验证 BGM 通路：MCI 是否真的进入 playing 状态。

为什么不能只看"代码调用过 bgmPlayFile"：
  MCI 是**异步**的，`open` 成功不代表能出声 —— 解码器类型选错（拿 waveaudio
  去开 MP3）、文件损坏、设备被独占，都要到 `play` 阶段才暴露。
  而且这些失败在游戏里是**静默**的（open 失败就 return），
  表现成"这首歌没声音"。所以必须查 `status mode`，只有 'playing' 才算数。

按 G 放第 0 关 BGM、按 H 放第 1 关（F1 不经过选关，测不到 BGM）。

用法： python _verify_bgm.py
"""
import ctypes
import os
import re
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(D, "_frames_bgm")


def find_window_by_pid(pid):
    """按 PID 精确找窗口（用 FindWindowW 会投给别的同名进程实例）。"""
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


def read_bgm(tag):
    p = os.path.join(FR, "stat%02d.txt" % tag)
    if not os.path.exists(p):
        return None
    txt = open(p, encoding="utf-8", errors="replace").read()
    # mode 后面还可能跟别的字段（如 vol=110），所以只取到空白为止 ——
    # 用 (.*)$ 会把 "playing vol=110" 整串捕获，比对必然失败
    m = re.search(r"^bgm open=(\d+) mode=(\S*)", txt, re.M)
    return m.groups() if m else None


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

    press(0x70)                       # F1 进战斗
    time.sleep(1.0)

    ok = True
    for key, tag, label in (("G", 20, "第 0 关（jazz wav）"),
                            ("H", 21, "第 1 关（lv1.mp3）")):
        press(ord(key))
        time.sleep(2.2)
        res = read_bgm(tag)
        if res is None:
            print("✗ %-22s 没有读到 bgm 状态行" % label)
            ok = False
            continue
        opened, mode = res
        good = (opened == "1" and mode.strip() == "playing")
        if not good:
            ok = False
        print("%s %-22s open=%s mode=%s"
              % ("✔" if good else "✗", label, opened, mode))

    alive = (p.poll() is None)
    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()

    print("\n进程存活 = %s" % alive)
    if not alive:
        ok = False
    print("结论：" + ("✔ BGM 通路正常（两关都进入 playing）"
                     if ok else "✗ BGM 通路有问题，见上方 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
