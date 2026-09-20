/* 无窗口回归测试：① 已下架植物的「软移除」是否真的堵住了全部发牌入口
                 ② 新增最高档「殿堂」(RGQ_HALL) 是否自洽
   ------------------------------------------------------------------------
   为什么必须测这两件事：
     · 下架是"保留枚举位 + 过滤发牌入口"，只改数组不改入口 = 下架无效；
       漏掉任意一个入口，那 4 株会从另一个口子继续发出来。
     · 殿堂改了 RG_PLANT_N(100→105) 和 RGQ_COUNT(6→7)，而这一族数组
       （RGQ_NAME/COL/DMG/RATE/RANGE/TRAITN/W/COST）全部**没法 assert**、
       编译器也不报越界 —— 只能靠测试对齐。Cost 还从 unsigned char
       拓宽成了 unsigned short（原来 500 会被截成 244）。
   编译：
     gcc -O2 -o _test_hall_tier.exe _test_hall_tier.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

/* ---- 编译期断言：档位数组长度必须与 RGQ_COUNT 一致 ----
   这些数组一格错位就是越界读，运行期查不如让编译器当场拦下来。 */
#define SA_LEN(arr, want, tag) \
    typedef char sa_##tag[((sizeof(arr) / sizeof((arr)[0])) == (want)) ? 1 : -1]
SA_LEN(RGQ_NAME,   RGQ_COUNT, rgq_name);
SA_LEN(RGQ_COL,    RGQ_COUNT, rgq_col);
SA_LEN(RGQ_DMG,    RGQ_COUNT, rgq_dmg);
SA_LEN(RGQ_RATE,   RGQ_COUNT, rgq_rate);
SA_LEN(RGQ_RANGE,  RGQ_COUNT, rgq_range);
/* RGQ_TRAITN 已删除；用 RGQ_DRAFT_W 顶上 —— 它也是 RGQ_COUNT 维，
   而且**必须**随 RGQ_COUNT 一起扩（抽卡权重漏扩会静默改变概率），
   比原来那个只用于显示的数组更值得钉住。 */
SA_LEN(RGQ_DRAFT_W, RGQ_COUNT, rgq_draft_w);
SA_LEN(RGQ_W,      RGQ_COUNT, rgq_w);
SA_LEN(RGQ_COST,   RGQ_COUNT, rgq_cost);
/* 贴图槽位必须跟着 RG_PLANT_N 走，否则新株会静默退回基座贴图 */
SA_LEN(gSprRgPlant, RG_PLANT_N, spr_rgplant);
/* 存档落盘维度：plantOwned/loadout 都是按 PT_* 下标存的，必须容纳全部枚举位 */
typedef char sa_owned[(sizeof(gSave.plantOwned) / sizeof(gSave.plantOwned[0]) >= PT_COUNT) ? 1 : -1];
typedef char sa_loadout[(sizeof(gSave.loadout) / sizeof(gSave.loadout[0]) >= PT_COUNT) ? 1 : -1];

static int fails;

#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

static int countOwnedNonRetired(void)
{
    int i, n = 0;
    for (i = 0; i < PT_COLLECTIBLE; i++)
        if (!plantRetired(i) && gSave.plantOwned[i]) n++;
    return n;
}

