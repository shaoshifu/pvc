# -*- coding: utf-8 -*-
"""定位 C 源文件里括号不平衡的位置。

为什么需要：`gcc` 报的 "invalid storage class for function 'X'" 只是**症状** ——
真正的病因是 X **之前**某个函数多（或少）了一个 `}`，
编译器于是把 X 当成了嵌套在别的函数里的局部声明。
报错行号离病因可能差几百上千行，肉眼翻不动。

模式：
  python _brace_scan.py 5771 7090          # 打印该区间内所有"顶层块闭合"点
  python _brace_scan.py 5771 6000 --d      # 只在深度变化时打印（最有信息量）
  python _brace_scan.py 5426 5448 --v      # 逐行打印
  python _brace_scan.py --stack            # 直接列出**未闭合的 { 起始行**
"""
import re
import sys

_DEF = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"
# 允许 --file=xxx 指定别的文件。排查时最有用的用法是拿一份**改动前的副本**
# （本项目有 _dump.c）对照跑，一眼就能看出差异是新引入的还是原本就有的。
PATH = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--file=")), _DEF)


def strip_noise(src):
    """去掉注释与各种字面量 —— 里面的括号不算数。

    ⚠️ 块注释**必须替换成等量的换行**，不能删成空串：
    删掉换行会让 clean 的行号与原文错位，"第 N 行不平衡"的结论就全是错的。
    实测因此把一处正常的 `for` 循环误判成病因，白查一轮。
    """
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"),
                 src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    # 宽字符串 L"..."
    src = re.sub(r'L"(?:\\.|[^"\\])*"', 'L""', src)
    src = re.sub(r'"(?:\\.|[^"\\])*"', '""', src)
    src = re.sub(r"'(?:\\.|[^'\\])*'", "''", src)
    return src


def main():
    raw = open(PATH, encoding="utf-8", errors="replace").read()
    rawlines = raw.split("\n")
    clean = strip_noise(raw)

    depth = 0
    stack = []          # 未闭合的 '{' 所在行号
    events = []         # (行号, 深度, 原文)

    for i, line in enumerate(clean.split("\n"), 1):
        for ch in line:
            if ch == "{":
                stack.append(i)
                depth += 1
            elif ch == "}":
                if stack:
                    stack.pop()
                depth -= 1
        events.append((i, depth))

    if "--stack" in sys.argv:
        print("未闭合的 { 起始行（共 %d 个）：" % len(stack))
        for ln in stack:
            print("  %5d  %s" % (ln, rawlines[ln - 1][:76]))
        print("\n最终 depth = %d（应为 0）" % depth)
        return

    # 过滤掉 --file=xxx 这类开关，否则它会被当成行号去 int() 而崩溃
    pos = [a for a in sys.argv[1:] if not a.startswith("--")]
    lo = int(pos[0]) if len(pos) > 0 else 1
    hi = int(pos[1]) if len(pos) > 1 else 10 ** 9
    print("行号   depth  内容")
    prev = 0
    for i, depth in events:
        if not (lo <= i <= hi):
            prev = depth
            continue
        if "--v" in sys.argv:
            print("%5d  %5d  %s" % (i, depth, rawlines[i - 1][:66]))
        elif "--d" in sys.argv:
            # 只在深度变化时打印。1300 行的区间用 --v 逐行输出看不过来，
            # 而"深度变化点"正好就是块的开闭位置，信息量最高。
            if depth != prev:
                print("%5d  %5d  %s" % (i, depth, rawlines[i - 1][:70]))
        elif depth == 0:
            print("%5d  %5d  ← 顶层块在此闭合  %s" % (i, depth, rawlines[i - 1][:64]))
        prev = depth
    print("\n最终 depth = %d（应为 0）" % depth)


if __name__ == "__main__":
    main()
