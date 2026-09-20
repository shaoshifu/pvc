/* 遗物表 ↔ 枚举对齐自检。
   ------------------------------------------------------------------------
   为什么要这个测试：relicDefs[R_COUNT] 是"枚举索引 + 按位置对齐"的奖励表。
   2026-09-20 复查发现它的行顺序和 enum 顺序不一致（enum 把 R_BANKER /
   R_NECROMANCER 放在 9/10，表里却把「银行家」「摄魂者」放在 45/46），
   于是中间约 38 个遗物**卡名/描述与真正生效的不是同一个** ——
   玩家点「磨刀石」实际拿到的是金气系统。
   修复办法是把表改成指定初始化 `[R_X] = {...}`，本测试守住这个不变量。

   判据两条：
     ① 锚点断言：一批语义明确的遗物，其卡名必须与"效果所属枚举位"对得上；
     ② 全量打印 77 项，人工一眼能看出有没有串位。

   编译：
     gcc -O2 -o _test_relic_align.exe _test_relic_align.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
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

/* 效果 → 应当显示的卡名。左侧来自 relicsRecalc() 里 HAS() 的实际效果 */
static const struct { int id; const char *want; } ANCHOR[] = {
    { R_START_SUN,      "储蓄罐"   },   /* gStartSunAdd += 50          */
    { R_INTEREST,       "利滚利"   },   /* gInterest = 8               */
    { R_BANKER,         "银行家"   },   /* gBanker = 1（金气）          */
    { R_NECROMANCER,    "摄魂者"   },   /* gNecromancer = 1（幽能）     */
    { R_PEA_DMG,        "磨刀石"   },   /* gDmgMul *= 1.12f            */
    { R_PEA_DMG_BIG,    "精钢弹头" },   /* gDmgMul *= 1.25f            */
    { R_RATE,           "疾风"     },   /* gRateMul *= 0.88f           */
    { R_THORNS,         "荆棘"     },   /* gThorns = 22                */
    { R_MEDIC,          "前线医疗" },   /* gMedic = 1                  */
    { R_IRON_WALL,      "铁壁"     },   /* gWallnutHpBoost = 120       */
    { R_SLOW_LONG,      "霜冻"     },   /* gSlowMul *= 1.50f           */
    { R_MINIMAL,        "极简主义" },   /* gCostMul *= 0.65f 冷却+60%  */
    { R_SUN_GAMBLE,     "阳光豪赌" },   /* 开局 -120，产出 ×2          */
    { R_BLOOD_PACT,     "血祭"     },   /* 费用 -45%，种下损血         */
    { R_ALL_IN,         "孤注一掷" },   /* 僵尸速度 +15%，击杀阳光 ×3  */
    { R_LOADOUT_PLUS,   "随机赠卡" },
    { R_FIRST_STRIKE,   "先手"     },
    { R_LAST_STAND,     "背水"     },
};

int main(void)
{
    int i, j;
    char buf[128];
    SetConsoleOutputCP(CP_UTF8);
    _wputenv(L"PVZ_SAVE_FILE=_test_relic_align_save.dat");

    printf("== 锚点：效果所属的枚举位 → 应当显示的卡名 ==\n");
    for (i = 0; i < (int)(sizeof(ANCHOR) / sizeof(ANCHOR[0])); i++) {
        buf[0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, relicDefs[ANCHOR[i].id].name, -1, buf, sizeof(buf), 0, 0);
        printf("   %-18s 位%2d →「%s」 期望「%s」  %s\n", "", ANCHOR[i].id, buf, ANCHOR[i].want,
               strcmp(buf, ANCHOR[i].want) == 0 ? "✔" : "★ 错位");
        CHECK(strcmp(buf, ANCHOR[i].want) == 0, ANCHOR[i].want);
    }

    /* 完整性：除 R_NONE 外，每个槽位都要有名字和描述，且名字不重复 */
    {
        int empty = 0, dup = 0;
        for (i = 1; i < R_COUNT; i++) {
            if (!relicDefs[i].name[0] || !relicDefs[i].desc[0]) { empty++; printf("   [!] 槽位 %d 缺名字或描述\n", i); }
            for (j = i + 1; j < R_COUNT; j++)
                if (relicDefs[i].name[0] && wcscmp(relicDefs[i].name, relicDefs[j].name) == 0) {
                    dup++; printf("   [!] 槽位 %d 与 %d 卡名重复\n", i, j);
                }
        }
        CHECK(empty == 0, "all relic slots have a name and a description");
        CHECK(dup == 0, "no duplicated relic names");
    }

    printf("\n== 全量（枚举位 → 卡名 → 描述）==\n");
    for (i = 0; i < R_COUNT; i++) {
        buf[0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, relicDefs[i].name, -1, buf, sizeof(buf), 0, 0);
        printf("%2d %-8s ", i, buf[0] ? buf : "·");
        pn(relicDefs[i].desc);
        printf("\n");
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_relic_align_save.dat");
    return fails ? 1 : 0;
}
