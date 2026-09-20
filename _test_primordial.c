/* 始祖级（RGQ_PRIMORDIAL）专项测试：三尊属性 / 主题触发与持久 / 价格门。
   ------------------------------------------------------------------------
   为什么单独一个文件：这一批功能有三条**互相独立**的链路，
   崩任何一条都不会让别的报错，混在大测试里很容易"看着全绿其实漏了"：
     ① 档位数值（1000× 伤害、64 条能力、10000 售价不被 CLAMP 夹掉）
     ② 主题（出现即改写背景与音乐；此后不回退，直到下一个始祖出现）
     ③ 价格门（始祖是唯一不免费的档位，阳光不足时不能白拿）
   编译：
     gcc -O2 -o _test_primordial.exe _test_primordial.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
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

int main(void)
{
    int i;

    /* ⚠️ 隔离存档：applyRelic 的普通植物分支里有 saveFlush()。 */
    _wputenv(L"PVZ_SAVE_FILE=_test_primordial_save.dat");
    SetConsoleOutputCP(CP_UTF8);

/* ======================= ① 三尊的档位数值 ======================= */
    {
        static const int PB[3] = { PT_SPIKEWEED, PT_WALLNUT, PT_PEASHOOTER };
        int bad = 0;
        CHECK(RGQ_COUNT == 8 && RGQ_PRIMORDIAL == 7, "tier: 始祖 is the top tier (8th)");
        /* 卡表在 2026-09-21 从 108 扩到 158（+20 殿堂 +30 始祖）。
           ⚠️ 这里原本写死 108，扩容后必然失败 ——
              写死行数是对的（要钉住"表真的扩了"），但必须跟着一起改。 */
        CHECK(RG_PLANT_N == 158, "tier: plant table holds 158 entries");
        for (i = 0; i < 3; i++) {
            int idx = 105 + i;
            if (rgPlants[idx].tier != RGQ_PRIMORDIAL) bad++;
            if (rgPlants[idx].base != PB[i]) bad++;
            if (rgPlants[idx].traits != RG_ALL_TRAITS) bad++;
            if (rgPlants[idx].cost != 10000) bad++;            /* 字段本身没被截断 */
            if (rgPlantCost(idx) != 10000) bad++;              /* 也没被 CLAMP 夹掉 */
            /* 始祖伤害倍率在 2026-09-21 从 1000 提到 4000（用户要求"过强"）。 */
            if (rgDmgMul(rgPlants[idx].tier) != 4000.0f) bad++;/* 实际伤害倍率 */
        }
        CHECK(bad == 0, "tier: 地刺/坚果/豌豆炮, all-traits, 4000x damage, 10000 sun");
        printf("      三尊：");
        for (i = 0; i < 3; i++) { pn(rgPlants[105 + i].name); printf("  "); }
        /* ⚠️ RGQ_TRAITN（"该档位典型能力条数"）已在 2026-09-21 删除 ——
           卡片上现在直接对 traits 掩码做 popcount，报的是"这张卡实际开几位"。
           所以这里也要按**实际值**打印，不能再读那个档位代表值。 */
        printf("（伤害 %d×　能力 %d 条　售价 %d）\n",
               (int)rgDmgMul(RGQ_PRIMORDIAL),
               rgPlantTraitCount(105),
               rgPlantCost(105));
    }

/* ======================= ② 主题：出现即切换 ======================= */
    gPrimordialTheme = 0;
    gLawnVariant = 0;
    {
        int hit;
        /* 无始祖的普通三选一：不该切主题 */
        for (i = 0; i < DRAFT_N; i++) gDraft[i] = GROWTH_CARD_BASE + (i % GROW_COUNT);
        hit = draftScanPrimordial();
        CHECK(hit == 0 && gPrimordialTheme == 0,
              "theme: an ordinary draft never triggers the primordial theme");
        CHECK(levelLawnVariant(0) == levelDefs[0].lawnVariant,
              "theme: before it appears, levels keep their own background");

        /* 摆一张始祖卡 + 两张普通卡：必须切主题 */
        gDraft[0] = RG_CARD_BASE + 105;      /* 始源棘原 */
        gDraft[1] = RG_CARD_BASE + 100;
        gDraft[2] = GROWTH_CARD_BASE + 0;
        hit = draftScanPrimordial();
        CHECK(hit == 1 && gPrimordialTheme == 1,
              "theme: a primordial card in the draft switches the theme");
        CHECK(gLawnVariant == LV_VARIANT_PRIMORDIAL,
              "theme: the background switches to the primordial variant");
    }

/* ======================= ②b 主题：不回退（持久） ======================= */
    {
        int lv, stayed = 1;
        for (lv = 0; lv < LV_COUNT; lv++)
            if (levelLawnVariant(lv) != LV_VARIANT_PRIMORDIAL) stayed = 0;
        CHECK(stayed, "theme: every level now renders the primordial background");

        /* 换关（bgmForLevel）也不该把主题洗掉 */
        bgmForLevel(3);
        CHECK(gPrimordialTheme == 1 && levelLawnVariant(3) == LV_VARIANT_PRIMORDIAL,
              "theme: switching levels does not undo it");

        /* 之后再来一次【不含始祖】的三选一：仍然保持，不会自己恢复 */
        for (i = 0; i < DRAFT_N; i++) gDraft[i] = RG_CARD_BASE + 1 + i;   /* 稀有档 */
        CHECK(draftScanPrimordial() == 0 && gPrimordialTheme == 1,
              "theme: later ordinary drafts keep it switched on");

        /* 再出现一次 → 重新执行一遍（音乐重放、再闪屏），状态仍是开启 */
        gDraft[0] = RG_CARD_BASE + 107;
        CHECK(draftScanPrimordial() == 1 && gPrimordialTheme == 1,
              "theme: the next primordial re-applies the theme");
    }

/* ======================= ③ 价格门 ======================= */
    {
        int ok;
        gRgN = 0; gRgPicked = 0;
        memset(gRgFree, 0, sizeof(gRgFree));

        /* 阳光不足：不能白拿，也不能扣钱 */
        gSun = 9999;
        ok = rgGrant(105, 0);
        CHECK(ok == 0 && gRgN == 0 && gSun == 9999,
              "price: 9999 sun cannot buy a 10000 primordial card");

        /* 恰好够：扣光并拿到卡 */
        gSun = 10000;
        ok = rgGrant(105, 0);
        CHECK(ok == 1 && gRgN == 1 && gRgCard[0] == 105 && gSun == 0,
              "price: 10000 sun buys it and is deducted in full");

        /* 选卡路径：即便三选一处在"免费"状态，始祖照样收费 */
        gRgN = 0; gRgPicked = 0; gDraftFree = 1; gSun = 10000;
        applyRelic(RG_CARD_BASE + 106);
        CHECK(gRgN == 1 && gSun == 0,
              "price: the free-per-wave rule does NOT apply to 始祖");

        /* 同一条路径下，别的档位仍然免费 */
        gRgN = 0; gRgPicked = 0; gDraftFree = 1; gSun = 0;
        applyRelic(RG_CARD_BASE + 40);
        CHECK(gRgN == 1 && gSun == 0,
              "price: other tiers stay free as before");
        gDraftFree = 0;
    }

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_primordial_save.dat");
    return fails ? 1 : 0;
}
