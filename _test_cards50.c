/* 新增 50 张卡牌（20 殿堂 + 30 始祖）的接入验证。
   ------------------------------------------------------------------------
   为什么单独测：扩卡牌表会同时牵动四样东西，任何一样错了都不报错——
     ① rgPlants 行数 / 档位分布（少一行就是越界读）
     ② aspect 字段（0 必须解释成"基准"，否则全场植物攻血归零）
     ③ 抽卡权重（株数从 108 涨到 158，不重算概率会整体跑偏）
     ④ 用户硬要求：始祖伤害 ≥ 殿堂 5 倍
   本测试逐条把它们钉住，并且**用运行时取值**而不是读源码常量。

   编译：
     gcc -O2 -o _test_cards50.exe _test_cards50.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
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

/* 各档株数（运行时统计，不写死） */
static int tierCount(int tier)
{
    int i, n = 0;
    for (i = 0; i < RG_PLANT_N; i++) if (rgPlants[i].tier == tier) n++;
    return n;
}

int main(void)
{
    int i;
    _wputenv(L"PVZ_SAVE_FILE=_test_cards50_save.dat");
    SetConsoleOutputCP(CP_UTF8);

/* ================= A. 表结构 ================= */
    printf("== A：卡牌表结构 ==\n");
    printf("  RG_PLANT_N = %d\n", RG_PLANT_N);
    CHECK(RG_PLANT_N == 158, "table: RG_PLANT_N is 158 (108 + 20 + 30)");

    {
        int hall = tierCount(RGQ_HALL), prim = tierCount(RGQ_PRIMORDIAL);
        printf("  殿堂 %d 张 / 始祖 %d 张\n", hall, prim);
        CHECK(hall == 25, "table: 25 殿堂 cards (5 legacy + 20 new)");
        CHECK(prim == 33, "table: 33 始祖 cards (3 legacy + 30 new)");
    }

    /* 新增卡：名字非空、基座合法、索引连续 */
    {
        int bad = 0;
        for (i = 108; i < 158; i++) {
            if (!rgPlants[i].name || !rgPlants[i].name[0]) { bad++; continue; }
            if (rgPlants[i].base >= PT_COUNT) bad++;
            if (i < 128) { if (rgPlants[i].tier != RGQ_HALL) bad++; }
            else         { if (rgPlants[i].tier != RGQ_PRIMORDIAL) bad++; }
        }
        CHECK(bad == 0, "table: all 50 new cards have a name, a legal base and the right tier");
    }

/* ================= B. 能力位条数 ================= */
    printf("\n== B：能力位条数（卡片上印的就是这个）==\n");
    {
        int hallBad = 0, primBad = 0, minH = 99, maxH = 0;
        for (i = 108; i < 128; i++) {
            int c = rgPlantTraitCount(i);
            if (c < 6) hallBad++;
            if (c < minH) minH = c;
            if (c > maxH) maxH = c;
        }
        for (i = 128; i < 158; i++) if (rgPlantTraitCount(i) != 64) primBad++;
        printf("  殿堂能力条数 %d~%d；始祖全部 64 条（不合规 %d 张）\n", minH, maxH, primBad);
        CHECK(hallBad == 0, "traits: every new 殿堂 card carries at least 6 ability bits");
        CHECK(primBad == 0, "traits: every new 始祖 card has all 64 ability bits open");
        /* 顺便核对"卡片显示值 = 真实 popcount"：拿一张已知的比对 */
        CHECK(rgPlantTraitCount(128) == 64 && rgPlantTraitCount(0) >= 1,
              "traits: rgPlantTraitCount works for both new and legacy cards");
        CHECK(rgPlantTraitCount(-1) == 0 && rgPlantTraitCount(RG_PLANT_N) == 0,
              "traits: out-of-range indices return 0 (no OOB read)");
    }

/* ================= C. aspect 字段 ================= */
    printf("\n== C：aspect（单卡强度系数，0 = 基准 100）==\n");
    {
        int primMin = 999, primMax = 0, legacyAspect = 0;
        for (i = 128; i < 158; i++) {
            int a = rgPlants[i].aspect;
            if (a < primMin) primMin = a;
            if (a > primMax) primMax = a;
        }
        for (i = 0; i < 108; i++) if (rgPlants[i].aspect != 0) legacyAspect++;
        printf("  始祖 aspect %d~%d；老卡片里非 0 的 aspect = %d 张\n",
               primMin, primMax, legacyAspect);
        CHECK(primMin >= 100 && primMax <= 200,
              "aspect: 始祖 aspects stay in a sane 100~200 range");
        CHECK(legacyAspect == 0,
              "aspect: legacy cards all leave it 0 (C zero-init, means baseline)");

        /* ★ 最关键的一条：0 必须被解释成"基准 ×1.0"，不是"零倍" */
        {
            Plant *p;
            float expect, got;
            int r, c, j;
            for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++)
                for (j = 0; j < 2; j++) {
                    Plant *q = (j == 0) ? &grid[r][c] : &grid2[r][c];
                    q->alive = 0;
                }
            gCurLevel = 0;
            resetGame();
            gPendingRg = 0;                       /* 索引 0：老卡，aspect = 0 */
            plantIt(2, 3, (int)rgPlants[0].base);
            gPendingRg = -1;
            p = plantAt(2, 3);
            CHECK(p != NULL && p->alive, "aspect: probe plant placed");
            if (p) {
                got = p->maxhp;
                /* 期望 = 基座血量 × 档位倍率 × fuseMul(1.0) × aspect(1.0) */
                expect = plantHpMax((int)rgPlants[0].base)
                       * rgDmgMul(rgPlants[0].tier) * 1.0f;
                printf("  老卡实际 maxhp %.0f / 期望 %.0f（比值 %.4f）\n",
                       (double)got, (double)expect, (double)(got / expect));
                CHECK(fabsf(got / expect - 1.0f) < 0.02f,
                      "aspect: aspect==0 behaves as x1.0 baseline (NOT zero)");
            }
        }

        /* aspect 真的被乘进去：拿始祖里 aspect 最低与最高两张比 */
        {
            int lo = 128, hi = 128;
            float rlo, rhi;
            for (i = 128; i < 158; i++) {
                if (rgPlants[i].aspect < rgPlants[lo].aspect) lo = i;
                if (rgPlants[i].aspect > rgPlants[hi].aspect) hi = i;
            }
            rlo = (float)rgPlants[lo].aspect / 100.0f;
            rhi = (float)rgPlants[hi].aspect / 100.0f;
            printf("  始祖内部梯度：%d(%dx aspect=%.2f) ~ %d(%dx aspect=%.2f)\n",
                   lo, (int)rgDmgMul(RGQ_PRIMORDIAL), (double)rlo,
                   hi, (int)rgDmgMul(RGQ_PRIMORDIAL), (double)rhi);
            CHECK(rhi > rlo, "aspect: there is a real strength gradient inside 始祖");
        }
    }

