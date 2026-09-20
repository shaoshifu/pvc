# -*- coding: utf-8 -*-
r"""给 rgPlants 首批 50 条补上阳光下标 cost，消掉 -Wmissing-field-initializers。

背景：RgPlantDef 末尾新增了 cost 字段。为了让首批 50 条"零改动"，
一开始打算靠 C 的自动补 0 —— 但那会触发 50 条编译警告，而本项目要求零警告。
所以还是老老实实补数据，只是用脚本批量做。

为什么不用一条大正则：
  第一版写的 `(/\* \d{2} \*/ \{ L"[^"]+", (RGQ_[A-Z]+),[^;{}]*?L"[^"]+")(\s*\},)`
  只命中 13/50 —— 含 `A | B` 多 trait 的条目全部漏掉（TRAITS 段里的分支让
  非贪婪段的行为和预期不一致）。改成**先把数组按 `/* NN */ { ... },` 切成条目，
  再逐条判断尾部是不是 `L"..."`**，语义清晰且不会误伤已有 cost 的新条目。

用法： python _add_cost.py [--dry]
"""
import re
import sys

PATH = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"
ARRAY_HEAD = "static const RgPlantDef rgPlants[RG_PLANT_N] = {"

# 品级基准价（与 C 侧 RGQ_COST 一致；个别条目在平衡阶段可单独覆写）
COST = {
    "RGQ_COMMON": 30, "RGQ_RARE": 55, "RGQ_EPIC": 85,
    "RGQ_LEGEND": 120, "RGQ_MYTH": 155, "RGQ_ULTRA": 190,
}

ITEM = re.compile(r"(/\* (\d{2}) \*/ \{)(.*?)(\},)", re.S)
TAIL_DESC = re.compile(r'L"[^"]+"\s*$')      # 条目尾部就是描述串 == 还没有 cost


def main():
    dry = "--dry" in sys.argv
    src = open(PATH, encoding="utf-8").read()
    i0 = src.index(ARRAY_HEAD)
    i1 = src.index("\n};", i0)
    head, body, tail = src[:i0], src[i0:i1], src[i1:]

    added, skipped = [], []

    def rep(m):
        pre, idx, mid, post = m.groups()
        if not TAIL_DESC.search(mid):
            skipped.append(idx)
            return m.group(0)                       # 已有 cost，原样返回
        tier = re.search(r"(RGQ_[A-Z]+)", mid).group(1)
        added.append((idx, tier, COST.get(tier, 30)))
        return pre + mid + (", %d" % COST.get(tier, 30)) + post

    body2 = ITEM.sub(rep, body)
    print("补价 %d 条，已有 cost %d 条" % (len(added), len(skipped)))
    bad = [s for s in skipped if int(s) < 50]
    if bad:
        print("!! 旧条目里被误判为「已有 cost」的：%s" % bad)
    for idx, tier, c in added[:5]:
        print("  /* %s */ %-11s -> %d" % (idx, tier, c))
    if len(added) > 5:
        print("  ...")
    if dry:
        print("[dry] 未写盘")
        return
    # 首批共 50 条；其中 /* 49 */ 当初是手工加过价的，会被正确跳过，
    # 所以期望补价数 = 50 - 旧条目里"已有 cost"的条数。
    expect = 50 - len(bad)
    if len(added) != expect:
        print("!! 期望补 %d 条，实际 %d，格式可能有偏差，已中止写盘" % (expect, len(added)))
        return
    open(PATH, "w", encoding="utf-8", newline="").write(head + body2 + tail)
    print("已写回", PATH)


if __name__ == "__main__":
    main()
