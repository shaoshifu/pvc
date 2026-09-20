# -*- coding: utf-8 -*-
"""把 pvz.c 里的 100 株肉鸽植物导出成可核对的 Markdown 表。

为什么要有这个：
  用户要求"为每种植物明确设定品级/价格/伤害/机制/范围/攻速/增益/减益"，
  这些信息分散在 pvz.c 的三个地方（RgPlantDef 表、RGQ_* 倍率表、rgPlantTraitTick 的实现）。
  人肉核对 100 行不现实，也容易漏掉"某个 trait 位其实没写实现"这种情况。
  本脚本把它们拼成一张表，顺便做两项自检：
    · 每个用到的 trait 位，在 rgPlantTraitTick 里是否有对应分支；
    · 价格是否落在用户要求的 10~200 区间内。

用法： python _dump_roster.py > ROSTER.md
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = open(os.path.join(ROOT, "pvz.c"), encoding="utf-8", errors="replace").read()

TIER_CN = {"RGQ_COMMON": "普通", "RGQ_RARE": "稀有", "RGQ_EPIC": "史诗",
           "RGQ_LEGEND": "传说", "RGQ_MYTH": "神级", "RGQ_ULTRA": "传奇"}

# trait 名 -> 中文简述（核对用；与 pvz.c 的 #define 注释一一对应）
TRAIT_CN = {
    "RG_T_MULTILANE": "多线齐射", "RG_T_TIMEFREEZE": "时停", "RG_T_CORRODE": "腐蚀破甲",
    "RG_T_DEVOURBUL": "吞弹转阳光", "RG_T_MIRROR": "镜像分身", "RG_T_GROWEAT": "吞噬成长",
    "RG_T_CHAINBOOM": "链式爆炸", "RG_T_GAMBLEDMG": "概率十倍", "RG_T_SHARESUN": "共享阳光",
    "RG_T_GRAVITY": "重力眩晕", "RG_T_DROPCARD": "掉落卡牌", "RG_T_LOAN": "贷款",
    "RG_T_WEBSLOW": "蛛网减速", "RG_T_ROOFWALK": "沿格移动", "RG_T_REWIND": "时间回溯",
    "RG_T_DOUBLEBODY": "敌后分身", "RG_T_CONFUSE": "混乱走位", "RG_T_ICERESOURCE": "冰冻转资源",
    "RG_T_SELFDESTRUCT": "低血自爆", "RG_T_MEMESPREAD": "模因传播", "RG_T_LASERCUT": "激光切割",
    "RG_T_GAMBLERISK": "赌徒翻倍", "RG_T_STEPMOVE": "攻后位移", "RG_T_DIGITRAIN": "数字雨",
    "RG_T_RHYTHM": "节拍增伤", "RG_T_PIXELCRUSH": "像素即死", "RG_T_HIJACK": "劫持武器",
    "RG_T_FUSEPLANT": "融合植物", "RG_T_BLINK": "随机传送", "RG_T_PETALBARRAGE": "花瓣弹幕",
    "RG_T_LIFESTEAL": "吸血", "RG_T_CURSE": "诅咒掉血", "RG_T_RECOMBINE": "重组再爆",
    "RG_T_INFECTTURN": "感染反击", "RG_T_DODGE": "概率闪避", "RG_T_EATSUN": "吸阳光",
    "RG_T_BOUNCE5": "弹射5次", "RG_T_MAZEWALL": "迷宫路障", "RG_T_WEATHER": "随机天气",
    "RG_T_DRAWCARD": "发射抽卡", "RG_T_ASSASSIN": "隐身必杀", "RG_T_LULLABY": "催眠",
    "RG_T_CHI": "内力护盾", "RG_T_PLAGUE": "中毒扩散", "RG_T_BLACKHOLE": "黑洞吞噬",
    "RG_T_FAKEHP": "假血欺骗", "RG_T_AURABUFF": "光环增伤", "RG_T_COPYEAT": "吞噬复制",
    "RG_T_ARMORBREAK": "破甲", "RG_T_MARK": "标记集火", "RG_T_CRIT": "暴击",
    "RG_T_KNOCKBACK": "击退", "RG_T_PULL": "拉拽", "RG_T_SUMMON": "召唤",
    "RG_T_ORBITAL": "轨道轰炸", "RG_T_EXECUTE": "处决", "RG_T_SHIELDALLY": "护盾光环",
    "RG_T_ENERGYGAIN": "阳光光环", "RG_T_SLOWFIELD": "减速场", "RG_T_OVERLOAD": "过载",
    "RG_T_CONVERT": "吸伤转能量", "RG_T_PURGE": "净化", "RG_T_LASTSTAND": "濒死狂暴",
    "RG_T_TIMEHEAL": "时序回溯",
}

TIER_DMG = [2, 4, 7, 11, 15, 20]
TIER_RATE = [1.0, 1.0, 1.15, 1.30, 1.45, 1.60]
TIER_RANGE = [1.0, 1.2, 1.5, 1.9, 2.4, 3.0]
TIER_COST = [30, 55, 85, 120, 155, 190]
TIER_IDX = {k: (i + 1) for i, k in enumerate(
    ["RGQ_COMMON", "RGQ_RARE", "RGQ_EPIC", "RGQ_LEGEND", "RGQ_MYTH", "RGQ_ULTRA"])}
TIER_IDX = {k: i for i, k in enumerate(
    ["RGQ_COMMON", "RGQ_RARE", "RGQ_EPIC", "RGQ_LEGEND", "RGQ_MYTH", "RGQ_ULTRA"])}


def parse_plants():
    i0 = SRC.index("static const RgPlantDef rgPlants[RG_PLANT_N] = {")
    i1 = SRC.index("\n};", i0)
    body = SRC[i0:i1]
    out = []
    # 尾部的 `\s*,\s*` 不能收紧成 `,`：首批条目的 cost 是脚本追加的，
    # 一度写成 `" , 30},`（引号后带空格），紧跟逗号的写法会整批漏匹配。
    for m in re.finditer(r"/\* (\d{2}) \*/ \{\s*L\"([^\"]+)\",\s*(RGQ_\w+),\s*(\w+),\s*([^,]+),\s*"
                         r"L\"([^\"]*)\"\s*,\s*(\d+)\s*\},", body, re.S):
        idx = int(m.group(1))
        traits = re.findall(r"RG_T_\w+|RG_ALL_TRAITS", m.group(5))
        out.append(dict(idx=idx, name=m.group(2), tier=m.group(3), base=m.group(4),
                        traits=traits, desc=m.group(6), cost=int(m.group(7))))
    return out


def logic_body():
    """去掉 #define 行与 rgPlants 数据表之后的源码 —— 剩下的才是"实现"。

    ⚠️ 第一版自检只在 `rgPlantTraitTick()` 里找 trait 名，结果把
    GROWEAT / LOAN / ROOFWALK / REWIND / DOUBLEBODY / STEPMOVE / HIJACK /
    FUSEPLANT / INFECTTURN 报成"没实现" —— 其实它们本来就**不该**在脉冲函数里：
    吞噬成长在啃食结算里、贷款在波次结算里、沿格移动在位移逻辑里、
    劫持在子弹处理里、融合在 plantIt 里、感染反击在僵尸死亡里。
    判据必须覆盖整个文件，否则每次都会刷一堆假警报。
    """
    i0 = SRC.index("static const RgPlantDef rgPlants")
    body = SRC[:i0] + SRC[SRC.index("\n};", i0) + 3:]
    return re.sub(r"^#define\s+RG_T_\w+.*$", "", body, flags=re.M)


def alias_map():
    """`#define RG_T_XXX_SAFE RG_T_XXX` 这批别名。

    为什么要解析它：数据表里写的是 `RG_T_BOUNCE5`，而实现里判断的是
    `RG_T_BOUNCE_SAFE`（同一个值的两个名字）。只查真名会把已经实现的
    机制误报成"没实现"，反过来只查别名又会漏掉真名，所以两边都要看。
    """
    am = {}
    for m in re.finditer(r"^#define\s+(RG_T_\w+)\s+(RG_T_\w+)\s*$", SRC, re.M):
        am.setdefault(m.group(2), []).append(m.group(1))
    return am


def main():
    plants = parse_plants()
    logic = logic_body()
    aliases = alias_map()

    print("# 肉鸽植物全表（%d 株）\n" % len(plants))
    print("> 由 `_dump_roster.py` 从 pvz.c 自动导出。"
          "伤害/攻速/范围三列是**按品级推导**的倍率，不是手填值。\n")
    print("| # | 名称 | 品级 | 阳光价 | 伤害× | 攻速× | 范围× | 机制 | 说明 |")
    print("|---|---|---|---|---|---|---|---|---|")
    missing = []
    bad_cost = []
    for p in plants:
        t = TIER_IDX[p["tier"]]
        if p["traits"] == ["RG_ALL_TRAITS"]:
            mech = "全部机制"
        else:
            mech = "、".join(TRAIT_CN.get(x, x) for x in p["traits"])
            for x in p["traits"]:
                if x not in TRAIT_CN:
                    continue
                # 自检：这个 trait 位（或其 _SAFE 别名）在源码里真的有实现引用吗
                used = (x in logic) or any(a in logic for a in aliases.get(x, []))
                if not used:
                    missing.append((p["idx"], x))
        if not (10 <= p["cost"] <= 200):
            bad_cost.append((p["idx"], p["cost"]))
        print("| %02d | %s | %s | %d | %d | %.2f | %.2f | %s | %s |"
              % (p["idx"], p["name"], TIER_CN[p["tier"]], p["cost"],
                 TIER_DMG[t], TIER_RATE[t], TIER_RANGE[t], mech, p["desc"]))

    print("\n## 自检\n")
    print("- 株数：%d（期望 100）%s" % (len(plants), "✔" if len(plants) == 100 else "✗"))
    print("- 价格区间 10~200：%s" % ("✔ 全部合规" if not bad_cost else "✗ " + str(bad_cost)))
    if not missing:
        print("- 机制位实现覆盖：✔ 每个 trait 位在源码里都能找到实现引用")
    else:
        old = [m for m in missing if m[0] < 50]
        new = [m for m in missing if m[0] >= 50]
        print("- 机制位实现覆盖：✗ 共 %d 处声明了但没有实现引用" % len(missing))
        if new:
            print("  - **第二批（本轮新增）**：%s" % new)
        if old:
            names = sorted(set(x[1] for x in old))
            print("  - 首批历史遗留（本轮未处理）：%s" % "、".join(
                TRAIT_CN.get(n, n) for n in names))
        print("  - 说明：数据表里挂了 trait 位、但 rgPlantTraitTick 及其它逻辑处"
              "没有任何 `& 该位` 的判断 —— 等于这几株植物只是数值怪，机制没生效。")
    print("\n### 品级分布\n")
    print("| 品级 | 株数 | 伤害倍率 | 攻速倍率 | 范围倍率 | 基准价 |")
    print("|---|---|---|---|---|---|")
    for k, cn in TIER_CN.items():
        n = len([p for p in plants if p["tier"] == k])
        t = TIER_IDX[k]
        print("| %s | %d | %d | %.2f | %.2f | %d |"
              % (cn, n, TIER_DMG[t], TIER_RATE[t], TIER_RANGE[t], TIER_COST[t]))


if __name__ == "__main__":
    main()