/* ================= D. 用户硬要求 ================= */
    printf("\n== D：始祖伤害 ≥ 殿堂 5 倍（用户硬要求）==\n");
    {
        /* ★ 要比的是**单卡实际伤害**的极值：最弱始祖 vs 最强殿堂，
           两边都必须把 aspect 算进来。
           上一版这里只拿 rgDmgMul(RGQ_HALL) 当基准（不含 aspect），
           于是"殿堂里存在高 aspect 的卡"这件事完全逃过检查 ——
           而当时殿堂 20 张的 aspect 一条都没填（数据源漏了），
           这个漏洞与漏填同时存在，测试照样全绿。
           现在两档都有 aspect 梯度，就必须逐卡取极值。 */
        float hMax = 0.0f, pMin = 1e18f, hMin = 1e18f;
        int hi = -1, pi = -1, nHall = 0, nPrim = 0, nHallAsp = 0, nPrimAsp = 0;
        for (i = 0; i < RG_PLANT_N; i++) {
            float m;
            if (rgPlants[i].tier == RGQ_HALL) {
                nHall++;
                if (rgPlants[i].aspect) nHallAsp++;
            } else if (rgPlants[i].tier == RGQ_PRIMORDIAL) {
                nPrim++;
                if (rgPlants[i].aspect) nPrimAsp++;
            } else continue;
            m = rgDmgMul(rgPlants[i].tier);
            if (rgPlants[i].aspect > 0) m *= (float)rgPlants[i].aspect / 100.0f;
            if (rgPlants[i].tier == RGQ_HALL) {
                if (m > hMax) { hMax = m; hi = i; }
                if (m < hMin) hMin = m;
            } else if (m < pMin) { pMin = m; pi = i; }
        }
        printf("  殿堂 %d 张（其中 %d 张带 aspect）：伤害 %.0f× ~ %.0f×\n",
               nHall, nHallAsp, (double)hMin, (double)hMax);
        printf("  始祖 %d 张（其中 %d 张带 aspect）：最低 %.0f×\n",
               nPrim, nPrimAsp, (double)pMin);
        printf("  → 最弱始祖 #%d / 最强殿堂 #%d = %.0f / %.0f = %.1f 倍\n",
               pi, hi, (double)pMin, (double)hMax, (double)(pMin / hMax));
        CHECK(pMin >= RG_PRIM_MIN_RATIO * hMax,
              "requirement: the WEAKEST 始祖 (with aspect) >= 5x the STRONGEST 殿堂");
        CHECK((int)rgDmgMul(RGQ_PRIMORDIAL) == RG_DMGX_PRIMORDIAL &&
              (int)rgDmgMul(RGQ_HALL) == RG_DMGX_HALL,
              "requirement: the integer mirror used by _Static_assert matches RGQ_DMG");
        CHECK(RG_DMGX_PRIMORDIAL >= RG_PRIM_MIN_RATIO * RG_DMGX_HALL,
              "requirement: compile-time mirror also satisfies >= 5x");
        /* 两档都必须有自己的内部强弱梯度 —— 否则"30/20 张卡"会退化成
           "同一个数字换 20 个名字"，那是没做平衡设计。 */
        CHECK(nHallAsp >= 15, "balance: 殿堂 has >= 15 cards with its own aspect");
        CHECK(nPrimAsp >= 25, "balance: 始祖 has >= 25 cards with its own aspect");
        CHECK(hMax > hMin, "balance: 殿堂 has an internal strength gradient");
    }

