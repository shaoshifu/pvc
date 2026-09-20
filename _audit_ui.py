# -*- coding: utf-8 -*-
"""
UI 文本布局审计：跑一遍游戏所有界面，收集每一帧每个文字的「字形墨迹」矩形，
自动找出「同帧内互相压字」和「越出 1000x650 视口」两类布局 bug。

为什么必须运行时实测，而不是静态读源码：
  pvz.c 有 115 处 putText 调用，坐标大量是算出来的（卡片槽位、天赋节点、
  关卡网格、编队栏……）。只有让 render() 自己把每个字符串真渲染一次的
  墨迹范围吐出来，拿到的才是眼睛看到的占位。

前置：先跑 _mk_uiaudit.py 生成 _uiaudit.exe（带钩子的审计版）。

判定规则
  · 墨迹矩形相交 ≥ 24 px² 且 ≥ 较小者的 20%      → 压字（bug）
  · 同一字符串不算（putTextS 的描边阴影用同一个串，属正常叠加）
  · 矩形越出 0..1000 / 0..650                     → 出血（字被 DC 边缘裁掉）
  · 结果按「调用点行号对」聚合，报告里直接给 pvz.c 行号
"""
import ctypes
import json
import os
import subprocess
import sys
import time
from collections import defaultdict

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
EXE = os.path.join(D, "_uiaudit.exe")
SRC = os.path.join(D, "pvz.c")
TRACE = os.path.join(D, "_ui_trace.tsv")
REPORT = os.path.join(D, "_ui_audit_report.txt")
LINEMAP = os.path.join(D, "_uiaudit_lines.json")

VIEW_W, VIEW_H = 1000, 650
PARK = (8, 8)          # 中立鼠标位：所有界面这一带都是空的，不会触发悬停气泡


# ------------------------------------------- 审计副本行号 -> pvz.c 真实行号
_BOUNDS = []
_SRCLINES = []


def load_linemap():
    global _BOUNDS, _SRCLINES
    try:
        _BOUNDS = json.load(open(LINEMAP, encoding="utf-8"))["bounds"]
    except Exception:
        _BOUNDS = []
    with open(SRC, encoding="utf-8", errors="replace") as f:
        _SRCLINES = f.read().split("\n")


def to_src(audit_line):
    """副本行号 -> pvz.c 行号（按各补丁插入前的累计偏移回推）。"""
    d = 0
    for at, cum in _BOUNDS:
        if audit_line >= at:
            d = cum
        else:
            break
    return audit_line - d


def src_text(line, width=120):
    if 1 <= line <= len(_SRCLINES):
        t = _SRCLINES[line - 1].strip()
        return t[:width] + ("…" if len(t) > width else "")
    return "?"


STATE_NAME = {
    0: "ST_MENU", 1: "ST_PLAY", 2: "ST_PAUSE", 3: "ST_WIN", 4: "ST_LOSE",
    5: "ST_DRAFT", 6: "ST_LEVELS", 7: "ST_TALENTS", 8: "ST_ACH",
    9: "ST_GACHA", 10: "ST_LOADOUT", 11: "ST_REWARD", 12: "ST_DAILY",
}


# ----------------------------------------------------------------- 驱动游戏
u = ctypes.windll.user32

WM_KEYDOWN, WM_KEYUP, WM_MOUSEMOVE = 0x100, 0x101, 0x200
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x201, 0x202
KEY = {'M': 0x4D, 'P': 0x50, 'S': 0x53, 'R': 0x52,
       'F1': 0x70, 'F2': 0x71, 'F3': 0x72, 'F4': 0x73}

hwnd = None


def key(name):
    wp = KEY[name]
    u.PostMessageW(hwnd, WM_KEYDOWN, wp, 0)
    u.PostMessageW(hwnd, WM_KEYUP, wp, 0)


def click(x, y):
    lp = (y << 16) | x
    u.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp)
    u.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, lp)
    u.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)


