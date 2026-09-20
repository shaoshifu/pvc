/* 三选一抽卡概率实测（无窗口）。
   ------------------------------------------------------------------------
   为什么必须实测而不是"算一下 RGQ_W 就行"：
     1. 候选池 = 全部 105 株**减去本局已拿到的**（rgOwned / RG_MAX_SLOTS=8），
        所以池子在局内会收缩，概率不是常数；
     2. 一次三选一里植物占几个卡位随波次变化：
        第 1~10 波 2 株、第 11 波起 1 株（另一/两格给成长卡）；
     3. 同一次里抽出的植物**互不重复**（抽掉即从候选池移除），
        所以第 2 株的条件概率与第 1 株不同。
   所以这里跑蒙特卡洛拿真值，同时用解析权重做交叉校验 —— 两道闸门。

   编译：
     gcc -O2 -o _test_draft_odds.exe _test_draft_odds.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

#include <time.h>

static const wchar_t *TIER_NAME[RGQ_COUNT] = {
    L"普通", L"稀有", L"史诗", L"传说", L"神级", L"传奇", L"殿堂", L"始祖" };

/* 把宽字符名字转 UTF-8 打出来（MinGW 控制台 %ls 会静默输出空串） */
static void pn(const wchar_t *w)
{
    char b[128];
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), NULL, NULL) <= 0) b[0] = 0;
    fputs(b, stdout);
}

/* 数一遍各档有几株。onlyEligible=1 时只数"够门槛、会上三选一牌"的那些 ——
   否则解析表会把已经不上牌的普通档也算进去，与实际抽卡对不上。 */
static void countTiers(int *out, int onlyEligible)
{
    int i;
    for (i = 0; i < RGQ_COUNT; i++) out[i] = 0;
    for (i = 0; i < RG_PLANT_N; i++) {
        if (onlyEligible && !rgDraftEligible(i)) continue;
        out[rgPlants[i].tier]++;
    }
}

