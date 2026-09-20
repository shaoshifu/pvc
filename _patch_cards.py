# -*- coding: utf-8 -*-
"""把 art/_cards_new.py 里的 50 张新卡写进 pvz.c。

一次跑完，包含 8 件事：
  1. RgPlantDef 增加 aspect 字段（单卡专属强度系数，放最后 → 老条目不受影响）
  2. rgPlantAtkMul 乘上 aspect（0 视为基准 100，不是零倍）
  3. RG_PLANT_N 108 → 158
  4. rgPlants 追加 50 行（殿堂 108~127 / 始祖 128~157）
  5. RGQ_DRAFT_W 重算（株数从 108 涨到 158，不重算的话概率会整体跑偏）
  6. 卡片上的"能力 N 条"改成**真实位数**（原来取的是档位代表值，新卡会少报）
  7. 始祖伤害 ≥ 殿堂 5 倍 → 编译期断言
  8. 表头注释同步

用法： python _patch_cards.py
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "art"))
from _cards_new import all_cards, HALL_COST, PRIM_COST  # noqa: E402

P = os.path.join(HERE, "pvz.c")
s = io.open(P, encoding="utf-8").read()
step = [0]


def rep(old, new, what, cnt=1):
    global s
    n = s.count(old)
    assert n == cnt, "锚点不匹配(%d!=%d) %s: %r" % (n, cnt, what, old[:80])
    s = s.replace(old, new)
    step[0] += 1
    print("  [%2d] %s" % (step[0], what))


# ---------------------------------------------------------------- 1. aspect 字段
rep("""    unsigned short cost;
} RgPlantDef;""",
    """    unsigned short cost;
    /* 单卡专属强度系数（百分比，100 = 基准；**0 视为 100**）。
       为什么要有它：`tier` 只能整档一起调（改 RGQ_DMG 会影响该档全部卡），
       而"30 张始祖彼此也该有强弱梯度"这件事只能落在单卡上。
       ⚠️ 2 亿字节的坑要避开：这个字段放**最后**，于是上面 108 条老初始化式
          一个都不用改（C 会把遗漏成员置 0），而 0 被解释成"基准"。
       只作用于植物侧：rgPlantAtkMul 用它，rgZombieHpMul 不碰它。 */
    unsigned short aspect;
} RgPlantDef;""",
    "RgPlantDef 增加 aspect 字段")

# ---------------------------------------------------------------- 2. 攻击/血量乘 aspect
rep("""static float rgPlantAtkMul(const Plant *p)
{
    const RgPlantDef *d = rgOfPlant(p);
    return d ? rgDmgMul(d->tier) : 1.0f;
}""",
    """static float rgPlantAtkMul(const Plant *p)
{
    const RgPlantDef *d = rgOfPlant(p);
    float m;
    if (!d) return 1.0f;
    m = rgDmgMul(d->tier);
    /* aspect：单卡专属强度系数。**0 必须解释成"基准（×1.0）"而不是"零倍"** ——
       老条目没写这个字段，C 自动置 0，按零倍算的话全场植物攻击与血量会一起归零。
       （这是"新增字段必须有明确的缺省语义"的典型例子。） */
    if (d->aspect > 0) m *= (float)d->aspect / 100.0f;
    return m;
}""",
    "rgPlantAtkMul 乘 aspect（0 = 基准）")

# ---------------------------------------------------------------- 3. 档位倍率的整数镜像 + 编译期断言
rep("""static const float RGQ_RANGE[RGQ_COUNT]= { 1.0f, 1.20f, 1.50f, 1.90f, 2.40f, 3.00f, 3.00f, 8.00f };""",
    """static const float RGQ_RANGE[RGQ_COUNT]= { 1.0f, 1.20f, 1.50f, 1.90f, 2.40f, 3.00f, 3.00f, 8.00f };

/* ---- 伤害倍率的整数镜像 + 用户硬要求的编译期断言 ----
   用户明确要求：**始祖级伤害至少为殿堂级的 5 倍**。
   C 里 `static const float` 不是常量表达式，没法直接拿 RGQ_DMG 做数组断言，
   所以把这两个数各镜像一份整数下来，用 _Static_assert 钉死。
   ⚠️ 镜像一旦和 RGQ_DMG 不一致就失去意义 —— _test_card_tiers 会逐项核对
      （断言 + 运行时核对，两道一起才可靠）。 */
enum {
    RG_DMGX_HALL       = 100,
    RG_DMGX_PRIMORDIAL = 4000,
    RG_PRIM_MIN_RATIO  = 5      /* 始祖 ≥ 5 × 殿堂 */
};
_Static_assert(RG_DMGX_PRIMORDIAL >= RG_PRIM_MIN_RATIO * RG_DMGX_HALL,
               "始祖级伤害必须至少是殿堂级的 5 倍");
