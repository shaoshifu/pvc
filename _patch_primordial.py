# -*- coding: utf-8 -*-
"""始祖级强化 + 三选一开牌庆典特效。

用法： python _patch_primordial.py
"""
import io
import sys

P = 'pvz.c'
s = io.open(P, encoding='utf-8').read()
step = [0]


def rep(old, new, cnt=1, what=""):
    global s
    n = s.count(old)
    assert n == cnt, "锚点不匹配(%d!=%d) %s: %r" % (n, cnt, what, old[:70])
    s = s.replace(old, new)
    step[0] += 1
    print("  [%2d] %s" % (step[0], what or old[:48]))


# ============================================================
# 一、始祖级数值强化
# ============================================================
rep("""static const float RGQ_DMG[RGQ_COUNT]  = { 2.0f, 4.0f, 7.0f, 11.0f, 15.0f, 20.0f, 100.0f, 1000.0f };""",
    """/* 伤害倍率。
   ★ 始祖（末位）从 1000 拉到 4000 —— 用户要求"与其他植物强度不匹配、需要过强"。
     这一步是安全的：RGQ_DMG 只被 rgDmgMul（植物）与 rgZombieHpMul（僵尸）
     两处消费，而**没有任何僵尸使用 RGQ_PRIMORDIAL 档位**（已核对全表），
     所以抬高末位不会连带把僵尸血量也放大 1680 倍。
     rgZombieHpMul 另外加了一道保险，见那里的注释。 */
static const float RGQ_DMG[RGQ_COUNT]  = { 2.0f, 4.0f, 7.0f, 11.0f, 15.0f, 20.0f, 100.0f, 4000.0f };""",
    what="RGQ_DMG 始祖 1000→4000")

rep("""static const float RGQ_RATE[RGQ_COUNT] = { 1.00f, 1.00f, 1.15f, 1.30f, 1.45f, 1.60f, 1.60f, 1.60f };""",
    """/* 发射频率倍率（越大越快，作用在冷却上取倒数）。
   始祖 1.60 → 3.00：脉冲周期 2.8/3.0 ≈ 0.93 秒，比殿堂快将近一倍。 */
static const float RGQ_RATE[RGQ_COUNT] = { 1.00f, 1.00f, 1.15f, 1.30f, 1.45f, 1.60f, 1.60f, 3.00f };""",
    what="RGQ_RATE 始祖 1.6→3.0")

rep("""static const float RGQ_RANGE[RGQ_COUNT]= { 1.0f, 1.20f, 1.50f, 1.90f, 2.40f, 3.00f, 3.00f, 3.00f };""",
    """/* 杀伤范围倍率（溅射半径 / 覆盖格数 / 弹幕数量）。
   始祖 3.00 → 8.00：地刺铺满半场、齐射覆盖全屏。 */
static const float RGQ_RANGE[RGQ_COUNT]= { 1.0f, 1.20f, 1.50f, 1.90f, 2.40f, 3.00f, 3.00f, 8.00f };""",
    what="RGQ_RANGE 始祖 3.0→8.0")

rep("""    return 1.0f + (RGQ_DMG[tier] - 2.0f) * 0.42f;    /* 普通 1.0 → 传奇 8.56 */""",
    """    /* ⚠️ 保险：僵尸永远不该吃到 RGQ_PRIMORDIAL 档的倍率。
       该档位的 RGQ_DMG 是为植物准备的 4000×，套到这个公式上会变成 1680 倍血量，
       直接把关卡变成无解。今天全表没有僵尸使用这一档（已核对），
       但"以后有人加一只始祖僵尸"是个很自然的动作，所以在这里钉死上界 ——
       越界时按殿堂档结算，属于**保守**的降级，不会静默失控。 */
    if (tier >= RGQ_PRIMORDIAL) tier = RGQ_HALL;
    return 1.0f + (RGQ_DMG[tier] - 2.0f) * 0.42f;    /* 普通 1.0 → 传奇 8.56 */""",
    what="rgZombieHpMul 加始祖档保险")

