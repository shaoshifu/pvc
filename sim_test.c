/* 无窗口逻辑模拟：自动打完一整局，验证波次/经济/胜负判定（临时测试用） */
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int gPlanted;
static int gSunCollected;

/* 机器人：模拟一个「会挑遗物、按固定阵型铺场」的中等水平玩家。
   注意：羁绊系统已被移除（gTraitCount 恒为 0），所以下面所有 gTraitCount
   判断都退化成固定阵型 —— 这正是当前版本的真实现状，胜率可与历史基线直接比较。 */
static void bot(void)
{
    int r, c, i;

    /* 自动收阳光 */
    for (i = 0; i < MAX_SUNS; i++)
        if (suns[i].active && !suns[i].flying) { suns[i].flying = 1; gSunCollected++; }

    /* 会用消费点：金气 >= 10 就在最右列放个南瓜炸弹；幽能 >= 20 就全屏减速 */
    if (gBanker && gGold >= 10) {
        for (r = 0; r < ROWS; r++) if (zombiesAlive() > 3) {
            for (i = 0; i < MAX_ZOMBIES; i++) if (zombies[i].active && zombies[i].row == r &&
                                                 zombies[i].x > 700.0f) {
                int cc = (int)((zombies[i].x - LAWN_X) / CELL_W);
                if (cc >= 0 && cc < COLS && cellLegal(r, cc, 1, 1)) {
                    gGold -= 10;
                    plantIt(r, cc, PT_CHERRY);
                    if (grid[r][cc].alive) { grid[r][cc].summon = 1; grid[r][cc].fuse = 0.6f; }
                }
                break;
            }
            break;
        }
    }
    if (gNecromancer && gGhost >= 20 && zombiesAlive() > 6) {
        gGhost -= 20;
        for (i = 0; i < MAX_ZOMBIES; i++)
            if (zombies[i].active && !zombies[i].dead) zombies[i].slow = 6.0f;
    }

    /* 波次奖励植物入槽后是免费种植、只受自身冷却限制。 */
    if (gRgN > 0) {
        int h;
        for (h = 0; h < gRgN; h++) {
            int ri = gRgCard[h], cc2;
            if (ri < 0 || ri >= RG_PLANT_N || rgCardCD[h] > 0.0f ||
                (!gRgFree[h] && gSun < rgPlantCost(ri))) continue;
            for (cc2 = 2; cc2 < 8; cc2++) for (r = 0; r < ROWS; r++)
                if (!plantAt(r, cc2) && cellLegal(r, cc2, 1, 1)) {
                    gPendingRg = ri;
                    plantIt(r, cc2, (int)rgPlants[ri].base);
                    gPendingRg = -1;
                    if (gRgFree[h]) gRgFree[h] = 0;
                    else            gSun -= rgPlantCost(ri);
                    rgCardCD[h] = (float)(20 - 2 * rgPlants[ri].tier);
                    gPlanted++;
                    return;
                }
        }
    }

    /* 0) 神级植物：本局三选一拿到的额外卡槽。
       机器人原来完全不种它们 —— 等于把最贵的一批奖励扔掉，
       测出来的胜率严重偏低。有富余阳光就种在中场。 */
    /* 经济门槛：神级植物 250~650 阳光，前期买它会把向日葵饿死。
       先有 4 株向日葵的经济再考虑上神级，这才是真人的打法。 */
    if (gBonusN > 0 && countPlants(PT_SUNFLOWER) >= 4) {
        int h;
        for (h = 0; h < gBonusN; h++) {
            int t = gBonusPlant[h];
            int cc2;
            if (cardCD[t] > 0.0f || gSun < plantCost(t) + 25) continue;
            for (cc2 = 2; cc2 < 8; cc2++) for (r = 0; r < ROWS; r++)
                if (!plantAt(r, cc2) && cellLegal(r, cc2, 1, 1)) {
                    plantIt(r, cc2, t); gSun -= plantCost(t);
                    cardCD[t] = plantCd(t);
                    gPlanted++; return;
                }
        }
    }

    /* 1) 经济：先铺满左两列的向日葵 */
    {
        int want = 4;
        if (countPlants(PT_SUNFLOWER) < want)
            for (c = 0; c < 2; c++) for (r = 0; r < ROWS; r++)
                if (!plantAt(r, c) && cardCD[PT_SUNFLOWER] <= 0.0f && gSun >= plantCost(PT_SUNFLOWER)) {
                    plantIt(r, c, PT_SUNFLOWER); gSun -= plantCost(PT_SUNFLOWER);
                    cardCD[PT_SUNFLOWER] = plantCd(PT_SUNFLOWER);
                    gPlanted++; return;
                }
    }
    /* 2) 壁垒：每行最右放一株坚果墙挡枪 */
    if (gTraitCount[TR_BULWARK] < 2 || zombiesAlive() > 4)
        for (r = 0; r < ROWS; r++)
            if (!plantAt(r, 8) && cardCD[PT_WALLNUT] <= 0.0f && gSun >= plantCost(PT_WALLNUT)) {
                plantIt(r, 8, PT_WALLNUT); gSun -= plantCost(PT_WALLNUT);
                cardCD[PT_WALLNUT] = plantCd(PT_WALLNUT);
                gPlanted++; return;
            }
    /* 3) 射手：从中间往右铺豌豆（有余钱换双发） */
    for (c = 2; c < 8; c++)
        for (r = 0; r < ROWS; r++)
            if (!plantAt(r, c) && cardCD[PT_PEASHOOTER] <= 0.0f &&
                gSun >= plantCost(PT_PEASHOOTER)) {
                /* 有闲钱且射手已到 1 档就换成双发（DPS 更高） */
                int t = (gTraitCount[TR_SHOOTER] >= 6 && gSun >= plantCost(PT_REPEATER) &&
                         cardCD[PT_REPEATER] <= 0.0f) ? PT_REPEATER : PT_PEASHOOTER;
                plantIt(r, c, t); gSun -= plantCost(t);
                cardCD[t] = plantCd(t);
                gPlanted++; return;
            }
    /* 4) 减速：补寒冰射手（羁绊移除后此分支不再触发，保留以对比） */
    if (gTraitCount[TR_FROST] < 4 && gTraitCount[TR_SHOOTER] >= 6 && gSun >= plantCost(PT_SNOWPEA))
        for (c = 2; c < 8; c++) for (r = 0; r < ROWS; r++)
            if (!plantAt(r, c) && cardCD[PT_SNOWPEA] <= 0.0f) {
                plantIt(r, c, PT_SNOWPEA); gSun -= plantCost(PT_SNOWPEA);
                cardCD[PT_SNOWPEA] = plantCd(PT_SNOWPEA);
                gPlanted++; return;
            }
    /* 5) 地刺：守第 8 列，专治挖掘僵尸 */
    if (gSun >= 400)
        for (r = 0; r < ROWS; r++)
            if (!plantAt(r, 7) && cardCD[PT_SPIKEWEED] <= 0.0f && gSun >= plantCost(PT_SPIKEWEED)) {
                plantIt(r, 7, PT_SPIKEWEED); gSun -= plantCost(PT_SPIKEWEED);
                cardCD[PT_SPIKEWEED] = plantCd(PT_SPIKEWEED);
                gPlanted++; return;
            }
}

