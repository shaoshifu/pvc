# -*- coding: utf-8 -*-
"""把"新五关专属规则"接进游戏。

设计取舍（重要）：
  规则尽量做成**种植约束**或**场地事件**，而不是去改植物/僵尸的行为逻辑。
  原因：伤害点有十几处、植物行为分支极多，逐处嵌入关卡规则既容易漏、
  又会把"关卡规则"和"单位行为"耦合死，以后加第 12 关就得再动一遍那些地方。
  约束类和事件类规则只在少数几个稳定的位置挂钩，风险低得多。

挂钩点：
  1. cellLegal()          —— 薄冰不可种 / 供电必须连链
  2. 僵尸更新循环          —— 冰面击退（靠 flash 边沿检测，天然覆盖所有伤害来源）
  3. drawZombie()         —— 迷雾中隐藏
  4. drawTerrainOverlay() —— 绘制薄冰 / 裂隙 / 迷雾 / 断电警示
  5. updateGame()         —— 裂隙周期 / 陨石 / 阶段推进
  6. resetGame()          —— 初始化

用法： python _impl_rules.py
"""
import sys

P = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"

# ---------------------------------------------------------------- 规则模块本体
MODULE = r'''
/* ========================================================================
   新五关（第 6~10 关）专属场地规则
   ------------------------------------------------------------------------
   五关各有一条不可替代的规则，难度来自"规则变化"而非数值堆叠：
     6 霜牙隘口  RULE_ICE     冰面击退（受击被推远）+ 薄冰格不可种
     7 熔心裂谷  RULE_RIFT    岩浆裂隙周期喷发，摧毁植物也伤僵尸
     8 幽影回廊  RULE_FOG     右四列迷雾，其中僵尸不可见
     9 机枢要塞  RULE_POWER   必须贴着已有的植物往外种（连链）
    10 星界王座  RULE_ASTRAL  三阶段：引力加速 / 星陨 / 时空闪现
   ======================================================================== */
#define RULE_NONE    0
#define RULE_ICE     1
#define RULE_RIFT    2
#define RULE_FOG     3
#define RULE_POWER   4
#define RULE_ASTRAL  5

static int   gRuleKind = RULE_NONE;

/* --- 关 6 --- */
static unsigned char gThinIce[ROWS][COLS];   /* 1 = 薄冰格，不可种植 */
/* --- 关 7 --- */
static int   gRiftR[3], gRiftC[3];           /* 三处裂隙的行列 */
static float gRiftT;                         /* 距下次轮换 */
static float gRiftWarn;                      /* 预警剩余（>0 时画裂纹） */
static float gRiftBlast;                     /* 喷发表现剩余 */
static float gHeatWave;                      /* 热浪：僵尸速度加成（累积） */
/* --- 关 8 --- */
static int   gFogFromCol = 5;                /* 迷雾起始列 */
static float gFogRevealT;                    /* 揭示窗口剩余 */
/* --- 关 9 --- */
static int   gPowerCutRow[ROWS];             /* 该行断电剩余秒数（保留给短路僵尸） */
/* --- 关 10 --- */
static int   gPhase  = 1;                    /* 1~3 */
static float gPhaseT;                        /* 本阶段已进行时间 */
static float gGravAcc;                       /* P1：累积加速 */
static float gMeteorT;                       /* P2：陨石计时 */
static float gBlinkT;                        /* P3：群体闪现计时 */

/* 关 7 裂隙的轮换周期（秒） */
#define RIFT_CYCLE   12.0f
#define RIFT_WARN     3.0f

static void newLevelInit(void)
{
    int i, r, c, guard;
    gRuleKind = RULE_NONE;

    memset(gThinIce, 0, sizeof(gThinIce));
    memset(gPowerCutRow, 0, sizeof(gPowerCutRow));
    gRiftT = RIFT_CYCLE; gRiftWarn = 0.0f; gRiftBlast = 0.0f;
    gHeatWave = 0.0f;
    gFogFromCol = 5; gFogRevealT = 0.0f;
    gPhase = 1; gPhaseT = 0.0f; gGravAcc = 0.0f;
    gMeteorT = 10.0f; gBlinkT = 6.0f;
    for (i = 0; i < 3; i++) { gRiftR[i] = 0; gRiftC[i] = 0; }

    if (gCurLevel < 6 || gCurLevel >= LV_COUNT) return;   /* 老关卡不启用任何新规则 */
    switch (gCurLevel) {
    case 6:  gRuleKind = RULE_ICE;     break;
    case 7:  gRuleKind = RULE_RIFT;    break;
    case 8:  gRuleKind = RULE_FOG;     break;
    case 9:  gRuleKind = RULE_POWER;   break;
    case 10: gRuleKind = RULE_ASTRAL;  break;
    default: gRuleKind = RULE_NONE;    break;
    }

    /* 关 6：随机撒 6 格薄冰。
       用 guard 兜底避免"薄冰格子太少时 while 死循环"—— 格子数远大于 6，
       但这类循环一旦条件写错就是硬挂起，值得加一道保险。 */
    if (gRuleKind == RULE_ICE) {
        int placed = 0;
        guard = 0;
        while (placed < 6 && guard++ < 500) {
            r = rndi(0, ROWS); c = rndi(0, COLS);
            if (gThinIce[r][c]) continue;
            gThinIce[r][c] = 1;
            placed++;
        }
    }
    /* 关 7：三处裂隙初始位置（避开最左两列，那里是玩家唯一的稳定后方） */
    if (gRuleKind == RULE_RIFT) {
        for (i = 0; i < 3; i++) {
            gRiftR[i] = rndi(0, ROWS);
            gRiftC[i] = rndi(2, COLS);
        }
    }
}

/* 关卡规则：该格能不能种植（供 cellLegal 调用） */
static int newLevelCellBlocked(int r, int c)
{
    if (gRuleKind == RULE_ICE && r >= 0 && c >= 0 &&
        r < ROWS && c < COLS && gThinIce[r][c]) return 1;   /* 薄冰种不了 */

    /* 关 9 供电：必须与同行已有植物相邻（第 0 列直接接电源）。
       做成"种植约束"而不是"断电后失效"，是因为后者要在植物行为的每个
       分支里查电，改动面大且容易漏；前者在放置那一刻就拦住了，
       玩家看到的是"种不下去"这个立刻能理解的反馈。 */
    if (gRuleKind == RULE_POWER && c > 0) {
        if (!(c - 1 >= 0 && c - 1 < COLS &&
              (grid[r][c - 1].alive || grid2[r][c - 1].alive))) return 1;
    }
    return 0;
}

/* 刷新阶段（关 10）：波次推进时调用 */
static void newLevelRefreshPhase(void)
{
    int want;
    if (gRuleKind != RULE_ASTRAL) return;
    if (gWave <= 10)      want = 1;
    else if (gWave <= 20) want = 2;
    else                  want = 3;
    if (want != gPhase) {
        gPhase = want;
        gPhaseT = 0.0f;
        addShake(9.0f, 0.55f);        /* 阶段切换给一次明显的震屏 */
        flashRed = 0.55f;
        sfxPlay(L"alarm");
    }
}

/* 每帧推进关卡规则。由 updateGame 调用。 */
static void newLevelUpdate(float dt)
{
    int i, r, c;

    if (gRuleKind == RULE_NONE) return;

    /* ---------------- 关 7：裂隙轮换与喷发 ---------------- */
    if (gRuleKind == RULE_RIFT) {
        /* 热浪：每波结束累积，逼迫玩家速攻（设计：不能拖） */
        gHeatWave = (float)gWave * 0.04f;

        gRiftT -= dt;
        if (gRiftT < RIFT_WARN && gRiftT + dt >= RIFT_WARN) {
            addShake(3.0f, 0.2f);      /* 预警：轻微震屏 + 裂纹 */
        }
        if (gRiftT <= 0.0f) {
            /* 喷发结算：摧毁该格植物、波及邻格、重创范围内僵尸 */
            gRiftT = RIFT_CYCLE;
            gRiftBlast = 0.45f;
            addShake(7.0f, 0.35f);
            sfxPlay(L"explode");
            for (i = 0; i < 3; i++) {
                int rr = gRiftR[i], cc = gRiftC[i];
                int dr, dc;
                if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) continue;
                /* 该格植物直接烧毁 */
                if (grid[rr][cc].alive)  { grid[rr][cc].alive = 0; }
                if (grid2[rr][cc].alive) { grid2[rr][cc].alive = 0; }
                /* 邻格植物削血（不致死，但削弱防线） */
                for (dr = -1; dr <= 1; dr++) for (dc = -1; dc <= 1; dc++) {
                    int nr = rr + dr, nc = cc + dc;
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
                    if (grid[nr][nc].alive)  grid[nr][nc].hp  -= 40.0f;
                    if (grid2[nr][nc].alive) grid2[nr][nc].hp -= 40.0f;
                }
                /* 范围内僵尸重创（裂隙敌我不分，玩家可以赌它帮忙清怪） */
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    float dx, dy;
                    if (!z->active || z->dead) continue;
                    dx = (z->x - ((float)LAWN_X + ((float)cc + 0.5f) * (float)CELL_W));
                    dy = ((float)rr + 0.5f) * (float)CELL_H;
                    if (dx * dx + dy * dy < 90.0f * 90.0f) {
                        z->hp -= 150.0f;
                        z->flash = 0.12f;
                    }
                }
            }
            /* 轮换到新位置 */
            for (i = 0; i < 3; i++) {
                gRiftR[i] = rndi(0, ROWS);
                gRiftC[i] = rndi(2, COLS);
            }
        }
        if (gRiftBlast > 0.0f) gRiftBlast -= dt;
        if (gRiftT < RIFT_WARN) gRiftWarn = gRiftT; else gRiftWarn = 0.0f;
    }

    /* ---------------- 关 8：迷雾揭示窗口 ---------------- */
    if (gRuleKind == RULE_FOG) {
        if (gFogRevealT > 0.0f) gFogRevealT -= dt;
        /* 前段只盖右两列，第 10 波后扩到右四列（设计里的"迷雾扩张"转折） */
        gFogFromCol = (gWave >= 10) ? 5 : 7;
    }

    /* ---------------- 关 10：三阶段规则 ---------------- */
    if (gRuleKind == RULE_ASTRAL) {
        newLevelRefreshPhase();
        gPhaseT += dt;
        if (gPhase == 1) {
            /* P1 引力崩坏：僵尸缓慢累积加速，且击退无效（击退在冰面关才开） */
            gGravAcc = gPhaseT * 0.006f;
            if (gGravAcc > 0.6f) gGravAcc = 0.6f;
        } else if (gPhase == 2) {
            /* P2 星陨：每 10 秒随机 2 格落陨石，敌我不分 */
            gMeteorT -= dt;
            if (gMeteorT <= 0.0f) {
                gMeteorT = 10.0f;
                addShake(8.0f, 0.4f);
                sfxPlay(L"explode");
                for (i = 0; i < 2; i++) {
                    int rr = rndi(0, ROWS), cc = rndi(1, COLS);
                    if (grid[rr][cc].alive)  grid[rr][cc].alive = 0;
                    if (grid2[rr][cc].alive) grid2[rr][cc].alive = 0;
                    for (r = 0; r < MAX_ZOMBIES; r++) {
                        Zombie *z = &zombies[r];
                        float dx, dy;
                        if (!z->active || z->dead || z->row != rr) continue;
                        dx = (z->x - ((float)LAWN_X + ((float)cc + 0.5f) * (float)CELL_W));
                        dy = (z->x - z->x);
                        if (dx * dx + dy * dy < 80.0f * 80.0f) {
                            z->hp -= 200.0f;
                            z->flash = 0.12f;
                        }
                    }
                }
            }
        } else {
            /* P3 时空裂隙：每 6 秒让场上僵尸群体闪现前进 2 列 */
            gBlinkT -= dt;
            if (gBlinkT <= 0.0f) {
                gBlinkT = 6.0f;
                addShake(5.0f, 0.3f);
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    if (!z->active || z->dead) continue;
                    if (z->y < 0.0f) continue;
                    z->x -= (float)CELL_W * 1.6f;      /* 向左 = 前进 */
                }
            }
        }
    }
}

/* 僵尸的关卡速度修正（关 7 热浪 / 关 10 引力） */
static float newLevelSpeedMul(void)
{
    if (gRuleKind == RULE_RIFT)   return 1.0f + gHeatWave;
    if (gRuleKind == RULE_ASTRAL && gPhase == 1) return 1.0f + gGravAcc;
    return 1.0f;
}

/* 关 6 冰面击退。由僵尸更新循环调用（见那里的说明）。 */
static void newLevelKnockback(Zombie *z)
{
    if (gRuleKind != RULE_ICE) return;
    if (z->dead) return;
    /* 免疫击退的僵尸：冰川巨尸（本关的学习目标 —— 单一战术会被破解）。
       这里用 maxhp 阈值近似识别"重甲单位"，避免为此新增僵尸类型。 */
    if (z->type == ZT_GIANT) return;
    if (z->maxhp > 2500.0f) return;
    z->x += (float)CELL_W * 0.055f;      /* 向后退（僵尸自右向左推进） */
    if (z->x > (float)VIEW_W) z->x = (float)VIEW_W;
}

/* 关 8 迷雾：该僵尸此刻是否应该被隐藏 */
static int newLevelZombieHidden(const Zombie *z)
{
    int col;
    if (gRuleKind != RULE_FOG) return 0;
    if (gFogRevealT > 0.0f) return 0;            /* 揭示窗口内全部可见 */
    if (z->dead) return 0;
    col = (int)((z->x - (float)LAWN_X) / (float)CELL_W);
    return (col >= gFogFromCol && col < COLS + 2);
}
'''