# 始祖三尊的文案（把 4000× 与威压写进去，卡片不能骗人）
rep("""/* 105 */ { L"始源棘原", RGQ_PRIMORDIAL, PT_SPIKEWEED, RG_ALL_TRAITS,
            L"大地最初的荆棘。全额伤害的棘刺领域铺满半场，任何踏入者被持续撕裂、撕裂、再撕裂",
            10000 },""",
    """/* 105 */ { L"始源棘原", RGQ_PRIMORDIAL, PT_SPIKEWEED, RG_ALL_TRAITS,
            L"4000 倍伤害。棘刺领域铺满半场，踏入者被持续撕裂；始祖在场时全场僵尸持续失血",
            10000 },""",
    what="105 文案")

rep("""/* 106 */ { L"太初坚壁", RGQ_PRIMORDIAL, PT_WALLNUT, RG_ALL_TRAITS,
            L"时间之初就立在这里的墙。承受一切伤害并全额奉还给攻击者，被摧毁时会连根重生",
            10000 },""",
    """/* 106 */ { L"太初坚壁", RGQ_PRIMORDIAL, PT_WALLNUT, RG_ALL_TRAITS,
            L"4000 倍血量。承受一切并全额奉还，被摧毁时连根重生；始祖在场时全场僵尸持续失血",
            10000 },""",
    what="106 文案")

rep("""/* 107 */ { L"混沌齐射", RGQ_PRIMORDIAL, PT_PEASHOOTER, RG_ALL_TRAITS,
            L"同时向所有方向倾泻出创世之初的种子。每发都是全额伤害，命中即湮灭整行",
            10000 },""",
    """/* 107 */ { L"混沌齐射", RGQ_PRIMORDIAL, PT_PEASHOOTER, RG_ALL_TRAITS,
            L"4000 倍伤害 × 3 倍攻速 × 8 倍范围。向所有方向倾泻创世种子，命中即湮灭整行",
            10000 },""",
    what="107 文案")

# ============================================================
# 二、始祖威压（场上存在始祖时的全局被动）
# ============================================================
AURA = u'''/* ==================== 始祖威压：只有始祖才有的全局被动 ====================
   用户要求始祖"与当前版本其他植物的强度不相匹配、需要过强"。
   单纯堆数值（4000× 伤害 / 3× 攻速 / 8× 范围）只是"数字大"，
   玩家不一定能感觉到"这株更强" —— 所以再给一条**机制层面只有始祖才有**的被动：

     场上只要有 1 株存活的始祖植物，全场僵尸就持续受到
     「按最大生命的百分比」的真伤，并被持续压制减速。

   也就是说始祖一登场，整局的节奏就变了 —— 这才是"强度不匹配"的直观来源。

   为什么用百分比而不是固定伤害：
     僵尸血量随波次放大，固定值在后期等于没有；百分比不受放大影响，
     低波次也不会出现"一瞬间清空全场"的突兀感（它是持续的，不是秒杀）。
   为什么用"真伤"（直接扣 hp）而不是走护甲减伤：
     始祖的定位就是"没有东西能挡住它"，绕开护甲符合直觉；
     同时明确**不给**它任何免死/复活类效果，避免又踩到"最后一只打不死"那个坑。 */
#define RG_PRIMORDIAL_AURA_DPS   0.055f   /* 每秒 5.5% 最大生命 */
#define RG_PRIMORDIAL_AURA_SLOW  0.90f    /* 压制：减速计时被不断刷新 */
static int gPrimordialAlive = 0;          /* 存活始祖数（光环与 HUD 都用它） */

static int primordialAliveCount(void)
{
    int r, c, sl, n = 0;
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++)
            for (sl = 0; sl < 2; sl++) {
                /* 双株模式的第二株放在 grid2，两格都要数，否则会漏掉一半 */
                Plant *p = (sl == 0) ? &grid[r][c] : &grid2[r][c];
                if (!p->alive || p->rg <= 0) continue;
                if (rgPlants[p->rg - 1].tier == RGQ_PRIMORDIAL) n++;
            }
    return n;
}

static void primordialAuraUpdate(float dt)
{
    int i;
    gPrimordialAlive = primordialAliveCount();
    if (gPrimordialAlive <= 0) return;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        if (!z->active || z->dead) continue;
        z->hp -= z->maxhp * RG_PRIMORDIAL_AURA_DPS * dt;   /* 真伤：不吃护甲减伤 */
        if (z->slow < RG_PRIMORDIAL_AURA_SLOW) z->slow = RG_PRIMORDIAL_AURA_SLOW;
        if (z->hp <= 0.0f) zombieDie(z, 0);
    }
    /* 视觉反馈：偶尔在随机僵尸身上飘一簇金屑，让"全场在掉血"看得见。
       用概率而不是每帧全画 —— 每帧给几百只僵尸各加粒子会拖垮帧率。 */
    if (rnd(0.0f, 1.0f) < dt * 14.0f) {
        int k = rndi(0, MAX_ZOMBIES);
        Zombie *z = &zombies[k];
        if (z->active && !z->dead) {
            addParticle(z->x + rnd(-18, 18), z->y - rnd(20, 70),
                        rnd(-20, 20), rnd(-70, -30), 0.5f, rnd(3, 6),
                        RGB(255, 216, 120), 1);
        }
    }
}

'''
rep("static void updateGame(float dt)\n{", AURA + "static void updateGame(float dt)\n{",
    what="插入始祖威压实现")

