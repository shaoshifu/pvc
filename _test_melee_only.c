/* 僵尸「只能贴身伤害植物」验收测试。
   ------------------------------------------------------------------------
   背景：用户要求去掉僵尸远程伤害植物的机制（提升游戏体验）。
   修复手段是总开关 ZOMBIE_REMOTE_PLANT_DMG = 0，关掉 7 处远程/范围/间接伤害。

   本测试的判据（比"看代码"硬）：
     把每种僵尸放在同一行的远处，让它自己朝植物走过去，用真实的 updateGame 推进，
     记录**植物第一次掉血的那一刻，僵尸离植物还有多远**。
        · 修复前：带激光/菜刀/投掷/毒云的僵尸会在好几格外就扣血（距离很大）
        · 修复后：必须等僵尸**贴到植物面前啃食**才掉血（距离 ≈ 一格内）
     所以断言写成「首次掉血时的水平距离 < 1.2 格」——
     这个判据能同时抓住"远程还能打"和"近战被我误伤关掉了"两种翻车。

   另外单独断言：距离很远时跑大量脉冲，植物血量必须**一点不掉**。

   编译：
     gcc -O2 -o _test_melee_only.exe _test_melee_only.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
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

#define PLANT_ROW   2
#define PLANT_COL   2
#define FAR_COL     7

static void wipeField(void)
{
    int i, r, c;
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++) {
            Plant *p = plantAt(r, c);
            if (p) p->alive = 0;
            grid2[r][c].alive = 0;
            gTile[r][c] = 0; gTileT[r][c] = 0.0f;
        }
    for (i = 0; i < MAX_ZOMBIES; i++) { zombies[i].active = 0; zombies[i].dead = 0; }
    for (i = 0; i < MAX_PEAS; i++) peas[i].active = 0;
    gPendingRg = -1; gPendingRgZ = -1;
}

static Plant *plantNut(void)
{
    gPendingRg = -1;
    plantIt(PLANT_ROW, PLANT_COL, PT_WALLNUT);   /* 坚果：不还手、血厚，适合当靶子 */
    return plantAt(PLANT_ROW, PLANT_COL);
}

static Zombie *spawnRgFar(int rgIdx)
{
    int i;
    gPendingRgZ = rgIdx;
    spawnZombieAt((int)rgZombies[rgIdx].base, PLANT_ROW,
                  (float)LAWN_X + ((float)FAR_COL + 0.5f) * (float)CELL_W);
    gPendingRgZ = -1;
    for (i = 0; i < MAX_ZOMBIES; i++)
        if (zombies[i].active && !zombies[i].dead) return &zombies[i];
    return NULL;
}

/* ---- 测试 A：远处只跑能力脉冲，植物一点血都不能掉 ---- */
static float farPulseLoss(int rgIdx)
{
    Plant *p;
    Zombie *z;
    float x0, hp0;
    int k;

    gCurLevel = 0;
    resetGame();
    wipeField();
    p = plantNut();
    if (!p) return -1.0f;
    z = spawnRgFar(rgIdx);
    if (!z) return -1.0f;
    z->speed = 0.0f;                 /* 钉住：只测能力，不测移动 */
    x0 = z->x;
    hp0 = p->hp;

    for (k = 0; k < 30; k++) {
        if (!p->alive || !z->active) break;
        z->x = x0;                   /* 每次把僵尸按回远处 */
        z->rooted = 1.0f;
        rgZombieTraitTick(z, 3.0f);  /* dt=3s > 任意脉冲周期，每次必触发 */
    }
    return hp0 - p->hp;              /* 掉血量，期望 0 */
}

/* 是否有僵尸"贴到了这株植物前面"。
   ⚠️ 判据必须和游戏自己的啃食判定一致：僵尸前方 24px 落在植物所在格。
   用"追某一只僵尸的位置"是不对的 —— 召唤类僵尸（如尸王 RZ3_SUMMON）
   会派出小僵尸去啃，本体还在很远处，那样测出来的"距离"毫无意义
   （第一版就是这么误判的：尸王 124.6px 被判成"远程"，其实是它召唤的小僵尸在啃）。 */
static int anyZombieAdjacent(void)
{
    int i;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        int biteCol;
        if (!z->active || z->dead) continue;
        if (z->row != PLANT_ROW) continue;
        biteCol = (int)floor((z->x - 24.0f - LAWN_X) / (float)CELL_W);
        if (biteCol == PLANT_COL) return 1;
    }
    return 0;
}