static void report(const char *tag)
{
    int i, z = 0, p = 0;
    for (i = 0; i < MAX_ZOMBIES; i++) if (zombies[i].active) z++;
    for (i = 0; i < ROWS; i++) { int c; for (c = 0; c < COLS; c++) if (grid[i][c].alive) p++; }
    printf("%-14s t=%4ds 波=%2d 阳光=%4d 植物=%2d 僵尸=%2d 击杀=%3d 遗物=%2d 推车剩=%d\n",
           tag, (int)gElapsed, gWave, gSun, p, z, gKilled, gRelicCount,
           mowers[0].active + mowers[1].active + mowers[2].active + mowers[3].active + mowers[4].active);
}

int main(void)
{
    /* 测试隔离：把存档写到自己的文件，绝不碰玩家真实的 pvz_save.dat */
    _wputenv(L"PVZ_SAVE_FILE=_sim_test_save.dat");
    float dt = 1.0f / 60.0f;
    int sec, i, lastReport = -1;

    srand(12345);
    resetGame();
    gState = ST_PLAY;
    gWave = 0;

    for (sec = 0; sec < 3000 && (gState == ST_PLAY || gState == ST_DRAFT); sec++) {
        for (i = 0; i < 60; i++) {
            if (gState == ST_DRAFT) {
                /* 机器人策略：波次奖励优先叠火力/攻速，其次拿高品质植物；
                   通关的传统遗物三选一仍用原稀有度策略。 */
                int best = -1, bi;
                if (gDraftMode == 1) {
                    int pref[4] = { GROW_DMG, GROW_RATE, GROW_CD, GROW_HP };
                    int pi;
                    /* 先补到 4 张局内植物卡，再转入纯成长，更接近真人构筑。 */
                    if (gRgN < 4) {
                        int tier = -1;
                        for (bi = 0; bi < DRAFT_N; bi++) if (DRAFT_IS_RG(gDraft[bi])) {
                            int ri = gDraft[bi] - RG_CARD_BASE;
                            if (rgPlants[ri].tier > tier) { tier = rgPlants[ri].tier; best = bi; }
                        }
                    }
                    for (pi = 0; pi < 4 && best < 0; pi++)
                        for (bi = 0; bi < DRAFT_N; bi++)
                            if (gDraft[bi] == GROWTH_CARD_BASE + pref[pi]) { best = bi; break; }
                    if (best < 0) {
                        int tier = -1;
                        for (bi = 0; bi < DRAFT_N; bi++) if (DRAFT_IS_RG(gDraft[bi])) {
                            int ri = gDraft[bi] - RG_CARD_BASE;
                            if (rgPlants[ri].tier > tier) { tier = rgPlants[ri].tier; best = bi; }
                        }
                    }
                    if (best < 0) best = 0;
                } else if (!gDraftPlantPerm) {
                    for (bi = 0; bi < DRAFT_N; bi++) {
                        if (!DRAFT_IS_PLANT(gDraft[bi])) continue;
                        if (DRAFT_PLANT_ID(gDraft[bi]) >= PT_COUNT) continue;  /* 精华卡 */
                        best = bi; break;
                    }
                }
                if (gDraftMode == 0 && best < 0) {
                    best = 0;
                    for (bi = 0; bi < DRAFT_N; bi++) {
                        if (DRAFT_IS_PLANT(gDraft[bi])) continue;
                        if (DRAFT_IS_PLANT(gDraft[best]) ||
                            relicDefs[gDraft[bi]].rarity > relicDefs[gDraft[best]].rarity)
                            best = bi;
                    }
                    if ((rand() % 100) < 20) best = rand() % DRAFT_N;   /* 留一点随机性 */
                }
                applyRelic(gDraft[best]);
                gState = ST_PLAY;
            }
            if (gState == ST_PLAY) { bot(); updateGame(dt); }
            if (gState == ST_WIN || gState == ST_LOSE) break;
        }
        if (gElapsed >= (lastReport + 60)) {
            char buf[32];
            lastReport = (int)gElapsed;
            sprintf(buf, "[%d 秒]", lastReport);
            report(buf);
        }
    }

    printf("\n================ 结果 ================\n");
    if (gState == ST_WIN)       printf("胜利！守住了 20 波僵尸\n");
    else if (gState == ST_LOSE) printf("失败：被僵尸攻破\n");
    else                        printf("异常：超时未结束 (state=%d)\n", gState);
    printf("总时长 %.1f 秒   击杀 %d 只   种植 %d 株   收集阳光事件 %d 次\n",
           (double)gElapsed, gKilled, gPlanted, gSunCollected);
    printf("本局遗物 %d 项\n剩余小推车: %d / 5\n", gRelicCount,
           mowers[0].active + mowers[1].active + mowers[2].active + mowers[3].active + mowers[4].active);
    return 0;
}
