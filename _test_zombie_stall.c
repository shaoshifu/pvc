/* 复现并守住「最后一只僵尸打不死」这个问题。
   ------------------------------------------------------------------------
   现象（用户报障）：第 24/24 波，剩两只僵尸怎么打都不死，关卡无法结算。

   根因：单个僵尸能同时挂 4 个回血源，且都是**按脉冲**结算的：
       保命组(FAKEHP/CHISHIELD/DODGE30/HEALBACK/REFLECT/ENTANGLE/FUSEZOMBIE) 10%
       RZ3_SHIELD（护盾）   12%
       RZ3_REGEN（自愈）     9%
       RZ3_TAUNT（嘲讽）     5%        合计最多 36% 基础血/脉冲
       脉冲周期 = 4.6 - 0.42*tier 秒（传奇档 2.5 秒）
     → 最快 36% / 2.5s ≈ 14.4% 最大血/秒。
   而兜底防卡死（terrainUpdate 里 75 秒后开始）起始只有 **7%/秒**，
   追平要靠每秒 +1% 的递增，也就是要拖到 95 秒以后才压得住 ——
   玩家看到的就是"这两只永远打不死"。

   本测试的判据（比"看起来能打死"硬）：
     把每种肉鸽僵尸单独放到**弱火力**（一株史诗植物 ≈ 200 DPS）下，
     跑真实的 `terrainUpdate`（含防卡死）+ 真实的 `rgZombieTraitTick`，
     要求在有限时间内必定阵亡，并且给出最坏耗时。
   → 修复后：最坏耗时必须 < 75 秒（兜底生效前就靠自身火力打不死也没关系，
     但兜底必须能赢）。修复前：会有若干类型跑满上限仍存活。

   编译：
     gcc -O2 -o _test_zombie_stall.exe _test_zombie_stall.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
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

#define SIM_LIMIT   120.0f      /* 单个僵尸最多模拟多少秒 */
#define WEAK_DPS    200.0f      /* 弱火力：单行只放一株史诗植物的量级 */

/* 清场：把僵尸与地块都归零，保证每个样本互不影响 */
static void clearField(void)
{
    int i;
    for (i = 0; i < MAX_ZOMBIES; i++) { zombies[i].active = 0; zombies[i].dead = 0; }
    memset(gTile, 0, sizeof(gTile));
    memset(gTileT, 0, sizeof(gTileT));
    gPendingRgZ = -1; gPendingGenZ = 0;
    gWaveStallT = 0.0f;
    gTheme = TH_NONE; gThemeT = 0.0f;
}

/* 模拟一只僵尸在弱火力下的存活时长。返回 >=0 = 阵亡耗时；<0 = 活满上限 */
static float surviveTime(int rgIdx, float dps)
{
    float t = 0.0f;
    const float dt = 0.1f;
    int i;

    clearField();
    /* ⚠️ 必须用该僵尸**自己的基座**：铁桶/巨人的基础血量远高于普通僵尸，
       一律拿 ZT_NORMAL 测会低估存活时长（偏乐观）。 */
    gPendingRgZ = rgIdx;
    spawnZombieAt(rgZombies[rgIdx].base, 2, (float)LAWN_X + (float)COLS * (float)CELL_W - 40.0f);
    gPendingRgZ = -1;

    /* 找到刚生成的那只 */
    {
        Zombie *z = NULL;
        for (i = 0; i < MAX_ZOMBIES; i++) if (zombies[i].active) { z = &zombies[i]; break; }
        if (!z) return -1.0f;
        /* ★ 自检：血量必须非 0。若忘了 resetGame()，gZombieHpMul 还是 0，
           所有僵尸都会是 0 血 —— 那样测出来的是"全都 0.2 秒死"的假通过。 */
        if (z->maxhp <= 0.0f) {
            printf("[!] 探针无效：僵尸血量为 0（很可能漏了 resetGame()），结果不可信\n");
            return -2.0f;
        }

        while (t < SIM_LIMIT) {
            if (!z->active || z->dead) return t;
            z->speed  = 0.0f;        /* 钉在原地：只测"能不能被打死"，不测移动 */
            z->rooted = 1.0f;        /* 同上，避免它走到左边界被判定为突破 */
            z->x      = (float)LAWN_X + (float)COLS * (float)CELL_W - 40.0f;

            terrainUpdate(dt);          /* 真实兜底（防卡死） */
            rgZombieTraitTick(z, dt);   /* 真实能力脉冲（含全部回血） */

            if (!z->active || z->dead) return t;
            z->hp -= dps * dt;          /* 模拟植物输出 */
            if (z->hp <= 0.0f) zombieDie(z, 0);
            t += dt;
        }
    }
    return -1.0f;
}

