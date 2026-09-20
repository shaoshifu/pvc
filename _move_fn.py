# -*- coding: utf-8 -*-
"""把 renderEnsureBuffers()（含它的注释块）移到相关全局变量声明之后。

背景：新增的 renderEnsureBuffers 用到了 gFrameBM / gWorldBM / gViewW / gScaleXF，
但插入位置在那些声明的**前面**（它们原本散落在文件更下方），
于是编译报一堆 "undeclared identifier"。
手工剪切又容易把注释块落下，所以用脚本整块搬。

用法： python _move_fn.py [--dry]
"""
import sys

P = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"
MARK = "渲染缓冲：尺寸跟随视图"       # 注释块的标志句
ANCHOR = "static int  gViewW = VIEW_W"  # 移到这一行之后


def main():
    dry = "--dry" in sys.argv
    lines = open(P, encoding="utf-8").read().split("\n")

    mi = next(i for i, l in enumerate(lines) if MARK in l)
    # 注释块起点：往上找第一个 '/* ====' 行
    st = mi
    while st > 0 and not lines[st].lstrip().startswith("/* ===="):
        st -= 1
    # 函数终点：从 mark 往下第一个顶格 '}'
    fn = next(i for i in range(mi, len(lines)) if lines[i].startswith("static void renderEnsureBuffers"))
    en = next(i for i in range(fn + 1, len(lines)) if lines[i] == "}")

    block = lines[st:en + 1]
    print("搬移范围：%d - %d（%d 行）" % (st + 1, en + 1, len(block)))
    print("  首行：%s" % lines[st][:60])
    print("  末行：%s" % lines[en][:60])

    rest = lines[:st] + lines[en + 1:]
    ai = next(i for i, l in enumerate(rest) if l.startswith(ANCHOR))
    print("插入到第 %d 行之后：%s" % (ai + 1, rest[ai][:60]))

    out = rest[:ai + 1] + [""] + block + rest[ai + 1:]

    if dry:
        print("[dry] 未写盘")
        return
    open(P, "w", encoding="utf-8", newline="").write("\n".join(out))
    print("已写回", P)


if __name__ == "__main__":
    main()
