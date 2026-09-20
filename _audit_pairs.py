# -*- coding: utf-8 -*-
"""逐位对照表：能力位 → 挂载单位的卡片描述 → 实现块里的动作摘要。
用来一眼扫出"描述承诺了、实现没做"的位（静态引用检查查不出这类）。"""
import re

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
    return re.sub(r"//[^\n]*", "", s)

def block_after(s, m):
    i = s.find('{', m.end())
    if i < 0: return ""
    d = 0
    for j in range(i, len(s)):
        if s[j] == '{': d += 1
        elif s[j] == '}':
            d -= 1
            if d == 0: return s[i+1:j]
    return ""

def summarize(b):
    stmts = []
    for m in re.finditer(r"if\s*\([^)]*\b" + b + r"\b[^)]*\)", text):
        body = strip_comments(block_after(text, m))
        for st in body.split(';'):
            st = st.strip()
            if not st or st.startswith('{') or st in ('}',): continue
            stmts.append(st)
    # 只保留"有副作用"的语句，去掉纯 if 条件行
    acts = [x for x in stmts if re.search(r"(=|\(|\.)", x) and not x.startswith('if')]
    return acts[:3]

# 挂载单位 + 描述
units = {}
m = re.search(r"rgPlants\[RG_PLANT_N\]\s*=\s*\{(.*?)\n\};", src, re.S)
for mm in re.finditer(r'\{\s*L"([^"]+)"\s*,\s*(RGQ_\w+)\s*,\s*(\w+)\s*,\s*([^,]+?),\s*\n\s*L"([^"]*)"', m.group(1), re.S):
    name, traits, desc = mm.group(1), mm.group(4), mm.group(5)
    for b in re.findall(r"\b(RG_T_\w+)\b", traits):
        units.setdefault(b, []).append((name, desc))

bits = sorted(set(re.findall(r"^#define\s+(RG_T_\w+)\s", src, flags=re.M)))
for b in bits:
    us = units.get(b)
    if not us: continue
    print("── %s" % b)
    print("   描述: %s | %s" % (us[0][0], us[0][1]))
    acts = summarize(b)
    print("   实现: %s" % (" ; ".join(a[:70] for a in acts) if acts else "★ 没有任何带副作用的语句"))