rep("""    terrainUpdate(dt);""", """    terrainUpdate(dt);
    primordialAuraUpdate(dt);      /* 始祖威压：场上存在始祖时的全局被动 */""",
    cnt=1, what="updateGame 调用威压")

# ============================================================
# 三、三选一开牌庆典：殿堂级以上
# ============================================================
CELEB_CORE = u'''/* ==================== 三选一开牌庆典（殿堂级以上）====================
   用户要求：抽到殿堂级以上时，演出要有冲击力。
   触发条件 = 本次三个选项里的**最高档位** >= RGQ_HALL（殿堂）；
   始祖（RGQ_PRIMORDIAL）再叠一层更长更亮的强化版。

   演出分三层，按绘制顺序（全部画在世界层，与卡片共用 2x 坐标约定）：
     ① drawCelebBack  —— 卡片**后面**：旋转光扇 + 外扩光环 + 顶部光柱
     ② 卡片本身（原有翻牌逻辑完全不动）
     ③ drawCelebFront —— 卡片**上面**：全屏彩幕闪光 + 冲击波 + 迸射火花 + 卡缘脉冲
     ④ drawCelebText  —— 高分辨率文字层：档位横幅 + 卡名 + 副标题

   动画的时间基准统一用 gCelebT/gCelebDur 归一化成进度 p（0→1），
   所有效果都只依赖 p —— 于是**测试可以把 gCelebT 摆到任意进度抓帧**，
   不必等实时演出、也不必依赖帧率，逐像素基线照样成立。 */

#define CELEB_MIN_TIER    RGQ_HALL
#define CELEB_DUR_HALL    2.8f
#define CELEB_DUR_PRIM    4.2f
#define CELEB_RAY_N       18

static float    gCelebT    = 0.0f;   /* 剩余时长；<=0 表示无演出 */
static float    gCelebDur  = 0.0f;   /* 总时长 */
static int      gCelebTier = -1;     /* 本次最高档位 */
static int      gCelebSlot = -1;     /* 打出最高档的那张卡（0..DRAFT_N-1） */
static int      gCelebPrim = 0;      /* 1 = 始祖级（强化演出） */
static unsigned gCelebSeed = 1u;     /* 光扇相位 / 火花角度分布的固定种子 */

static void drawCelebBack(HDC dc);
static void drawCelebFront(HDC dc);
static void drawCelebText(HDC dc);
static void drawPrimordialHud(HDC dc);

static float celebEaseOut(float t)
{
    float u;
    t = CLAMP(t, 0.0f, 1.0f);
    u = 1.0f - t;
    return 1.0f - u * u * u;
}

/* 进度 0→1 */
static float celebP(void)
{
    if (gCelebDur <= 0.0f) return 1.0f;
    return CLAMP(1.0f - gCelebT / gCelebDur, 0.0f, 1.0f);
}

static int celebActive(void)
{
    return (gCelebT > 0.0f && gCelebTier >= CELEB_MIN_TIER && gCelebSlot >= 0 &&
            gCelebSlot < DRAFT_N);
}

static void celebBegin(int tier, int slot)
{
    gCelebTier = tier;
    gCelebSlot = slot;
    gCelebPrim = (tier >= RGQ_PRIMORDIAL);
    gCelebDur  = gCelebPrim ? CELEB_DUR_PRIM : CELEB_DUR_HALL;
    gCelebT    = gCelebDur;
    /* 种子只影响"光扇的起始角、火花的分布"，不影响任何游戏逻辑；
       固定一个非零值，保证抓帧可复现。 */
    gCelebSeed = (unsigned)(tier * 2654435761u) ^ 0x9E3779B9u;
    if (!gCelebSeed) gCelebSeed = 12345u;
    /* 始祖级：世界本身也要变（背景与音乐），这是既有的用户要求。
       放在这里而不是别处 —— "出现即改写"说的就是它进三选一那一刻。 */
    if (gCelebPrim) primordialThemeApply();
}

static void celebUpdate(float dt)
{
    if (gCelebT > 0.0f) {
        gCelebT -= dt;
        if (gCelebT <= 0.0f) { gCelebT = 0.0f; gCelebTier = -1; gCelebSlot = -1; }
    }
}

/* 扫描三个选项取最高档位；达到殿堂就开演出。
   与 draftScanPrimordial() 分开：那一个只负责"始祖改写世界"这件既有的事，
   保持它的行为与返回值不变，已有测试不受影响。 */
static int draftCelebScan(void)
{
    int k, best = -1, bestSlot = -1;
    for (k = 0; k < DRAFT_N; k++) {
        int v = gDraft[k];
        int t;
        if (!DRAFT_IS_RG(v)) continue;
        t = rgPlants[v - RG_CARD_BASE].tier;
        if (t > best) { best = t; bestSlot = k; }
    }
    if (best >= CELEB_MIN_TIER) celebBegin(best, bestSlot);
    return best;
}

'''
rep("static Sprite gSprPlant[PT_COUNT];", CELEB_CORE + "static Sprite gSprPlant[PT_COUNT];",
    what="插入庆典核心（状态机，不含绘制）")