# ---------------------------------------------------------------- 挂钩
HOOKS = [
    # 1. 前向声明（cellLegal 定义在模块之前）
    ("前向声明",
     "static int cellLegal(int r, int c, int w, int hh)\n{",
     "static int newLevelCellBlocked(int r, int c);   /* 定义在文件后段的"新五关规则" */\n\n"
     "static int cellLegal(int r, int c, int w, int hh)\n{"),

    # 2. cellLegal 里加关卡规则
    ("种植校验",
     "    if (gCurLevel >= 0 && gCurLevel < LV_COUNT) {\n"
     "        const LevelDef *ld = &levelDefs[gCurLevel];\n"
     "        int lo = ld->colFrom, hi = ld->colTo;",
     "    /* 新五关的场地规则（薄冰不可种 / 供电必须连链）优先于列范围判断：\n"
     "       规则性拒绝比范围性拒绝更"硬"，先判它能让玩家更快明白是规则问题 */\n"
     "    if (newLevelCellBlocked(r, c)) return 0;\n"
     "    if (gCurLevel >= 0 && gCurLevel < LV_COUNT) {\n"
     "        const LevelDef *ld = &levelDefs[gCurLevel];\n"
     "        int lo = ld->colFrom, hi = ld->colTo;"),

    # 3. resetGame 里初始化
    ("关卡初始化",
     "    gTime = 0; gWave = 0; gSel = -1; gShovel = 0;",
     "    gTime = 0; gWave = 0; gSel = -1; gShovel = 0;\n"
     "    newLevelInit();               /* 新五关的场地规则（薄冰/裂隙/迷雾…） */"),

    # 4. 僵尸更新：击退 + 速度修正
    ("僵尸更新钩子",
     "        z->anim += dt;\n"
     "        if (z->flash > 0) z->flash -= dt;\n"
     "        if (z->slow  > 0) z->slow  -= dt;",
     "        z->anim += dt;\n"
     "        /* 冰面击退：用 flash 的**上升沿**检测"这一帧刚被命中"。\n"
     "           为什么不直接在每个伤害点加击退：伤害来源有十几处\n"
     "           （豌豆/西瓜/闪电/雷击/裂隙/陨石…），逐处修改既容易漏，\n"
     "           又会把玩家所有攻击方式耦合到关卡规则上。\n"
     "           这里检测一次，任何来源的伤害都会触发击退。 */\n"
     "        if (z->flash > 0.0f && z->flashPrev <= 0.0f) newLevelKnockback(z);\n"
     "        z->flashPrev = z->flash;\n"
     "        if (z->flash > 0) z->flash -= dt;\n"
     "        if (z->slow  > 0) z->slow  -= dt;"),

    # 5. drawZombie 迷雾隐藏
    ("迷雾隐藏",
     "static void drawZombie(HDC dc, Zombie *z)\n{\n",
     "static void drawZombie(HDC dc, Zombie *z)\n{\n"
     "    /* 关 8：迷雾中的僵尸不渲染。设计上玩家仍能用范围攻击打中它们，\n"
     "       只是"看不见"—— 这一关考的就是预设火力网而非反应速度。 */\n"
     "    if (newLevelZombieHidden(z)) return;\n"),

    # 6. updateGame 里推进规则
    ("规则推进",
     "    traitsRecalc();          /* 羁绊已移除，保留空实现 */",
     "    newLevelUpdate(dt);      /* 新五关的场地规则推进（裂隙/迷雾/阶段/陨石） */\n"
     "    traitsRecalc();          /* 羁绊已移除，保留空实现 */"),

    # 7. 僵尸速度乘关卡修正
    ("速度修正",
     "        if (z->hp <= 0.0f) { zombieDie(z, 0); continue; }",
     "        if (z->hp <= 0.0f) { zombieDie(z, 0); continue; }\n"
     "        /* 关卡级速度修正（关7 热浪累积 / 关10 引力加速） */\n"
     "        { float km = newLevelSpeedMul(); if (km != 1.0f) z->x -= z->speed * (km - 1.0f) * dt; }"),

    # 8. Zombie 结构体新增 flashPrev
    ("结构体字段",
     "    float flash;      /* 受击闪白 */",
     "    float flash;      /* 受击闪白 */\n"
     "    float flashPrev;  /* 上一帧的 flash：用上升沿检测"刚被命中"（冰面击退用） */"),
]