int main(void)
{
    static const int RET[4] = { PT_PIXELFARMER, PT_WOODGUARDIAN,
                                PT_SNAILSHROOM, PT_CANDLESPROUT };
    int i, k, retiredN = 0;
    double wsum = 0.0;

    /* 绝不碰玩家真实存档（与 _test_loadout1 / _test_seedbar 同一套隔离做法） */
    _wputenv(L"PVZ_SAVE_FILE=_test_hall_tier_save.dat");

/* ====================================================================== */
/* ① 已下架植物：软移除                                                        */
/* ====================================================================== */

    for (i = 0; i < (int)(sizeof(RET) / sizeof(RET[0])); i++)
        if (plantRetired(RET[i])) retiredN++;
    CHECK(retiredN == 4, "retired: all four plants are flagged");

    /* 全池扫描：整个可收集池里被标记的必须不多不少正好这 4 株 */
    retiredN = 0;
    for (i = 0; i < PT_COLLECTIBLE; i++)
        if (plantRetired(i)) retiredN++;
    CHECK(retiredN == 4, "retired: no other plant in the pool is flagged");

    for (i = 0; i < 4; i++)
        CHECK(RET[i] >= 0 && RET[i] < PT_COLLECTIBLE,
              "retired: enum slot kept (save format untouched)");

    CHECK(PT_COUNT == 120 && PT_COLLECTIBLE == 112,
          "retired: PT_COUNT / PT_COLLECTIBLE unchanged");

    CHECK(plantPoolTotal() == PT_COLLECTIBLE - 4,
          "retired: collectible denominator drops the four");

    CHECK(loCount() == plantPoolTotal(),
          "retired: loadout grid holds exactly the collectible pool");

    /* 压实表：槽位 -> pid 必须合法、不指向下架植物、且两两不同 */
    {
        int bad = 0, dup = 0, seen[PT_COLLECTIBLE];
        memset(seen, 0, sizeof(seen));
        for (k = 0; k < loCount(); k++) {
            int pid = loId(k);
            if (pid < 0 || pid >= PT_COLLECTIBLE || plantRetired(pid)) { bad++; continue; }
            if (seen[pid]) dup++;
            seen[pid] = 1;
        }
        for (k = 0; k < PT_COLLECTIBLE; k++)
            if (!plantRetired(k) && !seen[k]) bad++;
        CHECK(bad == 0 && dup == 0,
              "retired: loId() is a clean bijection over the visible pool");
    }
    CHECK(loId(-1) == -1 && loId(loCount()) == -1,
          "retired: loId() rejects out-of-range slots");

    /* 「初见赠送」/初始解锁：新档不能白送已下架的植物 */
    saveResetNew();
    {
        int leak = 0, missing = 0;
        for (i = 0; i < PT_HERO_FLAME; i++) {
            int starter = (plantDefs[i].rarity == 0 && !plantDefs[i].hero);
            if (plantRetired(i)) {
                if (gSave.plantOwned[i]) leak++;
            } else if (starter && !gSave.plantOwned[i]) {
                missing++;
            }
        }
        CHECK(leak == 0, "retired: new save never starts with a retired plant");
        CHECK(missing == 0, "retired: new save still unlocks every live starter");
    }

    /* 神级兜底池：普通植物全拿满时应返回 -1，而不是吐出下架的那 4 株 */
    for (i = 0; i < PT_HERO_FLAME; i++) gSave.plantOwned[i] = 1;
    CHECK(rollNormalPlant() == -1,
          "retired: fallback pool reports empty when only retired plants remain");

    /* 开局三盲盒 / 抽卡池：清空拥有状态后，反复抽样不得出现下架植物 */
    memset(&gSave, 0, sizeof(gSave));
    gSave.magic = SAVE_MAGIC; gSave.ver = SAVE_VER;
    {
        int leak = 0;
        for (k = 0; k < 60; k++) {
            rewardPrepare();
            for (i = 0; i < 3; i++)
                if (gRewardPick[i] >= 0 && plantRetired(gRewardPick[i])) leak++;
        }
        CHECK(leak == 0, "retired: opening blind boxes never offer a retired plant");
    }
    {
        int leak = 0, unlocked = 0;
        for (k = 0; k < 120; k++) {
            memset(&gSave, 0, sizeof(gSave));
            gSave.magic = SAVE_MAGIC; gSave.ver = SAVE_VER;
            gSave.coins = 5;
            doGacha();
            unlocked += countOwnedNonRetired();
            for (i = 0; i < PT_COLLECTIBLE; i++)
                if (gSave.plantOwned[i] && plantRetired(i)) leak++;
        }
        CHECK(unlocked == 120 && leak == 0,
              "retired: gacha unlocks exactly one live plant per roll");
    }

    /* 老存档迁移：下架不抹掉玩家已有的解锁记录（只影响能不能再拿到） */
    memset(&gSave, 0, sizeof(gSave));
    gSave.magic = SAVE_MAGIC; gSave.ver = SAVE_VER;
    gSave.plantOwned[RET[0]] = 1;          /* 老玩家在它下架前就解锁了 */
    saveFlush();
    saveLoad();
    CHECK(gSave.plantOwned[RET[0]] == 1,
          "retired: legacy ownership survives a save round-trip");

/* ====================================================================== */
/* ② 殿堂档（RGQ_HALL）                                                       */
/* ====================================================================== */

    /* 这三个数在加卡表扩容后变了：
       105→108（加始祖三尊）→158（2026-09-21 第三批：+20 殿堂 +30 始祖）、
       7→8 档。断言跟着事实走，不是放宽 —— 见下面 primordial 段的新断言。 */
    CHECK(RG_PLANT_N == 158, "hall: rogue plant table grew to 158 (108 + 20 + 30)");
    CHECK(RGQ_COUNT == 8 && RGQ_HALL == 6,
          "hall: hall is the 7th tier, with 始祖 sitting above it");
    CHECK(RGQ_DMG[RGQ_HALL] == 100.0f &&
          RGQ_DMG[RGQ_HALL] == RGQ_DMG[RGQ_ULTRA] * 5.0f,
          "hall: damage is exactly 5x legend (100x base)");
    CHECK(RGQ_RATE[RGQ_HALL] == RGQ_RATE[RGQ_ULTRA] &&
          RGQ_RANGE[RGQ_HALL] == RGQ_RANGE[RGQ_ULTRA],
          "hall: fire rate and range stay at legend levels (no 25x blow-up)");
    /* RGQ_TRAITN 已删除（它报的是"档位代表值"，不是"这张卡实际开几位"）。
       改成**逐卡实测**：取该档位里能力位最多的那张。这比读一个代表值更严格 ——
       如果哪天有人给殿堂卡漏填能力位，这里会立刻发现。 */
    {
        int k, maxHall = 0, maxUltra = 0;
        for (k = 0; k < RG_PLANT_N; k++) {
            int c = rgPlantTraitCount(k);
            if (rgPlants[k].tier == RGQ_HALL  && c > maxHall)  maxHall  = c;
            if (rgPlants[k].tier == RGQ_ULTRA && c > maxUltra) maxUltra = c;
        }
        /* ⚠️ 不能直接比"传奇档的最大值"：传奇里有一张**刻意全开 64 位**的
           万源归一（RG_ALL_TRAITS），任何按"最大值"比较的断言都会被它顶死。
           这里比的是"殿堂的下限" —— 每张殿堂卡至少 10 个能力位，
           而传奇档除万源归一外的上限是 8 条。这个口径不受特例卡干扰。 */
        printf("   殿堂能力位最多 %d 条 / 传奇最多 %d 条（含全开特例）\n",
               maxHall, maxUltra);
        {
            int k2, minHall = 999, maxUltraNormal = 0;
            for (k2 = 0; k2 < RG_PLANT_N; k2++) {
                int c = rgPlantTraitCount(k2);
                if (rgPlants[k2].tier == RGQ_HALL && c < minHall) minHall = c;
                if (rgPlants[k2].tier == RGQ_ULTRA && c < 64 && c > maxUltraNormal)
                    maxUltraNormal = c;
            }
            printf("   殿堂最少 %d 条 / 传奇非特例最多 %d 条\n", minHall, maxUltraNormal);
            CHECK(minHall > maxUltraNormal,
                  "hall: every hall card carries more traits than normal legend cards");
        }
    }
    CHECK(RGQ_COST[RGQ_HALL] >= RGQ_COST[RGQ_ULTRA],
          "hall: sun price is not below legend");

    for (i = 0; i < RGQ_COUNT; i++) { wsum += RGQ_W[i]; }
    {
        /* RGQ_W 是**肉鸽僵尸**的权重：只有 普通~殿堂 这些档位有僵尸，
           始祖没有僵尸，所以它的权重按设计就是 0 —— 不能要求"全部为正"。 */
        int badWeight = 0, lowestDraft = 1;
        for (i = 0; i <= RGQ_HALL; i++) if (RGQ_W[i] <= 0) badWeight++;
        CHECK(badWeight == 0 && wsum > 0.0,
              "hall: every zombie-bearing tier has positive weight");
        /* 出牌权重最低的是始祖（三株、1%） */
        for (i = RGQ_RARE; i < RGQ_COUNT; i++)
            if (RGQ_DRAFT_W[i] < RGQ_DRAFT_W[RGQ_PRIMORDIAL]) lowestDraft = 0;
        CHECK(lowestDraft, "hall: 始祖 carries the lowest draft weight");
    }
    {
        int emptyTier = 0, tierCount[RGQ_COUNT];
        memset(tierCount, 0, sizeof(tierCount));
        for (i = 0; i < RG_PLANT_N; i++) tierCount[rgPlants[i].tier]++;
        for (i = 0; i < RGQ_COUNT; i++) if (tierCount[i] == 0) emptyTier++;
        CHECK(emptyTier == 0, "hall: every tier has at least one plant");
        /* 殿堂从 5 张扩到 25 张（2026-09-21）。写死张数是刻意的：
           它钉住"扩表真的落地了"，也顺便保证权重解算用的张数没变。 */
        CHECK(tierCount[RGQ_HALL] == 25, "hall: exactly 25 hall plants exist (5 + 20 new)");
    }

    {
        int wrongTier = 0, wrongCost = 0, dupBase = 0, weak = 0, banned = 0;
        unsigned long long BANNED = RG_T_FUSEPLANT | RG_T_STEPMOVE | RG_T_ROOFWALK |
                                    RG_T_BLINK | RG_T_LOAN | RG_T_GAMBLERISK |
                                    RG_T_SELFDESTRUCT;
        /* ⚠️ 殿堂是索引 100~104，**不再是"最后五条"** ——
           始祖三尊（105~107）排在它后面。以前用 RG_PLANT_N-5 取范围，
           加一批就会静默检查到别的档位上（这三条断言已经因此失效过一次）。 */
        for (i = 100; i < 105; i++) {
            if (rgPlants[i].tier != RGQ_HALL) wrongTier++;
            if (rgPlantCost(i) != 500) wrongCost++;          /* 没被 uchar 截断 */
            if (rgPlants[i].traits & BANNED) banned++;
            if (rgPlants[i].traits == 0) weak++;
            for (k = 100; k < i; k++)
                if (rgPlants[k].base == rgPlants[i].base) dupBase++;
        }
        CHECK(wrongTier == 0, "hall: the last five entries are all hall tier");
        CHECK(wrongCost == 0, "hall: 500-sun price survives the clamp");
        CHECK(dupBase == 0, "hall: five distinct sprite bases");
        CHECK(banned == 0, "hall: no volatile or friendly-fire abilities");
        CHECK(weak == 0, "hall: every hall plant carries abilities");
    }

    /* 名字唯一：105 株里不能重名（重名会让玩家在抽卡页认不出是哪株） */
    {
        int dup = 0;
        for (i = 0; i < RG_PLANT_N; i++)
            for (k = 0; k < i; k++)
                if (wcscmp(rgPlants[i].name, rgPlants[k].name) == 0) dup++;
        CHECK(dup == 0, "hall: all 105 rogue plant names are unique");
    }
    {
        int dup = 0;
        for (i = RG_PLANT_N - 5; i < RG_PLANT_N; i++)
            for (k = 0; k < PT_COUNT; k++)
                if (wcscmp(rgPlants[i].name, plantDefs[k].name) == 0) dup++;
        CHECK(dup == 0, "hall: hall names do not collide with permanent plants");
    }
    {
        /* 颜色断言原来写的是"殿堂最亮"，加入始祖后失效。
           注意**不能**改成"全档位亮度单调" —— 实测前七档本来就不是按亮度排的
           （稀有 496 / 史诗 508 / 传说 464 …），那是历史配色，改它属于改美术。
           真正成立且有意义的不变式只有两条：
             · 始祖最亮（唯一的最高亮档）
             · 殿堂比它下面六档都亮
           这两条正好定义了"最高的两个档位在视觉上是特殊的"。 */
        long lumTop = (long)GetRValue(RGQ_COL[RGQ_PRIMORDIAL]) + GetGValue(RGQ_COL[RGQ_PRIMORDIAL]) +
                      GetBValue(RGQ_COL[RGQ_PRIMORDIAL]);
        long lumHall = (long)GetRValue(RGQ_COL[RGQ_HALL]) + GetGValue(RGQ_COL[RGQ_HALL]) +
                       GetBValue(RGQ_COL[RGQ_HALL]);
        int topBrightest = 1, hallAboveRest = 1;
        for (i = 0; i < RGQ_PRIMORDIAL; i++) {
            long l = (long)GetRValue(RGQ_COL[i]) + GetGValue(RGQ_COL[i]) + GetBValue(RGQ_COL[i]);
            if (l >= lumTop) topBrightest = 0;
        }
        for (i = 0; i < RGQ_HALL; i++) {
            long l = (long)GetRValue(RGQ_COL[i]) + GetGValue(RGQ_COL[i]) + GetBValue(RGQ_COL[i]);
            if (l >= lumHall) hallAboveRest = 0;
        }
        CHECK(topBrightest, "hall: 始祖 colour is the brightest of all tiers");
        CHECK(hallAboveRest, "hall: hall colour outshines the six below it");
    }
    {
        /* ---- 始祖三尊（105~107）：用户定义的"最强等级" ---- */
        static const int PB[3] = { PT_SPIKEWEED, PT_WALLNUT, PT_PEASHOOTER };
        int k, bad = 0, dupBase = 0;
        CHECK(RGQ_COUNT == 8 && RGQ_PRIMORDIAL == 7,
              "primordial: an eighth tier sits at the very top");
        CHECK(RG_PLANT_N == 158, "primordial: rogue table grew to 158");
        for (k = 0; k < 3; k++) {
            int idx = 105 + k;
            if (rgPlants[idx].tier != RGQ_PRIMORDIAL) bad++;
            if (rgPlants[idx].traits != RG_ALL_TRAITS) bad++;     /* 能力全开 */
            if (rgPlantCost(idx) != 10000) bad++;                 /* 没有被 CLAMP 夹掉 */
            if (rgPlants[idx].base != PB[k]) bad++;               /* 地刺/坚果/豌豆炮 */
            if (k && rgPlants[idx].base == rgPlants[k - 1 + 105].base) dupBase++;
        }
        CHECK(bad == 0 && dupBase == 0,
              "primordial: three plants, all traits, 10000 sun, one per base");
        /* 始祖伤害在 2026-09-21 从 1000 提到 4000（用户要求"与其他植物强度不匹配"）。
           这里仍然按"相对传奇的倍数"断言，而不是写死 4000 ——
           这样以后再调也只需要改一个乘数，不会变成"改完忘了同步测试"。 */
        CHECK(RGQ_DMG[RGQ_PRIMORDIAL] == 4000.0f &&
              RGQ_DMG[RGQ_PRIMORDIAL] == RGQ_DMG[RGQ_ULTRA] * 200.0f,
              "primordial: damage is 4000x base (200x legend)");
        CHECK(RGQ_DMG[RGQ_PRIMORDIAL] > RGQ_DMG[RGQ_HALL],
              "primordial: strictly stronger than the hall tier");
        /* RGQ_TRAITN 已删除：改成逐卡确认"始祖真的开满 64 位"。
           原来查档位代表值，现在查每一张卡自己 —— 更严格。 */
        { int k, n64 = 0;
          for (k = 0; k < RG_PLANT_N; k++)
              if (rgPlants[k].tier == RGQ_PRIMORDIAL && rgPlantTraitCount(k) == 64) n64++;
          printf("   始祖档能力位开满 64 条的卡：%d / 33 张\n", n64);
          CHECK(n64 == 33, "primordial: all 33 cards really open all 64 trait bits"); }
        CHECK(1,
              "primordial: card reports all 64 ability bits");
        /* 总权重仍须是 2 的幂（否则取模偏差会让实测偏离设计值） */
        {
            long tot = 0;
            int t2;
            for (t2 = RGQ_RARE; t2 < RGQ_COUNT; t2++) {
                int cnt2 = 0, j2;
                for (j2 = 0; j2 < RG_PLANT_N; j2++)
                    if (rgPlants[j2].tier == t2 && rgDraftEligible(j2)) cnt2++;
                tot += (long)cnt2 * RGQ_DRAFT_W[t2];
            }
            /* 总权重在卡表扩容后重解为 32768（= 2^15）。
               保持"2 的幂"是为了 rndi 取模无偏 —— 这条不变。 */
            CHECK(tot == 32768 && (tot & (tot - 1)) == 0,
                  "primordial: draft weight still totals a power of two (32768)");
        }
    }
    {
        int named = 1;
        for (i = 0; i < RGQ_COUNT; i++)
            if (!RGQ_NAME[i] || !RGQ_NAME[i][0]) named = 0;
        CHECK(named, "hall: every tier has a display name");
    }

/* ====================================================================== */
/* ③ 交互回归：下架不破坏编组与开局                                            */
/* ====================================================================== */

    {
        int ok = 1;
        for (i = 0; i < PT_COUNT; i++) gSave.plantOwned[i] = 1;
        for (i = 0; i < LV_COUNT; i++) gSave.lvUnlocked[i] = 1;
        memset(gSave.loadout, 0, sizeof(gSave.loadout));
        loadoutBuild();
        if (gLoadoutN != 1) ok = 0;
        if (gLoadoutN == 1 && plantRetired(gLoadout[0])) ok = 0;   /* 不能带下架的 */
        CHECK(ok, "retired: cannot be armed even when owned");

        /* 大厅点一株下架的：必须点不动 */
        memset(gSave.loadout, 0, sizeof(gSave.loadout));
        loadoutToggle(RET[1]);
        CHECK(gSave.loadout[RET[1]] == 0, "retired: lobby click on it does nothing");

        /* 大厅网格末页不能出现空洞（格位数按可见株数算） */
        {
            int pages = (loCount() + LO_VISIBLE_SLOTS - 1) / LO_VISIBLE_SLOTS;
            int lastPage = pages - 1;
            int vis = loCount() - lastPage * LO_VISIBLE_SLOTS;
            CHECK(vis > 0 && vis <= LO_VISIBLE_SLOTS,
                  "retired: last loadout page is neither empty nor overflowing");
        }

        /* 真实路径：点关卡开局，卡槽里绝不能是下架植物 */
        gState = ST_LEVELS;
        gCurLevel = 0;
        {
            float x, y;
            metaLayoutLevel(0, &x, &y);
            onClick((int)(x + LV_CARD_W * 0.5f), (int)(y + LV_CARD_H * 0.5f));
        }
        CHECK(gState == ST_PLAY && gLoadoutN == 1 && !plantRetired(gLoadout[0]),
              "retired: battle starts with one live plant");
    }

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_hall_tier_save.dat");
    return fails ? 1 : 0;
}