# --- 绘制实现：放在 drawDraftShapes 之前（那时 fillRectA / strokeRound 都已定义）---
CELEB_DRAW = u'''/* ==================== 庆典演出：绘制部分 ==================== */

/* ① 卡片后面：旋转光扇 + 外扩光环 + 顶部光柱 */
static void drawCelebBack(HDC dc)
{
    float p, x, y, cx, cy, rot, fade;
    COLORREF tc;
    int k;

    if (!celebActive()) return;
    p  = celebP();
    tc = RGQ_COL[gCelebTier];
    draftCardRect(gCelebSlot, &x, &y);
    cx = x + CARD_W * 0.5f;
    cy = y + CARD_H * 0.5f;

    /* 顶部光柱：从画面上沿倾泻到卡心，先亮后收。
       用 fillRectA 才有真正的半透明（GDI 图元没有 alpha）。 */
    {
        float f = 1.0f - CLAMP((p - 0.55f) / 0.45f, 0.0f, 1.0f);
        float w = (gCelebPrim ? 150.0f : 100.0f) + 60.0f * f;
        if (f > 0.01f) {
            fillRectA(dc, cx - w, 0.0f, cx + w, cy, tint(tc, 0.45f), (int)(64.0f * f));
            fillRectA(dc, cx - w * 0.34f, 0.0f, cx + w * 0.34f, cy,
                      tint(tc, 0.85f), (int)(92.0f * f));
        }
    }

    /* 旋转光扇：18 道宽窄相间的楔形，长度先冲出去再收，整体缓慢自转。
       楔形用四点 poly —— 只给三角形做不出"有宽度的光柱"。 */
    {
        float grow = celebEaseOut(p / 0.45f);
        float r0 = (gCelebPrim ? 140.0f : 120.0f);
        float r1 = r0 + (gCelebPrim ? 2100.0f : 1250.0f) * grow;
        fade = 1.0f - CLAMP((p - 0.52f) / 0.48f, 0.0f, 1.0f);
        rot  = p * (gCelebPrim ? 1.35f : 0.85f) * 6.28318f
             + (float)(gCelebSeed & 255u) * 0.0246f;
        if (fade > 0.02f) {
            for (k = 0; k < CELEB_RAY_N; k++) {
                float a  = rot + 6.28318f * (float)k / (float)CELEB_RAY_N;
                float aw = (k & 1) ? 0.062f : 0.030f;      /* 宽窄相间 */
                float br = (k & 1) ? 0.62f : 0.34f;        /* 亮暗相间 */
                float ca = cosf(a),  sa = sinf(a);
                float cb = cosf(a + aw), sb = sinf(a + aw);
                POINT q[4];
                q[0].x = (int)(cx + ca * r0); q[0].y = (int)(cy + sa * r0);
                q[1].x = (int)(cx + ca * r1); q[1].y = (int)(cy + sa * r1);
                q[2].x = (int)(cx + cb * r1); q[2].y = (int)(cy + sb * r1);
                q[3].x = (int)(cx + cb * r0); q[3].y = (int)(cy + sb * r0);
                poly(dc, q, 4, tint(tc, br * fade), 0, 0, 0);
            }
        }
    }

    /* 外扩光环：一圈离散光点向外推开（GDI 画不了粗圆弧，用点阵最省事且更"能量"）。
       始祖多套一圈、推得更远。 */
    {
        int ring, rings = gCelebPrim ? 2 : 1;
        for (ring = 0; ring < rings; ring++) {
            float ph = CLAMP((p - (float)ring * 0.14f) / 0.62f, 0.0f, 1.0f);
            float rr, f2;
            int n = 44, i2;
            if (ph <= 0.0f || ph >= 1.0f) continue;
            rr = 90.0f + (gCelebPrim ? 620.0f : 430.0f) * celebEaseOut(ph);
            f2 = 1.0f - ph;
            for (i2 = 0; i2 < n; i2++) {
                float a = 6.28318f * (float)i2 / (float)n + rot * 0.7f;
                fillCircle(dc, cx + cosf(a) * rr, cy + sinf(a) * rr * 0.82f,
                           4.0f + 8.0f * f2, tint(tc, 0.25f + 0.55f * f2));
            }
        }
    }
}

/* ③ 卡片上面：全屏彩幕 + 冲击波 + 迸射火花 + 卡缘脉冲 */
static void drawCelebFront(HDC dc)
{
    float p, x, y, cx, cy;
    COLORREF tc;
    int k;

    if (!celebActive()) return;
    p  = celebP();
    tc = RGQ_COL[gCelebTier];
    draftCardRect(gCelebSlot, &x, &y);
    cx = x + CARD_W * 0.5f;
    cy = y + CARD_H * 0.5f;

    /* 全屏彩幕：开头 30% 时间的强闪，两层（档位色 + 白核）叠出"过曝"感 */
    {
        float f = 1.0f - CLAMP(p / 0.30f, 0.0f, 1.0f);
        if (f > 0.01f) {
            fillRectA(dc, 0, 0, VIEW_W, VIEW_H, tint(tc, 0.70f),
                      (int)((gCelebPrim ? 175.0f : 135.0f) * f));
            fillRectA(dc, 0, 0, VIEW_W, VIEW_H, RGB(255, 255, 255),
                      (int)((gCelebPrim ? 120.0f : 80.0f) * f * f));
        }
    }

    /* 卡缘脉冲：卡片外面套几圈逐渐变淡的描边，读起来像"卡在发光"。
       卡片本身是圆角矩形，所以描边也用圆角 + 逐圈外扩。 */
    {
        float pulse = 0.55f + 0.45f * sinf(p * 12.56f);
        float fade  = 1.0f - CLAMP((p - 0.72f) / 0.28f, 0.0f, 1.0f);
        for (k = 0; k < 4; k++) {
            float o = 5.0f + (float)k * 6.0f + 4.0f * pulse;
            strokeRound(dc, x - o, y - o, x + CARD_W + o, y + CARD_H + o,
                        18.0f + o, tint(tc, (0.62f - 0.14f * (float)k) * fade),
                        (k == 0) ? 4 : 3);
        }
    }

    /* 冲击波：从卡心推出去的一圈粗点，比背层光环更实、更快 */
    {
        float ph = CLAMP(p / 0.55f, 0.0f, 1.0f);
        if (ph > 0.0f && ph < 1.0f) {
            float rr = 60.0f + (gCelebPrim ? 780.0f : 540.0f) * celebEaseOut(ph);
            float f  = 1.0f - ph;
            int n = 56, i2;
            for (i2 = 0; i2 < n; i2++) {
                float a = 6.28318f * (float)i2 / (float)n;
                fillCircle(dc, cx + cosf(a) * rr, cy + sinf(a) * rr * 0.8f,
                           9.0f * f + 2.0f, tint(tc, 0.55f + 0.4f * f));
            }
        }
    }

    /* 迸射火花：26 条带拖尾的亮点，角度/速度/延迟全部由 gCelebSeed 决定。
       固定种子 → 抓帧可复现；也保证同一次演出形状稳定，不会每帧乱跳。 */
    {
        int sparks = gCelebPrim ? 40 : 26;
        for (k = 0; k < sparks; k++) {
            unsigned h = gCelebSeed + (unsigned)k * 2246822519u;
            float a   = (float)((h >> 8) & 1023u) / 1023.0f * 6.28318f;
            float sp  = (gCelebPrim ? 380.0f : 270.0f) + (float)((h >> 18) & 255u);
            float dly = (float)((h >> 3) & 31u) / 31.0f * 0.22f;
            float lp  = (p - dly) / 0.58f;
            float d, f;
            if (lp <= 0.0f || lp >= 1.0f) continue;
            f = 1.0f - lp;
            d = sp * celebEaseOut(lp);
            fillCircle(dc, cx + cosf(a) * d, cy + sinf(a) * d * 0.74f,
                       6.5f * f + 1.5f, tint(tc, 0.80f));
            fillCircle(dc, cx + cosf(a) * d * 0.80f, cy + sinf(a) * d * 0.80f * 0.74f,
                       4.0f * f + 1.0f, tint(tc, 0.40f));
        }
    }
}

/* ④ 文字层：档位横幅 + 卡名 + 副标题。字体档位切换伪造"弹出"（GDI 不能缩放字体） */
static void drawCelebText(HDC dc)
{
    float p, x, y, cx, bar;
    COLORREF tc;
    HFONT fBig;
    const wchar_t *title, *sub;
    const wchar_t *cardname = L"";

    if (!celebActive()) return;
    p  = celebP();
    tc = RGQ_COL[gCelebTier];
    draftCardRect(gCelebSlot, &x, &y);
    cx = x + CARD_W * 0.5f;

    if (DRAFT_IS_RG(gDraft[gCelebSlot]))
        cardname = rgPlants[gDraft[gCelebSlot] - RG_CARD_BASE].name;

    title = gCelebPrim ? L"始 祖 降 临" : L"殿 堂 降 临";
    sub   = gCelebPrim ? L"世界已被改写 · 背景与音乐不再复原"
                       : L"顶级战力加入你的阵线";

    /* 弹出曲线：0.00~0.14 小 → 0.14~0.30 超大（过冲）→ 之后收稳。
       用三个字体档位切换，而不是真的缩放字体。 */
    if (p < 0.14f)      fBig = gF30;
    else if (p < 0.30f) fBig = gF72;
    else                fBig = gF54;

    /* 横幅底衬：一条横向光带，比纯文字更"有分量" */
    {
        float f = 1.0f - CLAMP((p - 0.70f) / 0.30f, 0.0f, 1.0f);
        float wide = 300.0f + 260.0f * celebEaseOut(p / 0.35f);
        bar = y - 96.0f;
        if (f > 0.01f) {
            fillRectA(dc, cx - wide, bar - 34.0f, cx + wide, bar + 46.0f,
                      tint(tc, -0.55f), (int)(120.0f * f));
            fillRectA(dc, cx - wide, bar + 34.0f, cx + wide, bar + 40.0f,
                      tint(tc, 0.35f), (int)(190.0f * f));
            fillRectA(dc, cx - wide, bar - 40.0f, cx + wide, bar - 34.0f,
                      tint(tc, 0.35f), (int)(190.0f * f));
        }
    }

    putTextCS(dc, fBig, tint(tc, 0.55f), RGB(24, 18, 10), title, cx, bar - 2.0f);
    putTextCS(dc, gF22, RGB(250, 246, 232), RGB(40, 30, 16), cardname, cx, bar + 26.0f);
    putTextCS(dc, gF15, tint(tc, 0.25f), RGB(30, 24, 14), sub, cx, bar + 52.0f);
}

/* ⑤ 战斗界面的小指示：始祖在场时把"威压"显示出来，
      否则玩家只会觉得"僵尸怎么自己掉血"而不知道来源。 */
static void drawPrimordialHud(HDC dc)
{
    wchar_t buf[64];
    float pulse;
    if (gPrimordialAlive <= 0) return;
    pulse = 0.55f + 0.45f * sinf(gTime * 3.4f);
    wsprintfW(buf, L"始祖威压 ×%d", gPrimordialAlive);
    fillRectA(dc, VIEW_W * 0.5f - 104.0f, 6.0f, VIEW_W * 0.5f + 104.0f, 30.0f,
              RGB(24, 16, 8), (int)(150.0f * pulse + 40.0f));
    putTextCS(dc, gF15, RGB(255, 226, 150), RGB(60, 40, 10), buf,
              VIEW_W * 0.5f, 18.0f);
}

'''
rep("static void drawDraftShapes(HDC dc)\n{", CELEB_DRAW + "static void drawDraftShapes(HDC dc)\n{",
    what="插入庆典绘制实现")