# 地形叠加绘制（插到 drawTerrainOverlay 末尾）
TERRAIN_DRAW = r'''
    /* ================= 新五关的场地可视元素 =================
       规则必须"可读"：玩家要能一眼看出哪格危险、哪片看不见。
       这里的绘制全部走世界层（AlphaBlend 系不受变换影响，已按 SS 换算）。 */
    if (gRuleKind != RULE_NONE) {
        int rr, cc;
        /* 关 6：薄冰格 —— 不能种，必须看得见 */
        if (gRuleKind == RULE_ICE) {
            for (rr = 0; rr < ROWS; rr++) for (cc = 0; cc < COLS; cc++) {
                float x0, y0;
                if (!gThinIce[rr][cc]) continue;
                x0 = (float)LAWN_X + (float)cc * (float)CELL_W;
                y0 = (float)LAWN_Y + (float)rr * (float)CELL_H;
                fillRect(dc, x0 + 6, y0 + 6, x0 + (float)CELL_W - 6, y0 + (float)CELL_H - 6,
                         RGB(176, 216, 240));
                strokeRound(dc, x0 + 6, y0 + 6, x0 + (float)CELL_W - 6,
                            y0 + (float)CELL_H - 6, 6, RGB(228, 246, 255), 2);
            }
        }
        /* 关 7：裂隙格（预警时闪红，喷发时高亮） */
        if (gRuleKind == RULE_RIFT) {
            for (rr = 0; rr < 3; rr++) {
                float x0, y0;
                COLORREF col;
                int a;
                if (gRiftR[rr] < 0 || gRiftR[rr] >= ROWS) continue;
                x0 = (float)LAWN_X + (float)gRiftC[rr] * (float)CELL_W;
                y0 = (float)LAWN_Y + (float)gRiftR[rr] * (float)CELL_H;
                if (gRiftBlast > 0.0f)      col = RGB(255, 232, 120);
                else if (gRiftWarn > 0.0f)  col = RGB(255, 128, 64);
                else                        col = RGB(150, 62, 30);
                a = (gRiftWarn > 0.0f || gRiftBlast > 0.0f) ? 200 : 90;
                (void)a;
                fillRect(dc, x0 + 8, y0 + 8, x0 + (float)CELL_W - 8, y0 + (float)CELL_H - 8, col);
                strokeRound(dc, x0 + 8, y0 + 8, x0 + (float)CELL_W - 8,
                            y0 + (float)CELL_H - 8, 5, RGB(255, 210, 140), 2);
            }
        }
        /* 关 8：迷雾覆盖右侧若干列 */
        if (gRuleKind == RULE_FOG) {
            float fx = (float)LAWN_X + (float)gFogFromCol * (float)CELL_W;
            float fw = (float)(COLS - gFogFromCol) * (float)CELL_W;
            int y;
            int alpha = (gFogRevealT > 0.0f) ? 70 : 205;
            if (!gBlackBrush) gBlackBrush = CreateSolidBrush(RGB(0, 0, 0));
            for (y = (int)LAWN_Y; y < (int)(LAWN_Y + ROWS * CELL_H); y += 3) {
                fillRectA(dc, fx, (float)y, fx + fw, (float)(y + 2),
                          RGB(46, 34, 68), alpha);
            }
            strokeRound(dc, fx, (float)LAWN_Y, fx + fw,
                        (float)(LAWN_Y + ROWS * CELL_H), 4, RGB(120, 96, 170), 2);
        }
        /* 关 9：电源节点 + 已连通范围提示 */
        if (gRuleKind == RULE_POWER) {
            for (rr = 0; rr < ROWS; rr++) {
                float x0 = (float)LAWN_X;
                float y0 = (float)LAWN_Y + (float)rr * (float)CELL_H;
                int run = 0, k;
                for (k = 0; k < COLS; k++) {
                    if (grid[rr][k].alive || grid2[rr][k].alive) run++;
                    else break;
                }
                fillRect(dc, x0 - 6, y0 + 34, x0 - 2, y0 + (float)CELL_H - 34,
                         gPowerCutRow[rr] > 0 ? RGB(200, 60, 60) : RGB(90, 240, 210));
                /* 断链点用一个醒目的缺口标出来 */
                if (run < COLS) {
                    float gx = x0 + (float)run * (float)CELL_W;
                    fillRect(dc, gx + 34, y0 + 40, gx + 62, y0 + (float)CELL_H - 40,
                             RGB(240, 90, 90));
                }
            }
        }
        /* 关 10：阶段提示条 */
        if (gRuleKind == RULE_ASTRAL) {
            COLORREF pc = (gPhase == 1) ? RGB(120, 150, 255)
                        : (gPhase == 2) ? RGB(255, 170, 80)
                                        : RGB(200, 90, 240);
            fillRect(dc, (float)LAWN_X, (float)LAWN_Y - 12,
                     (float)(LAWN_X + COLS * CELL_W), (float)LAWN_Y - 6, pc);
        }
    }
'''

