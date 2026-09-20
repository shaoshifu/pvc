# -*- coding: utf-8 -*-
import re
path = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"
txt = open(path, encoding='utf-8', errors='ignore').read()
m = re.search(r'static const RgZombieDef rgZombies\[RG_ZOMBIE_N\] = \{(.*?)\};', txt, re.S)
if not m:
    print("no zombies")
    exit(0)
block = m.group(1)
entries = re.finditer(r'/\* (\d+) \*/\s*\{\s*L"([^"]+)",\s*(RGQ_\w+),', block)
zombs=[]
for e in entries:
    idx=int(e.group(1)); name=e.group(2); tier=e.group(3)
    if tier in ('RGQ_LEGEND','RGQ_ULTRA'):
        zombs.append((idx,name,tier))
for idx,name,tier in zombs:
    print(f"Z{idx:02d} {tier} {name}")
print("count", len(zombs))
