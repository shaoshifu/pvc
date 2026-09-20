# -*- coding: utf-8 -*-
"""A/B 遮挡实验：把同一对标记块分别画在 StretchBlt 之前/之后。
   PRE  标记 (820,200)-(980,224) 品红  —— 若被擦掉 => 该层内容全部不可见
   POST 标记 (820,250)-(980,274) 纯绿  —— 必须可见（阳性对照）
"""
import io, os

D = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c"
src = open(os.path.join(D, "pvz.c"), "r", encoding="utf-8").read()

anchor = "    /* ================= 降采样到 1x ================= */"
assert src.count(anchor) == 1, src.count(anchor)

PRE = ("    fillRound(gFrameDC, 820, 200, 980, 224, 7, RGB(255, 0, 255));\n"
       "    putTextCS(gFrameDC, gF18, RGB(0,0,0), RGB(0,0,0), L\"PRE\", 900, 212);\n")
POST = ("    fillRound(gFrameDC, 820, 250, 980, 274, 7, RGB(0, 255, 0));\n"
        "    putTextCS(gFrameDC, gF18, RGB(0,0,0), RGB(0,0,0), L\"POST\", 900, 262);\n")

out = src.replace(anchor, PRE + anchor + POST, 1)
open(os.path.join(D, "_hudtest.c"), "w", encoding="utf-8").write(out)
print("生成 _hudtest.c  OK")