/* ---- 测试 B：端到端 —— 植物掉血的那一刻，必须先有僵尸贴到它前面 ---- */
static int approachCheck(int rgIdx, float *outGap)
{
    Plant *p;
    Zombie *z;
    float hp0;
    int step;
    int hurt = 0, remoteHit = 0;

    gCurLevel = 0;
    resetGame();
    wipeField();
    p = plantNut();
    *outGap = -1.0f;
    if (!p) return 0;
    z = spawnRgFar(rgIdx);
    if (!z) return 0;
    hp0 = p->hp;

    for (step = 0; step < 1200; step++) {     /* 最多 60 秒 */
        updateGame(0.05f);
        if (!p->alive || p->hp < hp0 - 0.01f) {
            hurt = 1;
            /* 判据：这一刻必须有僵尸贴身；没有 = 还有远程伤害漏网 */
            if (!anyZombieAdjacent()) remoteHit = 1;
            break;
        }
    }
    *outGap = fabsf(z->x - cellCX(PLANT_COL));
    return hurt ? (remoteHit ? -1 : 1) : 0;   /* 1=正常近战掉血, -1=远程漏网, 0=没掉血 */
}

int main(void)
{
    int i;
    float gap = 0.0f, sumFar = 0.0f;
    int farLeak = 0, hurtCount = 0, remoteLeak = 0;

    _wputenv(L"PVZ_SAVE_FILE=_test_melee_only_save.dat");
    SetConsoleOutputCP(CP_UTF8);
    srand(20260920);

    printf("== A：僵尸钉在 %d 格外，只跑 30 次能力脉冲 —— 植物掉血量必须为 0 ==\n", FAR_COL - PLANT_COL);
    for (i = 0; i < RG_ZOMBIE_N; i++) {
        float loss = farPulseLoss(i);
        sumFar += loss;
        if (loss > 0.01f) {
            farLeak++;
            pn(rgZombies[i].name);
            printf("   ★ 远程打掉了植物 %.1f 血\n", (double)loss);
        }
    }
    printf("  远处仍能伤到植物的僵尸数 = %d\n", farLeak);
    CHECK(farLeak == 0, "remote: no zombie can damage a plant from a distance");

    printf("\n== B：让僵尸自己走过去，检查植物掉血那一刻是否真有僵尸贴身 ==\n");
    for (i = 0; i < RG_ZOMBIE_N; i++) {
        int r = approachCheck(i, &gap);
        if (r == 1) hurtCount++;
        else if (r == -1) {
            remoteLeak++;
            pn(rgZombies[i].name);
            printf("   ★ 植物在没有任何僵尸贴身的情况下掉血（远程漏网）\n");
        }
    }
    printf("  正常近战掉血的僵尸 = %d / %d 种\n", hurtCount, (int)RG_ZOMBIE_N);
    printf("  远程漏网 = %d 种\n", remoteLeak);

    CHECK(remoteLeak == 0,
          "melee: plants only lose HP while a zombie is adjacent to them");
    /* 近战必须还在（否则就是"把伤害全关了"而不是"只关远程"） */
    CHECK(hurtCount > 20,
          "melee: the normal chew path still damages plants (not over-disabled)");

    printf("\n== C：远程能力应改成「压制节奏」而不是变成空壳 ==\n");
    {
        /* 判据：僵尸在远处跑脉冲后，植物的 timer 必须被推后（攻速/产出变慢），
           但血量**一点不掉**。这两条同时成立才说明"伤害转成了压制"。 */
        int pushed = 0, leaked = 0, i2;
        for (i2 = 0; i2 < RG_ZOMBIE_N; i2++) {
            Plant *p2;
            Zombie *z2;
            float t0, hp0;
            int k;
            gCurLevel = 0;
            resetGame();
            wipeField();
            p2 = plantNut();
            if (!p2) continue;
            z2 = spawnRgFar(i2);
            if (!z2) continue;
            z2->speed = 0.0f;
            { float x0 = z2->x; t0 = p2->timer; hp0 = p2->hp;
              for (k = 0; k < 20; k++) {
                  if (!p2->alive || !z2->active) break;
                  z2->x = x0; z2->rooted = 1.0f;
                  rgZombieTraitTick(z2, 3.0f);
              } }
            if (p2->hp < hp0 - 0.01f) leaked++;
            if (p2->timer > t0 + 0.01f) pushed++;
        }
        printf("  会压制植物节奏的僵尸 = %d 种；仍然远程掉血的 = %d 种\n", pushed, leaked);
        CHECK(leaked == 0, "suppress: still zero remote HP loss");
        CHECK(pushed > 8, "suppress: the remote abilities really slow plants down (not dead code)");
    }

    printf("\n== D：开关必须真的是关的（防止有人改回去跑掉）==\n");
    CHECK(ZOMBIE_REMOTE_PLANT_DMG == 0,
          "switch: ZOMBIE_REMOTE_PLANT_DMG is 0 (remote plant damage disabled)");

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    (void)sumFar;
    remove("_test_melee_only_save.dat");
    return fails ? 1 : 0;
}
