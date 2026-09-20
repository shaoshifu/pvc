/* 无窗口回归：连锁反弹、召唤与三个死亡事件必须真的改变运行时状态。 */
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int fails;
#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

static void cleanWorld(void)
{
    int r;
    memset(grid, 0, sizeof(grid));
    memset(grid2, 0, sizeof(grid2));
    memset(zombies, 0, sizeof(zombies));
    memset(peas, 0, sizeof(peas));
    memset(suns, 0, sizeof(suns));
    memset(parts, 0, sizeof(parts));
    memset(spawnQ, 0, sizeof(spawnQ));
    spawnQN = 0;
    gState = ST_PLAY; gCurLevel = 0; gWave = 1; gRgDraftWave = 1;
    waveTimer = 999.0f; skySunTimer = 999.0f; gSfxOn = 0;
    gKilled = 0; gSun = 200; gPendingRgZ = -1; gPendingGenZ = 0;
    gRgAtkMul = 1.0f; gRgPeaBounce = 0; gPierceChance = 0;
    for (r = 0; r < ROWS; r++) {
        mowers[r].active = 1; mowers[r].running = 0;
        mowers[r].row = r; mowers[r].x = LAWN_X - 34.0f;
        mowers[r].speed = 430.0f;
    }
}

static int findTrait(unsigned long long bit)
{
    int i;
    for (i = 0; i < RG_ZOMBIE_N; i++)
        if (rgZombies[i].traits & bit) return i;
    return -1;
}

static Zombie *putRgZombie(int slot, int rg, float x)
{
    Zombie *z;
    gPendingRgZ = rg;
    spawnZombieAt(ZT_NORMAL, 0, x);
    gPendingRgZ = -1;
    z = &zombies[slot];
    z->speed = 0.0f;
    return z;
}

int main(void)
{
    int id, before, pea;
    Zombie *z;

    _wputenv(L"PVZ_SAVE_FILE=_test_mechanics_quality_save.dat");
    relicsRecalc();

    /* 创建函数必须保留肉鸽的 5 次连锁，且不污染武器自己的 bounces。 */
    cleanWorld();
    gRgPeaBounce = 5;
    pea = spawnPeaAt(275.0f, cellBaseY(0) - 62.0f, 400.0f, 0.0f,
                     80.0f, 0, PT_PEASHOOTER, 0);
    CHECK(pea >= 0 && peas[pea].chainLeft == 5 && peas[pea].bounces == 0,
          "rogue ricochet survives projectile initialization");

    /* 两个目标间真的发生一次跳转，而不只是字段非零。 */
    z = &zombies[0];
    memset(z, 0, sizeof(*z)); z->active = 1; z->row = 0; z->x = 300.0f;
    z->y = cellBaseY(0); z->hp = z->maxhp = z->basehp = 1000.0f;
    z = &zombies[1];
    memset(z, 0, sizeof(*z)); z->active = 1; z->row = 0; z->x = 410.0f;
    z->y = cellBaseY(0); z->hp = z->maxhp = z->basehp = 1000.0f;
    updateGame(0.01f);
    CHECK(peas[pea].active && peas[pea].chainLeft == 4 && peas[pea].lastHit == 0,
          "first hit redirects the projectile and consumes one chain");
    updateGame(0.20f);
    CHECK(zombies[1].hp < 1000.0f,
          "redirected projectile damages a second zombie");

    /* 召唤不能再受“波次排队数量”误伤；即使队列已满也要按在场上限工作。 */
    cleanWorld();
    id = findTrait(RZ3_SUMMON);
    CHECK(id >= 0, "summon trait exists in roster");
    z = putRgZombie(0, id, 650.0f);
    spawnQN = MAX_SPAWNS;                 /* 旧实现会在这里错误拒绝召唤 */
    before = zombiesAlive();
    z->rgt = 0.0f;
    rgZombieTraitTick(z, 0.01f);
    CHECK(zombiesAlive() == before + 1,
          "summon creates a minion even while the wave queue is full");

    cleanWorld();
    id = findTrait(RZ_REVIVE3);
    z = putRgZombie(0, id, 500.0f);
    z->hp = 0.0f; zombieDie(z, 0);
    CHECK(!z->dead && z->hp > 0.0f && z->rgRevive && gKilled == 0,
          "revive cancels the first death exactly once");

    cleanWorld();
    id = findTrait(RZ_FORKDEATH);
    z = putRgZombie(0, id, 500.0f);
    z->hp = 0.0f; zombieDie(z, 0);
    CHECK(z->dead && zombiesAlive() == 2 && gKilled == 1,
          "fork death produces two live first-generation children");

    cleanWorld();
    id = findTrait(RZ_CURSEDEATH);
    z = putRgZombie(0, id, 500.0f);
    cardCD[0] = 1.0f; rgCardCD[0] = 1.0f; gSun = 200;
    z->hp = 0.0f; zombieDie(z, 0);
    CHECK(z->dead && cardCD[0] >= 3.49f && rgCardCD[0] >= 3.49f && gSun < 200,
          "curse death delays both card pools and removes sunlight");

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