/* ================= E. 抽卡权重 ================= */
    printf("\n== E：抽卡权重（株数变了必须重算）==\n");
    {
        /* 设计目标：由"卡表张数 + 权重解算"共同决定，不是拍脑袋。
           卡表扩容（108→158）后必须重解 —— 张数一变，同样的权重会让
           各档占比整体跑偏。现值对应 RGQ_DRAFT_W = {30,410,328,310,278,461,87,22}。 */
        static const double TARGET[RGQ_COUNT] = { 0, 30.0, 26.0, 17.0, 11.0, 7.0, 6.6, 2.2 };
        int t, total = 0, worstT = -1;
        double worst = 0.0;
        /* ⚠️ 只统计**能上牌桌**的档位：rgOpenDraft 的候选池已经用
           RG_DRAFT_MIN_TIER 过滤掉普通档，所以普通档的权重不参与归一化。
           把索引 0 也算进来的话，总权重会多出 14×30=420，不再是 2 的幂 ——
           会得出"总权重不对"的错误结论（但真正该查的是别的）。 */
        for (t = RG_DRAFT_MIN_TIER; t < RGQ_COUNT; t++)
            total += tierCount(t) * RGQ_DRAFT_W[t];
        printf("  总权重 = %d\n", total);
        CHECK(total == 32768,
              "weights: sum(count x weight) == 32768 == 2^15 (unbiased rand()%tot)");
        CHECK(total > 0 && (32768 % total) == 0,
              "weights: total divides 32768 (no modulo bias)");
        /* ★ 硬约束：越稀有出现率越低。
           这条曾经被违反过：只按目标概率取整时，殿堂(25 张)总概率会反超
           传奇(5 张) —— 玩家会看到"殿堂比传奇还常见"。
           光看各档"偏差在容差内"是发现不了的，必须单独钉住序关系。 */
        {
            int ok = 1, t2;
            for (t2 = RG_DRAFT_MIN_TIER + 1; t2 < RGQ_COUNT; t2++) {
                double pPrev = 100.0 * tierCount(t2 - 1) * RGQ_DRAFT_W[t2 - 1] / (double)total;
                double pCur  = 100.0 * tierCount(t2) * RGQ_DRAFT_W[t2] / (double)total;
                if (pCur >= pPrev) ok = 0;
            }
            printf("    档位总概率严格降序（越稀有越低）：%s\n", ok ? "PASS" : "FAIL");
            CHECK(ok, "weights: tier probability strictly descends (rarer == less frequent)");
        }

        for (t = RG_DRAFT_MIN_TIER; t < RGQ_COUNT; t++) {
            double p = 100.0 * tierCount(t) * RGQ_DRAFT_W[t] / (double)total;
            double dev = p - TARGET[t];
            printf("    ");
            pn(RGQ_NAME[t]);
            printf(" %2d 张 × 权重 %3d → %6.2f%%（目标 %.1f%%，偏差 %+.2f）\n",
                   tierCount(t), RGQ_DRAFT_W[t], p, TARGET[t], dev);
            if (dev < 0) dev = -dev;
            if (dev > worst) { worst = dev; worstT = t; }
        }
        /* 允许 ±0.5pp：目标是设计值，实测来自 32768 的整数分配 */
        CHECK(worst <= 0.5,
              "weights: every tier lands within +/-0.5pp of its design target");
        (void)worstT;
    }

