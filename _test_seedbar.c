/* 无窗口：验证顶栏卡槽的「鼠标点击 / 键盘 / 悬停」槽位映射（槽位 -> 编组植物）。
   复现的 bug：编组 4 株 + 奖励 5 株（总数 9 > 8）时，卡片按 72px 收窄绘制，
   点击却按 86px 换算槽位 -> 点第 6 张实际选成第 7 张，越靠右偏得越多。
   同时验证：数字键选的是卡槽而不是植物类型，且 'A'/'M' 等键不再被吞。 */
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int gFails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); gFails++; } \
    else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void setupBar(int loadoutN, int bonusN)
{
    int i;
    memset(&gSave, 0, sizeof(gSave));
    relicsRecalc();                      /* 倍率归一：gCostMul=1 等 */
    memset(cardCD, 0, sizeof(cardCD));
    gSun = 99999;
    gSel = -1; gShovel = 0;
    gState = ST_PLAY;
    gLoadoutN = loadoutN;
    for (i = 0; i < loadoutN; i++) gLoadout[i] = 1 + i * 2;   /* 1,3,5,7 故意不连续 */
    gBonusN = bonusN;
    for (i = 0; i < bonusN; i++) gBonusPlant[i] = 11 + i;     /* 11.. */
}

/* 点在第 slot 张卡的绘制中心（用绘制同款间距反推坐标） */
static void clickSlot(int slot)
{
    float cgap = cardBarGap();
    float cw = cgap - 4.0f;                       /* 与绘制处一致 */
    onClick((int)(144.0f + slot * cgap + (cw - 4.0f) * 0.5f), 50);
}

int main(void)
{
    _wputenv(L"PVZ_SAVE_FILE=_test_seedbar_save.dat");   /* 绝不碰玩家真实存档 */

    printf("== 用例 1：4 张卡（无奖励植物），间距 86 ==\n");
    setupBar(4, 0);
    clickSlot(0);
    CHECK(gSel == gLoadout[0], "点第 1 张卡 -> 选中 %d（期望 %d）", gSel, gLoadout[0]);
    clickSlot(3);
    CHECK(gSel == gLoadout[3], "点第 4 张卡 -> 选中 %d（期望 %d）", gSel, gLoadout[3]);
    clickSlot(4);
    CHECK(gShovel == 1, "点第 5 个位置（铲子位）-> 铲子开（gShovel=%d）", gShovel);
    clickSlot(4);
    CHECK(gShovel == 0, "再点铲子位 -> 铲子关（gShovel=%d）", gShovel);

    printf("\n== 用例 2：4 编组 + 5 奖励（总数 9，卡片收窄到 72px）—— 本次 bug 场景 ==\n");
    setupBar(4, 5);
    clickSlot(8);
    CHECK(gSel == gBonusPlant[4], "点第 9 张卡 -> 选中 %d（期望奖励植物 %d）", gSel, gBonusPlant[4]);
    clickSlot(4);
    CHECK(gSel == gBonusPlant[0], "点第 5 张卡 -> 选中 %d（期望奖励植物 %d）", gSel, gBonusPlant[0]);
    clickSlot(0);
    CHECK(gSel == gLoadout[0], "点第 1 张卡 -> 选中 %d（期望 %d）", gSel, gLoadout[0]);
    clickSlot(9);
    CHECK(gShovel == 1, "点第 10 个位置（铲子位）-> 铲子开（gShovel=%d）", gShovel);
    gShovel = 0;

    printf("\n== 用例 3：悬停提示（富提示 gTipPlant）跟着槽位映射走 ==\n");
    setupBar(4, 5);
    mMouseY = 50;
    mMouseX = (int)(144.0f + 8 * cardBarGap() + (cardBarGap() - 8.0f) * 0.5f);
    hoverUpdate();
    CHECK(gTipShow && gTipPlant == gBonusPlant[4], "悬停第 9 张卡 -> 提示植物 %d（期望 %d）", gTipPlant, gBonusPlant[4]);

    printf("\n== 用例 4：数字键 = 卡槽位；字母键不被吞 ==\n");
    setupBar(4, 5);
    WndProc(NULL, WM_KEYDOWN, '5', 0);
    CHECK(gSel == gBonusPlant[0], "按 5 -> 选中槽位 4 的奖励植物 %d（期望 %d）", gSel, gBonusPlant[0]);
    WndProc(NULL, WM_KEYDOWN, '9', 0);
    CHECK(gSel == gBonusPlant[4], "按 9 -> 选中槽位 8 的奖励植物 %d（期望 %d）", gSel, gBonusPlant[4]);
    { int sel = gSel, as = gAutoSun;
      WndProc(NULL, WM_KEYDOWN, 'A', 0);
      CHECK(gSel == sel && gAutoSun == !as, "按 A -> 只切自动拾取，不被当成选卡（gSel=%d）", gSel); }
    { int sel = gSel;
      WndProc(NULL, WM_KEYDOWN, 'M', 0);
      CHECK(gSel == sel, "按 M -> 切模式/不动选卡（gSel=%d）", gSel); }

    printf("\n== 用例 5：阳光不够时点卡 -> 不选中 ==\n");
    setupBar(4, 0);
    gSun = 0;
    clickSlot(0);
    CHECK(gSel == -1, "阳光 0 点第 1 张 -> gSel=%d（期望 -1）", gSel);

    printf("\n%s（%d 个失败）\n", gFails ? "FAILED" : "ALL PASS", gFails);
    return gFails ? 1 : 0;
}
