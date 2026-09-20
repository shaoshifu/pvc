# -*- coding: utf-8 -*-
"""
UI 可见性证明分析器 —— 回答「这句话到底有没有画到屏幕上」。

与上一版 _audit_ui.py 的区别
---------------------------
上一版只测「文字被放在哪」（静态墨迹矩形），测不出可见性，于是把死代码
（右下角状态栏整块画进 gFrameDC，随后被 StretchBlt 整幅擦掉）当成了真实缺陷。

本版用「探针标记」把可见性变成像素问题：
  · 生成阶段（_mk_uiproof.py）在每个 putText* 调用点原位压一块**全局唯一纯色**小矩形；
  · 运行阶段让程序自己把最终帧 gFrameDC 导出成 BMP（唯一可信真值）；
  · 本脚本在读回的真帧里，统计每处文字的墨迹框内是否出现它自己的探针色：
      有 -> 真的画出来了（可见）
      无 -> 被后画的东西盖掉了（死代码 / 被遮挡）

判定规则
  · 可见   = 该调用点墨迹框内探针色像素 > 0
  · 压字   = 两条**可见**文字的墨迹矩形相交 ≥24px² 且 ≥ 较小者的 20%
  · 出血   = 可见文字的墨迹矩形越出 0..1000 / 0..650
  · 自检   = 墨迹框内找不到、但整帧能找到 -> 坐标映射假设有问题，单独列出

前置：python _mk_uiproof.py 生成并编译 _uiproof.exe
"""
import ctypes
import json
import os
import subprocess
import sys
import time
from collections import defaultdict

import numpy as np

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
EXE = os.path.join(D, "_uiproof.exe")
SRC = os.path.join(D, "pvz.c")
TRACE = os.path.join(D, "_ui_proof.tsv")
FR = os.path.join(D, "_frames2")
REPORT = os.path.join(D, "_ui_proof_report.txt")
LINEMAP = os.path.join(D, "_uiproof_lines.json")

VIEW_W, VIEW_H = 1000, 650

# 热键 / 状态标签 / 截图编号，与 _mk_uiproof.py 的 HOTKEYS 一一对应
STATES = [
    ("ST_MENU",    0x23, 0), ("ST_PLAY",    0x70, 1), ("ST_PAUSE",  0x71, 2),
    ("ST_WIN",     0x72, 3), ("ST_LOSE",    0x73, 4), ("ST_DRAFT",  0x74, 5),
    ("ST_LEVELS",  0x75, 6), ("ST_TALENTS", 0x76, 7), ("ST_ACH",    0x77, 8),
    ("ST_GACHA",   0x78, 9), ("ST_LOADOUT", 0x79, 10), ("ST_REWARD", 0x7B, 11),
    ("ST_DAILY",   0x24, 12),
]


# ------------------------------------------------ 探针色（必须与 C 侧完全一致）
def probe_color(src):
    return (40 + (src * 37) % 190, 30 + (src * 61) % 200, 90 + (src * 23) % 160)


# ------------------------------------------------ 审计副本行号 -> pvz.c 真实行号
_BOUNDS, _SRCLINES = [], []


def load_linemap():
    global _BOUNDS, _SRCLINES
    try:
        _BOUNDS = json.load(open(LINEMAP, encoding="utf-8"))["bounds"]
    except Exception:
        _BOUNDS = []
    with open(SRC, encoding="utf-8", errors="replace") as f:
        _SRCLINES = f.read().split("\n")


def to_src(dst_line):
    d = 0
    for at, cum in _BOUNDS:
        if dst_line >= at:
            d = cum
        else:
            break
    return dst_line - d


def src_text(line, width=110):
    if 1 <= line <= len(_SRCLINES):
        t = _SRCLINES[line - 1].strip()
        return t[:width] + ("…" if len(t) > width else "")
    return "?"


# ------------------------------------------------------------------ 驱动游戏
WM_KEYDOWN, WM_KEYUP = 0x100, 0x101