int main(void)
{
    const int N1 = 200000;          /* 单次三选一的采样数 */
    const int N2 = 20000;           /* 整局模拟的局数 */
    int cnt[RGQ_COUNT];
    int i, t, k, wave;
    int seed;

    /* ⚠️ 必须隔离存档：applyRelic 的普通植物分支里有 saveFlush()。 */
    _wputenv(L"PVZ_SAVE_FILE=_test_draft_odds_save.dat");
    SetConsoleOutputCP(CP_UTF8);
    countTiers(cnt, 1);            /* 只数合格池，与三选一实际取牌一致 */
    seed = (int)time(NULL);
    srand(seed);

    {
        int allc[RGQ_COUNT], allN = 0, eligN = 0, t2;
        countTiers(allc, 0);
        for (t2 = 0; t2 < RGQ_COUNT; t2++) { allN += allc[t2]; eligN += cnt[t2]; }
        printf("== 三选一实际取牌池：%d 株 / 全表 %d 株被挡在门槛外 %d 株"
               "（RG_DRAFT_MIN_TIER=%d）==\n",
               eligN, allN, allN - eligN, (int)RG_DRAFT_MIN_TIER);
    }
    {
        long tot = 0;
        int poolN = 0;
        for (t = 0; t < RGQ_COUNT; t++) { tot += (long)cnt[t] * RGQ_DRAFT_W[t]; poolN += cnt[t]; }
        printf("  （合格池共 %d 株，总权重 %ld）\n", poolN, tot);
        printf("  %-4s %4s %6s %14s %14s\n", "档位", "株数", "权重", "单株占比(解析)", "整档权重占比");
        for (t = 0; t < RGQ_COUNT; t++) {
            pn(TIER_NAME[t]);
            printf("   %4d %6d %13.4f%% %13.4f%%\n",
                   cnt[t], RGQ_DRAFT_W[t], 100.0 * RGQ_DRAFT_W[t] / (double)tot,
                   100.0 * (double)cnt[t] * RGQ_DRAFT_W[t] / (double)tot);
        }
        printf("  总权重 = %ld\n", tot);
    }

    /* ---- ① 单次三选一（开局状态：本局什么都没拿）---- */
    {
        int head[RGQ_COUNT], slot[RGQ_COUNT], plantHit[RG_PLANT_N];
        long slotTotal = 0;
        const int waves[2] = { 0, 12 };       /* 0 -> 保底 2 株；12 -> 只给 1 株 */
        int w;

        for (w = 0; w < 2; w++) {
            wave = waves[w];
            memset(head, 0, sizeof(head));
            memset(slot, 0, sizeof(slot));
            memset(plantHit, 0, sizeof(plantHit));
            slotTotal = 0;
            for (i = 0; i < N1; i++) {
                int seen[RGQ_COUNT];
                memset(seen, 0, sizeof(seen));
                gRgN = 0;                     /* 每轮都从"本局零卡"开局 */
                rgOpenDraft(wave);
                for (k = 0; k < DRAFT_N; k++) {
                    int v = gDraft[k];
                    if (!DRAFT_IS_RG(v)) continue;          /* 成长卡不算 */
                    t = rgPlants[v - RG_CARD_BASE].tier;
                    slot[t]++; slotTotal++;
                    plantHit[v - RG_CARD_BASE]++;
                    seen[t] = 1;
                }
                for (t = 0; t < RGQ_COUNT; t++) if (seen[t]) head[t]++;
            }
            printf("\n== 单次三选一 · 第 %d 波形态（植物卡 %d 张 / 共 %d 张）采样 %d 次 ==\n",
                   wave + 1, wave < 10 ? 2 : 1, DRAFT_N, N1);
            printf("  %-4s %16s %16s\n", "档位", "单个卡位出现率", "一次里至少出现一次");
            for (t = 0; t < RGQ_COUNT; t++) {
                pn(TIER_NAME[t]);
                printf("   %14.3f%% %15.3f%%\n",
                       100.0 * (double)slot[t] / (double)slotTotal,
                       100.0 * (double)head[t] / (double)N1);
            }
            /* 逐株：最快与最慢各 2 株，验证档位越稀有越难抽 */
            {
                int best = -1, worst = -1;
                double sum = 0.0;
                int elig = 0;
                for (i = 0; i < RG_PLANT_N; i++) {
                    if (!rgDraftEligible(i)) continue;      /* 不上牌的档位不参与比较 */
                    elig++;
                    sum += plantHit[i];
                    if (best < 0 || plantHit[i] > plantHit[best]) best = i;
                    if (worst < 0 || plantHit[i] < plantHit[worst]) worst = i;
                }
                printf("  逐株出现率（仅合格池 %d 株）：最高 ", elig);
                pn(rgPlants[best].name);
                printf("(%.3f%%)  最低 ", 100.0 * plantHit[best] / (double)N1);
                pn(rgPlants[worst].name);
                printf("(%.3f%%)  平均 %.3f%%\n",
                       100.0 * plantHit[worst] / (double)N1,
                       100.0 * sum / (double)N1 / (elig ? elig : 1));
            }
        }
    }

    /* ---- ② 整局：连续 20 次三选一，算"至少见到一次"的概率 ---- */
    {
        int runHead[RGQ_COUNT], acquired = 0;
        int draftWithHall = 0, draftWithUltra = 0, plantSlotsTotal = 0;
        memset(runHead, 0, sizeof(runHead));
        for (i = 0; i < N2; i++) {
            int seen[RGQ_COUNT];
            memset(seen, 0, sizeof(seen));
            gRgN = 0;
            memset(gGrowthStack, 0, sizeof(gGrowthStack));
            for (wave = 0; wave < 20; wave++) {
                int bestIdx = -1, bestTier = -1;
                rgOpenDraft(wave);
                for (k = 0; k < DRAFT_N; k++) {
                    int v = gDraft[k];
                    if (!DRAFT_IS_RG(v)) continue;
                    t = rgPlants[v - RG_CARD_BASE].tier;
                    seen[t] = 1;
                    plantSlotsTotal++;
                    if (t > bestTier) { bestTier = t; bestIdx = k; }
                }
                if (seen[RGQ_ULTRA]) draftWithUltra++;
                if (seen[RGQ_HALL])  draftWithHall++;
                /* 策略：总挑当前出现的最高档植物卡（逼近"认真玩"的上限） */
                if (bestIdx >= 0) applyRelic(gDraft[bestIdx]);
            }
            acquired += gRgN;
            for (t = 0; t < RGQ_COUNT; t++) if (seen[t]) runHead[t]++;
        }
        printf("\n== 一整局（20 次三选一）· 至少出现一次的概率 · 模拟 %d 局 ==\n", N2);
        printf("  %-4s %16s\n", "档位", "整局至少见一次");
        for (t = 0; t < RGQ_COUNT; t++) {
            pn(TIER_NAME[t]);
            printf("   %14.2f%%\n", 100.0 * (double)runHead[t] / (double)N2);
        }
        printf("  平均每局拿到 %.2f 株肉鸽植物（上限 %d）；"
               "整局共出现 %.1f 个植物卡位\n",
               (double)acquired / (double)N2, RG_MAX_SLOTS,
               (double)plantSlotsTotal / (double)N2);
        printf("  单次三选一里出现传奇的概率 %.2f%%，出现殿堂的概率 %.2f%%\n",
               100.0 * (double)draftWithUltra / ((double)N2 * 20.0),
               100.0 * (double)draftWithHall / ((double)N2 * 20.0));
    }

    /* ---- ③ 门槛校验：低于 RG_DRAFT_MIN_TIER 的档位一张都不许上牌 ---- */
    {
        int leak = 0, pool = 0, distinct[RG_PLANT_N];
        long drafts = 0;
        memset(distinct, 0, sizeof(distinct));
        for (i = 0; i < RG_PLANT_N; i++) if (rgDraftEligible(i)) pool++;
        for (i = 0; i < 20000; i++) {
            gRgN = 0;
            rgOpenDraft(0);
            drafts++;
            for (k = 0; k < DRAFT_N; k++) {
                int v = gDraft[k];
                if (!DRAFT_IS_RG(v)) continue;
                distinct[v - RG_CARD_BASE] = 1;
                if (rgPlants[v - RG_CARD_BASE].tier < RG_DRAFT_MIN_TIER) leak++;
            }
        }
        {
            int seen = 0;
            for (i = 0; i < RG_PLANT_N; i++) if (distinct[i]) seen++;
            printf("\n== 出牌门槛校验（%ld 次三选一）==\n", drafts);
            gPrimordialTheme = 0;   /* 门槛校验会顺带触发始祖主题，这里只关心卡本身 */
            printf("  合格池 %d 株 / 全表 %d 株；实际出现过 %d 株\n",
                   pool, (int)RG_PLANT_N, seen);
            printf("  低于门槛的卡出现次数 = %d %s\n", leak, leak ? "  FAIL" : "  PASS");
            printf("  合格池里还没被抽到过的是 %d 株（20 次采样不足以覆盖全部）\n",
                   pool - seen);
            if (leak != 0) return 1;
        }
    }

    /* ---- ④ 目标校验：用户指定的档位概率必须落到位、且保持降序 ---- */
    {
        /* ⚠️ 这组目标值必须跟着卡表规模走。
           卡表从 108 扩到 158（+20 殿堂 +30 始祖）之后，权重是重算过的
           （见 _test_cards50.c 的解算结果与 RGQ_DRAFT_W 的注释），
           原来那组 {10, 8, 5, 1} 是 108 张时的口径，已经作废。
           现值 = 重算后权重下的**设计目标**，不是"实际跑出来的数"。 */
        static const double want[RGQ_COUNT] = { 0, 0, 0, 0, 11.0, 7.0, 6.6, 2.2 };
        double p[RGQ_COUNT], sum = 0.0;
        long tot = 0;
        int bad = 0, desc = 1;
        for (t = 0; t < RGQ_COUNT; t++) tot += (long)cnt[t] * RGQ_DRAFT_W[t];
        for (t = 0; t < RGQ_COUNT; t++) {
            p[t] = 100.0 * (double)cnt[t] * RGQ_DRAFT_W[t] / (double)tot;
            sum += p[t];
        }
        printf("\n== 目标校验（解析口径，合格池 %ld 总权重）==\n", tot);
        for (t = RGQ_COMMON; t < RGQ_COUNT; t++) {
            if (cnt[t] == 0) continue;          /* 不下牌的档位跳过 */
            printf("  %-4s 实际 %6.2f%%", "", p[t]);
            pn(TIER_NAME[t]);
            if (want[t] > 0.0) {
                double d = p[t] - want[t];
                printf("   目标 %.1f%%  偏差 %+.2fpp%s", want[t], d,
                       (d > 0.5 || d < -0.5) ? "  FAIL" : "");
                if (d > 0.5 || d < -0.5) bad++;
            }
            printf("\n");
        }
        printf("  合计 %.2f%%%s\n", sum, (sum < 99.5 || sum > 100.5) ? "  FAIL" : "  PASS");
        if (sum < 99.5 || sum > 100.5) bad++;
        /* 降序：越稀有出现率越低（普通已下牌，从稀有开始比）。
           ⚠️ 这里比的是**每档总概率**，不是"每张卡的概率"。
              两者是不同的量，本测试只钉档位级 —— 因为"每张卡"的降序
              在张数悬殊时（传奇 5 张 vs 殿堂 25 张）做不到，
              除非把传奇压到基本抽不到。详见 RGQ_DRAFT_W 的注释。 */
        for (t = RGQ_RARE + 1; t < RGQ_COUNT; t++)
            if (cnt[t] > 0 && cnt[t - 1] > 0 && p[t] >= p[t - 1]) desc = 0;
        printf("  档位总概率严格降序（稀有→始祖）：%s\n", desc ? "PASS" : "FAIL");
        if (desc) {
            printf("   （殿堂 %.2f%% < 传奇 %.2f%% —— 这条曾经是反的：只按目标取整时\n"
                   "     25 张的殿堂总概率会反超 5 张的传奇，见 RGQ_DRAFT_W 注释）\n",
                   p[RGQ_HALL], p[RGQ_ULTRA]);
        }
        if (!desc) bad++;
        /* 僵尸那套权重不能被植物的改动带跑 */
        {
            static const int z[RGQ_COUNT] = { 30, 26, 20, 15, 10, 6, 2, 0 };
            int same = 1;
            for (t = 0; t < RGQ_COUNT; t++) if (RGQ_W[t] != z[t]) same = 0;
            printf("  肉鸽僵尸权重 RGQ_W 未被改动：%s\n", same ? "PASS" : "FAIL");
            if (!same) bad++;
        }
        if (bad) return 1;
    }

    printf("\nseed=%d（同 seed 可复现）\n", seed);
    remove("_test_draft_odds_save.dat");
    return 0;
}
