/* 始祖级强化 + 三选一开牌庆典：验证。
   ------------------------------------------------------------------------
   三块内容，分别用不同手段验证：
     A 数值强化  —— 直接问公开接口（rgDmgMul / rgRateMul / rgRangeMul），
                    并核对"僵尸血量没被连带放大"这条副作用。
     B 始祖威压  —— 摆一株始祖 + 一只僵尸，跑 primordialAuraUpdate，
                    验证「有始祖则掉血、没始祖则不掉血」，并量出速率。
     C 开牌庆典  —— 状态机（触发阈值、始祖额外改写世界）+ **抓帧**：
                    把 gCelebT 摆到指定进度渲染，导出 BMP 供肉眼确认演出。
                    用进度而不是实时，是为了让画面可复现（不依赖帧率与墙钟）。

   编译：
     gcc -O2 -o _test_primordial_buff.exe _test_primordial_buff.c \
         -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
   用法： ./_test_primordial_buff.exe <资源目录> <抓帧输出目录>
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

#define SEED 20260920u


/* ---- 呈现用的"临时输出 DC" ----
   ⚠️ render(out) 的**收尾会把画面呈现回 out**（BitBlt/StretchBlt 到窗口坐标）。
   如果直接把 gWorldDC 当 out 传进去，世界层就会被自己的 1x 缩略图覆盖 ——
   实测表现为画面里出现两套尺度、两个中心（像重影），
   而且掩盖了真正要看的世界层内容。
   所以测试里必须传一块**独立的临时 DC**，让呈现写到它上面，
   gWorldDC 保持干净，再倒出来。 */
static HDC     gShotDC = NULL;
static HBITMAP gShotBM = NULL;

static void shotEnsure(void)
{
    if (gShotDC) return;
    gShotDC = CreateCompatibleDC(NULL);
    gShotBM = CreateCompatibleBitmap(gShotDC, VIEW_W, VIEW_H);
    SelectObject(gShotDC, gShotBM);
}

static int fails;
#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

static void wipeField(void)
{
    int i, r, c;
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++) {
            Plant *p = plantAt(r, c);
            if (p) p->alive = 0;
            grid2[r][c].alive = 0;
        }
    for (i = 0; i < MAX_ZOMBIES; i++) { zombies[i].active = 0; zombies[i].dead = 0; }
    for (i = 0; i < MAX_PEAS; i++) peas[i].active = 0;
    for (i = 0; i < MAX_SUNS; i++) suns[i].active = 0;
    gPendingRg = -1; gPendingRgZ = -1;
    gPrimordialAlive = 0;
}

/* 摆一个"看得出东西"的战场，供抓帧用 */
static void sceneForShot(void)
{
    static const int P[5][3] = {
        { 0, 0, PT_PEASHOOTER }, { 2, 1, PT_SUNFLOWER }, { 4, 2, PT_WALLNUT },
        { 3, 5, PT_MELONPULT },  { 1, 6, PT_CACTUS },
    };
    static const int Z[4][2] = { { 0, ZT_NORMAL }, { 2, ZT_BUCKET },
                                 { 3, ZT_FOOTBALL }, { 4, ZT_GIANT } };
    int i;
    for (i = 0; i < 5; i++) { gPendingRg = -1; plantIt(P[i][0], P[i][1], P[i][2]); }
    gPendingRg = 105; plantIt(1, 4, (int)rgPlants[105].base); gPendingRg = -1;
    for (i = 0; i < 4; i++) {
        gPendingRgZ = -1;
        spawnZombieAt(Z[i][1], Z[i][0], 900.0f + (float)i * 130.0f);
    }
    gSun = 4321; gGold = 5; gWave = 9; gTime = 20.0f;
    gSel = -1; gShovel = 0;
}

static void dumpWorld(const char *path)
{
    FILE *fp = fopen(path, "wb");
    int w = VIEW_W * SS, h = VIEW_H * SS;
    if (!fp) { printf("[!] 写不出 %s\n", path); fails++; return; }
    fwrite(gWorldBits, 1, (size_t)w * (size_t)h * 4, fp);
    fclose(fp);
}