def drive():
    os.makedirs(FR, exist_ok=True)
    for f in os.listdir(FR):
        os.remove(os.path.join(FR, f))
    subprocess.run(["taskkill", "/F", "/IM", "_uiproof.exe"], capture_output=True)
    if os.path.exists(TRACE):
        os.remove(TRACE)

    env = dict(os.environ)
    env["PVZ_UITRACE"] = TRACE
    env["PVZ_DUMPDIR"] = FR
    env["PVZ_PROBE"] = "1"
    p = subprocess.Popen([EXE], cwd=D, env=env)
    try:
        time.sleep(4.5)
        u = ctypes.windll.user32
        hwnd = u.FindWindowW("PvZClassC", None)
        if not hwnd:
            sys.exit("[X] 找不到窗口 PvZClassC")
        print("    hwnd = %s" % hwnd)
        for name, vk, tag in STATES:
            u.PostMessageW(hwnd, WM_KEYDOWN, vk, 0)
            u.PostMessageW(hwnd, WM_KEYUP, vk, 0)
            time.sleep(1.4)
            print("    → %-10s st%02d" % (name, tag))
        time.sleep(0.4)
    finally:
        p.terminate()
        try:
            p.wait(timeout=3)
        except Exception:
            p.kill()


# ------------------------------------------------------------------ 读 trace
def load_trace():
    """T 行字段：帧 src dc 原色 墨迹(x,y,w,h) 探针块(x,y,w,h) 文本"""
    frames = defaultdict(list)     # 帧号 -> [(src, dc, rgb, ix, iy, iw, ih, mx, my, mw, mh, text)]
    fmeta = {}                     # 帧号 -> (state, worldDC, frameDC)
    with open(TRACE, encoding="utf-8", errors="replace") as f:
        for ln in f:
            p = ln.rstrip("\n").split("\t")
            if not p or not p[0]:
                continue
            if p[0] == "F" and len(p) >= 5:
                fmeta[int(p[1])] = (int(p[2]), p[3], p[4])
            elif p[0] == "T" and len(p) >= 14:
                frames[int(p[1])].append(
                    (int(p[2]), p[3], tuple(int(v) for v in p[4].split(",")),
                     float(p[5]), float(p[6]), int(p[7]), int(p[8]),
                     float(p[9]), float(p[10]), float(p[11]), float(p[12]), p[13]))
    return frames, fmeta


def load_bmp(path):
    with open(path, "rb") as f:
        b = f.read()
    off = int.from_bytes(b[10:14], "little")
    w = int.from_bytes(b[18:22], "little", signed=True)
    h = int.from_bytes(b[22:26], "little", signed=True)
    a = np.frombuffer(b[off:], np.uint8).reshape(abs(h), abs(w), 4)
    return a[:, :, :3][:, :, ::-1].copy()


def color_hist(a):
    cols, cnts = np.unique(a.reshape(-1, 3), axis=0, return_counts=True)
    return {tuple(int(v) for v in c): int(n) for c, n in zip(cols, cnts)}


def box_hits(a, rgb, x, y, w, h):
    """返回 (框内该色像素数, 质心)。"""
    x0, y0 = max(0, int(x)), max(0, int(y))
    x1, y1 = min(VIEW_W, int(x + w)), min(VIEW_H, int(y + h))
    if x1 <= x0 or y1 <= y0:
        return 0, None
    sub = a[y0:y1, x0:x1]
    m = (sub[:, :, 0] == rgb[0]) & (sub[:, :, 1] == rgb[1]) & (sub[:, :, 2] == rgb[2])
    n = int(m.sum())
    if not n:
        return 0, None
    ys, xs = np.nonzero(m)
    return n, (x0 + float(xs.mean()), y0 + float(ys.mean()))


def inter(a, b):
    x0, y0 = max(a[3], b[3]), max(a[4], b[4])
    x1 = min(a[3] + a[5], b[3] + b[5])
    y1 = min(a[4] + a[6], b[4] + b[6])
    return max(0.0, x1 - x0) * max(0.0, y1 - y0)


