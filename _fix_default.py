# -*- coding: utf-8 -*-
"""把 updateGame 里那个括号不平衡的 default 块回退成 `default: break;`。

背景：
  这个「通用射手兜底」块是在某次**外部编辑**里加进来的 ——
  证据是 `_dump.c`（16:01 由本项目工具生成，是 pvz.c 的副本）里还没有它，
  而 16:22 的 pvz.c 里已经有了。它插入时吃掉了 `case PT_HERO_DOOM: {` 的闭括号，
  于是整个 updateGame 少一个 `}`，后面的 onClick / draftCardRect / openDraft …
  全被编译器当成"嵌套在 updateGame 里的局部声明"，
  报出来的是一串 "invalid storage class for function"（症状离病因 1000 多行）。

  定位手段：`_brace_scan.py --stack` 直接给出"未闭合的 { 起始行 = 5772"，
  也就是 updateGame 的函数体括号，一步把范围锁死。

本脚本把它整块摘掉、恢复成原来的单行 `default: break;`，
先让括号回到平衡、能编译；要不要以正确姿势把兜底逻辑加回来另行决定。

用法： python _fix_default.py [--dry]
"""
import sys

P = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"
KEY = "通用射手兜底"

src = open(P, encoding="utf-8").read().split("\n")

hits = [i for i, l in enumerate(src) if KEY in l]
if not hits:
    raise SystemExit("找不到「%s」——可能已经处理过了" % KEY)
ki = hits[0]

# 向前找这个块的 `default: {`
st = None
for i in range(ki, max(-1, ki - 6), -1):
    if src[i].strip() == "default: {":
        st = i
        break
if st is None:
    raise SystemExit("找到关键字但没找到配对的 `default: {`，请人工检查")

# 从 st 开始做括号匹配，找到块的闭合
d = 0
en = None
for i in range(st, len(src)):
    d += src[i].count("{") - src[i].count("}")
    if d == 0 and i > st:
        en = i
        break
if en is None:
    raise SystemExit("块一直没闭合 —— 这本身就是不平衡的证据，需要人工看")

print("块范围：第 %d - %d 行（共 %d 行）" % (st + 1, en + 1, en - st + 1))
print("首行：%s" % src[st][:70])
print("尾行：%s" % src[en][:70])

if "--dry" in sys.argv:
    print("[dry] 未写盘")
    sys.exit(0)

new = list(src)
new[st:en + 1] = ["                        default: break;"]
open(P, "w", encoding="utf-8", newline="").write("\n".join(new))
print("已替换为单行 `default: break;`")