# --- 接线：开牌时扫描 / 每帧推进 / 分层绘制 ---
rep("""    draftScanPrimordial();""",
    """    draftScanPrimordial();
    draftCelebScan();          /* 殿堂级以上：开牌庆典演出 */""",
    what="rgOpenDraft 接线庆典扫描")

rep("""        else { gTime += dt; weatherUpdate(dt); if (gLvUpT > 0.0f) gLvUpT -= dt; }   /* 菜单/结算界面保持动画 */""",
    """        else {
            gTime += dt; weatherUpdate(dt); if (gLvUpT > 0.0f) gLvUpT -= dt;
            /* 庆典演出发生在 ST_DRAFT（属于"非战斗"分支），
               所以必须在这里推进计时 —— 放 updateGame 里的话演出根本不会走。 */
            celebUpdate(dt);
        }""",
    what="推进庆典计时")

rep("""        for (yy = 0; yy < VIEW_H; yy += 2)
            fillRect(gWorldDC, 0, yy, VIEW_W, yy + 1, RGB(14, 24, 16));
        drawDraftShapes(gWorldDC);
    }""",
    """        for (yy = 0; yy < VIEW_H; yy += 2)
            fillRect(gWorldDC, 0, yy, VIEW_W, yy + 1, RGB(14, 24, 16));
        drawCelebBack(gWorldDC);       /* 光扇/光环：必须在卡片之下 */
        drawDraftShapes(gWorldDC);
        drawCelebFront(gWorldDC);      /* 闪光/冲击波/火花：压在卡片之上 */
    }""",
    what="三选一渲染分层")

rep("""        if (gState == ST_DRAFT) drawDraftText(gWorldDC);""",
    """        if (gState == ST_DRAFT) drawDraftText(gWorldDC);
        if (gState == ST_DRAFT) drawCelebText(gWorldDC);
        if (gState == ST_PLAY)  drawPrimordialHud(gWorldDC);   /* 始祖在场指示 */""",
    what="文字层接线")

io.open(P, 'w', encoding='utf-8', newline='\n').write(s)
print("\n  全部 %d 处改动完成" % step[0])
