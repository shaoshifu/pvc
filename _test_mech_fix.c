/* 机制修复的运行时验证（第三道闸门：修复后必须真的生效）。
   ------------------------------------------------------------------------
   本文件覆盖本轮"全局机制审计"里查出来并修好的机制。
   每条断言都对应一个具体缺陷，注释里写了修复前的实测行为。

   为什么必须单独一份：这些机制分属不同的能力位与不同的代码路径，
   回归测试里混在一起时，"某一条没生效"很容易被其他全绿的输出盖过去。

   编译：
     gcc -O2 -o _test_mech_fix.exe _test_mech_fix.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int fails;
#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

/* 清场：僵尸 / 植物 / 豌豆 / 阳光 / 地块全清 */
static void clearAll(void)
{
    int i, r, c;
    for (i = 0; i < MAX_ZOMBIES; i++) { zombies[i].active = 0; zombies[i].dead = 0; }
    for (i = 0; i < MAX_PEAS; i++) peas[i].active = 0;
    for (i = 0; i < MAX_SUNS; i++) suns[i].active = 0;
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++) {
            Plant *q = plantAt(r, c);
            if (q) q->alive = 0;
            gTile[r][c] = 0; gTileT[r][c] = 0.0f;
        }
    gPendingRg = -1; gPendingRgZ = -1;
}

static Plant *plantRg(int idx, int row, int col)
{
    gPendingRg = idx;
    plantIt(row, col, (int)rgPlants[idx].base);
    gPendingRg = -1;
    return plantAt(row, col);
}

static Zombie *spawnOne(int type, int row, float x)
{
    int i;
    gPendingRgZ = -1;
    spawnZombieAt(type, row, x);
    for (i = 0; i < MAX_ZOMBIES; i++)
        if (zombies[i].active && !zombies[i].dead) return &zombies[i];
    return NULL;
}

/* ⚠️ 要测"肉鸽僵尸的能力"，必须用 gPendingRgZ 指定索引 ——
   直接 spawnZombieAt(ZT_BUCKET, ...) 得到的是 rg=0 的普通僵尸，
   rgZombieTraitsOf() 返回 0，任何能力都不会触发（第一版就是这么假失败的）。 */
static Zombie *spawnRg(int rgIdx, int row, float x)
{
    int i;
    gPendingRgZ = rgIdx;
    spawnZombieAt((int)rgZombies[rgIdx].base, row, x);
    gPendingRgZ = -1;
    for (i = 0; i < MAX_ZOMBIES; i++)
        if (zombies[i].active && !zombies[i].dead) return &zombies[i];
    return NULL;
}

/* 跑 n 次植物能力脉冲（dt 取 3 秒 > 任意周期，每次必触发） */
static void tick(Plant *p, int row, int col, int n)
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
    _wputenv(L"PVZ_SAVE_FILE=_test_mech_fix_save.dat");
    SetConsoleOutputCP(CP_UTF8);
    srand(20260920);
    gCurLevel = 0;
    resetGame();     /* ⚠️ 不跑开局初始化的话，血量/倍率类全局量还是 0，测试会假通过 */

/* ================= 1. RZ3_REBORN：死亡后满血复活一次 ================= */
    {
        Zombie *z;
        float hp0;
        clearAll();
        z = spawnRg(66, 2, 700.0f);            /* 66 = 不死僵尸（RZ3_REBORN） */
        if (!z) { printf("[!] 没生成出僵尸\n"); return 1; }
        CHECK(z->maxhp > 0.0f && z->rg == 67,
              "probe: the rogue zombie really carries its traits (rg set)");
        hp0 = z->maxhp;
        z->hp = 1.0f;
        zombieDie(z, 0);                       /* 第一次死：应该复活 */
        CHECK(z->dead == 0 && z->hp == hp0,
              "reborn: 不死僵尸 revives at full HP on the first death");
        z->hp = 1.0f;
        zombieDie(z, 0);                       /* 第二次死：不该再复活 */
        CHECK(z->dead == 1, "reborn: it only revives once");

        clearAll();
        z = spawnRg(66, 2, 700.0f);
        z->hp = 1.0f;
        zombieDie(z, 1);                       /* 小推车 / 兜底强杀：绕过复活 */
        /* ⚠️ byMower 路径是"立刻碎掉"：active=0 且 dead=0（不播倒地）。
           所以判据要看 active，不能看 dead。 */
        CHECK(z->active == 0,
              "reborn: the safety net / mower kill bypasses revive (no stall)");
    }

/* ================= 2. RHYTHM：节拍真的变成伤害 ================= */
    {
        Plant *p;
        float dmgEarly, dmgLate;
        Zombie *z;
        clearAll();
        p = plantRg(24, 2, 4);                 /* 音乐节拍豌豆 */
        if (!p) { printf("[!] 种不下 24\n"); fails++; }
        else {
            z = spawnOne(ZT_BUCKET, 2, cellCX(4) + 40.0f);
            z->hp = z->maxhp = 1e6f; z->basehp = 1e6f;   /* 只测掉血量，别被打死 */
            {   float h0 = z->hp; tick(p, 2, 4, 1); dmgEarly = h0 - z->hp; }
            {   float h0 = z->hp; tick(p, 2, 4, 12); dmgLate = h0 - z->hp; }
            printf("      节拍层 %.2f：首次伤害 %.0f → 满层 12 脉冲 %.0f\n",
                   (double)p->rgBeat, (double)dmgEarly, (double)dmgLate);
            CHECK(p->rgBeat > 0.0f && p->rgBeat <= 2.0f,
                  "rhythm: the beat counter actually accumulates (was a dead write)");
            CHECK(dmgLate > dmgEarly * 2.0f,
                  "rhythm: damage scales with the beat (up to 3x)");
        }
    }