# fillRectA 辅助（带 alpha 的填充）
HELPER = r'''
/* 带 alpha 的矩形填充。
   世界层里 fillRect 是不透明的，而迷雾必须是半透明的
   （玩家要能隐约看到地形，否则分不清是"没画"还是"看不见"）。 */
static void fillRectA(HDC dc, float l, float t, float r, float b,
                      COLORREF col, int alpha)
{
    HDC tmp = CreateCompatibleDC(dc);
    HBITMAP bm, old;
    RECT rc;
    HBRUSH br;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    rc.left = 0; rc.top = 0; rc.right = (LONG)(r - l); rc.bottom = (LONG)(b - t);
    if (rc.right <= 0 || rc.bottom <= 0) { DeleteDC(tmp); return; }
    bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    old = (HBITMAP)SelectObject(tmp, bm);
    br = CreateSolidBrush(col);
    FillRect(tmp, &rc, br);
    DeleteObject(br);
    {
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
        bf.SourceConstantAlpha = (BYTE)alpha; bf.AlphaFormat = 0;
        AlphaBlend(dc, (int)l, (int)t, rc.right, rc.bottom, tmp, 0, 0,
                   rc.right, rc.bottom, bf);
    }
    SelectObject(tmp, old);
    DeleteObject(bm);
    DeleteDC(tmp);
}
'''