def park():
    """把鼠标移开再采集：悬停气泡会压在任何界面文字上，属于噪声不是 bug。"""
    u.PostMessageW(hwnd, WM_MOUSEMOVE, 0, (PARK[1] << 16) | PARK[0])


def drive():
    global hwnd
    p = subprocess.Popen([EXE], cwd=D, env={**os.environ, "PVZ_UITRACE": TRACE})
    try:
        time.sleep(4.0)
        hwnd = u.FindWindowW("PvZClassC", None)
        if not hwnd:
            sys.exit("[X] 找不到窗口 PvZClassC")
        # 菜单按钮行 y=330..376；每日营地 y=396..442；关卡卡 0 中心 (204,156)
        steps = [
            ("菜单 ST_MENU", []),
            ("编组 ST_LOADOUT", [("click", (385, 353)), ("key", "M")]),
            ("天赋 ST_TALENTS", [("click", (535, 353)), ("key", "M")]),
            ("成就 ST_ACH", [("click", (685, 353)), ("key", "M")]),
            ("抽卡 ST_GACHA", [("click", (865, 353)), ("key", "M")]),
            ("每日 ST_DAILY", [("click", (500, 419)), ("key", "M")]),
            ("选关 ST_LEVELS", [("click", (170, 353))]),
            ("战斗 ST_PLAY", [("click", (204, 156)), ("sleep", 5.0)]),
            ("暂停 ST_PAUSE", [("key", "P")]),
            ("回到战斗", [("key", "P")]),
            ("三选一 ST_DRAFT", [("key", "F1")]),
            ("胜利 ST_WIN", [("key", "F2")]),
            ("失败 ST_LOSE", [("key", "F3")]),
            ("抽卡结算 ST_REWARD", [("key", "F4")]),
            ("回菜单", [("key", "M")]),
        ]
        for name, acts in steps:
            print("  → %s" % name)
            for a, v in acts:
                if a == "click":
                    click(*v)
                elif a == "key":
                    key(v)
                elif a == "sleep":
                    time.sleep(v)
                time.sleep(0.5)
            park()                       # 采集前把鼠标挪到中立位
            time.sleep(1.2)
    finally:
        p.terminate()
        try:
            p.wait(timeout=3)
        except Exception:
            p.kill()


# ----------------------------------------------------------------- 读 trace
def load():
    frames = defaultdict(list)          # 帧号 -> [rect...]
    order = []                          # (帧号, 状态)
    cur = None
    with open(TRACE, encoding="utf-8", errors="replace") as f:
        for ln in f:
            parts = ln.rstrip("\n").split("\t")
            if not parts or not parts[0]:
                continue
            if parts[0] == "F" and len(parts) >= 3:
                cur = int(parts[1])
                order.append((cur, int(parts[2])))
            elif parts[0] == "T" and cur is not None and len(parts) >= 8:
                src = int(parts[1])
                x, y = float(parts[3]), float(parts[4])
                w, h = int(parts[5]), int(parts[6])
                frames[cur].append((src, x, y, w, h, parts[7]))
    return frames, order


def inter(a, b):
    """矩形元组布局：(src, x, y, w, h, text) —— 坐标从下标 1 开始。"""
    x0, y0 = max(a[1], b[1]), max(a[2], b[2])
    x1, y1 = min(a[1] + a[3], b[1] + b[3]), min(a[2] + a[4], b[2] + b[4])
    return max(0.0, x1 - x0) * max(0.0, y1 - y0)


