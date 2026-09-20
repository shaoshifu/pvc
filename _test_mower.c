/* 无窗口回归测试：击退方向、特殊位移边界、小推车与失败判定。 */
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
    memset(spawnQ, 0, sizeof(spawnQ));
    spawnQN = 0;
    gCurLevel = 0;
    gState = ST_PLAY;
    gWave = 1;
    gRgDraftWave = 1;
    waveTimer = 999.0f;
    skySunTimer = 999.0f;
    gNoMower = 0;
    gSpareTire = 0;
    for (r = 0; r < ROWS; r++) {
        mowers[r].active = 1;
        mowers[r].running = 0;
        mowers[r].row = r;
        mowers[r].x = LAWN_X - 34.0f;
        mowers[r].speed = 430.0f;
        mowers[r].recharge = 0.0f;
    }
}

static void putWalker(float x)
{
    Zombie *z = &zombies[0];
    memset(z, 0, sizeof(*z));
    z->active = 1;
    z->type = ZT_NORMAL;
    z->row = 0;
    z->x = x;
    z->y = cellBaseY(0);
    z->speed = 100.0f;
    z->hp = z->maxhp = z->basehp = 500.0f;
}

int main(void)
{
    Zombie z;
    int r, c, alive, hurt;
    _wputenv(L"PVZ_SAVE_FILE=_test_mower_save.dat");
    relicsRecalc();

    memset(&z, 0, sizeof(z));
    z.x = 300.0f;
    zombieKnockback(&z, 25.0f);
    CHECK(z.x == 325.0f, "plant knockback pushes zombies away from the house");

    z.x = 170.0f;
    zombieSpecialMove(&z, -300.0f);
    CHECK(z.x == cellCX(0), "blink and dash stop at the first-cell safety line");
    zombieSpecialMove(&z, -40.0f);
    CHECK(z.x == cellCX(0), "special movement cannot repeatedly cross the safety line");

    cleanWorld();
    putWalker(LAWN_X + 23.0f);
    updateGame(0.10f);
    CHECK(mowers[0].running && gState == ST_PLAY,
          "a real boundary crossing starts the mower without losing the game");

    cleanWorld();
    mowers[0].active = 0;
    putWalker(LAWN_X + 23.0f);
    updateGame(0.10f);
    CHECK(gState == ST_PLAY,
          "crossing the mower line alone does not cause an invisible instant loss");
    updateGame(0.45f);
    CHECK(gState == ST_LOSE, "loss occurs only after the zombie reaches the house line");

    cleanWorld();
    spawnZombieAt(ZT_DIGGER, 0, VIEW_W + 80.0f);
    CHECK(zombies[0].x > LAWN_X + COLS * CELL_W,
          "digger announces itself outside the lawn instead of appearing on a plant cell");

    cleanWorld();
    grid[0][1].alive = 1;
    grid[0][1].row = 0; grid[0][1].col = 1;
    grid[0][1].hp = grid[0][1].maxhp = 1000.0f;
    grid[0][6].alive = 1;
    grid[0][6].row = 0; grid[0][6].col = 6;
    grid[0][6].hp = grid[0][6].maxhp = 1000.0f;
    putWalker(cellCX(COLS - 1));
    zombies[0].rg = 69;                 /* 潜地僵尸：纯 RZ3_BURROW */
    zombies[0].rgt = 0.0f;
    rgZombieTraitTick(&zombies[0], 0.1f);
    CHECK(zombies[0].burrowPending && fabsf(zombies[0].x - cellCX(COLS - 1)) < 0.1f,
          "burrow first creates a warning and does not teleport immediately");
    CHECK(zombies[0].burrowTargetX > cellCX(6),
          "burrow target stays on the front plant's zombie-facing side");
    rgZombieTraitTick(&zombies[0], 1.4f);
    CHECK(zombies[0].vaulted && !zombies[0].burrowPending &&
          zombies[0].x > cellCX(6),
          "burrow completes without entering the protected back line");

    cleanWorld();
    grid[0][1].alive = 1; grid[0][1].row = 0; grid[0][1].col = 1;
    grid[0][1].hp = grid[0][1].maxhp = 1000.0f;
    grid[0][6].alive = 1; grid[0][6].row = 0; grid[0][6].col = 6;
    grid[0][6].hp = grid[0][6].maxhp = 1000.0f;
    putWalker(cellCX(1));                /* 模拟旧机制已把僵尸送到后排 */
    updateGame(0.10f);
    CHECK(grid[0][1].hp == 1000.0f,
          "a living front plant fully protects plants behind it");
    CHECK(grid[0][6].hp < 1000.0f && zombies[0].x > cellCX(6),
          "a bypassed zombie is returned to and attacks the front line");

    cleanWorld();
    grid[0][1].alive = 1; grid[0][1].row = 0; grid[0][1].col = 1;
    grid[0][1].hp = grid[0][1].maxhp = 1000.0f;
    grid[0][6].alive = 1; grid[0][6].row = 0; grid[0][6].col = 6;
    grid[0][6].hp = grid[0][6].maxhp = 1000.0f;
    putWalker(cellCX(8));
    for (c = 0; c < RG_ZOMBIE_N; c++)
        if (rgZombies[c].traits & RZ_LASERCUT) { zombies[0].rg = c + 1; break; }
    zombies[0].rgt = 0.0f;
    rgZombieTraitTick(&zombies[0], 0.1f);
    CHECK(grid[0][1].hp == 1000.0f && grid[0][6].hp < 1000.0f,
          "ranged zombie skills can only damage the current front plant");

    plantHazardDamage(&grid[0][6], 99999.0f);
    CHECK(grid[0][6].alive && grid[0][6].hp == 1.0f,
          "map hazards can wound but never silently erase a plant");

    cleanWorld();
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
        Plant *p = &grid[r][c];
        memset(p, 0, sizeof(*p));
        p->alive = 1; p->row = r; p->col = c;
        p->hp = p->maxhp = 1000.0f;
        p->w = p->hh = 1;
    }
    putWalker(cellCX(7));
    zombies[0].rg = 35;                 /* 逆向时间僵尸 */
    zombies[0].rgt = 0.0f;
    srand(7);
    rgZombieTraitTick(&zombies[0], 0.1f);
    alive = hurt = 0;
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
        if (grid[r][c].alive) alive++;
        if (grid[r][c].alive && grid[r][c].hp < grid[r][c].maxhp) hurt++;
    }
    CHECK(alive == ROWS * COLS, "rewind damages a plant instead of silently deleting it");
    CHECK(hurt == 1, "rewind leaves one clearly damaged target for the player to react to");

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