/* 同型两只同排：能吃彼此的光环回血，是最容易互相保命的组合 */
static float surviveTimePair(int rgIdx, float dps)
{
    float t = 0.0f;
    const float dt = 0.1f;
    int i, n;
    float px = (float)LAWN_X + (float)COLS * (float)CELL_W - 40.0f;

    clearField();
    for (n = 0; n < 2; n++) {
        gPendingRgZ = rgIdx;
        spawnZombieAt(rgZombies[rgIdx].base, 2, px - (float)n * 26.0f);
        gPendingRgZ = -1;
    }
    /* 同上的自检：成对场景下第一只就必须有血 */
    {
        int any = 0;
        for (i = 0; i < MAX_ZOMBIES; i++)
            if (zombies[i].active) { any = 1;
                if (zombies[i].maxhp <= 0.0f) {
                    printf("[!] 探针无效：成对场景血量为 0，结果不可信\n");
                    return -2.0f;
                } }
        if (!any) { printf("[!] 探针无效：成对场景没生成出僵尸\n"); return -2.0f; }
    }
    while (t < SIM_LIMIT) {
        int alive = 0;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead) continue;
            alive++;
            z->speed  = 0.0f;
            z->rooted = 1.0f;
            z->x      = px - (float)(i % 2) * 26.0f;
            terrainUpdate(dt);
            rgZombieTraitTick(z, dt);
            if (!z->active || z->dead) continue;
            z->hp -= dps * dt;                  /* 火力摊到两只身上 */
            if (z->hp <= 0.0f) zombieDie(z, 0);
        }
        if (alive == 0) return t;
        t += dt;
    }
    return -1.0f;
}

int main(void)
{
    int i;
    float worst = 0.0f, worstDps = 0.0f;
    int worstIdx = -1, stuck = 0;

    _wputenv(L"PVZ_SAVE_FILE=_test_zombie_stall_save.dat");
    SetConsoleOutputCP(CP_UTF8);
    srand(20260920);

    /* ⚠️ 必须先 resetGame()：僵尸血量 = zHp[t] * gZombieHpMul * 波次系数，
       而 gZombieHpMul 是全局、只在 resetGame() 里被置 1 ——
       不走这一步的话所有僵尸都是 **0 血**，"秒死"，测试会全绿但毫无意义
       （第一版就是这么被骗过去的：70 种僵尸最坏耗时 0.2 秒）。 */
    gCurLevel = 0;
    resetGame();

    printf("== 每种肉鸽僵尸（%d 种）在弱火力 %.0f DPS 下的存活时长 ==\n",
           (int)RG_ZOMBIE_N, WEAK_DPS);
    for (i = 0; i < RG_ZOMBIE_N; i++) {
        float t = surviveTime(i, WEAK_DPS);
        if (t == -2.0f) { fails++; break; }        /* 探针自身无效：直接失败 */
        if (t < 0.0f) {
            stuck++;
            pn(rgZombies[i].name);
            printf("  ✗ 活满 %.0f 秒仍未死（tier=%d）\n", (double)SIM_LIMIT,
                   (int)rgZombies[i].tier);
        } else if (t > worst) {
            worst = t; worstIdx = i;
        }
    }
    printf("\n  最坏耗时：");
    if (worstIdx >= 0) pn(rgZombies[worstIdx].name);
    printf("  %.1f 秒\n", (double)worst);
    printf("  活满上限仍不死的类型数 = %d\n", stuck);

    CHECK(stuck == 0, "stall: no rogue zombie survives the weak-fire simulation");
    CHECK(worst < 75.0f,
          "stall: the worst case dies before the 75s safety net finishes it");

    /* ---- 成对测试：用户遇到的是"最后两只"，两只同型可以互相吃光环回血 ---- */
    printf("\n== 成对（同型两只同排）· 强火力 800 DPS ==\n");
    worst = 0.0f; worstIdx = -1; stuck = 0;
    for (i = 0; i < RG_ZOMBIE_N; i++) {
        float t = surviveTimePair(i, 800.0f);
        if (t < 0.0f) {
            stuck++;
            pn(rgZombies[i].name);
            printf("  ✗ 有僵尸活满 %.0f 秒仍未死\n", (double)SIM_LIMIT);
        } else if (t > worst) {
            worst = t; worstIdx = i;
        }
    }
    printf("  最坏耗时：");
    if (worstIdx >= 0) pn(rgZombies[worstIdx].name);
    printf("  %.1f 秒   活满上限的类型数 = %d\n", (double)worst, stuck);
    CHECK(stuck == 0, "stall: a pair of the same heavy-healer still dies out");
    CHECK(worst < 75.0f, "stall: pairs die well before the safety net finishes them");

    /* 兜底本身仍然必须在场：把回血全关掉也不该有僵尸活过上限 */
    printf("\n== 兜底检查：极限情况（0 火力，只靠防卡死）==\n");
    worst = 0.0f; worstIdx = -1; stuck = 0;
    for (i = 0; i < RG_ZOMBIE_N; i++) {
        float t = surviveTime(i, 0.0f);
        if (t < 0.0f) { stuck++; }
        else if (t > worst) { worst = t; worstIdx = i; }
    }
    printf("  0 火力下最坏耗时：");
    if (worstIdx >= 0) pn(rgZombies[worstIdx].name);
    printf("  %.1f 秒（无一存活 = %s）\n", (double)worst, stuck == 0 ? "是" : "否");
    CHECK(stuck == 0, "stall: even with zero fire, the safety net clears every type");

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_zombie_stall_save.dat");
    (void)worstDps;
    return fails ? 1 : 0;
}