def main():
    if not os.path.exists(EXE):
        sys.exit("[X] 缺少 _uiaudit.exe，先跑：python _mk_uiaudit.py")
    reuse = "--reuse" in sys.argv and os.path.exists(TRACE)
    if reuse:
        print("[1/3] --reuse：跳过驱动，直接分析已有的 _ui_trace.tsv")
    else:
        if os.path.exists(TRACE):
            os.remove(TRACE)
        print("[1/3] 驱动游戏，采集各界面帧 ...")
        drive()

    print("[2/3] 分析 %s ..." % os.path.basename(TRACE))
    load_linemap()
    frames, order = load()
    state_of = dict(order)   # 帧号 -> 状态，报告里用

    per_state_frames = defaultdict(int)
    per_state_maxtext = defaultdict(int)
    collide = {}      # key -> [set(帧), 最大重叠比, 样例, 最小间距]
    bleed = {}        # key -> [set(帧), 最大越界px, 样例]

    for fidx in order:
        st = fidx[1]
        rects = frames.get(fidx[0], [])
        sname = STATE_NAME.get(st, "ST_%d" % st)
        per_state_frames[sname] += 1
        per_state_maxtext[sname] = max(per_state_maxtext[sname], len(rects))

        # --- 出血 ---
        for r in rects:
            src, x, y, w, h, txt = r
            worst = max(-x, -y, x + w - VIEW_W, y + h - VIEW_H)
            if worst > 0:
                k = (sname, src, txt)
                e = bleed.setdefault(k, [set(), 0.0, ""])
                e[0].add(fidx[0])
                e[1] = max(e[1], worst)
                e[2] = "x=%.0f y=%.0f w=%d h=%d" % (x, y, w, h)

        # --- 压字 ---
        n = len(rects)
        for i in range(n):
            for j in range(i + 1, n):
                a, b = rects[i], rects[j]
                if a[5] == b[5]:
                    continue                     # 同串（描边阴影），正常叠加
                ar = inter(a, b)
                if ar < 24.0:
                    continue
                amin = min(a[3] * a[4], b[3] * b[4])
                ratio = ar / amin if amin else 0.0
                if ratio < 0.20:
                    continue
                k = (sname,) + tuple(sorted([(a[0], a[5]), (b[0], b[5])]))
                e = collide.setdefault(k, [set(), 0.0, None])
                e[0].add(fidx[0])
                if ratio > e[1]:
                    e[1] = ratio
                    e[2] = (a, b, ar)

    out = []

    def W(s=""):
        out.append(s)

    W("=" * 96)
    W("UI 文本布局审计报告（运行时实测字形墨迹，非字体行高）")
    W("=" * 96)
    W("采到 %d 帧；各界面统计：" % len(order))
    for s in sorted(per_state_maxtext, key=lambda k: -per_state_maxtext[k]):
        W("    %-12s %4d 帧   最多 %3d 个文字" % (s, per_state_frames[s], per_state_maxtext[s]))

    W("")
    W("-" * 96)
    W("A. 同帧内压字（字形墨迹真的叠在一起）：%d 类" % len(collide))
    W("-" * 96)
    if not collide:
        W("    无")
    for k, (fs, ratio, sample) in sorted(collide.items(), key=lambda kv: -len(kv[1][0]))[:30]:
        A, B, ar = sample
        W("  [%s] %d 帧 · 重叠比最高 %.0f%% · 重叠面积 %.0fpx²" % (k[0], len(fs), ratio * 100, ar))
        for tag, r in (("一", A), ("二", B)):
            ln = to_src(r[0])
            W("      压字%s  pvz.c:%-5d 墨迹(%.0f,%.0f  %dx%d)  「%s」"
              % (tag, ln, r[1], r[2], r[3], r[4], r[5]))
            W("             %s" % src_text(ln))

    W("")
    W("-" * 96)
    W("B. 越出视口 1000x650 的文字（会被 DC 边缘裁掉）：%d 类" % len(bleed))
    W("-" * 96)
    if not bleed:
        W("    无")
    for k, (fs, worst, sample) in sorted(bleed.items(), key=lambda kv: -kv[1][1])[:30]:
        ln = to_src(k[1])
        W("  [%s] %d 帧 · 最多越界 %.0fpx · pvz.c:%d  %s  「%s」"
          % (k[0], len(fs), worst, ln, sample, k[2]))
        W("        %s" % src_text(ln))

    txt = "\n".join(out)
    print(txt)
    open(REPORT, "w", encoding="utf-8").write(txt + "\n")
    print("\n[3/3] 报告已写入 %s" % REPORT)


if __name__ == "__main__":
    main()
