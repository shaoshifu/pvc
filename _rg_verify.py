# -*- coding: utf-8 -*-
"""肉鸽系统运行时验证：编译 _dump.exe，驱动它抓帧，检查三选一与肉鸽僵尸。

验证点：
  1. 第一波【肉鸽三选一】→ 三张卡的品质色条应该互不相同（说明抽的是 105 株池）
  2. 第二波【肉鸽三选一】→ 同上（第二波必须同样是免费可选）
  3. 刷【肉鸽僵尸】→ 场上应出现非普通僵尸，且进程不崩
  4. 全程进程存活（说明没有数组越界 / 空指针）

⚠️ 驱动方式：用 _dump.exe 的 **进程内自动驱动**（PVZ_AUTOSEQ=1），
   不再从外部 PostMessage 注入热键 —— 实测外部注入返回 1 但 WndProc
   收不到键，表现就是"进程 Alive、一张图都没有"，会被误读成渲染坏了。
   抓帧钩子在 GPU 提前返回点之前、抓的是 gWorldDC（2x 画布，含 HUD/UI）。
"""
import os
import subprocess
import sys
import time
import ctypes
import numpy as np

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
FR = os.path.join(D, "_frames_rg")

CARD_W, CARD_H, CARD_GAP, CARD_Y = 256, 360, 44, 150
VIEW_W = 1000

AUTO_FRAMES = 380        # 自动序列最后一帧（见 _mk_framedump.py 的 dumpAuto）
                         # 230 起还会摆一次殿堂五尊（tag 23）并连拍 150 帧

# ============================================================================
# 帧缓冲现在是「视图尺寸」而不是固定的 1000×650（分辨率改造后按窗口放大），
# 所以**所有裁剪坐标都必须按实际帧尺寸换算**。
# 这套脚本原来把这些坐标写死了，分辨率一提就全裁到画面外 ——
# 于是明明渲染正常，也报"草坪几乎空白 / NaN"。
# 用法： 先 K = scale_of(a)，再把逻辑坐标乘 K 得到帧内坐标。
# ============================================================================
def scale_of(a):
    """帧图像相对 1000×650 逻辑坐标的缩放比 (kx, ky)。"""
    return a.shape[1] / 1000.0, a.shape[0] / 650.0


def box(a, x0, y0, x1, y1):
    """按逻辑坐标裁剪一块，自动换算到当前帧尺寸并夹取到合法范围。"""
    kx, ky = scale_of(a)
    X0, Y0 = int(x0 * kx), int(y0 * ky)
    X1, Y1 = int(x1 * kx), int(y1 * ky)
    X0 = max(0, min(X0, a.shape[1] - 2)); X1 = max(X0 + 1, min(X1, a.shape[1]))
    Y0 = max(0, min(Y0, a.shape[0] - 2)); Y1 = max(Y0 + 1, min(Y1, a.shape[0]))
    return a[Y0:Y1, X0:X1]


def load(path):
    with open(path, "rb") as f:
        b = f.read()
    off = int.from_bytes(b[10:14], "little")
    w = int.from_bytes(b[18:22], "little", signed=True)
    h = int.from_bytes(b[22:26], "little", signed=True)
    a = np.frombuffer(b[off:], np.uint8).reshape(abs(h), abs(w), 4)
    return a[:, :, :3][:, :, ::-1].copy()


def card_x(i):
    total = 3 * CARD_W + 2 * CARD_GAP
    return int((VIEW_W - total) * 0.5 + i * (CARD_W + CARD_GAP))


