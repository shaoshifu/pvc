/* 吞噬系植物：运行时验证（第二道闸门）。
   ------------------------------------------------------------------------
   静态审计（能力位有没有被引用）已通过 —— 三个位都各有 1 处实现引用。
   但"接通了"≠"实现了"：这三处的实现都可能与描述不符。本测试逐个量化。

   被测对象（rgPlants 里带吞噬类能力位的 8 株）：
     03 黑洞磁力菇   RG_T_DEVOURBUL          「吞噬范围内所有来弹，转化为阳光」
     05 贪吃蛇大嘴花 RG_T_GROWEAT            「吞掉僵尸后体型与伤害永久成长」
     27 塔防合成人   RG_T_FUSEPLANT          「吞噬相邻植物，融合其全部能力」
     32 镜像大嘴     MIRROR|GROWEAT|LIFESTEAL
     48 吞噬黑洞葵   BLACKHOLE|DEVOURBUL
     82 食人花圃     LIFESTEAL|GROWEAT
     83 永生藤       LIFESTEAL|TIMEHEAL|GROWEAT
     98 创世树       FUSEPLANT|GROWEAT|SHARESUN|AURABUFF

   三个判据（**修复后**应该成立的行为）：
     A 该不该成长 —— 只有"真的吞掉过附近的僵尸"才成长，空场不涨
     B 有没有上限 —— 成长封顶在出场血量的 2.5 倍
     C 吞噬动作是否真的发生 —— 来弹被清除、邻居被吃掉（只吃一次）

   编译：
     gcc -O2 -o _test_devour_plants.exe _test_devour_plants.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int fails;
#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

static void pn(const wchar_t *w)
{
    char b[128];
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), NULL, NULL) <= 0) b[0] = 0;
    fputs(b, stdout);
}

/* 清场：僵尸、植物、豌豆、阳光全清 */
static void clearAll(void)
{
    int i, r, c;
    for (i = 0; i < MAX_ZOMBIES; i++) { zombies[i].active = 0; zombies[i].dead = 0; }
    for (i = 0; i < MAX_PEAS; i++) peas[i].active = 0;
    for (i = 0; i < MAX_SUNS; i++) suns[i].active = 0;
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++) {
            Plant *q = plantAt(r, c);
            if (q) { q->alive = 0; }
        }
    gPendingRg = -1;
}

static Plant *plantRg(int idx, int row, int col)
{
    gPendingRg = idx;
    plantIt(row, col, (int)rgPlants[idx].base);
    gPendingRg = -1;
    return plantAt(row, col);
}

/* 跑 n 次能力脉冲。dt 取 3 秒 > 任意脉冲周期（2.8 / rate），每次必触发一次 */
static void tickPulses(Plant *p, int row, int col, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!p || !p->alive) return;
        rgPlantTraitTick(p, 3.0f, row, col,
                         cellCX(col), cellBaseY(row), cellBaseY(row) - 62.0f);
    }
}

static int countSuns(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_SUNS; i++) if (suns[i].active) n++;
    return n;
}