# ------------------------------------------------------------------ 主流程
def main():
    if not os.path.exists(EXE):
        sys.exit("[X] 缺少 _uiproof.exe，先跑：python _mk_uiproof.py")
    reuse = "--reuse" in sys.argv
    if reuse:
        print("[1/3] --reuse：跳过驱动")
    else:
        print("[1/3] 驱动游戏走遍 13 个界面（每界面连拍 6 帧导出最终帧）...")
        drive()

    print("[2/3] 分析可见性 ...")
    load_linemap()
    frames, fmeta = load_trace()

    # 全局统计
    stat = defaultdict(lambda: {"drawn": 0, "vis": 0, "inv": 0})
    dead = defaultdict(lambda: [0, 0, "", None, set()])   # src -> [不可见帧, 总帧, 样例文本, 层, 界面集]
    live = defaultdict(lambda: [0, 0, "", None, set()])   # src -> [可见帧, 总帧, 样例文本, 层, 界面集]
    collide = {}
    bleed = {}
    selfcheck = defaultdict(lambda: [0, ""])
    per_state = []

    for name, vk, tag in STATES:
        bp = os.path.join(FR, "st%02d.bmp" % tag)
        tp = os.path.join(FR, "st%02d.txt" % tag)
        if not (os.path.exists(bp) and os.path.exists(tp)):
            print("    ### %-10s ✗ 缺导出文件" % name)
            continue
        fr = int(open(tp).read().strip())
        if fr not in fmeta or fr not in frames:
            print("    ### %-10s ✗ 帧号 %d 不在 trace 中" % (name, fr))
            continue
        st, wdc, fdc = fmeta[fr]
        lines = frames[fr]
        a = load_bmp(bp)
        hist = color_hist(a)

        nvis = ninv = 0
        vis_lines = []
        for L in lines:
            src, dc, rgb = L[0], L[1], L[2]
            ix, iy, iw, ih = L[3], L[4], L[5], L[6]
            mx, my, mw, mh = L[7], L[8], L[9], L[10]
            txt = L[11]
            pc = probe_color(src)
            # 可见性只看探针块自己那块矩形：块是纯色填充，不经抗锯齿，
            # 世界的 2x->1x 平均在块内部也还原成同一个颜色，所以有像素就是硬证据。
            inbox, cen = box_hits(a, pc, mx, my, mw, mh)
            whole = hist.get(pc, 0)
            layer = ("frame" if dc == fdc else ("world" if dc == wdc else "?" + dc))
            ok = inbox > 0
            stat[name]["drawn"] += 1
            stat[name]["vis" if ok else "inv"] += 1
            if ok:
                nvis += 1
                vis_lines.append((src, dc, rgb, ix, iy, iw, ih, txt, layer, inbox, cen))
            else:
                ninv += 1
            e = (live if ok else dead)[src]
            e[1] += 1
            if ok:
                e[0] += 1
            if not e[2]:
                e[2] = txt
            if e[3] is None:
                e[3] = layer
            e[4].add(name)
            if not ok and whole > 0:
                k = to_src(src)
                selfcheck[k][0] += 1
                selfcheck[k][1] = txt

        per_state.append((name, fr, len(lines), nvis, ninv, wdc, fdc))

        # --- 压字（只在可见文字之间判） ---
        n = len(vis_lines)
        for i in range(n):
            for j in range(i + 1, n):
                A, B = vis_lines[i], vis_lines[j]
                if A[0] == B[0]:
                    continue                     # 同一调用点（putTextS 的描边阴影）
                ar = inter(A, B)
                if ar < 24.0:
                    continue
                amin = min(A[5] * A[6], B[5] * B[6])
                ratio = ar / amin if amin else 0.0
                if ratio < 0.20:
                    continue
                k = (name,) + tuple(sorted([(A[0], A[7]), (B[0], B[7])]))
                e = collide.setdefault(k, [0, 0.0, None])
                e[0] += 1
                if ratio > e[1]:
                    e[1] = ratio
                    e[2] = (A, B, ar)

        # --- 出血（同样只在可见文字之间判） ---
        for A in vis_lines:
            worst = max(-A[3], -A[4], A[3] + A[5] - VIEW_W, A[4] + A[6] - VIEW_H)
            if worst > 0:
                k = (name, A[0], A[7])
                e = bleed.setdefault(k, [0, 0.0, ""])
                e[0] += 1
                e[1] = max(e[1], worst)
                e[2] = "x=%.0f y=%.0f w=%d h=%d %s" % (A[3], A[4], A[5], A[6], A[8])

    # ------------------------------------------------------------- 出报告
    out = []

    def W(s=""):
        out.append(s)

    W("=" * 98)
    W("UI 可见性证明报告 —— 每条都基于「程序自导的最终帧 BMP」里的探针色像素实测")
    W("=" * 98)
    W("")
    W("A. 各界面：画了多少处文字 / 其中多少真的出现在屏幕上")
    W("-" * 98)
    W("  %-10s %5s | %6s %6s %6s" % ("界面", "帧号", "调用", "可见", "不可见"))
    for name, fr, tot, nvis, ninv, wdc, fdc in per_state:
        W("  %-10s %5d | %6d %6d %6d" % (name, fr, tot, nvis, ninv))
    W("")

    n_dead_src = sum(1 for v in dead.values() if v[0] == 0)
    n_part_src = sum(1 for v in dead.values() if v[0] > 0)
    W("B. 全状态可见性汇总（按调用点 / pvz.c 行号）")
    W("-" * 98)
    W("  从未可见的调用点：%d 处" % n_dead_src)
    W("  部分状态可见、部分不可见：%d 处" % n_part_src)
    W("")
    W("  ▸ 从未可见（= 死代码 或 恒被遮挡）：")
    if not n_dead_src:
        W("      无")
    for src, v in sorted(dead.items()):
        if v[0] != 0:
            continue
        ln = to_src(src)
        W("      pvz.c:%-5d [%s层] %d 帧全不可见 · 界面 %s" %
          (ln, v[3], v[1], ",".join(sorted(v[4]))))
        W("              「%s」" % v[2])
        W("              %s" % src_text(ln))
    W("")
    W("  ▸ 部分可见（说明它依赖状态/条件分支，不是死代码）：")
    if not n_part_src:
        W("      无")
    for src, v in sorted(dead.items()):
        if v[0] == 0:
            continue
        ln = to_src(src)
        W("      pvz.c:%-5d [%s层] 可见 %d/%d 帧  「%s」" % (ln, v[3], v[0], v[1], v[2]))
    W("")

    W("C. 真·压字（两条**可见**文字的墨迹确实叠在一起）：%d 类" % len(collide))
    W("-" * 98)
    if not collide:
        W("      无")
    for k, (nf, ratio, sample) in sorted(collide.items(), key=lambda kv: -kv[1][1])[:40]:
        A, B, ar = sample
        W("  [%s] 重叠比 %.0f%% · 重叠面积 %.0fpx²" % (k[0], ratio * 100, ar))
        for tagl, R in (("一", A), ("二", B)):
            ln = to_src(R[0])
            W("      压字%s pvz.c:%-5d 墨迹(%.0f,%.0f %dx%d) %s 「%s」"
              % (tagl, ln, R[3], R[4], R[5], R[6], R[8], R[7]))
            W("            %s" % src_text(ln))
    W("")

    W("D. 真·出血（**可见**文字越出 1000x650，会被 DC 边缘裁掉）：%d 类" % len(bleed))
    W("-" * 98)
    if not bleed:
        W("      无")
    for k, (nf, worst, sample) in sorted(bleed.items(), key=lambda kv: -kv[1][1])[:40]:
        ln = to_src(k[1])
        W("  [%s] 最多越界 %.0fpx · pvz.c:%-5d 「%s」" % (k[0], worst, ln, k[2]))
        W("        %s" % src_text(ln))
    W("")

    W("E. 坐标映射自检（框内找不到、整帧却找得到 -> 说明映射假设不成立）")
    W("-" * 98)
    if not selfcheck:
        W("      通过：所有探针都出现在自己墨迹框内，world 层 1:1 映射假设成立")
    for ln, (cnt, txt) in sorted(selfcheck.items()):
        W("      pvz.c:%-5d %d 帧  「%s」" % (ln, cnt, txt))

    txt = "\n".join(out)
    print(txt)
    open(REPORT, "w", encoding="utf-8").write(txt + "\n")
    print("\n[3/3] 报告已写入 %s" % REPORT)


if __name__ == "__main__":
    main()
