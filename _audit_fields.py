# -*- coding: utf-8 -*-
"""字段读写成对审计：找出"只写不读"（设了没人消费）与"只读不写"（永远是初值）。

规则（来自 game-mechanics-audit 技能）：
  · 必须同时覆盖 `->f` 和 `.f` —— 游戏里大量用 units[i].f / grid[r][c].f，
    只查 ->f 会让一半字段判错。
  · 排除数据表（已剥）与 #define 行。
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

KW = {"const","unsigned","signed","int","float","double","char","short","long","void",
      "static","struct","union","enum","wchar_t","COLRORREF","COLORREF","Sprite","Pea",
      "Zombie","Plant","SunDrop","Particle","Mower","RgZombieDef","RgPlantDef"}

def struct_fields(name):
    end = src.index("} %s;" % name)
    start = src.rindex("typedef struct", 0, end)
    body = re.sub(r"/\*.*?\*/", "", src[start:end], flags=re.S)
    out = []
    for decl in body.split(";"):
        line = decl.strip().split("\n")[-1].strip()
        ids = re.findall(r"[A-Za-z_]\w*", line)
        if len(ids) >= 2:
            out += [n for n in ids[1:] if n not in KW]
    return out

def stats(f):
    w = len(re.findall(r"(?:->|\.)\s*" + f + r"\s*(?:[-+*/%|&^]?=(?!=)|\+\+|--)", text))
    r = len(re.findall(r"(?:->|\.)\s*" + f + r"\b(?!\s*(?:[-+*/%|&^]?=(?!=)|\+\+|--))", text))
    return r, w

for sname in ("Plant", "Zombie"):
    print("=== %s ===" % sname)
    onlyw, onlyr = [], []
    for f in struct_fields(sname):
        r, w = stats(f)
        if w > 0 and r == 0: onlyw.append((f, w))
        if r > 0 and w == 0: onlyr.append((f, r))
    print("  只写不读（设了没人消费）: %s" % (", ".join("%s(w=%d)" % t for t in onlyw) or "无"))
    print("  只读不写（永远是初值）  : %s" % (", ".join("%s(r=%d)" % t for t in onlyr) or "无"))
    print()