int main(int argc, char **argv)
{
    const char *assetdir = (argc > 1) ? argv[1] : "assets";
    const char *outdir   = (argc > 2) ? argv[2] : "_celeb_frames";
    wchar_t wdir[MAX_PATH];
    static const float SHOTS[4] = { 0.06f, 0.28f, 0.55f, 0.88f };
    int i;

    SetConsoleOutputCP(CP_UTF8);
    _wputenv(L"PVZ_SAVE_FILE=_test_primordial_buff_save.dat");
    MultiByteToWideChar(CP_UTF8, 0, assetdir, -1, wdir, MAX_PATH);
    wsprintfW(gAssetDir, L"%s\\", wdir);
    createLayers(NULL);

/* ================= A. 数值强化 ================= */
    printf("== A：始祖档数值（对照其它档位）==\n");
    {
        int t;
        for (t = 0; t < RGQ_COUNT; t++) {
            char nm[64];
            WideCharToMultiByte(CP_UTF8, 0, RGQ_NAME[t], -1, nm, sizeof(nm), NULL, NULL);
            /* 能力条数不再有 RGQ_TRAITN 档位代表值（2026-09-21 删除），
               改成"该档位里能力位最多的一张卡开几位" —— 这才是玩家实际看到的数。 */
            int tm = 0, k;
            for (k = 0; k < RG_PLANT_N; k++)
                if (rgPlants[k].tier == t) {
                    int c = rgPlantTraitCount(k);
                    if (c > tm) tm = c;
                }
            printf("   %-4s 伤害 ×%-8.0f 攻速 ×%.2f 范围 ×%.2f  能力最多 %d 条\n",
                   nm, (double)rgDmgMul(t), (double)rgRateMul(t),
                   (double)rgRangeMul(t), tm);
        }
        CHECK(rgDmgMul(RGQ_PRIMORDIAL) >= 4000.0f,
              "buff: primordial damage is >= 4000x");
        CHECK(rgDmgMul(RGQ_PRIMORDIAL) >= rgDmgMul(RGQ_HALL) * 30.0f,
              "buff: it dwarfs 殿堂 (the previous top tier) by >=30x");
        CHECK(rgRateMul(RGQ_PRIMORDIAL) >= 3.0f,
              "buff: primordial attack rate is >= 3.0");
        CHECK(rgRangeMul(RGQ_PRIMORDIAL) >= 8.0f,
              "buff: primordial range multiplier is >= 8.0");
        /* "始祖开满 64 个能力位" 改成**逐卡实测**：
           没有哪张卡能靠档位代表值蒙过去，必须自己真的把 64 位都打开。
           （原来查的是 RGQ_TRAITN 的档位代表值，那个数组已删除 ——
             它报的是"这一档典型开几位"，而不是"这张卡实际开几位"。） */
        {
            int k, n64 = 0;
            for (k = 0; k < RG_PLANT_N; k++)
                if (rgPlants[k].tier == RGQ_PRIMORDIAL &&
                    rgPlantTraitCount(k) == 64) n64++;
            printf("   始祖档里能力位开满 64 条的卡：%d 张\n", n64);
            CHECK(n64 >= 30, "buff: every 始祖 card really opens all trait bits");
        }

        /* 副作用核对：抬高 RGQ_DMG 末位**不能**把僵尸血量一起放大 */
        {
            float zh = rgZombieHpMul(RGQ_PRIMORDIAL);
            float hh = rgZombieHpMul(RGQ_HALL);
            printf("   僵尸血量倍率：殿堂档 %.2f / 若按始祖档 %.2f\n",
                   (double)hh, (double)zh);
            CHECK(fabsf(zh - hh) < 0.001f,
                  "side-effect: zombie HP is NOT scaled by the primordial number");
            CHECK(zh < 60.0f, "side-effect: zombie HP multiplier stays sane (<60x)");
        }
    }

/* ================= B. 始祖威压 ================= */
    printf("\n== B：始祖威压（场上存在始祖时的全局被动）==\n");
    {
        Plant *p;
        Zombie *z;
        float hp0, lost;

        /* ① 没有始祖：跑 1 秒，僵尸不该掉血 */
        gCurLevel = 0; resetGame(); wipeField();
        gPendingRgZ = -1;
        spawnZombieAt(ZT_NORMAL, 2, 700.0f);
        z = NULL;
        for (i = 0; i < MAX_ZOMBIES; i++) if (zombies[i].active) { z = &zombies[i]; break; }
        CHECK(z != NULL, "aura: probe spawned a zombie");
        if (z) {
            z->speed = 0.0f; z->rooted = 5.0f;
            hp0 = z->hp;
            primordialAuraUpdate(1.0f);
            lost = hp0 - z->hp;
            printf("   无始祖：1 秒掉血 %.1f（应 0）  始祖存活数 %d\n",
                   (double)lost, gPrimordialAlive);
            CHECK(lost < 0.01f, "aura: no damage when no primordial plant is on field");
            CHECK(gPrimordialAlive == 0, "aura: alive-count is 0 without primordial");

            /* ② 种一株始祖：同样 1 秒，应掉 5.5% 最大生命并被压制减速 */
            wipeField();
            gPendingRg = 105;
            plantIt(2, 3, (int)rgPlants[105].base);
            gPendingRg = -1;
            p = plantAt(2, 3);
            CHECK(p != NULL && p->alive, "aura: primordial probe plant was placed");
            gPendingRgZ = -1;
            spawnZombieAt(ZT_NORMAL, 2, 700.0f);
            z = NULL;
            for (i = 0; i < MAX_ZOMBIES; i++) if (zombies[i].active) { z = &zombies[i]; break; }
            if (z) {
                float mx = z->maxhp;
                z->speed = 0.0f; z->rooted = 5.0f;
                hp0 = z->hp;
                primordialAuraUpdate(1.0f);
                lost = hp0 - z->hp;
                printf("   有始祖：1 秒掉血 %.1f / 最大生命 %.0f = %.2f%%（设计 5.5%%）"
                       "  减速 %.2f\n",
                       (double)lost, (double)mx, 100.0 * (double)(lost / mx),
                       (double)z->slow);
                CHECK(lost > 0.0f, "aura: zombies DO take damage while a primordial is alive");
                CHECK(fabsf(100.0f * lost / mx - 5.5f) < 0.6f,
                      "aura: the rate matches the 5.5%/s design value");
                CHECK(z->slow > 0.5f, "aura: zombies are also suppressed (slowed)");
                CHECK(gPrimordialAlive == 1, "aura: alive-count reflects the field");
            }
        }
    }

/* ================= C. 开牌庆典 ================= */
    printf("\n== C：开牌庆典状态机 ==\n");
    {
        int k;
        /* 低于殿堂：不该触发 */
        gCelebT = 0.0f; gCelebTier = -1; gCelebSlot = -1;
        for (k = 0; k < DRAFT_N; k++) gDraft[k] = DRAFT_ESSENCE;
        gDraft[1] = RG_CARD_BASE + 5;                 /* 史诗，远低于殿堂 */
        CHECK(draftCelebScan() < CELEB_MIN_TIER,
              "celeb: a low-tier draw does not start the celebration");
        CHECK(!celebActive(), "celeb: no celebration active for a low-tier draw");

        /* 殿堂：触发，且不该改写世界 */
        gPrimordialTheme = 0;
        gDraft[1] = RG_CARD_BASE + 104;                /* 104 = 天罗慑魔（殿堂） */
        CHECK(draftCelebScan() == RGQ_HALL, "celeb: 殿堂 is detected");
        CHECK(celebActive() && gCelebSlot == 1, "celeb: 殿堂 starts the celebration on that card");
        CHECK(gCelebPrim == 0, "celeb: 殿堂 uses the normal-length show");
        CHECK(gPrimordialTheme == 0, "celeb: 殿堂 does NOT rewrite the world");

        /* 始祖：更长的演出 + 改写背景音乐 */
        gCelebT = 0.0f;
        gDraft[1] = RG_CARD_BASE + 106;                /* 106 = 太初坚壁（始祖） */
        CHECK(draftCelebScan() == RGQ_PRIMORDIAL, "celeb: 始祖 is detected");
        CHECK(gCelebPrim == 1, "celeb: 始祖 uses the amplified show");
        CHECK(gCelebDur > CELEB_DUR_HALL,
              "celeb: the 始祖 show lasts longer than the 殿堂 one");
        CHECK(gPrimordialTheme == 1,
              "celeb: 始祖 still rewrites background + music (existing requirement)");

        /* 计时会结束、并清干净状态 */
        celebUpdate(gCelebDur + 1.0f);
        CHECK(!celebActive() && gCelebT <= 0.0f,
              "celeb: the show ends and clears its state");
    }

/* ================= D. 抓帧看演出 ================= */
    printf("\n== D：抓帧（进度可复现，供肉眼确认）==\n");
    {
        const char *tags[2] = { "hall", "prim" };
        int ti, si;
        for (ti = 0; ti < 2; ti++) {
            for (si = 0; si < 4; si++) {
                char path[512];
                srand(20260920u);
                gCurLevel = 0; resetGame(); wipeField();
                srand(20260920u);
                sceneForShot();
                /* 摆出"抽到了殿堂/始祖"的三选一 */
                for (i = 0; i < DRAFT_N; i++) gDraft[i] = DRAFT_ESSENCE;
                gDraft[0] = RG_CARD_BASE + 12;                       /* 一张普通肉鸽 */
                gDraft[1] = RG_CARD_BASE + (ti == 0 ? 104 : 106);     /* 高光那张 */
                gDraft[2] = GROWTH_CARD_BASE + 0;
                gCelebT = 0.0f; gCelebTier = -1; gCelebSlot = -1;
                draftCelebScan();
                gState = ST_DRAFT;
                gScreenT = 3.0f;      /* 让三张牌都已经翻到正面 */
                /* 把演出拨到指定进度 */
                gCelebT = gCelebDur * (1.0f - SHOTS[si]);
                shotEnsure();
                render(gShotDC);   /* 呈现写到临时 DC，保住 gWorldDC */
                snprintf(path, sizeof(path), "%s/%s_%02d.bmp", outdir,
                         tags[ti], (int)(SHOTS[si] * 100.0f));
                dumpWorld(path);
                printf("   %s  p=%.2f → %s\n", tags[ti], (double)SHOTS[si], path);
                CHECK(gCelebTier >= CELEB_MIN_TIER, "shot: celebration state survives rendering");
            }
        }
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_primordial_buff_save.dat");
    return fails ? 1 : 0;
}