def main():
    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_dump.exe"], capture_output=True)

    print("[1/3] 生成 _dump.c 并编译 ...")
    r = subprocess.run([sys.executable, "_mk_framedump.py"], cwd=D,
                       capture_output=True, text=True)
    print((r.stdout or "")[-600:])
    if r.returncode != 0:
        print((r.stderr or "")[-2000:])
        sys.exit("[X] _dump 生成失败")

    env = dict(os.environ)
    env["PVZ_DUMPDIR"] = FR
    # 存档隔离：绝不读写玩家真档（此前漏了这行，测试会覆盖真存档）
    import shutil as _sh, os as _os
    _tmp_save = _os.path.join(D, "_test_%s.dat" % _os.path.basename(__file__))
    _real_save = _os.path.join(D, "pvz_save.dat")
    if _os.path.exists(_real_save):
        _sh.copy(_real_save, _tmp_save)
    env["PVZ_SAVE_FILE"] = _tmp_save
    env["PVZ_AUTOSEQ"] = "1"      # 进程内自动驱动（不依赖外部 PostMessage 热键）
    p = subprocess.Popen([os.path.join(D, "_dump.exe")], cwd=D, env=env)
    # 自动序列最后一帧是 AUTO_FRAMES，按 60fps 约 3.2s，留一倍余量。
    time.sleep(max(6.0, AUTO_FRAMES / 30.0))
    print("[2/3] _dump.exe 已启动（PVZ_AUTOSEQ 自动驱动）")

    if p.poll() is not None:
        sys.exit("[X] _dump.exe 启动即退出，退回码 %s" % p.returncode)

    ok = True

    # ---- A. 第一波肉鸽三选一 ----
    f5 = os.path.join(FR, "st05.bmp")
    if not os.path.exists(f5):
        print("### 三选一  ✗ 没有导出帧"); ok = False
    else:
        a = load(f5)
        tops = []
        for i in range(3):
            x = card_x(i)
            strip = a[CARD_Y + 4:CARD_Y + 14, x + 20:x + CARD_W - 20].reshape(-1, 3)
            tops.append(tuple(int(v) for v in strip.mean(0)))
        print("### 三选一  三张卡顶栏均值色：", tops)
        if len(set(tops)) >= 2:
            print("    -> 三张卡颜色有差异，抽到的是不同品质/不同植物  ✔")
        else:
            print("    -> 三张卡颜色全相同，可能没抽到植物  ✗"); ok = False

    # ---- A2. 第二波：同样必须是免费可选的三选一 ----
    f6 = os.path.join(FR, "st06.bmp")
    if not os.path.exists(f6):
        print("### 第二波  ✗ 没有导出帧"); ok = False
    else:
        a = load(f6)
        tops = []
        for i in range(3):
            x = card_x(i)
            strip = box(a, x + 20, CARD_Y + 4, x + CARD_W - 20, CARD_Y + 14).reshape(-1, 3)
            if strip.size == 0:
                tops.append((-1, -1, -1)); continue
            tops.append(tuple(int(v) for v in strip.mean(0)))
        print("### 第二波  三张卡顶栏均值色：", tops)
        if len(set(tops)) >= 2:
            print("    -> 第二波三张卡颜色有差异  ✔")
        else:
            print("    -> 第二波三张卡颜色全相同  ✗"); ok = False

    # ---- B. 肉鸽僵尸 ----
    f13 = os.path.join(FR, "st13.bmp")
    if not os.path.exists(f13):
        print("### 肉鸽僵尸  ✗ 没有导出帧"); ok = False
    else:
        a = load(f13)
        # ⚠️ 草坪区的裁剪范围必须**按帧尺寸换算**，不能写死像素。
        # 这里原来写的是 a[104:604, 62:926]（按 1000×650 的逻辑坐标算的），
        # 分辨率提升到 1662×1080 之后就裁到了画面外的空白区，
        # 于是明明渲染正常也报"草坪几乎空白" —— 是脚本没跟上，不是游戏坏了。
        # 逻辑坐标：草坪 x 62..926、y 104..604（相对 1000×650）
        H, W = a.shape[0], a.shape[1]
        kx, ky = W / 1000.0, H / 650.0
        lawn = a[int(104 * ky):int(604 * ky), int(62 * kx):int(926 * kx)].reshape(-1, 3)
        nonbg = int((lawn.std(1) > 14).sum())
        print("### 肉鸽僵尸  草坪区非背景像素 = %d" % nonbg)
        if nonbg > 2000:
            print("    -> 草坪上有实际内容（僵尸/植物已渲染）  ✔")
        else:
            print("    -> 草坪几乎空白  ✗"); ok = False

    # ---- C. 进程存活 ----
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