/* ================= F. 全部新卡可上牌桌 ================= */
    printf("\n== F：新卡都能进三选一候选池 ==\n");
    {
        int bad = 0;
        for (i = 108; i < 158; i++) if (!rgDraftEligible(i)) bad++;
        CHECK(bad == 0, "draft: all 50 new cards are draft-eligible");
        CHECK(rgDraftEligible(0) == 0,
              "draft: 普通 tier is still filtered out (14 cards stay off the table)");
    }

/* ================= G. 抽卡实跑：能真的抽到新卡 ================= */
    printf("\n== G：连开 4000 次三选一，看新卡是否真的出现 ==\n");
    {
        int hallSeen = 0, primSeen = 0, k, n = 0;
        int hallIdx[20], primIdx[30];
        for (k = 0; k < 20; k++) hallIdx[k] = 0;
        for (k = 0; k < 30; k++) primIdx[k] = 0;
        srand(20260921u);
        gCurLevel = 0;
        resetGame();
        gRgN = 0;                                /* 清空本局已拥有，保证池子最大 */
        memset(gRgCard, 0, sizeof(gRgCard));
        for (k = 0; k < 4000; k++) {
            int d;
            rgOpenDraft(3);                      /* waveIdx=3 → 三张里两张是植物 */
            for (d = 0; d < DRAFT_N; d++) {
                int v = gDraft[d];
                if (!DRAFT_IS_RG(v)) continue;
                v -= RG_CARD_BASE;
                if (v >= 108 && v < 158) {
                    if (v < 128) { hallSeen++; hallIdx[v - 108]++; }
                    else         { primSeen++; primIdx[v - 128]++; }
                }
            }
            n++;
            /* 每轮把已拥有的清掉，模拟"每局都是新池子" —— 否则几次之后
               新卡全被 rgOwned 过滤掉，后面几千次什么都抽不到。 */
            gRgN = 0;
            memset(gRgCard, 0, sizeof(gRgCard));
            gState = ST_PLAY;
        }
        printf("  %d 次三选一：新殿堂卡出现 %d 次，新始祖卡出现 %d 次\n", n, hallSeen, primSeen);
        CHECK(hallSeen > 0, "draft: new 殿堂 cards really appear in the draft");
        CHECK(primSeen > 0, "draft: new 始祖 cards really appear in the draft");
        {
            int neverH = 0, neverP = 0;
            for (k = 0; k < 20; k++) if (!hallIdx[k]) neverH++;
            for (k = 0; k < 30; k++) if (!primIdx[k]) neverP++;
            printf("  从未出现的新卡：殿堂 %d 张 / 始祖 %d 张（4000 次采样下属正常波动）\n",
                   neverH, neverP);
            CHECK(neverH <= 8 && neverP <= 20,
                  "draft: the spread across individual new cards is not pathologically skewed");
        }
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_cards50_save.dat");
    return fails ? 1 : 0;
}
