/* 无窗口回归测试：开局编组硬锁「每关只能选 1 株植物」。
   ------------------------------------------------------------------------
   覆盖四条可能突破限制的旁路：
     ① 遗物 R_LOADOUT_PLUS（原来会把上限顶到 3）；
     ② 大厅点击 loadoutToggle（原来 cap=1 时会退化成一页点不动）；
     ③ 存档旧数据（原来 loadout 里有多个 1 不会收敛）；
     ④ 局内"新植物自动加入编组"（原来会写出第 2 个 loadout=1）。
   编译：
     gcc -O2 -o _test_loadout1.exe _test_loadout1.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int fails;

#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

/* 统计编制里置 1 的普通植物数量 */
static int selCount(void)
{
    int i, n = 0;
    for (i = 0; i < PT_HERO_FLAME; i++) if (gSave.loadout[i]) n++;
    return n;
}

int main(void)
{
    int i;

    /* 绝不碰玩家真实存档：savePath() 支持 PVZ_SAVE_FILE 覆盖（与 _test_seedbar 同一套做法） */
    _wputenv(L"PVZ_SAVE_FILE=_test_loadout1_save.dat");
    memset(&gSave, 0, sizeof(gSave));
    for (i = 0; i < PT_HERO_FLAME; i++) gSave.plantOwned[i] = 1;   /* 全部解锁 */

    /* ---- ① 默认上限就是 1 ---- */
    memset(gRelic, 0, sizeof(gRelic));
    relicsRecalc();
    CHECK(gCardSlots == 1 && gCandidateSlots == 1,
          "default loadout caps are both 1");
    CHECK(loadoutCap() == 1, "loadoutCap() reports 1");
    CHECK(LOADOUT_SLOTS_MAX == 1, "LOADOUT_SLOTS_MAX is 1");

    /* ---- ② 遗物不能再把上限顶开 ---- */
    gRelic[R_LOADOUT_PLUS] = 1;
    relicsRecalc();
    CHECK(gCardSlots == 1 && gCandidateSlots == 1 && loadoutCap() == 1,
          "R_LOADOUT_PLUS no longer widens the cap");
    memset(gRelic, 0, sizeof(gRelic));
    relicsRecalc();

    /* ---- ③ 大厅点击 = 单选 / 顶替 ---- */
    memset(gSave.loadout, 0, sizeof(gSave.loadout));
    loadoutBuild();                 /* 兜底自动挑 1 株 */
    CHECK(gLoadoutN == 1, "loadoutBuild falls back to exactly one plant");

    memset(gSave.loadout, 0, sizeof(gSave.loadout));
    loadoutToggle(5);
    CHECK(selCount() == 1 && gSave.loadout[5] == 1, "toggle selects one plant");
    loadoutToggle(9);
    CHECK(selCount() == 1 && gSave.loadout[9] == 1 && !gSave.loadout[5],
          "toggling another plant replaces it (still exactly one)");
    loadoutToggle(9);
    CHECK(selCount() == 1 && gSave.loadout[9] == 1,
          "clicking the selected plant cannot leave the loadout empty");
    loadoutToggle(30);
    loadoutToggle(77);
    CHECK(selCount() == 1 && gSave.loadout[77] == 1,
          "repeated toggles never exceed one plant");
    loadoutBuild();
    CHECK(gLoadoutN == 1 && gLoadout[0] == 77,
          "battle card bar carries exactly the one selected plant");

    /* ---- ④ 未拥有的植物点不动 ---- */
    gSave.plantOwned[40] = 0;
    loadoutToggle(40);
    CHECK(selCount() == 1 && !gSave.loadout[40],
          "unowned plants cannot be selected");

    /* ---- ⑤ 脏存档收敛：多个 1 只保留索引最小的那个 ---- */
    memset(gSave.loadout, 0, sizeof(gSave.loadout));
    gSave.loadout[2] = 1; gSave.loadout[11] = 1; gSave.loadout[77] = 1;
    gSave.loadout[PT_HERO_FLAME + 3] = 1;      /* 神级区间脏值 */
    loadoutNormalize();
    CHECK(selCount() == 1 && gSave.loadout[2] == 1,
          "legacy save with several picks collapses to the lowest one");
    CHECK(gSave.loadout[PT_HERO_FLAME + 3] == 0,
          "hero-tier dirty entry is cleared");
    CHECK(gLoadoutN == 1 && gLoadout[0] == 2,
          "loadoutBuild after normalize yields one armed plant");

    /* ---- ⑥ loadoutEquipOnly：局内解锁的植物顶替成为唯一出战植物 ---- */
    loadoutEquipOnly(33);
    CHECK(selCount() == 1 && gSave.loadout[33] == 1 && gLoadoutN == 1 &&
          gLoadout[0] == 33,
          "loadoutEquipOnly replaces instead of appending");
    loadoutEquipOnly(0);            /* id 0 可能是占位，确保不崩 */
    loadoutEquipOnly(-1);
    loadoutEquipOnly(PT_COUNT + 5);
    CHECK(selCount() <= 1, "out-of-range ids never add a second pick");

    /* ---- ⑦ 遗物「随机赠卡」：给 1 张免费卡、不扣阳光、不重复给 ---- */
    memset(gRelic, 0, sizeof(gRelic));
    relicsRecalc();
    gRgN = 0; gRgPicked = 0;
    for (i = 0; i < RG_MAX_SLOTS; i++) { gRgCard[i] = -1; gRgFree[i] = 0; }
    gSun = 0;
    rgGrantStarterCard();
    CHECK(gRgN == 1 && gRgFree[0] == 1 && gSun == 0,
          "starter card grants one free rogue plant without spending sun");

    memset(gRelic, 0, sizeof(gRelic));
    relicsRecalc();
    gRgN = 0; gRgPicked = 0;
    for (i = 0; i < RG_MAX_SLOTS; i++) { gRgCard[i] = -1; gRgFree[i] = 0; }
    gSun = 500;
    applyRelic(R_LOADOUT_PLUS);
    CHECK(gRgN == 1, "picking R_LOADOUT_PLUS hands out a card immediately");
    applyRelic(R_LOADOUT_PLUS);
    CHECK(gRgN == 1, "picking the same relic again does not grant a second card");

    /* ---- ⑧ 真实开局路径 resetGame()：开局手里只有 1 张编组卡 ---- */
    gCurLevel = 0;
    memset(gSave.loadout, 0, sizeof(gSave.loadout));
    gSave.loadout[6] = 1;
    for (i = 0; i < PT_COUNT; i++) gSave.plantOwned[i] = 1;
    resetGame();
    CHECK(gLoadoutN == 1, "resetGame arms exactly one plant");
    CHECK(gLoadoutN + gBonusN + gRgN == 1,
          "battle card bar starts with a single card (plus shovel)");

    /* ---- ⑨ 真实 UI 路径：大厅点卡 → 点关卡 → 开局只有 1 张卡 ---- */
    for (i = 0; i < LV_COUNT; i++) gSave.lvUnlocked[i] = 1;
    memset(gSave.loadout, 0, sizeof(gSave.loadout));
    gSave.loadout[3] = 1;
    loadoutBuild();

    gState = ST_LOADOUT;
    gLoadoutPage = 0;
    {
        float x, y;
        loLayout(0, &x, &y);
        onClick((int)(x + LO_CARD_W * 0.5f), (int)(y + LO_CARD_H * 0.5f));
        CHECK(selCount() == 1 && gSave.loadout[0] == 1,
              "lobby click switches the single armed plant");
    }
    gCurLevel = 0;
    {
        float x, y;
        metaLayoutLevel(0, &x, &y);
        gState = ST_LEVELS;
        onClick((int)(x + LV_CARD_W * 0.5f), (int)(y + LV_CARD_H * 0.5f));
    }
    CHECK(gState == ST_PLAY, "clicking a level starts the battle");
    CHECK(gLoadoutN == 1 && gLoadout[0] == 0,
          "battle starts with exactly the plant chosen in the lobby");
    CHECK(gLoadoutN + gBonusN + gRgN == 1,
          "battle card bar holds exactly one card right after level start");

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_loadout1_save.dat");     /* 测试档用完即删 */
    return fails ? 1 : 0;
}
