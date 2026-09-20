# -*- coding: utf-8 -*-
"""统计植物分级分布：肉鸽 100 株按 RGQ_* 档位、永久植物按 plantDefs.rarity。"""
import re
src = open("pvz.c", encoding="utf-8").read()

def block(start_marker, end_marker="\n};"):
    i = src.index(start_marker)
    j = src.index(end_marker, i)
    return src[i:j]

# ---- 肉鸽 100 株 ----
rg = block("static const RgPlantDef rgPlants[RG_PLANT_N] = {")
RGQ = ["RGQ_COMMON", "RGQ_RARE", "RGQ_EPIC", "RGQ_LEGEND", "RGQ_MYTH", "RGQ_ULTRA",
       "RGQ_HALL"]
NAME = ["普通", "稀有", "史诗", "传说", "神级", "传奇", "殿堂"]
cnt = {q: 0 for q in RGQ}
rows = re.findall(r"\{\s*L\"([^\"]+)\"\s*,\s*(RGQ_\w+)", rg)
for n, q in rows:
    cnt[q] += 1
print("肉鸽植物 rgPlants：共 %d 株" % len(rows))
for q, nm in zip(RGQ, NAME):
    print("   %-4s %-11s %3d 株" % (nm, q, cnt[q]))

# ---- 永久植物 plantDefs ----
pd = block("static const PlantDef plantDefs[PT_COUNT] = {")
RAR = {0: "普通", 1: "稀有", 2: "史诗", 3: "传奇", 4: "神话"}
hero_cnt = {}
norm_cnt = {}
for m in re.finditer(r"\{\s*L\"([^\"]+)\"\s*,\s*[^,]+,\s*[^,]+,\s*[^,]+,\s*(\d+)\s*,\s*(\d)\s*,", pd):
    name, rar, hero = m.group(1), int(m.group(2)), int(m.group(3))
    (hero_cnt if hero else norm_cnt)[rar] = (hero_cnt if hero else norm_cnt).get(rar, 0) + 1
print("\n永久植物 plantDefs（普通卡池）：共 %d 株" % sum(norm_cnt.values()))
for r in sorted(norm_cnt):
    print("   %-4s rarity=%d  %3d 株" % (RAR[r], r, norm_cnt[r]))
print("神级植物 PT_HERO_*：共 %d 株" % sum(hero_cnt.values()))
for r in sorted(hero_cnt):
    print("   %-4s rarity=%d  %3d 株" % (RAR[r], r, hero_cnt[r]))
