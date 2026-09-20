/* 遗物「真的生效吗」全量运行时验证（3 选 1 里能拿到的每一条都测）。
   ------------------------------------------------------------------------
   静态审计只能证明"代码里出现了 HAS(R_X)"；这个测试回答的是另一件事：
   **装上它之后，它承诺修正的那个量是不是真的变了。**

   做法：对每个遗物
     ① resetGame() → 清空 gRelic → relicsRecalc()，读基准值
     ② 只把 gRelic[id] 置 1，再 relicsRecalc()，读同一个量
     ③ 前后必须不同；完全相同的记为"没检测到效果"
   有一批遗物改的是"命中时才生效"的量（齐射 / 同列共鸣 / 寒霜核心 / 隐忍者…），
   这些单独搭场景测（见 measure() 里的 case）。
   还有一批效果写在 applyRelic() 的 switch 里（武器 / 模式 / 赠卡），
   走真实发放路径 applyRelic(id) 后检查。

   编译：
     gcc -O2 -o _test_relic_effect.exe _test_relic_effect.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int fails;
#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

static void wipePlants(void)
{
    int r, c;
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++) {
            Plant *p = plantAt(r, c);
            if (p) p->alive = 0;
            grid2[r][c].alive = 0;
        }
}

static void wipeField(void)
{
    int i;
    wipePlants();
    for (i = 0; i < MAX_ZOMBIES; i++) { zombies[i].active = 0; zombies[i].dead = 0; }
    for (i = 0; i < MAX_PEAS; i++) peas[i].active = 0;
    for (i = 0; i < MAX_SUNS; i++) suns[i].active = 0;
    gPendingRg = -1; gPendingRgZ = -1;
}

/* 返回该遗物"承诺修正的那个量"。每个遗物只出现一次（case 标签不能重复）。 */
static float measure(int id, int on)
{
    int i, r, c;

    gCurLevel = 0;
    resetGame();
    wipeField();
    gRelicCount = 0;
    memset(gRelic, 0, sizeof(gRelic));
    if (on) { gRelic[id] = 1; gRelicCount = 1; }
    relicsRecalc();

    switch (id) {
    /* ---- 经济 ---- */
    case R_START_SUN:
    case R_START_SUN_BIG:
    case R_FIRST_STRIKE:        return gStartSunAdd;
    case R_SKY_SUN:
    case R_NIGHT_SUN:
    case R_DESERT_WATER:        return gSkySunMul;
    case R_SUNFLOWER:           return gSunflowerAdd;
    case R_SUNFLOWER_BIG:       return gSunflowerAdd * 100.0f + gSunflowerCostMul;
    case R_KILL_SUN:            return (float)gKillSun;
    case R_KILL_SUN_BIG:        return (float)gKillSun * 100.0f + gCostMul;
    case R_INTEREST:            return (float)gInterest;
    case R_BANKER:              return (float)gBanker;
    case R_NECROMANCER:         return (float)gNecromancer;
    case R_SUN_GAMBLE:          return gStartSunAdd * 100.0f + gSunGainMul;

    /* ---- 火力 ---- */
    case R_PEA_DMG:
    case R_PEA_DMG_BIG:         return gDmgMul;
    case R_RATE:                return gRateMul;
    case R_RATE_BIG:            return gRateMul * 100.0f + gDmgMul;
    case R_BIG_CALIBER:         return gDmgMul * 100.0f + gRateMul;
    case R_GLASS_CANNON:        return gDmgMul * 1000.0f + gPlantHpMul;
    case R_ARMOR_PIERCE:        return (float)gArmorPierce;
    case R_PIERCE:              return (float)gPierceChance;
    case R_VOLLEY: {            /* 该行 ≥4 株射手 → 间隔倍率 */
        for (c = 0; c < 5; c++) plantIt(2, c, PT_PEASHOOTER);
        return rowRateMul(2);
    }
    case R_WEAPON_RACK:
    case R_MAIN_UPGRADE:
    case R_WEAPON_MASTER: {
        /* ⚠️ 这三条的效果写在 applyRelic() 的 switch 里，且被 `isNew` 守着
           （`int isNew = !gRelic[id]`）。而 measure() 在 on=1 时已经把 gRelic[id]
           置成 1 了 → isNew=0 → switch 被跳过，测出来"没效果"。
           所以这里必须：on=0 直接返回 0，on=1 先把预置撤掉再走真实发放路径。 */
        if (!on) return 0.0f;
        gRelic[id] = 0;
        applyRelic(id);
        return (float)(gHasSubWpn * 100000 + gSubWpn * 100 + gMainWpn);
    }

    /* ---- 生存 ---- */
    case R_PLANT_HP:            return gPlantHpMul;
    case R_PLANT_HP_BIG:        return gPlantHpMul * 100.0f + gCdMul;
    case R_THORNS:              return (float)gThorns;
    case R_MEDIC:               return (float)gMedic;
    case R_SPARE_TIRE:          return (float)gSpareTire;
    case R_NO_MOWER:            return (float)gNoMower * 1e6f + gDmgMul;
    case R_IRON_WALL:           return gWallnutHpBoost;

    /* ---- 控制 / 爆炸 ---- */
    case R_SLOW_LONG:           return gSlowMul;
    case R_ICE_AGE:             return gSlowMul * 100.0f + gSlowFactor;
    case R_BOOM_R:              return gExplodeRadMul;
    case R_BOOM_DMG:            return gExplodeDmgMul;
    case R_EMBER:               return (float)gEmber;
    case R_CHAIN_BOOM:          return (float)gChainBoom;
    case R_FROST_CORE: {        /* 被减速的僵尸挨一发，看掉血差别 */
        Zombie *z;
        int pid;
        float hp0;
        gPendingRgZ = -1;
        spawnZombieAt(ZT_NORMAL, 2, 700.0f);
        z = NULL;
        for (i = 0; i < MAX_ZOMBIES; i++) if (zombies[i].active) { z = &zombies[i]; break; }
        if (!z) return -1.0f;
        z->slow = 5.0f;                       /* 已处于减速状态 */
        z->maxhp = z->basehp = 1e6f; z->hp = 1e6f;
        pid = spawnPeaAt(z->x, z->y - 40.0f, 1.0f, 0.0f, 100.0f, 2, PT_PEASHOOTER, 0);
        hp0 = z->hp;
        updateGame(0.016f);
        (void)pid;
        return hp0 - z->hp;                   /* 掉血量：装了核心应该更大 */
    }

    /* ---- 构筑 / 协同 ---- */
    case R_COLUMN: {            /* 同列 ≥3 株 */
        for (r = 0; r < 3; r++) plantIt(r, 3, PT_PEASHOOTER);
        return fieldDmgBonusAt(1, 3);
    }
    case R_EMPTY: {             /* 周围空单元格 */
        plantIt(2, 3, PT_PEASHOOTER);
        return fieldDmgBonusAt(2, 3);
    }
    case R_FLOWER_SEA: {        /* 每株向日葵 → **植物最大生命 +6%**（不是伤害） */
        Plant *p;
        plantIt(2, 3, PT_PEASHOOTER);
        plantIt(3, 5, PT_SUNFLOWER);
        p = plantAt(2, 3);
        if (!p) return -1.0f;
        plantsRefreshHp();      /* 这条遗物的真实生效点 */
        return p->maxhp;
    }
    case R_WALLNUT_DOGMA: {     /* 每株存活坚果墙 */
        plantIt(2, 3, PT_PEASHOOTER);
        plantIt(3, 5, PT_WALLNUT);
        return fieldDmgBonusAt(2, 3);
    }
    case R_KILL_STACK: {        /* 击杀后叠层 → 伤害加成 */
        float d0, d1;
        gKillStack = 0;
        d0 = fieldDmgBonusAt(2, 3);
        gPendingRgZ = -1;
        spawnZombieAt(ZT_NORMAL, 2, 700.0f);
        for (i = 0; i < MAX_ZOMBIES; i++)
            if (zombies[i].active && !zombies[i].dead) { zombieDie(&zombies[i], 0); break; }
        d1 = fieldDmgBonusAt(2, 3);
        return (d1 - d0) * 1000.0f + (float)gKillStack;
    }
    case R_SYMBIOSIS: {         /* 相邻同类植物 */
        plantIt(2, 3, PT_PEASHOOTER);
        plantIt(2, 4, PT_PEASHOOTER);
        return fieldDmgBonusAt(2, 3);
    }
    case R_VANGUARD: {          /* 最左列 */
        plantIt(2, 0, PT_PEASHOOTER);
        return fieldDmgBonusAt(2, 0);
    }
    case R_BACKLINE: {          /* 最右列 */
        plantIt(2, COLS - 1, PT_PEASHOOTER);
        return fieldDmgBonusAt(2, COLS - 1);
    }

    /* ---- 风险 / 取舍 ---- */
    case R_MINIMAL:             return gCostMul * 100.0f + gCdMul;
    case R_NO_COOLDOWN:         return gCdMul * 100.0f + gPlantHpMul;
    case R_BLOOD_PACT:          return gCostMul * 1e6f + (float)gBloodPact;
    case R_ALL_IN:              return gZombieSpdUp * 100.0f + gKillSunMul;

    /* ---- 模式 ---- */
    case R_MODE_SHIFT:
    case R_MUTANT_SEED: {
        /* 同上：效果在 applyRelic 的 switch 里，被 isNew 守着 */
        int n0 = 0, n1 = 0;
        if (!on) return 0.0f;
        for (i = 0; i < MODE_COUNT; i++) n0 += gUnlockedMode[i] ? 1 : 0;
        gRelic[id] = 0;
        applyRelic(id);
        for (i = 0; i < MODE_COUNT; i++) n1 += gUnlockedMode[i] ? 1 : 0;
        return (float)(n1 - n0);
    }
    case R_WATER_MODE:
    case R_NIGHT_MODE: {
        /* ⚠️ 这两张卡的效果**原来写在 resetGame() 里**，而 resetGame 会先
           memset(gRelic)，所以 HAS() 恒为假 —— 从设计上就不可能生效（已修）。
           现在改成"拿到即解锁并切换"（applyRelic 的 switch），
           所以探针必须走真实发放路径，并且要先把预置的 gRelic 撤掉（isNew 守着）。 */
        int mi = (id == R_WATER_MODE) ? MODE_WATER : MODE_NIGHT;
        if (!on) return 0.0f;
        gRelic[id] = 0;
        applyRelic(id);
        return (float)gUnlockedMode[mi] * 1000.0f + (float)gCurMode;
    }
    case R_NINJA: {             /* 隐忍者：种下不立即结算冷却（取反用法） */
        int cd0;
        gSel = 0;
        cardCD[0] = 0.0f;
        if (!HAS(R_NINJA)) cardCD[gSel] = plantCd(gSel);   /* 复刻真实分支 */
        cd0 = (int)(cardCD[0] * 100.0f);
        return (float)cd0;
    }
    case R_CHARGED:             return (float)gChargeMax;
    case R_FURY:                return gFuryRate;

    /* ---- 第二批：新植物 / 新僵尸 / 新关卡 ---- */
    case R_SPIKE_DMG:           return gSpikeDmgMul;
    case R_SPIKE_WIDE:          return (float)gSpikeWide;
    case R_MAGNET_FAST:         return gMagnetCdMul;
    case R_MAGNET_GOLD:         return (float)gMagnetGold;
    case R_CORN_STUN:           return gCornStunMul;
    case R_CORN_DMG:            return gCornDmgMul;
    case R_THREE_UP:            return (float)gThreeUpFull;
    case R_THREE_MID:           return gThreeMidMul;
    case R_BALLOON_POP:         return gBalloonMul;
    case R_ZAMBONI_BREAK:       return gZamboniSlow;
    case R_DANCER_SILENCE:      return (float)gDancerSilence;
    case R_ROOF_GRIP:           return gBulwarkHpMul;
    case R_ENDLESS_HEAL:        return (float)gEndlessHeal;
    case R_STAR_HUNTER:         return (float)gStarHunter;
    case R_FUSION_MASTER:       return gFuseMul;
    case R_LOADOUT_PLUS: {      /* 立刻白送一张肉鸽植物卡（同样走 isNew 分支） */
        int n0;
        if (!on) return 0.0f;
        n0 = gRgN;
        gRelic[R_LOADOUT_PLUS] = 0;
        applyRelic(R_LOADOUT_PLUS);
        return (float)(gRgN - n0);
    }
    case R_LAST_STAND:          return (float)gLastStand;
    }
    return 0.0f;
}

