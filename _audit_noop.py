# -*- coding: utf-8 -*-
"""探测"空实现"的能力位：`if (T & BIT) { 只有注释 }` 这种写法。

为什么需要它：静态审计只能回答"这个位被引用了吗"。
但引用可以是一个**空壳** —— 本项目就有一个真实案例：
    if (T & RZ3_REBORN) {
        /* 复生由 zombieDie() 统一处理；这里不再等 hp<=0 的 tick */
    }
注释说"由别处统一处理"，而别处根本没有这段代码 ——
卡片上写的"死亡后满血复活"从未生效，静态审计却会判它"已接通"。
"""
import re, sys

src = open('pvz.c', encoding='utf-8').read()
lines = src.split('\n')

tbl = []
i = 0
while i < len(lines):
    l = lines[i]
    if re.match(r"^static\s+(const\s+)?[\w\s\*]+\[\s*[\w\s]*\]\s*=\s*\{", l):
        if l.rstrip().endswith("};"):
            tbl.append((i, i)); i += 1; continue
        for j in range(i + 1, min(i + 6000, len(lines))):
            if lines[j] == "};":
                tbl.append((i, j)); i = j; break
    i += 1
keep = [l for n, l in enumerate(lines)
        if not l.lstrip().startswith('#define') and not any(a <= n <= b for a, b in tbl)]
text = '\n'.join(keep)

def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s

def find_block(s, start):
    """从 s[start:] 里第一个 '{' 开始，返回匹配的花括号内容（含嵌套）。"""
    i = s.find('{', start)
    if i < 0: return None, -1
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '{': depth += 1
        elif s[j] == '}':
            depth -= 1
            if depth == 0: return s[i+1:j], j
    return None, -1

defs = sorted(set(re.findall(r"^#define\s+(RG_T_\w+|RZ\d?_\w+)\s+", src, flags=re.M)))
alias = {}
for l in lines:
    m = re.match(r"#define\s+(\w+)\s+(RG_T_\w+|RZ\d?_\w+)\s*$", l.strip())
    if m: alias.setdefault(m.group(2), []).append(m.group(1))

# 收集数据表里的挂载单位（表已被剥掉，这里单独解析名称）
units = {}
for tbl_name, pat in (("rgPlants", r'\{\s*L"([^"]+)"\s*,\s*(RGQ_\w+)\s*,\s*(\w+)\s*,\s*([^,]+?),'),
                      ("rgZombies", r'\{\s*L"([^"]+)"\s*,\s*(RGQ_\w+)\s*,\s*(\w+)\s*,\s*([^,]+?),')):
    m = re.search(tbl_name + r"\[.*?\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m: continue
    for mm in re.finditer(pat, m.group(1), re.S):
        name, tier, base, traits = mm.group(1), mm.group(2), mm.group(3), mm.group(4)
        for b in re.findall(r"\b(RG_T_\w+|RZ\d?_\w+)\b", traits):
            units.setdefault(b, []).append(name)

noop = []
for b in defs:
    names = [b] + alias.get(b, [])
    found_total = 0
    empty_total = 0
    for nm in names:
        for m in re.finditer(r"if\s*\([^)]*\b" + nm + r"\b[^)]*\)", text):
            body, _ = find_block(text, m.end())
            if body is None: continue
            found_total += 1
            if strip_comments(body).strip() == "":
                empty_total += 1
    if found_total > 0 and empty_total == found_total:
        noop.append((b, names, found_total, units.get(b, [])))

print("★ 只有空壳 if 的能力位：%d 个\n" % len(noop))
for b, names, n, us in sorted(noop):
    print("   %-18s %d 处引用全是空壳   挂载: %s" % (b, n, ", ".join(us) if us else "（无）"))
    for nm in names:
        if nm != b: print("        别名: %s" % nm)
