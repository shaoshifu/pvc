# -*- coding: utf-8 -*-
"""把 art/_cards_new.py 里的 aspect 同步进 pvz.c 的 rgPlants 表。

【为什么要有这个脚本】
  aspect 有三份可能的载体：数据源（_cards_new.py）、补丁脚本（_patch_cards.py）、
  以及最终的 pvz.c。三份手工同步必然会漂移 —— 这一轮就漂了：
  始祖 30 张有 aspect、殿堂 20 张一条都没有（数据源里根本没写），
  结果是"殿堂档没有内部强弱梯度"，而这件事不报错、不崩溃，只是设计意图落不了地。

  所以约定：**_cards_new.py 是 aspect 的唯一真源**，pvz.c 由本脚本同步。
  重放 `_patch_cards.py` 也能得到同样的结果（它读同一个数据源），
  但在已经打过补丁的 pvz.c 上重放会重复插入，所以日常改数值走这个脚本。

【幂等性】
  可以反复跑。已存在 aspect 就改值，没有就补上，不多插一个逗号。
  跑完自己回读校验，并打印"改了哪些行"。

用法：
    python _sync_aspect.py            # 同步
    python _sync_aspect.py --check    # 只检查是否一致，不写文件
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "art"))

import _cards_new as CN                                              # noqa: E402

SRC = os.path.join(HERE, "pvz.c")
START = "static const RgPlantDef rgPlants[RG_PLANT_N] = {"
END = "\n};\n#pragma GCC diagnostic pop"


def want():
    """{卡名: aspect}，只收有 aspect 的（0/缺失 = 基准 100，不写出来）。"""
    d = {}
    for _idx, c, _prim in CN.all_cards():
        a = c.get("aspect")
        if a:
            d[c["name"]] = int(a)
    return d


def main():
    check_only = "--check" in sys.argv
    wantd = want()
    s = io.open(SRC, encoding="utf-8").read()
    i = s.index(START) + len(START)
    j = s.index(END, i)
    body = s[i:j]

    # 按 "/* N */ {" 切分条目（保留分隔符，方便原样拼回）
    parts = re.split(r"(?=/\* \d+ \*/ \{)", body)
    changed, missing, already = [], [], []

    for k, part in enumerate(parts):
        m = re.match(r'/\* (\d+) \*/\s*\{\s*L"([^"]+)"\s*,\s*(RGQ_\w+)\s*,\s*(PT_\w+)\s*,', part)
        if not m:
            continue
        idx, name, tier, base = int(m.group(1)), m.group(2), m.group(3), m.group(4)
        if name not in wantd:
            continue
        asp = wantd[name]
        # 尾部形如：L"desc", cost }  或  L"desc", cost, aspect }
        # ⚠️ 正则**不能**匹配到条目的结尾（`},` 和换行属于分隔符）。
        #    第一版写成了 `\}\s*,?\s*$`，把 `},\n` 一起吃掉了 →
        #    重建出来是 `}/* 109 */ {`，整个表变成一行、C 直接语法错误。
        #    所以这里只吃到 `}`，后面原样保留。
        tail = re.search(r'(L"[^"]*"\s*,\s*)(\d+)(?:\s*,\s*(\d+))?\s*\}', part)
        if not tail:
            missing.append((idx, name))
            continue
        old_asp = int(tail.group(3)) if tail.group(3) else 0
        if old_asp == asp:
            already.append(name)
            continue
        newtail = tail.group(1) + tail.group(2) + ", " + str(asp) + " }"
        parts[k] = part[:tail.start()] + newtail + part[tail.end():]
        changed.append((idx, name, old_asp, asp))

    print("  %-14s %d 张" % ("数据源里有 aspect", len(wantd)))
    print("  %-14s %d 张" % ("已一致", len(already)))
    print("  %-14s %d 张" % ("需要改", len(changed)))
    for idx, name, a0, a1 in changed:
        print("    #%-4d %-10s %s → %d" % (idx, name, (str(a0) if a0 else "(无)"), a1))
    if missing:
        print("  ★ 有 %d 张在 pvz.c 里找不到对应行：%s" % (len(missing), missing))

    if check_only:
        return 0 if not changed else 1
    if changed:
        io.open(SRC, "w", encoding="utf-8", newline="\n").write(s[:i] + "".join(parts) + s[j:])
        print("  已写入 pvz.c")
    else:
        print("  pvz.c 已是最新，未改动")
    return 0


if __name__ == "__main__":
    sys.exit(main())
