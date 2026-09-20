# -*- coding: utf-8 -*-
r"""把 `_add_cost.py` 写出的 `L"..." , 30},` 整理成 `L"...", 30 },`。

为什么要单独走一趟：
  _add_cost.py 是在"条目尾部"追加 `, %d`，而尾部原本是 `" },`（引号 + 空格 + `},`），
  于是拼出来变成 `" , 30},` —— 语法正确但排版难看，而且会让下游按
  `L"[^"]*",\s*\d+\s*\},` 解析的脚本（如 _dump_roster.py）全部漏匹配。
  与其让每个消费方都写宽松正则，不如把源文件整理干净。

用法： python _tidy_cost.py [--dry]
"""
import re
import sys

PATH = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"
PAT = re.compile(r'"\s+,\s*(\d+)\s*\},')


def main():
    dry = "--dry" in sys.argv
    src = open(PATH, encoding="utf-8").read()
    out, n = PAT.subn(lambda m: '", %s },' % m.group(1), src)
    print("整理了 %d 处" % n)
    if dry:
        print("[dry] 未写盘")
        return
    open(PATH, "w", encoding="utf-8", newline="").write(out)
    print("已写回", PATH)


if __name__ == "__main__":
    main()