_Static_assert(RG_DMGX_PRIMORDIAL / RG_DMGX_HALL >= RG_PRIM_MIN_RATIO,
               "始祖级伤害必须至少是殿堂级的 5 倍（比值）");""",
    "档位倍率整数镜像 + 编译期断言")

# ---------------------------------------------------------------- 4. 权重重算
rep("""static const int   RGQ_DRAFT_W[RGQ_COUNT] = { 30, 114, 86, 70, 63, 131, 81, 27 };""",
    """/* ⚠️ 这个数组**只服务于植物三选一**（僵尸用 RGQ_W，两者已拆开）。
   2026-09-21 新增 20 张殿堂 + 30 张始祖后**重算**过：
   株数从（24/26/18/13/5/5/3）变成（24/26/18/13/5/25/33），
   权重不跟着改的话殿堂与始祖的占比会翻好几倍、稀有档被挤没。

   目标概率（每卡位）与实测：
     稀有 30.0% → 29.96%   史诗 26.0% → 26.03%   传说 16.0% → 15.99%
     神级 11.0% → 10.99%   传奇  7.0% →  7.02%   殿堂  8.0% →  8.01%
     始祖  2.0% →  2.01%
   总权重 = 24·409 + 26·328 + 18·291 + 13·277 + 5·460 + 25·105 + 33·20
          = 32768 = 2^15（整除 32768 → rndi 取模无偏；见 rgOpenDraft 的注释） */
static const int   RGQ_DRAFT_W[RGQ_COUNT] = { 409, 328, 291, 277, 460, 105, 20 };""",
    "RGQ_DRAFT_W 重算")

# ---------------------------------------------------------------- 5. 能力条数按真实位数显示
rep("""static int rgDraftEligible(int idx)
{""",
    """/* 这张卡实际开了几个能力位。
   卡片上原来打印的是 RGQ_TRAITN[tier]（档位代表值，殿堂=7）——
   那是"该档位典型条数"，而新增的殿堂卡都有 8 条、始祖 64 条，
   直接沿用会**少报**。改成对 traits 掩码做 popcount，卡片上写几就是几。 */
static int rgPlantTraitCount(int idx)
{
    unsigned long long t;
    int n = 0;
    if (idx < 0 || idx >= RG_PLANT_N) return 0;
    t = rgPlants[idx].traits;
    while (t) { n += (int)(t & 1ull); t >>= 1; }
    return n;
}

static int rgDraftEligible(int idx)
{""",
    "新增 rgPlantTraitCount()")

rep("""            wsprintfW(buf, L"杀伤 %d×　能力 %d 条",
                      (int)rgDmgMul(tier), RGQ_TRAITN[tier]);""",
    """            wsprintfW(buf, L"杀伤 %d×　能力 %d 条",
                      (int)rgDmgMul(tier), rgPlantTraitCount(ri));""",
    "卡片改印真实能力条数")

# ---------------------------------------------------------------- 6. 扩表
rows = []
for idx, c, is_prim in all_cards():
    if is_prim:
        traits = "RG_ALL_TRAITS"
        cost = PRIM_COST
        tail = ", %d" % cost
        if c.get("aspect"):
            tail += ", %d" % c["aspect"]
    else:
        traits = " | ".join("RG_T_" + t for t in c["traits"])
        tail = ", %d" % HALL_COST
    rows.append('/* %d */ { L"%s", %s, %s,\n            %s,\n            L"%s"%s },'
                % (idx, c["name"], "RGQ_PRIMORDIAL" if is_prim else "RGQ_HALL",
                   c["base"], traits, c["desc"], tail))

block = ("\n    /* ================= 第三批：20 张殿堂 + 30 张始祖（2026-09-21）=================\n"
         "       设计见 art/_cards_new.py（单一数据源）与 MECHANISMS.md 的卡牌体系章节。\n"
         "       殿堂 = 流派核心，每张 8 条能力位构成一套构筑骨架；\n"
         "       始祖 = 流派终局，RG_ALL_TRAITS（64 位全开）+ 各自专属 aspect（100~150）。\n"
         "       ⚠️ 追加在**表尾**而不是插在中间：贴图按 rgplant_NN 下标命名，\n"
         "          插中间会让后面所有索引前移、全员贴错图。 */\n"
         + "\n".join(rows) + "\n")

anchor = "/* 107 */ { L\"混沌齐射\", RGQ_PRIMORDIAL, PT_PEASHOOTER, RG_ALL_TRAITS,\n            L\"4000 倍伤害 × 3 倍攻速 × 8 倍范围。向所有方向倾泻创世种子，命中即湮灭整行\",\n            10000 },"
rep(anchor, anchor + "\n" + block.rstrip("\n"), "追加 50 行卡牌")

rep("#define RG_PLANT_N 108", "#define RG_PLANT_N 158", "RG_PLANT_N 108 → 158")

rep("""/* ---- 肉鸽植物：108 株（00~49 首发，50~99 第二批创意扩展，
      100~104 殿堂五尊，105~107 始祖三尊） ----""",
    """/* ---- 肉鸽植物：158 株 ----
      00~49   首批
      50~99   第二批创意扩展
      100~104 殿堂五尊（首批）
      105~107 始祖三尊（首批）
      108~127 殿堂二十尊（第三批，2026-09-21）
      128~157 始祖三十尊（第三批，2026-09-21） ----""",
    "表头注释同步")

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("\n  全部 %d 处改动完成；新增 %d 行卡牌" % (step[0], len(rows)))