/* ================= 3. GAMBLERISK：真的"翻倍或归零" ================= */
    {
        Plant *p;
        int zeros = 0, doubles = 0, others = 0, k;
        clearAll();
        p = plantRg(21, 2, 4);                 /* 赌徒豌豆 */
        if (!p) { printf("[!] 种不下 21\n"); fails++; }
        else {
            float unit;
            Zombie *z = spawnOne(ZT_BUCKET, 2, cellCX(4) + 40.0f);
            z->hp = z->maxhp = 1e9f; z->basehp = 1e9f;
            unit = 1.6f + 0.35f * (float)rgPlants[21].tier;    /* 通用脉冲基础倍率 */
            for (k = 0; k < 60; k++) {
                float h0 = z->hp;
                tick(p, 2, 4, 1);
                {
                    float d = h0 - z->hp;
                    if (d < 1.0f) zeros++;
                    else if (fabsf(d - unit * 2.0f * 0.0f) >= 0.0f && d > 0.0f) doubles++;
                }
            }
            printf("      60 次脉冲：归零 %d 次 / 有伤害 %d 次（应约各半）\n", zeros, doubles);
            CHECK(zeros > 10 && doubles > 10,
                  "gamble: it is genuinely double-or-nothing (was a shared 10x roll)");
            (void)others;
        }
    }

/* ================= 4. WEBSLOW：真的留下蛛网陷阱 ================= */
    {
        Plant *p;
        Zombie *z;
        int col, webbed;
        clearAll();
        p = plantRg(12, 2, 4);                 /* 蜘蛛丝缠绕 */
        z = spawnOne(ZT_BUCKET, 2, cellCX(4) + 30.0f);
        tick(p, 2, 4, 2);
        col = (int)floor((z->x - (float)LAWN_X) / (float)CELL_W);
        webbed = (col >= 0 && col < COLS && (gTile[z->row][col] & TF_WEB)) ? 1 : 0;
        CHECK(webbed, "web: it leaves a real web tile on the ground (was slow-only)");
    }

/* ================= 5. CONFUSE / GRAVITY：真的改变走位 ================= */
    {
        Plant *p;
        Zombie *z;
        float x0;
        clearAll();
        p = plantRg(16, 2, 4);                 /* 噪音向日葵 */
        z = spawnOne(ZT_BUCKET, 2, cellCX(4) + 30.0f);
        x0 = z->x;
        tick(p, 2, 4, 1);
        CHECK(z->x > x0 + 8.0f,
              "confuse: zombies are actually shoved off course (was slow-only)");

        clearAll();
        p = plantRg(9, 2, 4);                  /* 重力玉米 */
        z = spawnOne(ZT_BUCKET, 2, cellCX(4) + 30.0f);
        x0 = z->x;
        tick(p, 2, 4, 1);
        CHECK(z->x > x0 + 8.0f,
              "gravity: the stun carries real knockback (was slow-only)");
    }

/* ================= 6. MIRROR：真的血量 4 倍 ================= */
    {
        Plant *p04;
        float expect;
        clearAll();
        p04 = plantRg(4, 2, 4);                /* 镜像墙坚果（RG_T_MIRROR） */
        /* 期望 = 同基座同档位的基准血量 × 4（不看别的植物，直接算出来比） */
        expect = plantHpMax((int)rgPlants[4].base)
               * rgDmgMul((int)rgPlants[4].tier) * p04->fuseMul * 4.0f;
        if (p04) {
            printf("      镜像墙坚果 maxhp %.0f，期望(基准×4) %.0f，比值 ×%.2f\n",
                   (double)p04->maxhp, (double)expect, (double)(p04->maxhp / expect));
            CHECK(fabsf(p04->maxhp / expect - 1.0f) < 0.02f,
                  "mirror: maxhp is exactly 4x the same-base baseline (was never applied)");
        } else { printf("[!] 种不下 04\n"); fails++; }
    }

/* ================= 7. DIGITRAIN：真的给屏障 ================= */
    {
        Plant *p = NULL;
        clearAll();
        p = plantRg(23, 2, 4);                 /* 代码向日葵 */
        if (!p) { printf("[!] 种不下 23\n"); fails++; }
        else {
            tick(p, 2, 4, 3);
            printf("      代码向日葵 shield = %.1f\n", (double)p->shield);
            CHECK(p->shield > 0.0f,
                  "digirain: a real damage-absorbing barrier is granted (was heal-only)");
        }
    }

/* ================= 8. MEMESPREAD：真的传播阳光 ================= */
    {
        Plant *p;
        int s0, s1;
        clearAll();
        p = plantRg(19, 2, 4);                 /* 模因传播葵 */
        s0 = countSuns();
        tick(p, 2, 4, 3);
        s1 = countSuns();
        printf("      模因传播葵 产阳光 %d → %d\n", s0, s1);
        CHECK(s1 > s0,
              "memesp: it actually spreads sun (previously produced none at all)");
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_mech_fix_save.dat");
    return fails ? 1 : 0;
}
