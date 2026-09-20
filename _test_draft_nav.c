/* 无窗口回归测试：三选一、成长卡、Esc 暂停菜单和难度/价格曲线。 */
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int fails;

#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

static void clearDraftState(void)
{
    int i;
    memset(gRelic, 0, sizeof(gRelic));
    gRgN = 0;
    gRgPicked = 0;
    for (i = 0; i < RG_MAX_SLOTS; i++) {
        gRgCard[i] = -1;
        gRgFree[i] = 0;
    }
    gSun = 0;                 /* 故意没有阳光：奖励仍必须可选 */
    gPendingWin = 0;
    gBuyFailed = 0;
    memset(gGrowthStack, 0, sizeof(gGrowthStack));
    relicsRecalc();
    gState = ST_PLAY;
}

int main(void)
{
    float x, y;

    /* ⚠️ 必须隔离存档：本测试会走 rgOpenDraft / 三选一选择路径，
       那些分支里有 saveFlush()。原来漏了这行，跑一次就按新档覆盖玩家真存档
       （真档被清成 "21 株 / 0 星 / 3 金币" 就是这个原因）。
       规则：任何会进游戏逻辑的测试程序，第一件事就是设 PVZ_SAVE_FILE。 */
    _wputenv(L"PVZ_SAVE_FILE=_test_draft_nav_save.dat");

    clearDraftState();
    srand(7);
    rgOpenDraft(1);           /* 第二波（索引 1） */
    CHECK(gState == ST_DRAFT, "second-wave draft opens");
    CHECK(gDraftFree == 1, "second-wave choices are free");
    {
        int i, plants = 0, growth = 0;
        for (i = 0; i < DRAFT_N; i++) {
            if (DRAFT_IS_RG(gDraft[i])) plants++;
            if (DRAFT_IS_GROWTH(gDraft[i])) growth++;
        }
        CHECK(plants >= 2 && growth == 1,
              "waves 1-10 guarantee at least two plant choices");
    }

    /* 把第一格固定为一张高价植物卡，确保测到的是曾经被阳光拦住的路径。 */
    gDraft[0] = RG_CARD_BASE;
    draftCardRect(0, &x, &y);
    onClick((int)(x + CARD_W * 0.5f), (int)(y + CARD_H * 0.5f));
    CHECK(gState == ST_PLAY, "first box accepts click and returns to battle");
    CHECK(gRgN == 1 && gRgCard[0] == 0, "first-box plant is granted");
    CHECK(gSun == 0 && gBuyFailed == 0, "free reward needs no sunlight");

    /* 奖励卡首次落地免费，之后必须支付 50~500 区间内的真实价格。 */
    gSel = RG_CARD_BASE;
    onClick(LAWN_X + 10, LAWN_Y + 10);
    CHECK(plantAt(0, 0) != NULL && gRgFree[0] == 0 && gSun == 0,
          "reward plant gets exactly one free placement");
    rgCardCD[0] = 0.0f;
    gSun = rgPlantCost(0) - 25;
    gSel = RG_CARD_BASE;
    onClick(LAWN_X + CELL_W + 10, LAWN_Y + 10);
    CHECK(plantAt(0, 1) == NULL, "repeat reward plant respects sunlight cost");
    gSun = rgPlantCost(0);
    gSel = RG_CARD_BASE;
    onClick(LAWN_X + CELL_W + 10, LAWN_Y + 10);
    CHECK(plantAt(0, 1) != NULL && gSun == 0,
          "repeat reward plant deducts its displayed cost");

    clearDraftState();
    srand(17);
    rgOpenDraft(10);          /* 第十一波（索引 10） */
    {
        int i, plants = 0, growth = 0;
        for (i = 0; i < DRAFT_N; i++) {
            if (DRAFT_IS_RG(gDraft[i])) plants++;
            if (DRAFT_IS_GROWTH(gDraft[i])) growth++;
        }
        CHECK(plants == 1 && growth == 2,
              "wave 11 onward offers one plant and two growth cards");
    }

    clearDraftState();
    {
        int i, plants = 0;
        gRgN = RG_MAX_SLOTS;                 /* 卡槽满也不能改变前十波配比 */
        for (i = 0; i < gRgN; i++) gRgCard[i] = i;
        rgOpenDraft(9);                      /* 第十波（索引 9） */
        for (i = 0; i < DRAFT_N; i++) if (DRAFT_IS_RG(gDraft[i])) plants++;
        CHECK(plants >= 2, "wave-10 plant guarantee survives a full card bar");
    }

    clearDraftState();
    rgOpenDraft(1);
    WndProc(NULL, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(gState == ST_PAUSE && gPauseFrom == ST_DRAFT, "Esc opens pause menu from draft");
    WndProc(NULL, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(gState == ST_DRAFT, "second Esc continues the draft");

    clearDraftState();
    WndProc(NULL, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(gState == ST_PAUSE && gPauseFrom == ST_PLAY, "Esc opens pause menu from battle");
    onClick((int)(PAUSE_BTN_X + PAUSE_BTN_W * 0.5f),
            (int)(PAUSE_CONT_Y + PAUSE_BTN_H * 0.5f));
    CHECK(gState == ST_PLAY, "continue button resumes battle");

    WndProc(NULL, WM_KEYDOWN, VK_ESCAPE, 0);
    onClick((int)(PAUSE_BTN_X + PAUSE_BTN_W * 0.5f),
            (int)(PAUSE_HOME_Y + PAUSE_BTN_H * 0.5f));
    CHECK(gState == ST_MENU, "home button returns to main menu");

    clearDraftState();
    applyRelic(GROWTH_CARD_BASE + GROW_DMG);
    applyRelic(GROWTH_CARD_BASE + GROW_DMG);
    CHECK(gGrowthStack[GROW_DMG] == 2 && gDmgMul > 1.19f,
          "growth cards stack and immediately change combat stats");

    {
        int i, lo = 9999, hi = 0;
        for (i = 0; i < PT_COUNT; i++) {
            int c = plantCost(i);
            if (c < lo) lo = c;
            if (c > hi) hi = c;
        }
        CHECK(lo == 50 && hi == 500, "plant prices cover the 50-500 range");
    }
    {
        /* ⚠️ 口径要排除始祖。始祖是**唯一不享受"每波免费"**的档位 ——
           它的售价 10000 是真要付的（见 drawDraftText 里 isPrim 的分支），
           而其余档位作为每波奖励都是免费的，价格只用于显示。
           原来这里一律断言 hi <= 500，加上 33 张始祖之后必然失败；
           但那不是"价格失控"，而是"始祖该有高价"这个设计本身。 */
        int i, lo = 9999, hi = 0, primLo = 99999, primHi = 0;
        for (i = 0; i < RG_PLANT_N; i++) {
            int c = rgPlantCost(i);
            if (rgPlants[i].tier == RGQ_PRIMORDIAL) {
                if (c < primLo) primLo = c;
                if (c > primHi) primHi = c;
            } else {
                if (c < lo) lo = c;
                if (c > hi) hi = c;
            }
        }
        CHECK(lo >= 50 && hi <= 500, "non-primordial plant prices stay inside 50-500");
        CHECK(primLo == 10000 && primHi == 10000,
              "every primordial is priced 10000 (the only tier that is really paid)");
    }

    gWave = 1;
    { float hp1 = zombieWaveHpMul(), sp1 = zombieWaveSpeedMul(), bite1 = zombieWaveBiteMul();
      gWave = 20;
      CHECK(zombieWaveHpMul() > hp1 * 5.0f, "late-wave zombie health grows strongly");
      CHECK(zombieWaveSpeedMul() > sp1 && zombieWaveBiteMul() > bite1,
            "late-wave speed and bite damage both grow"); }

    {
        unsigned long long forbidden = RZ_REVIVE3 | RZ_FORKDEATH | RZ_HEALBACK |
                                       RZ2_DODGE30 | RZ3_SHIELD | RZ3_DEFLECT |
                                       RZ3_SPLIT | RZ3_REGEN | RZ3_REBORN;
        CHECK((rgZombies[49].traits & forbidden) == 0,
              "ultimate zombie has no perpetual-survival trait bundle");
    }

    {
        Plant p;
        Zombie *z = &zombies[0];
        float before;
        memset(&p, 0, sizeof(p));
        memset(zombies, 0, sizeof(zombies));
        p.type = PT_VOIDDEVOURER; p.alive = 1; p.hp = 120.0f; p.maxhp = 350.0f;
        z->active = 1; z->type = ZT_GIANT; z->row = 2;
        z->x = cellCX(4) + 110.0f; z->y = cellBaseY(2);
        z->hp = z->maxhp = z->basehp = 10000.0f;
        before = z->hp;
        plantVoidDevourTick(&p, 0.1f, 2, 4, cellCX(4), cellBaseY(2), 1.0f);
        CHECK(z->hp < before && p.hp > 120.0f,
              "void devourer damages bosses and heals itself");
        z->type = ZT_NORMAL; z->hp = z->maxhp * 0.40f; z->dead = 0; p.timer = 0.0f;
        plantVoidDevourTick(&p, 0.1f, 2, 4, cellCX(4), cellBaseY(2), 1.0f);
        CHECK(!z->active, "void devourer consumes wounded non-bosses");
    }

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
