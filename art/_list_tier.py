# -*- coding: utf-8 -*-
import re
path = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"
txt = open(path, encoding='utf-8', errors='ignore').read()
# 提取 rgPlants 定义块
m = re.search(r'static const RgPlantDef rgPlants\[RG_PLANT_N\] = \{(.*?)\};', txt, re.S)
if not m:
    print("no plants")
    exit(0)
block = m.group(1)
entries = re.finditer(r'/\* (\d+) \*/\s*\{\s*L"([^"]+)",\s*(RGQ_\w+),', block)
plants = []
for e in entries:
    idx = int(e.group(1))
    name = e.group(2)
    tier = e.group(3)
    plants.append((idx, name, tier))
# 输出 tier 3 LEGEND 和 5 ULTRA
for idx,name,tier in plants:
    if tier in ('RGQ_LEGEND','RGQ_ULTRA'):
        print(f"P{idx:02d} {tier} {name}")
print("count", len([t for t in plants if t[2] in ('RGQ_LEGEND','RGQ_ULTRA')]))
