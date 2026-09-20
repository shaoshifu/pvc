# -*- coding: utf-8 -*-
"""把「1x 层变换」那组全局声明/函数整体前移到绘图小工具之前。

背景：drawVignette（约 307 行）和 9 个绘图小工具（85~200 行）都要用
gFrameDC / gFrameK / gFrameXF / frameXFSync，而它们原本声明在 2600 行附近。
C 语言要求先声明后使用，所以整块搬到前面。

顺带删掉两个已经用不上的函数（fxPx / viewSS）—— 留着会触发
-Wunused-function，而本项目要求零警告。

用法： python _move_block.py [--dry]
"""
import sys

P = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"
START = "static HDC   gFrameDC;              /* 帧缓冲（1x 层）：绘制辅助函数要用到 */"
END = "static float   gDbgGotXF  = -1.0f;"
ANCHOR = "static void frameXFSync(HDC dc);"

DROP = ["static int viewSS(void) { return (gViewW >= 1400) ? 1 : SS; }\n",
        "static int fxPx(float v) { return (int)(v * (float)gViewW / (float)VIEW_W + 0.5f); }\n"]


def main():
    dry = "--dry" in sys.argv
    s = open(P, encoding="utf-8").read()

    # 1) 摘出要搬的整块
    i0 = s.index(START)
    i1 = s.index(END) + len(END) + 1
    block = s[i0:i1]
    s = s[:i0] + s[i1:]
    print("摘出 %d 字符" % len(block))

    # 2) 删掉不再使用的函数
    for d in DROP:
        if d in s:
            s = s.replace(d, "")
            print("  删除未使用：%s" % d.strip()[:56])

    # 3) 插到绘图小工具之前（替换掉原来那行单句前向声明）
    if ANCHOR not in s:
        raise SystemExit("找不到锚点：" + ANCHOR)
    s = s.replace(ANCHOR + "\n", block, 1)
    print("已插入到绘图小工具之前")

    if dry:
        print("[dry] 未写盘")
        return
    open(P, "w", encoding="utf-8", newline="").write(s)
    print("已写回", P)


if __name__ == "__main__":
    main()
