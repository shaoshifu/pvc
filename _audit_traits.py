# -*- coding: utf-8 -*-
"""能力位静态审计：逻辑区（去掉 #define 与全部数据表）里每个位被消费了几次。

踩过的两个坑（都会让结论完全反向，必须防）：
 1) 表尾必须认**行首无缩进**的 "};" —— 否则函数内的局部数组会把几百行实现吞掉；
 2) **单行表**（`static const int X[..] = { 1, 2, 3 };`）必须当场判定为表，
    否则会一路找到下一个行首 "};"，实测吞掉 3816 行实现。
"""
import re, sys

src = open('pvz.c', encoding='utf-8').read()
lines = src.split('\n')

tbl = []
i = 0
while i < len(lines):
    l = lines[i]
    if re.match(r"^static\s+(const\s+)?[\w\s\*]+\[\s*[\w\s]*\]\s*=\s*\{", l):
        if l.rstrip().endswith("};"):          # 单行表
            tbl.append((i, i)); i += 1; continue
        for j in range(i + 1, min(i + 6000, len(lines))):
            if lines[j] == "};":
                tbl.append((i, j)); i = j; break
    i += 1

def in_tbl(n):
    return any(a <= n <= b for a, b in tbl)

logic = [(n + 1, l) for n, l in enumerate(lines)
         if not l.lstrip().startswith('#define') and not in_tbl(n)]
logic_txt = '\n'.join(l for _, l in logic)

defs = {}
for n, l in enumerate(lines):
    m = re.match(r"#define\s+(RG_T_\w+|RZ\d?_\w+)\s+\(?\(?(1u?l?l?\s*<<\s*\d+|0x[0-9A-Fa-f]+)", l)
    if m:
        defs[m.group(1)] = n + 1
alias = {}
for n, l in enumerate(lines):
    m = re.match(r"#define\s+(\w+)\s+(RG_T_\w+|RZ\d?_\w+)\s*$", l.strip())
    if m and m.group(1) not in defs:
        alias[m.group(1)] = m.group(2)

def consume(bit):
    names = [bit] + [a for a, y in alias.items() if y == bit]
    return sum(len(re.findall(r"\b" + nm + r"\b", logic_txt)) for nm in names), names

zero = [(b, *consume(b)) for b in defs if consume(b)[0] == 0]

if len(sys.argv) > 1 and sys.argv[1] == '--ranges':
    print("表 %d 张 / %d 行；逻辑区 %d 行" % (len(tbl), sum(b-a+1 for a, b in tbl), len(logic)))
    for a, b in tbl:
        print("   %5d-%5d  %s" % (a+1, b+1, lines[a][:56]))
    sys.exit()

print("表 %d 张 / %d 行；逻辑区 %d 行" % (len(tbl), sum(b-a+1 for a, b in tbl), len(logic)))
print("能力位 %d 个，其中逻辑区零消费 %d 个\n" % (len(defs), len(zero)))
for b, c, names in sorted(zero):
    al = ("  ← 别名 %s" % ",".join(n for n in names if n != b)) if len(names) > 1 else ""
    print("   %-18s%s" % (b, al))