int main(void)
{
    static const int DEVOUR[8] = { 3, 5, 27, 32, 48, 82, 83, 98 };
    static const int HAS_GROWEAT[8] = { 0, 1, 0, 1, 0, 1, 1, 1 };

    _wputenv(L"PVZ_SAVE_FILE=_test_devour_plants_save.dat");
    SetConsoleOutputCP(CP_UTF8);
    srand(20260920);
    /* ⚠️ 必须先开局初始化：植物的基础血量/攻击倍率里有只在 resetGame() 里置位的全局量，
       不走这一步会得到一堆 0 值，测试会"全绿但什么都没测"。 */
    gCurLevel = 0;
    resetGame();

/* ================= A/B：GROWEAT 该不该成长、有没有上限 ================= */
    printf("== A/B：空场（无僵尸、无相邻植物）连跑 40 个脉冲 ==\n");
    {
        int k;
        float base05 = 0.0f, base98 = 0.0f;
        Plant *p;
        int grewWithoutEating = 0, unbounded = 0;

        for (k = 0; k < 8; k++) {
            int idx = DEVOUR[k];
            float h0, h40, hAfterEats, ratioEats;
            clearAll();
            /* FUSEPLANT 的"空场"定义 = 周围没有别的植物（下面 C 段单独测邻居） */
            p = plantRg(idx, 2, 4);
            if (!p) { printf("  [!] 种不下 idx=%d\n", idx); fails++; continue; }
            h0 = p->maxhp;
            tickPulses(p, 2, 4, 40);
            h40 = p->maxhp;

            /* 喂它僵尸：在半径内连续击杀，看该不该长大。
               ⚠️ 要喂到**越过上限**才算真的验证了封顶。
               实现是"一个脉冲兑现一次成长"（限速），所以要跑够脉冲：
               分 6 批喂 30 只、每批跑 5 个脉冲 = 足够 16 次成长。
               无上限时 1.06^16 ≈ 2.54 且之后继续涨；封顶后必须停在 2.5 倍。 */
            if (HAS_GROWEAT[k]) {
                int b, j;
                for (b = 0; b < 6; b++) {
                    for (j = 0; j < 5; j++) {
                        gPendingRgZ = -1;
                        spawnZombieAt(ZT_NORMAL, 2, cellCX(4) + 40.0f);
                        { int q; for (q = 0; q < MAX_ZOMBIES; q++)
                            if (zombies[q].active && !zombies[q].dead) { zombieDie(&zombies[q], 0); break; } }
                    }
                    /* 每兑现一批后多跑几个脉冲：实现里是"每个脉冲只兑现一次成长"
                       （限速），所以要跑够脉冲数才能把上限顶到。 */
                    tickPulses(p, 2, 4, 5);
                }
                hAfterEats = p->maxhp;
                ratioEats = hAfterEats / h0;
            } else { hAfterEats = h40; ratioEats = h40 / h0; }

            printf("  %2d ", idx);
            pn(rgPlants[idx].name);
            printf(" 初始 %.0f | 空场 40 脉冲 ×%.2f | 再喂 6 只僵尸 ×%.2f\n",
                   (double)h0, (double)(h40 / h0), (double)ratioEats);

            if (HAS_GROWEAT[k] && h40 > h0 * 1.001f) grewWithoutEating++;
            /* 30 只僵尸 ≈ 1.06^30 = 5.7 倍（若无上限），封顶必须把它压在 2.5 倍 */
            if (ratioEats > 2.51f) unbounded++;
            if (idx == 5)  base05 = ratioEats;
            if (idx == 98) base98 = ratioEats;
        }
        /* 修复后：空场不该涨 —— "吞掉僵尸后成长"这个前提必须真实存在 */
        CHECK(grewWithoutEating == 0,
              "grow: GROWEAT plants do NOT grow without eating a zombie");
        /* 修复后：喂了僵尸必须涨（否则机制等于被砍没了） */
        CHECK(base05 > 1.01f && base98 > 1.01f,
              "grow: they DO grow once zombies actually die nearby");
        /* 修复后：成长必须封顶在 2.5 倍 */
        CHECK(unbounded == 0,
              "grow: growth stays capped at 2.5x base HP");
    }

/* ================= C：FUSEPLANT 是否真的吞掉相邻植物 ================= */
    printf("\n== C：FUSEPLANT（27 / 98）旁边放一株普通植物 ==\n");
    {
        int idxs[2] = { 27, 98 };
        int k;
        for (k = 0; k < 2; k++) {
            Plant *p, *nb;
            float h0;
            int aliveAfter;
            clearAll();
            p = plantRg(idxs[k], 2, 4);
            if (!p) { printf("  [!] 种不下 idx=%d\n", idxs[k]); fails++; continue; }
            gPendingRg = -1;
            plantIt(2, 5, PT_PEASHOOTER);            /* 相邻一株普通植物 */
            nb = plantAt(2, 5);
            if (!nb) { printf("  [!] 邻居没种上\n"); fails++; continue; }
            h0 = p->maxhp;
            tickPulses(p, 2, 4, 5);
            aliveAfter = nb->alive;
            printf("  %2d ", idxs[k]);
            pn(rgPlants[idxs[k]].name);
            printf("  5 脉冲后：邻居存活=%d  自身 maxhp ×%.2f\n",
                   aliveAfter, (double)(p->maxhp / h0));
            CHECK(aliveAfter == 0,
                  "fuse: the neighbouring plant IS consumed (真的吞掉了)");
            /* 只吞一次：再放个邻居也不该再吃，且血量不再叠乘 */
            {
                float hh = p->maxhp;
                plantIt(2, 5, PT_PEASHOOTER);
                tickPulses(p, 2, 4, 5);
                CHECK(p->maxhp <= hh * 1.001f,
                      "fuse: it only fuses once (no per-pulse compounding)");
            }
        }
    }

/* ================= D：DEVOURBUL 是否真的吞掉来弹 ================= */
    printf("\n== D：DEVOURBUL（03 / 48）面前停一发豌豆 ==\n");
    {
        int idxs[2] = { 3, 48 };
        int k;
        for (k = 0; k < 2; k++) {
            Plant *p;
            int pid, stillThere;
            int sunWithPea, sunNoPea;

            /* 有豌豆 */
            clearAll();
            p = plantRg(idxs[k], 2, 4);
            if (!p) { printf("  [!] 种不下 idx=%d\n", idxs[k]); fails++; continue; }
            pid = spawnPeaAt(cellCX(4), cellBaseY(2) - 60.0f, -1.0f, 0.0f,
                             10.0f, 2, -1, 0);          /* vx<0 = 朝我方飞来的"来弹" */
            tickPulses(p, 2, 4, 6);
            stillThere = (pid >= 0 && peas[pid].active);
            sunWithPea = countSuns();

            /* 无豌豆（其余条件完全相同） */
            clearAll();
            p = plantRg(idxs[k], 2, 4);
            tickPulses(p, 2, 4, 6);
            sunNoPea = countSuns();

            printf("  %2d ", idxs[k]);
            pn(rgPlants[idxs[k]].name);
            printf("  6 脉冲后来弹仍在=%d  产阳光 有弹=%d / 无弹=%d\n",
                   stillThere, sunWithPea, sunNoPea);
            CHECK(stillThere == 0,
                  "devour: the incoming bullet IS swallowed (来弹真的被吃掉了)");
            CHECK(sunWithPea > sunNoPea,
                  "devour: eating a bullet yields extra sun (转化为阳光成立)");
            /* 反向的（我方射出去的）不该被误吃 */
            {
                int pid2;
                clearAll();
                p = plantRg(idxs[k], 2, 4);
                pid2 = spawnPeaAt(cellCX(4), cellBaseY(2) - 60.0f, 1.0f, 0.0f,
                                  10.0f, 2, -1, 0);     /* vx>0 = 我方出弹 */
                tickPulses(p, 2, 4, 6);
                CHECK(pid2 >= 0 && peas[pid2].active,
                      "devour: outgoing friendly peas are not swallowed");
            }
        }
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_devour_plants_save.dat");
    return fails ? 1 : 0;
}