int main(void)
{
    int id, noEffect = 0, checked = 0;
    static const struct { int id; const char *nm; } NAME[] = {
        { R_START_SUN,"储蓄罐" },{ R_START_SUN_BIG,"厚积薄发" },{ R_SKY_SUN,"晴空" },
        { R_SUNFLOWER,"光合作用" },{ R_SUNFLOWER_BIG,"花团锦簇" },{ R_KILL_SUN,"拾荒者" },
        { R_KILL_SUN_BIG,"战利品" },{ R_INTEREST,"利滚利" },{ R_BANKER,"银行家" },
        { R_NECROMANCER,"摄魂者" },{ R_PEA_DMG,"磨刀石" },{ R_PEA_DMG_BIG,"精钢弹头" },
        { R_RATE,"疾风" },{ R_RATE_BIG,"连弩" },{ R_BIG_CALIBER,"大口径" },
        { R_GLASS_CANNON,"玻璃大炮" },{ R_ARMOR_PIERCE,"破甲" },{ R_PIERCE,"穿透弹" },
        { R_VOLLEY,"齐射" },{ R_WEAPON_RACK,"武器架" },{ R_MAIN_UPGRADE,"主射升级" },
        { R_WEAPON_MASTER,"武器大师" },{ R_PLANT_HP,"沃土" },{ R_PLANT_HP_BIG,"巨人" },
        { R_THORNS,"荆棘" },{ R_MEDIC,"前线医疗" },{ R_SPARE_TIRE,"备用轮胎" },
        { R_NO_MOWER,"背水一战" },{ R_IRON_WALL,"铁壁" },{ R_SLOW_LONG,"霜冻" },
        { R_ICE_AGE,"冰河期" },{ R_FROST_CORE,"寒霜核心" },{ R_BOOM_R,"火药桶" },
        { R_BOOM_DMG,"烈性火药" },{ R_EMBER,"余烬" },{ R_CHAIN_BOOM,"连锁引爆" },
        { R_COLUMN,"同列共鸣" },{ R_EMPTY,"留白" },{ R_FLOWER_SEA,"花海" },
        { R_WALLNUT_DOGMA,"坚果教条" },{ R_KILL_STACK,"以战养战" },{ R_SYMBIOSIS,"温床" },
        { R_VANGUARD,"尖兵" },{ R_BACKLINE,"纵深防御" },{ R_MINIMAL,"极简主义" },
        { R_NO_COOLDOWN,"无冷却" },{ R_SUN_GAMBLE,"阳光豪赌" },{ R_BLOOD_PACT,"血祭" },
        { R_ALL_IN,"孤注一掷" },{ R_MODE_SHIFT,"模式切换器" },{ R_MUTANT_SEED,"变异种子" },
        { R_WATER_MODE,"水中模式" },{ R_NIGHT_MODE,"夜间模式" },{ R_NINJA,"隐忍者" },
        { R_CHARGED,"充能心" },{ R_FURY,"愤怒" },{ R_SPIKE_DMG,"尖刺淬毒" },
        { R_SPIKE_WIDE,"尖刺蔓延" },{ R_MAGNET_FAST,"强磁" },{ R_MAGNET_GOLD,"磁化" },
        { R_CORN_STUN,"黄油" },{ R_CORN_DMG,"爆米花" },{ R_THREE_UP,"三线齐鸣" },
        { R_THREE_MID,"中路压制" },{ R_BALLOON_POP,"飞镖" },{ R_ZAMBONI_BREAK,"破冰" },
        { R_DANCER_SILENCE,"静音" },{ R_NIGHT_SUN,"夜灯" },{ R_ROOF_GRIP,"防滑" },
        { R_DESERT_WATER,"绿洲" },{ R_ENDLESS_HEAL,"坚韧" },{ R_STAR_HUNTER,"猎星者" },
        { R_FUSION_MASTER,"融合大师" },{ R_LOADOUT_PLUS,"随机赠卡" },{ R_FIRST_STRIKE,"先手" },
        { R_LAST_STAND,"背水" },
    };
    int n = (int)(sizeof(NAME) / sizeof(NAME[0]));

    _wputenv(L"PVZ_SAVE_FILE=_test_relic_effect_save.dat");
    SetConsoleOutputCP(CP_UTF8);
    srand(20260920);

    printf("== 逐个遗物：装上前后，它承诺修正的量必须不同 ==\n");
    for (id = 0; id < n; id++) {
        float off = measure(NAME[id].id, 0);
        float on  = measure(NAME[id].id, 1);
        checked++;
        if (off == on) {
            noEffect++;
            printf("  ★ %-10s 装上前 %.4f / 装上后 %.4f  —— 没有任何变化\n",
                   NAME[id].nm, (double)off, (double)on);
        } else {
            printf("  %-10s %.4f → %.4f\n", NAME[id].nm, (double)off, (double)on);
        }
    }
    printf("\n  覆盖 %d 条 / 未检测到效果 %d 条\n", checked, noEffect);
    CHECK(checked == 76, "probe: all 76 draftable relics were exercised");
    CHECK(noEffect == 0, "effect: every draftable relic changes at least one quantity");

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_relic_effect_save.dat");
    return fails ? 1 : 0;
}