def main():
    s = open(P, encoding="utf-8").read()
    n0 = len(s)

    # 模块本体插到 render() 之前（那时所有全局量都已可见）
    anchor = "static void render(HDC out)\n{"
    assert anchor in s, "找不到 render 锚点"
    assert s.count(anchor) == 1, "render 锚点不唯一"
    s = s.replace(anchor, MODULE + "\n" + HELPER + "\n" + anchor, 1)
    print("✔ 规则模块已插入")

    for name, old, new in HOOKS:
        if old not in s:
            print("✗ 未找到挂钩：%s" % name)
            return 1
        if s.count(old) != 1:
            print("✗ 挂钩不唯一（%d 处）：%s" % (s.count(old), name))
            return 1
        s = s.replace(old, new, 1)
        print("✔ 挂钩：%s" % name)

    # 地形叠加绘制：插到 drawTerrainOverlay 的收尾之前
    #   该函数最后一段是主题淡出，找一个稳定锚点
    t_anchor = "/* 顶部 HUD */"
    if t_anchor in s:
        s = s.replace(t_anchor, TERRAIN_DRAW + "\n" + t_anchor, 1)
        print("✔ 地形叠加绘制已插入")
    else:
        print("✗ 找不到地形锚点")
        return 1

    open(P, "w", encoding="utf-8", newline="").write(s)
    print("\n写回完成：%d -> %d 字节" % (n0, len(s)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
