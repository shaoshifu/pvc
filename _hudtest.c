/* =========================================================================
   Plants vs Zombies  ——  C 语言复刻版
   -------------------------------------------------------------------------
   纯 C + Win32 GDI，零第三方依赖，单文件。
   渲染采用 2 倍超采样后降采样，获得抗锯齿效果。

   编译：
       gcc -O2 -mwindows -o pvz.exe pvz.c -lgdi32 -luser32 -lm
   ========================================================================= */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _USE_MATH_DEFINES
#include <windows.h>
#include <stdio.h>      /* _wfopen / FILE：存档读写 */
#ifdef __TINYC__
/* TinyCC 自带的 math.h 在 x86-64 下方有内联汇编的已知缺陷，这里绕开它。
   游戏内统一使用 double 版数学函数（自动转 float），对所有编译器都通用。 */
double sin(double x); double cos(double x); double sqrt(double x);
double floor(double x); double fabs(double x);
#else
#include <math.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

/* ---------------------------------------------------------------- 基本参数 */
#define VIEW_W      1000
#define VIEW_H      650
#define SS          2              /* 超采样倍数 */

#define COLS        9
#define ROWS        5
#define CELL_W      96
#define CELL_H      100
#define LAWN_X      62
#define LAWN_Y      104

#define MAX_ZOMBIES   160
#define MAX_PEAS      160
#define MAX_SUNS      80
#define MAX_PARTS     500
#define MAX_SPAWNS    160
#define MAX_WAVES     20

#define CLAMP(v,a,b) ((v)<(a)?(a):((v)>(b)?(b):(v)))
#define LERP(a,b,t)  ((a)+((b)-(a))*(t))

/* 随机浮点 [a,b) */
static float rnd(float a, float b) { return a + (b - a) * ((float)rand() / (float)RAND_MAX); }
static int   rndi(int a, int b)    { return (b <= a) ? a : (a + rand() % (b - a)); }

/* --------------------------------------------------------- GDI 对象缓存层 */
#define NCACHE 512
static HBRUSH  gBrush[NCACHE]; static COLORREF gBrushKey[NCACHE]; static int gBrushN;
static HPEN    gPen[NCACHE];   static DWORD      gPenKey[NCACHE];  static int gPenN;

static HBRUSH getBrush(COLORREF c)
{
    int i;
    for (i = 0; i < gBrushN; i++) if (gBrushKey[i] == c) return gBrush[i];
    if (gBrushN >= NCACHE) return gBrush[0];
    gBrushKey[gBrushN] = c;
    gBrush[gBrushN] = CreateSolidBrush(c);
    return gBrush[gBrushN++];
}
static HPEN getPen(COLORREF c, int w)
{
    DWORD k = ((DWORD)w << 24) ^ (DWORD)c;
    int i;
    if (w < 1) w = 1;
    for (i = 0; i < gPenN; i++) if (gPenKey[i] == k) return gPen[i];
    if (gPenN >= NCACHE) return gPen[0];
    gPenKey[gPenN] = k;
    gPen[gPenN] = CreatePen(PS_SOLID, w, c);
    return gPen[gPenN++];
}

/* ------------------------------------------------------------ 绘图小工具 */
static void fillCircle(HDC dc, float x, float y, float r, COLORREF c)
{
    SelectObject(dc, getBrush(c));
    SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, (int)(x - r), (int)(y - r), (int)(x + r + 1), (int)(y + r + 1));
}
static void fillEllipse(HDC dc, float x, float y, float rx, float ry, COLORREF c)
{
    SelectObject(dc, getBrush(c));
    SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, (int)(x - rx), (int)(y - ry), (int)(x + rx + 1), (int)(y + ry + 1));
}
static void fillRect(HDC dc, float l, float t, float r, float b, COLORREF c)
{
    RECT rc; rc.left = (int)l; rc.top = (int)t; rc.right = (int)r; rc.bottom = (int)b;
    FillRect(dc, &rc, getBrush(c));
}
static void strokeRect(HDC dc, float l, float t, float r, float b, COLORREF c, int w)
{
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    SelectObject(dc, getPen(c, w));
    Rectangle(dc, (int)l, (int)t, (int)r + 1, (int)b + 1);
}
static void fillRound(HDC dc, float l, float t, float r, float b, float rad, COLORREF c)
{
    SelectObject(dc, getBrush(c));
    SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, (int)l, (int)t, (int)r, (int)b, (int)(rad * 2), (int)(rad * 2));
}
static void strokeRound(HDC dc, float l, float t, float r, float b, float rad, COLORREF c, int w)
{
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    SelectObject(dc, getPen(c, w));
    RoundRect(dc, (int)l, (int)t, (int)r, (int)b, (int)(rad * 2), (int)(rad * 2));
}
static void line(HDC dc, float x1, float y1, float x2, float y2, COLORREF c, int w)
{
    SelectObject(dc, getPen(c, w));
    MoveToEx(dc, (int)x1, (int)y1, NULL);
    LineTo(dc, (int)x2, (int)y2);
}
static void poly(HDC dc, const POINT *p, int n, COLORREF c, int outline, COLORREF oc, int w)
{
    SelectObject(dc, getBrush(c));
    SelectObject(dc, outline ? getPen(oc, w) : (HPEN)GetStockObject(NULL_PEN));
    Polygon(dc, p, n);
}
static void fillTriangle(HDC dc, float x1, float y1, float x2, float y2, float x3, float y3, COLORREF c)
{
    POINT p[3];
    p[0].x = (int)x1; p[0].y = (int)y1;
    p[1].x = (int)x2; p[1].y = (int)y2;
    p[2].x = (int)x3; p[2].y = (int)y3;
    poly(dc, p, 3, c, 0, 0, 0);
}

/* 颜色微调：趋近白(f>0)或黑(f<0) */
static COLORREF tint(COLORREF c, float f)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    if (f >= 0) {
        r = r + (int)((255 - r) * f); g = g + (int)((255 - g) * f); b = b + (int)((255 - b) * f);
    } else {
        r = r + (int)(r * f); g = g + (int)(g * f); b = b + (int)(b * f);
    }
    return RGB(CLAMP(r,0,255), CLAMP(g,0,255), CLAMP(b,0,255));
}

/* ------------------------------------------------------------------ 字体 */
static HFONT gF15, gF18, gF22, gF30, gF54, gF72;
static HFONT mkFont(int px, int bold, const wchar_t *face)
{
    return CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}
static void putText(HDC dc, HFONT f, COLORREF c, const wchar_t *s, float x, float y, int align)
{
    SIZE sz; RECT rc;
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    if (align == 1) x -= sz.cx * 0.5f;
    else if (align == 2) x -= sz.cx;
    rc.left = (int)x; rc.top = (int)y; rc.right = rc.left + sz.cx + 4; rc.bottom = rc.top + sz.cy + 4;
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP);
}
static void putTextS(HDC dc, HFONT f, COLORREF c, COLORREF sh, const wchar_t *s, float x, float y, int align)
{
    putText(dc, f, sh, s, x + 2, y + 2, align);
    putText(dc, f, c, s, x, y, align);
}
static void putTextCS(HDC dc, HFONT f, COLORREF c, COLORREF sh, const wchar_t *s, float cx, float cy)
{
    SIZE sz;
    SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    putText(dc, f, sh, s, cx - sz.cx * 0.5f + 2, cy - sz.cy * 0.5f + 2, 0);
    putText(dc, f, c, s, cx - sz.cx * 0.5f, cy - sz.cy * 0.5f, 0);
}

/* ======================================================================== */
/*                                 游戏数据                                  */
/* ======================================================================== */

/* 植物类型 */
enum { PT_SUNFLOWER, PT_PEASHOOTER, PT_WALLNUT, PT_POTATOMINE,
       PT_SNOWPEA, PT_REPEATER, PT_CHERRY, PT_JALAPENO,
       PT_THREEPEATER, PT_SPIKEWEED, PT_MAGNET, PT_KERNELPULT,
       PT_STARFRUIT, PT_CACTUS, PT_SPLITPEA, PT_REED, PT_BLOOMERANG,
       PT_FUME, PT_LASERBEAN, PT_MELONPULT, PT_WINTERMELON, PT_GLOOM,
       PT_GOLDMAGNET, PT_TWINFLOWER, PT_MARROW, PT_PUMPKIN, PT_GARLIC,
       PT_HYPNOSHROOM, PT_ICESHROOM, PT_TANGLEKELP, PT_COBCANNON, PT_CATTAIL,
       /* 神级植物：不在抽卡池里，只在三选一里以"植物卡"形式出现。
          强度线：稀有 ≥2 倍普通 / 史诗 ≥3 倍 / 传奇 ≥4 倍 / 神话 ≥6 倍 */
       PT_HERO_FLAME,      /* 稀有：烈焰射手 */
       PT_HERO_IRONNUT,    /* 稀有：钢铁坚果 */
       PT_HERO_CRYSTAL,    /* 史诗：晶簇射手 */
       PT_HERO_THORNVINE,  /* 史诗：荆棘藤蔓 */
       PT_HERO_SUNGOD,     /* 传奇：太阳神花 */
       PT_HERO_FROST,      /* 传奇：霜之哀伤 */
       PT_HERO_WORLDTREE,  /* 神话：世界树 */
       PT_HERO_DOOM,       /* 神话：末日花 */
       PT_HERO_COUNT,
       PT_COUNT = PT_HERO_COUNT };

/* 僵尸类型 */
enum { ZT_NORMAL, ZT_CONE, ZT_BUCKET, ZT_FLAG,
       ZT_DANCER, ZT_ZAMBONI, ZT_BALLOON,
       /* 第二批：每种针对一种玩家策略做反制，用兵种相克加难度 */
       ZT_VAULTER,     /* 撑杆：跳过第一株植物 -> 克制前排坚果流 */
       ZT_SCREENDOOR,  /* 铁门：正面伤害减半 -> 逼你上磁力菇或爆炸 */
       ZT_FOOTBALL,    /* 橄榄球：高血高速 + 减速抗性 -> 逼你堆真实 DPS */
       ZT_NEWSPAPER,   /* 读报：破报后暴走 -> 打断"慢慢磨"的节奏 */
       ZT_DIGGER,      /* 挖掘：直接从最右列钻出来 -> 无视前排防线 */
       ZT_GIANT,       /* 巨人 BOSS：一击砸碎植物 -> 逼你集火 */
       ZT_COUNT };

typedef struct {
    const wchar_t *name;
    int   cost;
    float cooldown;
    float hp;
    unsigned char rarity;   /* 0 普通 1 稀有 2 史诗 3 传奇 4 神话 */
    unsigned char hero;     /* 1 = 神级植物：只在三选一里出现，不进抽卡池 */
    const wchar_t *desc;    /* 神级植物卡上显示的效果说明 */
} PlantDef;

static const PlantDef plantDefs[PT_COUNT] = {
    /* 向日葵     */ { L"向日葵",    50,  5.0f,  300,  0, 0, NULL },
    /* 豌豆射手   */ { L"豌豆射手", 100, 7.5f,  300,  0, 0, NULL },
    /* 坚果墙     */ { L"坚果墙",    50,  30.0f, 4000, 0, 0, NULL },
    /* 土豆雷     */ { L"土豆雷",    25,  30.0f, 300,  0, 0, NULL },
    /* 寒冰射手   */ { L"寒冰射手", 175, 7.5f,  300,  1, 0, NULL },
    /* 双发射手   */ { L"双发射手", 200, 7.5f,  300,  1, 0, NULL },
    /* 樱桃炸弹   */ { L"樱桃炸弹", 150, 50.0f, 0,    1, 0, NULL },
    /* 火爆辣椒   */ { L"火爆辣椒", 125, 50.0f, 0,    1, 0, NULL },
    /* 三线射手   */ { L"三线射手", 325, 12.0f, 300,  2, 0, NULL },
    /* 地刺       */ { L"地刺",      100, 12.0f, 600,  1, 0, NULL },
    /* 磁力菇     */ { L"磁力菇",    100, 20.0f, 300,  2, 0, NULL },
    /* 玉米投手   */ { L"玉米投手", 100, 8.0f,  300,  0, 0, NULL },
    { L"杨桃射手", 125, 8.0f,  300,  0, 0, L"一次射出五向星弹：本行 22，上下行各 14" },
    { L"仙人掌",   125, 10.0f, 450,  0, 0, L"穿刺针 28 伤害，对气球僵尸伤害 ×3" },
    { L"裂荚射手", 175, 9.0f,  320,  1, 0, L"前后各打一发：前方 24，后方 18" },
    { L"闪电芦苇", 175, 9.0f,  280,  1, 0, L"电弧 18 伤害，命中后最多弹射 3 个目标" },
    { L"回旋花",   175, 10.0f, 300,  1, 0, L"回旋镖去程与回程各打一次，单段 20" },
    { L"喷菇",     75,  7.5f,  280,  0, 0, L"短距穿透烟 16，无视铁门减伤" },
    { L"激光豆",   225, 12.0f, 320,  1, 0, L"整行瞬发激光，单发 40" },
    { L"西瓜投手", 300, 14.0f, 380,  2, 0, L"抛物线西瓜 70，邻行溅射 28" },
    { L"冰瓜投手", 400, 16.0f, 400,  2, 0, L"冰西瓜 70 + 溅射，并减速 2.2 秒" },
    { L"忧郁菇",   175, 12.0f, 350,  1, 0, L"周围 8 格脉冲孢子，每发 22" },
    { L"吸金菇",   50,  8.0f,  280,  0, 0, L"每 9 秒掉 15 阳光和 2 金气" },
    { L"双子葵",   150, 8.0f,  280,  1, 0, L"每次同时产出两颗阳光" },
    { L"骨髓花",   75,  10.0f, 260,  0, 0, L"邻格植物伤害 +18%" },
    { L"南瓜罩",   125, 22.0f, 2200, 1, 0, L"给本格已有植物套 2200 护盾；空格则当墙" },
    { L"大蒜",     50,  18.0f, 900,  0, 0, L"被啃一口就把该僵尸赶到邻行" },
    { L"迷幻菇",   75,  30.0f, 220,  1, 0, L"被啃时立刻消灭该僵尸并掉 50 阳光" },
    { L"寒冰菇",   75,  50.0f, 0,    1, 0, L"1 秒后全屏冻结 4 秒" },
    { L"缠绕水草", 25,  30.0f, 180,  0, 0, L"接触即吞掉 1 只地面僵尸" },
    { L"玉米加农", 500, 40.0f, 500,  2, 0, L"每 18 秒轰一发 520 伤害的范围弹" },
    { L"香蒲",     225, 14.0f, 340,  2, 0, L"追踪钉 26，优先打击气球僵尸" },
    /* ---- 神级 ---- */
    { L"烈焰射手", 250, 8.0f,  500,   1, 1, L"三行火焰弹，每发 58 伤害，可穿透 2 个目标并留下火焰" },
    { L"钢铁坚果", 150, 28.0f, 12000, 1, 1, L"血量 12000（普通坚果 4000），被啃食时每秒反伤 60" },
    { L"晶簇射手", 400, 10.0f, 600,   2, 1, L"上下三行齐射，每发 55 伤害（普通三线为 18）" },
    { L"荆棘藤蔓", 300, 24.0f, 1500,  2, 1, L"覆盖所在行全部 9 格，每秒 130 伤害（普通地刺 45）" },
    { L"太阳神花", 350, 18.0f, 900,   3, 1, L"每次产出 180 阳光（普通向日葵 25），每 8 秒治疗全部植物 400" },
    { L"霜之哀伤", 400, 14.0f, 700,   3, 1, L"每 4.5 秒冻结全屏僵尸 3 秒，自身发射高伤冰弹" },
    { L"世界树",   600, 30.0f, 3000,  4, 1, L"3x3 范围每秒 200 伤害，同时持续治疗范围内植物" },
    { L"末日花",   650, 26.0f, 2200,  4, 1, L"自动发射追踪弹，单发 280 伤害，无视距离" },
};

static int         zCost[ZT_COUNT]  = { 1, 2, 4, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   /* 0 = 尚未解锁 */
/* 血量整体上调 30%：移除羁绊后玩家 DPS 下降，僵尸血量同步补上强度 */
static const float zHp[ZT_COUNT]    = { 260.0f, 730.0f, 1430.0f, 260.0f,
                                         420.0f, 1760.0f, 220.0f,
                                         340.0f, 620.0f, 1950.0f,
                                         390.0f, 550.0f, 5460.0f };
/* 速度上调 12%：整体节奏更紧 */
static const float zSpeed[ZT_COUNT] = { 15.7f, 15.7f, 15.7f, 21.3f,
                                         13.4f, 24.6f, 16.8f,
                                         20.2f, 12.3f, 26.9f,
                                         11.2f, 15.7f, 7.8f };

/* ======================================================================== */
/*                     存档 / 关卡 / 天赋 / 成就（元进程）                     */
/* ======================================================================== */
/* 这一层是「天赋 / 成就 / 抽卡」的共同地基 —— 没有存档，那三个只能是摆设。 */

#define SAVE_FILE   L"pvz_save.dat"
#define SAVE_MAGIC  0x5A565031u      /* 'PVZ1' */
#define SAVE_VER    4       /* v4: + 等级/经验/纪录/每日营地；旧档按字段前缀迁移 */

/* ---- 关卡 ---- */
#define LV_COUNT 6
typedef struct {
    const wchar_t *name;
    const wchar_t *desc;
    int   waves;
    int   startSun;
    int   lawnVariant;      /* 0 普通 1 夜间 2 水中 3 屋顶 4 沙漠 */
    int   colFrom, colTo;   /* 可种植的列范围 */
    float hpMul;            /* 僵尸血量倍率 */
    float spdMul;           /* 僵尸速度倍率 */
    float sunMul;           /* 天空阳光间隔倍率（越小越勤） */
    int   starReq;          /* 解锁所需星星 */
    int   zwStart, zwEnd;   /* 僵尸种类解锁波次区间（按关内波数比例） */
} LevelDef;

static const LevelDef levelDefs[LV_COUNT] = {
    { L"前院 · 白天", L"标准草坪，适合熟悉节奏",   20, 150, 0, 0, 9, 1.00f, 1.00f, 1.00f, 0,  0, 0 },
    { L"后院 · 泳池", L"只能种在右侧的泥地上",     22, 175, 2, 3, 9, 1.05f, 1.00f, 1.00f, 3,  1, 3 },
    { L"月夜 · 墓园", L"夜间阳光稀少，僵尸更快",   20, 200, 1, 0, 9, 1.10f, 1.10f, 1.55f, 6,  2, 5 },
    { L"屋顶 · 天台", L"瓦片上的阵地战",           24, 150, 3, 0, 9, 1.25f, 1.05f, 1.00f, 10, 3, 6 },
    { L"沙漠 · 遗迹", L"烈日下阳光来得慢",         26, 225, 4, 0, 9, 1.40f, 1.10f, 1.45f, 14, 4, 6 },
    { L"无尽 · 挑战", L"没有尽头，看你能撑多久",    999, 200, 0, 0, 9, 1.20f, 1.05f, 1.00f, 20, 5, 6 },
};

/* ---- 天赋树：3 分支 x 4 级 ---- */
#define TAL_COUNT 12
static const wchar_t *talName[TAL_COUNT] = {
    L"园艺 I",   L"园艺 II",  L"园艺 III", L"园艺 IV",
    L"战斗 I",   L"战斗 II",  L"战斗 III", L"战斗 IV",
    L"坚韧 I",   L"坚韧 II",  L"坚韧 III", L"坚韧 IV" };
static const wchar_t *talDesc[TAL_COUNT] = {
    L"开局阳光 +25",       L"向日葵费用 -8%",     L"阳光产出 +12%",      L"开局阳光再 +50",
    L"豌豆伤害 +6%",       L"卡片冷却 -8%",       L"爆炸伤害 +18%",      L"豌豆伤害再 +10%",
    L"植物最大生命 +10%", L"小推车用后 20 秒补回", L"僵尸速度 -6%",      L"植物最大生命再 +15%" };
static const int talCost[TAL_COUNT] = { 2, 3, 5, 8,  2, 4, 6, 9,  2, 4, 6, 9 };
static const int talCat[TAL_COUNT]  = { 0, 0, 0, 0,  1, 1, 1, 1,  2, 2, 2, 2 };

/* ---- 成就 ---- */
#define ACH_COUNT 20
static const wchar_t *achName[ACH_COUNT] = {
    L"初战告捷",   L"百人斩",     L"千尸之王",   L"零伤亡",
    L"一命通关",   L"收藏家",     L"构筑大师",   L"富甲一方",
    L"寒冰使者",   L"爆破专家",   L"三线齐发",   L"夜幕行者",
    L"水中花园",   L"屋顶守卫",   L"沙漠行者",   L"无尽之路",
    L"融合实验",   L"天赋初开",   L"满级天赋",   L"抽卡新手" };
static const wchar_t *achDesc[ACH_COUNT] = {
    L"首次通关任意关卡",     L"单局击杀 100 只僵尸",  L"累计击杀 1000 只",
    L"不损失任何小推车通关", L"不用小推车且零植物损失通关", L"解锁全部 12 种植物",
    L"单局获得 12 项以上遗物", L"单局结束持有 3000 阳光",
    L"用寒冰效果冻结 100 只僵尸", L"用爆炸炸死 200 只僵尸",
    L"用三线射手击杀 50 只",  L"通关月夜墓园",      L"通关后院泳池",
    L"通关屋顶天台",         L"通关沙漠遗迹",       L"无尽模式撑过 30 波",
    L"完成一次植物融合",     L"点亮任意一个天赋",    L"点亮全部 12 个天赋",
    L"抽卡 10 次" };
/* 每关通关基础星星；成就额外给星 */
#define STAR_PER_WIN  2
#define STAR_ACH      3

/* ---- 存档结构 ---- */
typedef struct {
    unsigned int magic, ver;
    int   stars;                    /* 星星：买天赋 */
    int   coins;                    /* 金币：抽卡 */
    int   lvStars[LV_COUNT];        /* 每关最高星级 0..3 */
    int   lvUnlocked[LV_COUNT];
    int   loadout[PT_COUNT];     /* 出战编组（1 = 带上场），最多 gCardSlots 张 */
    int   plantOwned[PT_COUNT];     /* 抽卡解锁的植物 */
    int   talents[TAL_COUNT];       /* 天赋等级 0/1 */
    int   ach[ACH_COUNT];
    int   bestWave;
    int   gachaCount;
    int   totalKills;
    int   totalFreeze, totalBoom;
    int   fusionCount;
    /* v4：本地留存系统 —— 玩家等级 / 个人纪录 / 每日营地 */
    int   xp, level;             /* 玩家等级 / 当前等级内经验 */
    int   bestKills;             /* 单局最高击杀 */
    int   bestGold;              /* 单局最高金气 */
    int   bestTimeSec;           /* 最快通关秒数 */
    int   lastCheckIn;           /* 最近签到日 YYYYMMDD */
    int   checkStreak;           /* 连续签到天数 */
    int   dailyDoneDate;         /* 每日挑战完成日 YYYYMMDD */
    int   dailyBestWave;         /* 每日挑战历史最高波数 */
} SaveData;

static SaveData gSave;
static int      gAchPopup = -1;      /* 刚解锁的成就索引，-1 表示不显示 */
static float    gAchPopupT = 0.0f;

/* 存档路径。默认放在 exe 同目录；环境变量 PVZ_SAVE_FILE 可以覆盖它 ——
   无窗口测试程序必须设置这个变量，否则 saveFlush() 会直接覆盖玩家的真实存档。 */
static void savePath(wchar_t *out)
{
    const wchar_t *env = _wgetenv(L"PVZ_SAVE_FILE");
    if (env && env[0]) {
        wcsncpy(out, env, (size_t)MAX_PATH - 1);
        out[MAX_PATH - 1] = 0;
        return;
    }
    GetModuleFileNameW(NULL, out, MAX_PATH);
    { wchar_t *p = wcsrchr(out, L'\\'); if (p) p[1] = 0; }
    wcscat(out, SAVE_FILE);
}

static void saveFlush(void)
{
    wchar_t path[MAX_PATH];
    FILE *fp;
    savePath(path);
    fp = _wfopen(path, L"wb");
    if (!fp) return;
    fwrite(&gSave, sizeof(gSave), 1, fp);
    fclose(fp);
}

/* v2 存档：普通植物 12 + 神级 8 = 20。v3 在神级前插入了 20 株新植物。 */
typedef struct {
    unsigned int magic, ver;
    int   stars, coins;
    int   lvStars[LV_COUNT];
    int   lvUnlocked[LV_COUNT];
    int   loadout[20];
    int   plantOwned[20];
    int   talents[TAL_COUNT];
    int   ach[ACH_COUNT];
    int   bestWave, gachaCount, totalKills, totalFreeze, totalBoom, fusionCount;
} SaveDataV2;

/* v3 存档：与 v3 时代的 SaveData 完全一致（loadout/plantOwned 已是 PT_COUNT）。
   v4 只在尾部追加字段，前缀布局没动，所以整块 memcpy 再补默认值即可。 */
typedef struct {
    unsigned int magic, ver;
    int   stars, coins;
    int   lvStars[LV_COUNT];
    int   lvUnlocked[LV_COUNT];
    int   loadout[PT_COUNT];
    int   plantOwned[PT_COUNT];
    int   talents[TAL_COUNT];
    int   ach[ACH_COUNT];
    int   bestWave, gachaCount, totalKills, totalFreeze, totalBoom, fusionCount;
} SaveDataV3;

static void saveResetNew(void)
{
    int t;
    memset(&gSave, 0, sizeof(gSave));
    gSave.magic = SAVE_MAGIC;
    gSave.ver   = SAVE_VER;
    gSave.level  = 1;
    gSave.coins  = 3;
    gSave.lvUnlocked[0] = 1;
    for (t = 0; t < PT_COUNT; t++)
        if (plantDefs[t].rarity == 0 && !plantDefs[t].hero) gSave.plantOwned[t] = 1;
}

static void saveLoad(void)
{
    wchar_t path[MAX_PATH];
    FILE *fp;
    unsigned char raw[4096];
    size_t nread = 0;
    memset(&gSave, 0, sizeof(gSave));
    savePath(path);
    fp = _wfopen(path, L"rb");
    if (fp) {
        nread = fread(raw, 1, sizeof(raw), fp);
        fclose(fp);
    }
    if (nread >= sizeof(SaveData) && ((SaveData *)raw)->magic == SAVE_MAGIC &&
        ((SaveData *)raw)->ver == SAVE_VER) {
        memcpy(&gSave, raw, sizeof(gSave));
        return;
    }
    if (nread >= sizeof(SaveDataV2) && ((SaveDataV2 *)raw)->magic == SAVE_MAGIC &&
        ((SaveDataV2 *)raw)->ver == 2) {
        SaveDataV2 old;
        int i;
        memcpy(&old, raw, sizeof(old));
        memset(&gSave, 0, sizeof(gSave));
        gSave.magic = SAVE_MAGIC; gSave.ver = SAVE_VER;
        gSave.stars = old.stars; gSave.coins = old.coins;
        memcpy(gSave.lvStars, old.lvStars, sizeof(gSave.lvStars));
        memcpy(gSave.lvUnlocked, old.lvUnlocked, sizeof(gSave.lvUnlocked));
        memcpy(gSave.talents, old.talents, sizeof(gSave.talents));
        memcpy(gSave.ach, old.ach, sizeof(gSave.ach));
        gSave.bestWave = old.bestWave; gSave.gachaCount = old.gachaCount;
        gSave.totalKills = old.totalKills; gSave.totalFreeze = old.totalFreeze;
        gSave.totalBoom = old.totalBoom; gSave.fusionCount = old.fusionCount;
        for (i = 0; i < 12; i++) {                 /* 旧普通植物 */
            gSave.loadout[i] = old.loadout[i];
            gSave.plantOwned[i] = old.plantOwned[i];
        }
        for (i = 0; i < 8; i++) {                  /* 旧神级 → 新神级槽 */
            gSave.loadout[PT_HERO_FLAME + i] = old.loadout[12 + i];
            gSave.plantOwned[PT_HERO_FLAME + i] = old.plantOwned[12 + i];
        }
        saveFlush();
        return;
    }
    if (nread >= sizeof(SaveDataV3) && ((SaveDataV3 *)raw)->magic == SAVE_MAGIC &&
        ((SaveDataV3 *)raw)->ver == 3) {
        SaveDataV3 old;
        memcpy(&old, raw, sizeof(old));
        memset(&gSave, 0, sizeof(gSave));
        gSave.magic = SAVE_MAGIC; gSave.ver = SAVE_VER;
        gSave.stars = old.stars; gSave.coins = old.coins;
        memcpy(gSave.lvStars, old.lvStars, sizeof(gSave.lvStars));
        memcpy(gSave.lvUnlocked, old.lvUnlocked, sizeof(gSave.lvUnlocked));
        memcpy(gSave.loadout, old.loadout, sizeof(gSave.loadout));
        memcpy(gSave.plantOwned, old.plantOwned, sizeof(gSave.plantOwned));
        memcpy(gSave.talents, old.talents, sizeof(gSave.talents));
        memcpy(gSave.ach, old.ach, sizeof(gSave.ach));
        gSave.bestWave = old.bestWave; gSave.gachaCount = old.gachaCount;
        gSave.totalKills = old.totalKills; gSave.totalFreeze = old.totalFreeze;
        gSave.totalBoom = old.totalBoom; gSave.fusionCount = old.fusionCount;
        gSave.level = 1; gSave.xp = 0;
        saveFlush();
        return;
    }
    saveResetNew();
    saveFlush();
}

/* ---------------- 本地留存系统：等级 / 签到 / 每日挑战 / 星期主题 ---------------- */
/* 全部离线、读系统时钟，不依赖任何网络 —— 目的是让玩家「每天都值得开一局」。 */
static int    gLvUpNew = 0;      /* 最近升到的等级，0 = 无 */
static float  gLvUpT   = 0.0f;   /* 升级横幅剩余显示时间 */
static float  gWeekGoldMul = 1.0f;   /* 星期主题：金气倍率 */
static int    gWeekSunAdd  = 0;      /* 星期主题：开局额外阳光 */
static const wchar_t *gWeekDesc = L"";
static int    gDailyMode = 0;        /* 1 = 本局是每日挑战 */
static int    gDailySeed = 0;        /* 当日挑战种子（由日期唯一决定） */
static int    gDailyTarget = 20;     /* 每日挑战目标波数 */
static float  gDailyHpMul = 1.0f, gDailySpdMul = 1.0f, gDailySunMul = 1.0f;
static int    gRecNew = 0;           /* 本局是否刷新了个人纪录 */

static int plantOwnedCount(void)
{
    int i, n = 0;
    for (i = 0; i < PT_COUNT; i++) if (gSave.plantOwned[i]) n++;
    return n;
}

/* 今天 / n 天前的 YYYYMMDD 整数（纯本地时间，方便与存档字段比大小） */
static int todayYMD(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}
static int ymdDaysAgo(int n)
{
    time_t t = time(NULL);
    struct tm *tmv;
    t -= (time_t)n * 86400L;
    tmv = localtime(&t);
    return (tmv->tm_year + 1900) * 10000 + (tmv->tm_mon + 1) * 100 + tmv->tm_mday;
}

/* 升到 lv+1 所需经验：曲线前缓后陡，前期给足正反馈 */
static int xpNeed(int lv) { return 50 + 30 * lv; }

/* 加经验，跨级自动升级；每次升级发星星/金币，弹升级横幅 */
static void gainXp(int n)
{
    if (n <= 0) return;
    gSave.xp += n;
    while (gSave.xp >= xpNeed(gSave.level)) {
        gSave.xp -= xpNeed(gSave.level);
        gSave.level++;
        gSave.stars += 2;             /* 升级奖励：星星 +2、金币 +3 */
        gSave.coins  += 3;
        gLvUpNew = gSave.level;
        gLvUpT   = 3.0f;
    }
    saveFlush();
}

/* 每日签到：给星星/金币，连续满 7 天发大奖 */
static void doCheckIn(void)
{
    int today = todayYMD();
    if (gSave.lastCheckIn == today) return;             /* 今天已签 */
    if (gSave.lastCheckIn == ymdDaysAgo(1)) gSave.checkStreak++;  /* 连续 */
    else                                    gSave.checkStreak = 1; /* 断签重来 */
    gSave.lastCheckIn = today;
    gSave.stars += 1;
    gSave.coins += 2;
    if (gSave.checkStreak >= 7) { gSave.stars += 3; gSave.coins += 5; }  /* 7 连大奖 */
    gainXp(15);
}

static void achUnlock(int id)
{
    if (id < 0 || id >= ACH_COUNT || gSave.ach[id]) return;
    gSave.ach[id] = 1;
    gSave.stars += STAR_ACH;
    gAchPopup = id;
    gAchPopupT = 3.2f;
    saveFlush();
}

/* 关卡是否可玩 */
static int levelPlayable(int i)
{
    if (i < 0 || i >= LV_COUNT) return 0;
    return gSave.lvUnlocked[i] || gSave.stars >= levelDefs[i].starReq;
}

/* ======================================================================== */
/*                     羁绊系统（云顶之弈式植物组合）                          */
/* ======================================================================== */
/* 统计「草坪上存活植物」的羁绊数量（重复计数，和云顶一样）。
   羁绊是被动的、永久的组合奖励 ——
   你不必抽到某张遗物才能组合，只要按羁绊摆植物就会生效。
   这解决了「植物之间没有配合」的问题：想吃到 4 档射手，就得真的摆 6 株射手，
   而摆 6 株射手意味着你放弃了其它羁绊 —— 这就是构筑取舍。 */

#define TR_COUNT 6
enum { TR_SHOOTER, TR_GARDEN, TR_BULWARK, TR_FROST, TR_DEMO, TR_MUTANT };

typedef struct {
    const wchar_t *name;
    const wchar_t *effect;       /* 每档效果说明（不含数值，数值在下面） */
    int   need[3];               /* 三档门槛，need[2]==0 表示只有两档 */
    COLORREF col;
} TraitDef;


/* 植物 -> 羁绊映射（每株最多 2 个，和云顶一样） */

static int gTraitCount[TR_COUNT];    /* 场上数量 */
static int gTraitTier[TR_COUNT];     /* -1 未激活，0/1/2 档位 */
/* 羁绊带来的修正量（traitsRecalc 统一重算） */
static float gTrShotRate = 1.0f, gTrGarden = 1.0f, gTrBulwark = 1.0f;
static float gTrFrost = 1.0f, gTrDemoDmg = 1.0f, gTrMutant = 1.0f;


static void traitsRecalc(void);   /* 实现在 grid 声明之后 */

/* ======================================================================== */
/*                          肉鸽系统（强化池 / 模式 / 武器 / 货币）             */
/* ======================================================================== */
/* 设计原则：随机只发生在「选择点」—— 你拿到什么遗物是随机的；
   一旦拿到，效果 100% 确定。绝不引入「一定概率打不中」这类破坏策略感的随机。 */
enum { R_NONE = 0,
    /* 经济 */
    R_START_SUN, R_START_SUN_BIG, R_SKY_SUN, R_SUNFLOWER, R_SUNFLOWER_BIG,
    R_KILL_SUN, R_KILL_SUN_BIG, R_INTEREST,
    R_BANKER,                       /* 银行家：解锁金气系统 */
    R_NECROMANCER,                  /* 摄魂者：解锁幽能系统 */
    /* 火力 */
    R_PEA_DMG, R_PEA_DMG_BIG, R_RATE, R_RATE_BIG, R_BIG_CALIBER,
    R_GLASS_CANNON, R_ARMOR_PIERCE, R_PIERCE, R_VOLLEY,
    R_WEAPON_RACK,                  /* 武器架：副射槽解锁 + 随机武器 */
    R_MAIN_UPGRADE,                 /* 主射升级：把主射也换武器 */
    R_WEAPON_MASTER,                /* 武器大师：双武器，攻击频次不变 */
    /* 生存 */
    R_PLANT_HP, R_PLANT_HP_BIG, R_THORNS, R_MEDIC, R_SPARE_TIRE,
    R_NO_MOWER, R_IRON_WALL,
    /* 控制 / 爆炸 */
    R_SLOW_LONG, R_ICE_AGE, R_FROST_CORE, R_BOOM_R, R_BOOM_DMG,
    R_EMBER, R_CHAIN_BOOM,
    /* 构筑 / 协同 */
    R_COLUMN, R_EMPTY, R_FLOWER_SEA, R_WALLNUT_DOGMA, R_KILL_STACK,
    R_SYMBIOSIS, R_VANGUARD, R_BACKLINE,
    /* 风险 / 取舍 */
    R_MINIMAL, R_NO_COOLDOWN, R_SUN_GAMBLE, R_BLOOD_PACT, R_ALL_IN,
    /* 模式 */
    R_MODE_SHIFT,                   /* 模式切换器：解锁第二种模式可手动切 */
    R_MUTANT_SEED,                  /* 变异种子：随机解锁一种新模式 */
    R_WATER_MODE,                   /* 水中模式：开局直接解锁水中 */
    R_NIGHT_MODE,                   /* 夜间模式：开局直接解锁夜间 */
    R_NINJA,                        /* 隐忍者：种下不立即结算冷却 */
    R_CHARGED,                      /* 充能心：充能上限 +1 */
    R_FURY,                         /* 愤怒：愤怒条涨得更快 + 可主动爆 */
    /* ---- 第二批：配合新植物 / 新僵尸 / 新关卡 ---- */
    R_SPIKE_DMG,        /* 尖刺淬毒：地刺伤害翻倍 */
    R_SPIKE_WIDE,       /* 尖刺蔓延：地刺同时伤害相邻两行 */
    R_MAGNET_FAST,      /* 强磁：磁力菇冷却减半 */
    R_MAGNET_GOLD,      /* 磁化：被吸走护具的僵尸掉落金气 */
    R_CORN_STUN,        /* 黄油：玉米投手眩晕时间翻倍 */
    R_CORN_DMG,         /* 爆米花：玉米伤害 +80% */
    R_THREE_UP,         /* 三线齐鸣：三线射手上下两行伤害不再衰减 */
    R_THREE_MID,        /* 中路压制：三线射手中间行伤害 +60% */
    R_BALLOON_POP,      /* 飞镖：对气球僵尸伤害 +150% */
    R_ZAMBONI_BREAK,    /* 破冰：冰车僵尸移动速度 -30% */
    R_DANCER_SILENCE,   /* 静音：舞王无法召唤伴舞 */
    R_NIGHT_SUN,        /* 夜灯：夜间关卡阳光产出不再减半 */
    R_ROOF_GRIP,        /* 防滑：屋顶关卡植物血量 +30% */
    R_DESERT_WATER,     /* 绿洲：沙漠关卡阳光掉落加速 40% */
    R_ENDLESS_HEAL,     /* 坚韧：每 3 波为受伤最重的植物回复 200 */
    R_STAR_HUNTER,      /* 猎星者：通关时额外获得 2 金币 */
    R_FUSION_MASTER,    /* 融合大师：合成后保留两只亲本的图鉴效果 */
    R_LOADOUT_PLUS,     /* 扩容卡组：出战卡槽 +2（可带 10 张） */
    R_FIRST_STRIKE,     /* 先手：第 1 波开始前获得 150 阳光 */
    R_LAST_STAND,       /* 背水：小推车全用完后，全植物伤害 +40% */
    R_COUNT
};

/* 类别：0 经济 1 火力 2 生存 3 控制 4 构筑 5 风险 */
typedef struct {
    const wchar_t *name;
    const wchar_t *desc;
    unsigned char  rarity;      /* 0 普通 1 稀有 2 传说 */
    unsigned char  cat;
} RelicDef;

static const RelicDef relicDefs[R_COUNT] = {
/*           名称          描述                                    稀有 类别 */
/* R_NONE */ { L"", L"", 0, 0 },
/* 经济 */
{ L"储蓄罐",     L"开局阳光 +50",                              0, 0 },
{ L"厚积薄发",   L"开局阳光 +120",                             1, 0 },
{ L"晴空",       L"天空阳光掉落间隔 -15%",                     0, 0 },
{ L"光合作用",   L"向日葵每次产出 +12 阳光",                   0, 0 },
{ L"花团锦簇",   L"向日葵产出 +25，但费用 +20%",               1, 0 },
{ L"拾荒者",     L"每击杀 1 只僵尸获得 +2 阳光",               0, 0 },
{ L"战利品",     L"每击杀 +5 阳光，但植物费用 +8%",            1, 0 },
{ L"利滚利",     L"每波结束时，按当前阳光的 8% 额外获得",      1, 0 },
/* 火力 */
{ L"磨刀石",     L"豌豆伤害 +12%",                             0, 1 },
{ L"精钢弹头",   L"豌豆伤害 +25%",                             1, 1 },
{ L"疾风",       L"所有射手射击间隔 -12%",                     0, 1 },
{ L"连弩",       L"射击间隔 -22%，但豌豆伤害 -10%",            1, 1 },
{ L"大口径",     L"豌豆伤害 +45%，射击间隔 +20%",              1, 1 },
{ L"玻璃大炮",   L"植物伤害 +80%，植物最大生命 -45%",         2, 1 },
{ L"破甲",       L"对路障 / 铁桶僵尸伤害 +60%",                1, 1 },
{ L"穿透弹",     L"豌豆命中后 25% 概率继续飞行再命中一次",     2, 1 },
{ L"齐射",       L"同一行 ≥4 株射手时，该行射击间隔 -30%",     1, 4 },
/* 生存 */
{ L"沃土",       L"植物最大生命 +15%",                         0, 2 },
{ L"巨人",       L"植物最大生命 +40%，卡片冷却 +15%",         1, 2 },
{ L"荆棘",       L"僵尸啃食植物时，每秒反受 22 伤害",          1, 2 },
{ L"前线医疗",   L"每 12 秒治疗血量最低的植物 120",            1, 2 },
{ L"备用轮胎",   L"小推车用掉后，30 秒自动补回",               2, 2 },
{ L"背水一战",   L"移除全部小推车，全植物伤害 +65%",           2, 5 },
{ L"铁壁",       L"坚果墙最大生命 +120%",                      0, 2 },
/* 控制 / 爆炸 */
{ L"霜冻",       L"减速持续时间 +50%",                         0, 3 },
{ L"冰河期",     L"减速持续时间翻倍，减速幅度提升",            1, 3 },
{ L"寒霜核心",   L"被减速的僵尸受到的所有伤害 +35%",           2, 4 },
{ L"火药桶",     L"爆炸半径 +20%",                             0, 3 },
{ L"烈性火药",   L"爆炸伤害 +55%",                             1, 3 },
{ L"余烬",       L"爆炸后原地留下 4 秒火焰，每秒 35 伤害",     2, 3 },
{ L"连锁引爆",   L"被爆炸炸死的僵尸会原地再爆一次",            2, 3 },
/* 构筑 / 协同 */
{ L"同列共鸣",   L"同一列 ≥3 株植物时，该列伤害 +25%",         1, 4 },
{ L"留白",       L"每个空单元格使相邻植物伤害 +7%",            1, 4 },
{ L"花海",       L"每株向日葵使所有植物最大生命 +6%",          1, 4 },
{ L"坚果教条",   L"每株存活的坚果墙使射手伤害 +9%",            1, 4 },
{ L"以战养战",   L"每击杀 1 只，全植物伤害本局永久 +0.6%（上限 +60%）", 2, 4 },
{ L"温床",       L"相邻有同类植物的，伤害 +8%",                1, 4 },
{ L"尖兵",       L"最左列植物伤害 +40%，其余列 -10%",          1, 4 },
{ L"纵深防御",   L"最右列植物伤害 +60%",                       1, 4 },
/* 风险 / 取舍 */
{ L"极简主义",   L"植物费用 -35%，但卡片冷却 +60%",            1, 5 },
{ L"无冷却",     L"卡片冷却 -45%，但植物最大生命 -30%",       1, 5 },
{ L"阳光豪赌",   L"开局阳光 -120，但全部阳光产出翻倍",         2, 5 },
{ L"血祭",       L"植物费用 -45%，但种下时损失 30% 生命",      1, 5 },
{ L"孤注一掷",   L"僵尸速度 +15%，但击杀阳光 x3",              1, 5 },
/* 模式 / 武器 / 货币 */
{ L"银行家",     L"解锁金气系统：击杀获得金气，可召唤增援",  1, 0 },
{ L"摄魂者",     L"解锁幽能系统：减速获得幽能，强化夜间",     1, 0 },
{ L"武器架",     L"为每株植物解锁副射槽，并随机给一种武器",  1, 1 },
{ L"主射升级",   L"把所有植物的主射也换成随机武器",            2, 1 },
{ L"武器大师",   L"每株植物拥有双武器，攻击频次不变",          2, 1 },
{ L"模式切换器", L"解锁第二种模式，游戏中可按 M 手动切换",  2, 4 },
{ L"变异种子",   L"立即随机解锁一种新的种植模式",              1, 4 },
{ L"水中模式",   L"游戏开始即解锁水中种植模式",                1, 4 },
{ L"夜间模式",   L"游戏开始即解锁夜间种植模式",                1, 4 },
{ L"隐忍者",     L"种下植物不立即结算冷却，下一次种才扣",     1, 4 },
{ L"充能心",     L"樱桃 / 辣椒的充能上限 +1，可放大招",       1, 4 },
{ L"愤怒",       L"愤怒条涨速 +50%，且可主动引爆全屏雷击",     2, 4 },
/* 第二批 */
{ L"尖刺淬毒",   L"地刺伤害翻倍",                              1, 1 },
{ L"尖刺蔓延",   L"地刺同时伤害相邻两行",                      1, 4 },
{ L"强磁",       L"磁力菇吸取间隔减半",                        1, 2 },
{ L"磁化",       L"护具被吸走的僵尸掉落 15 金气",              1, 0 },
{ L"黄油",       L"玉米投手眩晕时间翻倍",                      1, 3 },
{ L"爆米花",     L"玉米投手伤害 +80%",                         1, 1 },
{ L"三线齐鸣",   L"三线射手上下两行伤害不再衰减",              2, 4 },
{ L"中路压制",   L"三线射手中间行伤害 +60%",                   1, 4 },
{ L"飞镖",       L"对气球僵尸伤害 +150%",                      1, 1 },
{ L"破冰",       L"冰车僵尸移动速度 -30%",                     1, 3 },
{ L"静音",       L"舞王僵尸无法召唤伴舞",                      1, 3 },
{ L"夜灯",       L"光照术：所有关卡阳光掉落加速 25%",          1, 0 },
{ L"防滑",       L"所有植物最大生命 +30%",                     1, 2 },
{ L"绿洲",       L"阳光产出翻倍",                              2, 0 },
{ L"坚韧",       L"每 3 波治疗受伤最重的植物 200",             1, 2 },
{ L"猎星者",     L"击杀僵尸有 3% 概率掉落 1 金币",             1, 0 },
{ L"融合大师",   L"合成的混种植物效果 +35%",                   2, 4 },
{ L"扩容卡组",   L"出战卡槽 +2（可带 10 张）",                 2, 4 },
{ L"先手",       L"开局额外获得 150 阳光",                     1, 0 },
{ L"背水",       L"小推车全用完后，全植物伤害 +40%",           1, 5 },
};

static void openDraft(void);               /* 前向声明：updateGame 里要用 */

/* ---- 元界面（选关 / 天赋 / 成就 / 抽卡）几何 ---- */
#define META_TITLE_Y   62.0f
#define LV_CARD_W      292.0f
#define LV_CARD_H      96.0f
#define LV_COL_X0      58.0f
#define LV_ROW_Y0      108.0f
#define LV_GAP_X       12.0f
#define LV_GAP_Y       12.0f
#define TAL_NODE_W     132.0f
#define TAL_NODE_H     84.0f
#define TAL_X0         56.0f
#define TAL_Y0         128.0f
#define TAL_GAP_X      10.0f
#define TAL_GAP_Y      14.0f
#define GACHA_BTN_Y    300.0f

/* 大厅卡组编组 / 结算抽卡 的几何常量与原型（实现在抽卡模块里） */
#define LO_CARD_W   108.0f
#define LO_CARD_H   118.0f
#define LO_X0       28.0f
#define LO_Y0       108.0f
#define LO_GAP_X     8.0f
#define LO_GAP_Y     8.0f
#define LO_COLS        8
#define RW_CARD_W   200.0f
#define RW_CARD_H   268.0f
#define RW_GAP      40.0f
#define RW_Y        190.0f
static void loLayout(int i, float *x, float *y);
static int  loHit(int mx, int my);
static void rwLayout(int i, float *x, float *y);
static int  rwHit(int mx, int my);

static void metaLayoutLevel(int i, float *x, float *y);
static void doGacha(void);
static void rewardPrepare(void);
static void rewardChoose(int idx);
static void loadoutToggle(int t);
static void metaLayoutTalent(int i, float *x, float *y);
static void drawMetaShapes(HDC dc);
static void drawMetaText(HDC dc);
static int  metaHitLevel(int mx, int my);
static int  metaHitTalent(int mx, int my);

static void applyWeaponRack(int sub);
static void applyMainUpgrade(void);

static unsigned char gRelic[R_COUNT];      /* 本局已获得的遗物（0/1） */
static int   gRelicCount = 0;

/* ---- 全局修正量（relicsRecalc 统一重算） ---- */
static float gDmgMul, gRateMul, gCostMul, gCdMul;
static float gPlantHpMul, gZombieHpMul, gZombieSpdMul;
static float gExplodeDmgMul, gExplodeRadMul, gSlowMul;
static float gSkySunMul, gSunflowerAdd, gSunGainMul, gSunflowerCostMul;
static float gSlowFactor, gKillSunMul, gZombieSpdUp;
static int   gStartSunAdd, gKillSun, gArmorPierce, gPierceChance;
static int   gThorns, gMedic, gEmber, gChainBoom, gSpareTire;
static int   gNoMower, gBloodPact, gWallnutHpBoost, gInterest;
static int   gKillStack;                   /* 以战养战的叠层计数 */
static int   gCardSlots = 4;       /* 战斗中最多携带 4 株；大厅可先选 5 株候选 */
static int   gCandidateSlots = 5;  /* 开战前的候选池上限 */
/* ---- 每局结算抽卡 ---- */
static int   gRewardPick[3];      /* 三个盲盒各自对应的植物（-1 = 已经是金币） */
static int   gRewardChosen;       /* 翻开了哪一张，-1 = 还没翻 */
static float gRewardT;            /* 翻开后的展示计时 */
static int   gRewardCoins;        /* 本次给了多少金币（植物全解锁时的补偿） */                     /* 出战卡槽数（扩容卡组可加到 10） */
static int   gLastStand = 0, gMagnetGold = 0, gNightSun = 0;
static int   gMowerAlive = 5;       /* 还在的小推车数量（背水遗物要看） */
static int   gBanker = 0, gNecromancer = 0;   /* 金气 / 幽能系统是否解锁 */
static int   gChargeMax = 1;                  /* 充能槽上限 */
/* 第三批遗物用到的修正量（原来这些遗物只改了名字、没有效果） */
static float gCornDmgMul  = 1.0f;   /* 玉米投手专属伤害倍率 */
static float gSpikeDmgMul = 1.0f;   /* 地刺伤害倍率 */
static int   gSpikeWide   = 0;      /* 地刺打相邻两行 */
static float gMagnetCdMul = 1.0f;   /* 磁力菇冷却倍率 */
static float gCornStunMul = 1.0f;   /* 玉米眩晕倍率 */
static int   gThreeUpFull = 0;      /* 三线上下行不衰减 */
static float gThreeMidMul = 1.0f;   /* 三线中间行倍率 */
static float gBalloonMul  = 1.0f;   /* 对气球僵尸伤害倍率 */
static float gZamboniSlow = 1.0f;   /* 冰车速度倍率 */
static int   gDancerSilence = 0;    /* 舞王静音 */
static float gBulwarkHpMul = 1.0f;  /* 坚果/地刺血量倍率 */
static int   gStarHunter  = 0;      /* 击杀掉金币 */
static int   gEndlessHeal = 0;      /* 每 3 波治疗 */
static float gFuseMul     = 1.0f;   /* 混种强度倍率 */

#define HAS(r) (gRelic[r] != 0)

/* 星期主题：每天一个轻量规则，叠加在遗物与天赋之后。 */
static void weeklyThemeRecalc(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    gWeekGoldMul = 1.0f;
    gWeekSunAdd = 0;
    gWeekDesc = L"";
    switch (st.wDayOfWeek) { /* 0=周日 .. 6=周六 */
    case 0:
        gPlantHpMul *= 1.15f; gZombieSpdMul *= 0.92f;
        gWeekDesc = L"周日·铁壁：植物生命 +15%，僵尸速度 -8%";
        break;
    case 1:
        gDmgMul *= 1.10f; gWeekGoldMul = 1.50f;
        gWeekDesc = L"周一·火力：伤害 +10%，金气 +50%";
        break;
    case 2:
        gSunGainMul *= 1.20f; gSkySunMul *= 0.85f;
        gWeekDesc = L"周二·丰收：阳光产出 +20%，掉落更勤";
        break;
    case 3:
        gCostMul *= 0.90f; gCdMul *= 0.90f;
        gWeekDesc = L"周三·基建：植物费用 -10%，冷却 -10%";
        break;
    case 4:
        gExplodeDmgMul *= 1.30f; gExplodeRadMul *= 1.15f;
        gWeekDesc = L"周四·爆破：爆炸伤害 +30%，范围 +15%";
        break;
    case 5:
        gZombieSpdMul *= 1.08f; gKillSunMul *= 2.0f;
        gWeekDesc = L"周五·猎杀：僵尸速度 +8%，击杀阳光翻倍";
        break;
    case 6:
        gWeekSunAdd = 75; gWeekGoldMul = 1.30f;
        gWeekDesc = L"周六·丰收日：开局阳光 +75，金气 +30%";
        break;
    }
}

static void relicsRecalc(void)
{
    gDmgMul = 1.0f; gRateMul = 1.0f; gCostMul = 1.0f; gCdMul = 1.0f;
    gPlantHpMul = 1.0f; gZombieHpMul = 1.0f; gZombieSpdMul = 1.0f;
    gExplodeDmgMul = 1.0f; gExplodeRadMul = 1.0f; gSlowMul = 1.0f;
    gSkySunMul = 1.0f; gSunflowerAdd = 0.0f; gSunGainMul = 1.0f;
    gSunflowerCostMul = 1.0f; gSlowFactor = 0.50f;
    gCornDmgMul = 1.0f; gSpikeDmgMul = 1.0f; gSpikeWide = 0;
    gMagnetCdMul = 1.0f; gCornStunMul = 1.0f; gThreeUpFull = 0; gThreeMidMul = 1.0f;
    gBalloonMul = 1.0f; gZamboniSlow = 1.0f; gDancerSilence = 0;
    gBulwarkHpMul = 1.0f; gStarHunter = 0; gEndlessHeal = 0; gFuseMul = 1.0f;
    gKillSunMul = 1.0f; gZombieSpdUp = 1.0f;
    gStartSunAdd = 0; gKillSun = 0; gArmorPierce = 0; gPierceChance = 0;
    gThorns = 0; gMedic = 0; gEmber = 0; gChainBoom = 0; gSpareTire = 0;
    gNoMower = 0; gBloodPact = 0; gWallnutHpBoost = 0; gInterest = 0;
    gLastStand = 0; gMagnetGold = 0; gNightSun = 0; gFuseMul = 1.0f;
    gBanker = 0; gNecromancer = 0;

    /* 经济 */
    if (HAS(R_START_SUN))      gStartSunAdd += 50;
    if (HAS(R_START_SUN_BIG))  gStartSunAdd += 120;
    if (HAS(R_SKY_SUN))        gSkySunMul *= 0.85f;
    if (HAS(R_SUNFLOWER))      gSunflowerAdd += 12.0f;
    if (HAS(R_SUNFLOWER_BIG))  { gSunflowerAdd += 25.0f; gSunflowerCostMul *= 1.20f; }
    if (HAS(R_KILL_SUN))       gKillSun += 2;
    if (HAS(R_KILL_SUN_BIG))   { gKillSun += 5; gCostMul *= 1.08f; }
    if (HAS(R_INTEREST))       gInterest = 8;
    /* 火力 */
    if (HAS(R_PEA_DMG))        gDmgMul *= 1.12f;
    if (HAS(R_PEA_DMG_BIG))    gDmgMul *= 1.25f;
    if (HAS(R_RATE))           gRateMul *= 0.88f;
    if (HAS(R_RATE_BIG))       { gRateMul *= 0.78f; gDmgMul *= 0.90f; }
    if (HAS(R_BIG_CALIBER))    { gDmgMul *= 1.45f; gRateMul *= 1.20f; }
    if (HAS(R_GLASS_CANNON))   { gDmgMul *= 1.80f; gPlantHpMul *= 0.55f; }
    if (HAS(R_ARMOR_PIERCE))   gArmorPierce = 1;
    if (HAS(R_PIERCE))         gPierceChance = 25;
    /* 生存 */
    if (HAS(R_PLANT_HP))       gPlantHpMul *= 1.15f;
    if (HAS(R_PLANT_HP_BIG))   { gPlantHpMul *= 1.40f; gCdMul *= 1.15f; }
    if (HAS(R_THORNS))         gThorns = 22;
    if (HAS(R_MEDIC))          gMedic = 1;
    if (HAS(R_SPARE_TIRE))     gSpareTire = 1;
    if (HAS(R_NO_MOWER))       { gNoMower = 1; gDmgMul *= 1.65f; }
    if (HAS(R_IRON_WALL))      gWallnutHpBoost = 120;
    /* 控制 / 爆炸 */
    if (HAS(R_SLOW_LONG))      gSlowMul *= 1.50f;
    if (HAS(R_ICE_AGE))        { gSlowMul *= 2.00f; gSlowFactor = 0.35f; }
    if (HAS(R_BOOM_R))         gExplodeRadMul *= 1.20f;
    if (HAS(R_BOOM_DMG))       gExplodeDmgMul *= 1.55f;
    if (HAS(R_EMBER))          gEmber = 4;
    if (HAS(R_CHAIN_BOOM))     gChainBoom = 1;
    /* 风险 */
    if (HAS(R_MINIMAL))        { gCostMul *= 0.65f; gCdMul *= 1.60f; }
    if (HAS(R_NO_COOLDOWN))    { gCdMul *= 0.55f; gPlantHpMul *= 0.70f; }
    if (HAS(R_SUN_GAMBLE))     { gStartSunAdd -= 120; gSunGainMul = 2.0f; }
    if (HAS(R_BLOOD_PACT))     { gCostMul *= 0.55f; gBloodPact = 1; }
    if (HAS(R_ALL_IN))         { gZombieSpdUp = 1.15f; gKillSunMul = 3.0f; }

    /* ---- 第二批遗物 ---- */
    if (HAS(R_CORN_DMG))     gCornDmgMul = 1.80f;  /* 只影响玉米投手（开火处乘） */
    /* ---- 第三批：新植物 / 新僵尸 专用遗物 ---- */
    if (HAS(R_SPIKE_DMG))    gSpikeDmgMul = 2.0f;   /* 地刺伤害翻倍 */
    if (HAS(R_SPIKE_WIDE))   gSpikeWide = 1;        /* 地刺同时打相邻两行 */
    if (HAS(R_MAGNET_FAST))  gMagnetCdMul = 0.5f;   /* 磁力菇冷却减半 */
    if (HAS(R_CORN_STUN))    gCornStunMul = 2.0f;   /* 玉米眩晕翻倍 */
    if (HAS(R_THREE_UP))     gThreeUpFull = 1;      /* 三线上下行不衰减 */
    if (HAS(R_THREE_MID))    gThreeMidMul = 1.60f;  /* 三线中间行 +60% */
    if (HAS(R_BALLOON_POP))  gBalloonMul = 2.50f;   /* 对气球僵尸伤害 x2.5 */
    if (HAS(R_ZAMBONI_BREAK))gZamboniSlow = 0.70f;  /* 冰车速度 -30% */
    if (HAS(R_DANCER_SILENCE)) gDancerSilence = 1;  /* 舞王不召唤 */
    if (HAS(R_ROOF_GRIP))    gBulwarkHpMul = 1.80f; /* 坚果/地刺血量 +80% */
    if (HAS(R_STAR_HUNTER))  gStarHunter = 1;       /* 击杀 3% 掉金币 */
    if (HAS(R_ENDLESS_HEAL)) gEndlessHeal = 1;      /* 每 3 波治疗 */
    if (HAS(R_FUSION_MASTER)) gFuseMul = 1.35f;     /* 混种强度 +35% */
    if (HAS(R_NIGHT_SUN))    gSkySunMul *= 0.75f;   /* 阳光掉落加速 25% */
    if (HAS(R_MAGNET_GOLD))  gMagnetGold = 1;       /* 吸护具掉金气 */
    /* 新规则：永久编组最多 5 株候选，进入战斗时从中选择 4 株；
       旧的扩容遗物不再突破战斗上限，避免构筑被卡槽数量碾平。 */
    gCandidateSlots = 5;
    gCardSlots = 4;
    if (HAS(R_FIRST_STRIKE)) gStartSunAdd += 150;
    if (HAS(R_DESERT_WATER)) gSkySunMul *= 0.75f;
    if (HAS(R_LAST_STAND))   gLastStand = 1;
    /* 这三个原来只写在 applyRelic 的 switch 里 —— 换个来源拿遗物就不生效。
       挪进 relicsRecalc，保证「持有即生效」，和其他遗物走同一条路。 */
    if (HAS(R_BANKER))       gBanker = 1;
    if (HAS(R_NECROMANCER))  gNecromancer = 1;
    gChargeMax = 1;
    if (HAS(R_CHARGED))      gChargeMax += 1;

    /* ---- 天赋树（跨局永久加成，与遗物共用同一套修正量） ---- */
    if (gSave.talents[0]) gStartSunAdd += 25;
    if (gSave.talents[1]) gSunflowerCostMul *= 0.92f;
    if (gSave.talents[2]) gSunGainMul *= 1.12f;
    if (gSave.talents[3]) gStartSunAdd += 50;
    if (gSave.talents[4])  gDmgMul *= 1.06f;
    if (gSave.talents[5])  gCdMul *= 0.92f;
    if (gSave.talents[6])  gExplodeDmgMul *= 1.18f;
    if (gSave.talents[7])  gDmgMul *= 1.10f;
    if (gSave.talents[8])  gPlantHpMul *= 1.10f;
    if (gSave.talents[9])  gSpareTire = 1;
    if (gSave.talents[10]) gZombieSpdMul *= 0.94f;
    if (gSave.talents[11]) gPlantHpMul *= 1.15f;

    /* 星期主题收尾：天赋 / 遗物算完后再叠 */
    weeklyThemeRecalc();
    /* 每日挑战：由当日种子派生的规则再叠一层 */
    if (gDailyMode) {
        gZombieHpMul *= gDailyHpMul;
        gZombieSpdMul *= gDailySpdMul;
        gSkySunMul *= gDailySunMul;
    }
}

/* ---- 数值取值函数：所有可被遗物影响的数字都必须走这里 ---- */
static int   plantCost(int t)
{
    float c = plantDefs[t].cost * gCostMul;
    if (t == PT_SUNFLOWER) c *= gSunflowerCostMul;
    c += 0.5f;
    return c < 0.0f ? 0 : (int)c;
}
static float plantCd(int t)    { return plantDefs[t].cooldown * gCdMul; }
static float plantHpMax(int t)
{
    float hp = plantDefs[t].hp;
    if (hp <= 0.0f) return 1.0f;
    if (t == PT_WALLNUT) hp += gWallnutHpBoost;
    if (t == PT_WALLNUT || t == PT_SPIKEWEED) hp *= gBulwarkHpMul;   /* 防滑 */
    hp *= gTrBulwark;                                                 /* 壁垒羁绊 */
    return hp * gPlantHpMul;
}
static float peaBaseDmg(void)
{
    float d = 24.0f * gDmgMul;
    if (gLastStand && gMowerAlive == 0) d *= 1.40f;   /* 背水：小推车全没了 */
    return d;
}
static float shootGap(void)    { return 1.4f * gRateMul * gTrShotRate; }   /* 射手羁绊 */
static float slowDur(void)     { return 3.0f * gSlowMul * gTrFrost; }      /* 冰霜羁绊 */
static float zombieHpMax(int t){ return zHp[t] * gZombieHpMul; }


typedef struct {
    int   alive;
    int   type;
    int   row, col;
    float hp, maxhp;
    float phase;      /* 动画相位 */
    float timer;      /* 技能计时 */
    int   armed;      /* 土豆雷是否已激活 */
    float fuse;       /* 炸弹引信 */
    float recoil;     /* 射击后坐 */
    float eaten;      /* 被啃食抖动 */
    int   burst;      /* 双发射手第二发标记 */
    int   stack;      /* 双株模式：第几株（0/1） */
    float shield;     /* 南瓜罩护盾 */
    int   summon;     /* 金气召唤的南瓜炸弹（用南瓜贴图） */
    int   fused;      /* 合成混种 */
    int   fuseA, fuseB;   /* 两个亲本类型（用于叠加渲染） */
    float fuseMul;        /* 混种强度倍率 */
    int   w;          /* 多列/塔防模式：占几格宽 */
    int   hh;         /* 塔防模式：占几格高 */
} Plant;

typedef struct {
    int   active, type;
    float x, y;       /* x=脚部中心, y=脚底 */
    int   row;
    float hp, maxhp;
    float speed;
    float anim;
    float slow;       /* 减速剩余时间 */
    float rooted;     /* 定身剩余时间 */
    float flash;      /* 受击闪白 */
    float armSwing;
    int   eating;
    float eatBob;
    int   shield;     /* 铁门还在（正面减伤） */
    int   vaulted;    /* 撑杆僵尸已经跳过一次 */
    float atkCd;      /* 巨人砸植物的冷却 */
    int   dead;       /* 正在播倒地动画 */
    float deadT;      /* 倒地动画剩余时间 */
} Zombie;

typedef struct { int active; float x, y, vx, vy, dmg; int row, frozen;
                 int wpn;          /* 当前武器 */
                 int pierceLeft;   /* 还能穿透几个目标 */
                 int bounces;      /* 剩余反弹次数 */
                 int freeze;       /* 直接冻结时长（毫秒） */
                 int splash;       /* 溅射模式：剩余子弹 */
                 int root;         /* 命中后定身时长（毫秒） */
                 int knockback;    /* 命中后击退距离 */
                 int homing;       /* 是否按跨行追踪弹判定 */
                 int sub;          /* 0=主射 1=副射 */
                 int src;          /* 发射它的植物类型（决定子弹贴图） */
                 int bouncesBase;
               } Pea;

typedef struct {
    int   active;
    float x, y, vy, life, value;
    int   flying;     /* 被点击后飞向阳光计数器 */
    float fx, fy;
    float phase;
} SunDrop;

typedef struct { int active; float x; int row; float speed; int running; float recharge; } Mower;

typedef struct { int active; float x, y, vx, vy, life, maxlife, size;
                 COLORREF col; int grav; } Particle;

#define MAX_WEATHER   44
#define DEATH_DUR     0.42f

typedef struct {
    int      active;
    float    x, y, vy, size, phase, sway, spin;
    COLORREF col;
} Weather;

static Weather gWeather[MAX_WEATHER];
static int     gWeatherOn = 1;      /* 天气层开关（W 键） */

typedef struct { int type, row; float t; } Spawn;

/* -------------------------------------------------------------- 全局状态 */
enum { ST_MENU, ST_PLAY, ST_PAUSE, ST_WIN, ST_LOSE, ST_DRAFT,
       ST_LEVELS, ST_TALENTS, ST_ACH, ST_GACHA, ST_LOADOUT, ST_REWARD, ST_DAILY };

static Plant     grid[ROWS][COLS];
/* 双株模式的第二槽位。只在 MODE_DOUBLE 下使用；
   用并列数组而不是 grid[ROWS][COLS][2]，是为了不动所有已有的 grid[r][c] 写法。 */
static Plant     grid2[ROWS][COLS];
static Zombie    zombies[MAX_ZOMBIES];
static Pea       peas[MAX_PEAS];
static SunDrop   suns[MAX_SUNS];
static Particle  parts[MAX_PARTS];
static Mower     mowers[ROWS];
static Spawn     spawnQ[MAX_SPAWNS];
static int       spawnQN;

static int   gState   = ST_MENU;
static float gTime    = 0;
static int   gSun     = 100;
static float cardCD[PT_COUNT];
/* 出战编组：本局实际带上场的植物编号（只从候选池前 4 株进入战斗） */
static int   gLoadout[PT_COUNT];
static int   gLoadoutN = 0;
/* 出战编组：大厅保存 5 株候选；开局实际只带前 4 株，保留一个明确取舍位。 */
static void loadoutBuild(void)
{
    int i, n = 0;
    gLoadoutN = 0;
    for (i = 0; i < PT_COUNT && n < gCardSlots; i++)
        if (gSave.plantOwned[i] && gSave.loadout[i]) gLoadout[n++] = i;
    if (n > 0) { gLoadoutN = n; return; }
    for (i = 0; i < PT_COUNT && n < gCandidateSlots; i++)
        if (gSave.plantOwned[i]) { gSave.loadout[i] = 1; if (n < gCardSlots) gLoadout[n++] = i; }
    gLoadoutN = n;
}

/* 大厅里点一下：切换候选植物（至少留 1 张，最多 5 张）；开局只取其中 4 张 */
static void loadoutToggle(int t)
{
    int i, cnt = 0;
    if (t < 0 || t >= PT_COUNT || !gSave.plantOwned[t]) return;
    for (i = 0; i < PT_COUNT; i++) if (gSave.loadout[i]) cnt++;
    if (gSave.loadout[t]) {
        if (cnt <= 1) return;                    /* 至少留一张 */
        gSave.loadout[t] = 0;
    } else {
        if (cnt >= gCandidateSlots) return;     /* 候选池满了 */
        gSave.loadout[t] = 1;
    }
    loadoutBuild();
    saveFlush();
}
static int   gSel     = -1;         /* 选中的卡片索引（PT_* 或 PT_COUNT=铲子） */
static int   gShovel  = 0;
static int   gAutoSun = 1;          /* 自动拾取阳光（默认开，A 键切换） */

/* ---- 神级植物：稀有度概率与展示 ---- */
/* 稀有 / 史诗 / 传奇 / 神话，概率递减。每档都保证强度 ≥ 2 倍普通植物 */
#define HERO_RARITY_COUNT 4
static const wchar_t *HERO_RARITY_NAME[HERO_RARITY_COUNT] = {
    L"稀有", L"史诗", L"传奇", L"神话" };
static const COLORREF HERO_RARITY_COL[HERO_RARITY_COUNT] = {
    RGB( 96, 168, 232),   /* 稀有 蓝 */
    RGB(168, 108, 232),   /* 史诗 紫 */
    RGB(240, 168,  56),   /* 传奇 金 */
    RGB(244,  88, 132),   /* 神话 洋红 */
};
/* 概率：稀有 55% / 史诗 28% / 传奇 13% / 神话 4% */
static const int HERO_RARITY_W[HERO_RARITY_COUNT] = { 55, 28, 13, 4 };
/* 稀有度 -> 植物 的映射不再单独列表：plantDefs[].rarity 就是权威来源，
   原来那张 HERO_BY_RARITY 表在"按剩余集合抽"后已经没有用了。 */

/* 本局通过三选一拿到的神级植物（额外卡槽，不占原有 8 张编组） */
#define MAX_BONUS_PLANT 6
static int   gBonusPlant[MAX_BONUS_PLANT];
static int   gBonusN = 0;

/* 从「本局还没拿到的神级植物」里按稀有度权重抽一株。
   旧实现是「先按概率抽稀有度，再看那一档的两株是否拿过」——
   只要那一档的两株都拿过就直接返回 -1，哪怕别的档还有没拿的，
   于是三选一经常一张植物卡都不出。现在改成在整个剩余集合里抽。
   返回 -1 = 8 株都拿过 / 本局植物卡槽已满。 */
static int rollHeroPlant(void)
{
    int cand[PT_COUNT], w[PT_COUNT], n = 0, i, total = 0, roll;
    if (gBonusN >= MAX_BONUS_PLANT) return -1;
    for (i = PT_HERO_FLAME; i < PT_COUNT; i++) {
        int dup = 0, k;
        for (k = 0; k < gBonusN; k++) if (gBonusPlant[k] == i) dup = 1;
        if (dup) continue;
        cand[n] = i;
        w[n] = HERO_RARITY_W[plantDefs[i].rarity - 1];
        total += w[n];
        n++;
    }
    if (n == 0) return -1;
    roll = rand() % total;
    for (i = 0; i < n; i++) { roll -= w[i]; if (roll < 0) return cand[i]; }
    return cand[n - 1];
}

/* 神级兜底：全部拿满时，改为抽一株还没解锁的普通植物（永久解锁）。
   返回 -1 = 普通植物也全解锁了。 */
static int rollNormalPlant(void)
{
    int pool[PT_HERO_FLAME], n = 0, i;
    for (i = 0; i < PT_HERO_FLAME; i++) if (!gSave.plantOwned[i]) pool[n++] = i;
    if (n == 0) return -1;
    return pool[rand() % n];
}

/* ---- 三选一抽卡（肉鸽） ---- */
#define DRAFT_N      3
#define DRAFT_SKIP  150          /* 跳过奖励换多少阳光 */
static int   gDraft[DRAFT_N];    /* 本次抽出的三个选项 id */
static int   gDraftHover = -1;
/* 本次植物卡的类型：0 = 神级（只在本局生效）  1 = 普通植物（永久解锁） */
static int   gDraftPlantPerm = 0;
static int   gLastDraft  = -1;   /* 已经给过奖励的最大波数，防止重复触发 */
static int   gDraftCount = 0;    /* 本局拿过几次奖励 */
static float gMedicT     = 12.0f;
static int   gKillStackDrawn = -1;

/* ======================================================================== */
/*                        模式 / 武器 / 货币                                  */
/* ======================================================================== */
#define MODE_STANDARD    0       /* 1 格 1 株，9x5 草坪 */
#define MODE_DOUBLE      1       /* 1 格可叠 2 株 */
#define MODE_MULTILANE   2       /* 1 株占 2 列 */
#define MODE_WATER       3       /* 仅后 3 列 + 水中纹理 */
#define MODE_NIGHT       4       /* 夜间草坪 + 植物发微光 */
#define MODE_TOWER       5       /* 1 株占 1x2 高格 */
#define MODE_COUNT       6
static int   gCurMode = MODE_STANDARD;
static int   gCurLevel = 0;                  /* 当前关卡 */
static int   gLawnVariant = 0;               /* 当前渲染用的草坪变体（关卡主导，模式可覆盖） */
static int   gUnlockedMode[MODE_COUNT];     /* 每个模式是否在本局被解锁 */
static const wchar_t *MODE_NAME[MODE_COUNT] = {
    L"标准", L"双株", L"多列", L"水中", L"夜间", L"塔防" };
static const COLORREF MODE_COL[MODE_COUNT] = {
    RGB(196, 200, 188), RGB(228, 188, 90), RGB(160, 130, 220),
    RGB( 86, 162, 226), RGB( 92, 110, 178), RGB(204, 130, 96) };

/* 武器模组 */
#define WPN_NORMAL   0
#define WPN_PIERCE   1
#define WPN_SPLASH   2
#define WPN_BOUNCE   3
#define WPN_FREEZE   4
#define WPN_BURN     5
#define WPN_COUNT    6
static int   gMainWpn = WPN_NORMAL;          /* 本局主射武器 */
static int   gHasSubWpn = 0;                 /* 副射槽是否解锁 */
static int   gSubWpn = WPN_NORMAL;
static const wchar_t *WPN_NAME[WPN_COUNT] = {
    L"直射", L"穿透", L"溅射", L"反弹", L"冰冻", L"灼烧" };
static const COLORREF WPN_COL[WPN_COUNT] = {
    RGB(126, 198, 96), RGB(116, 188, 246), RGB(254, 152, 56),
    RGB(238, 86, 86),  RGB(196, 226, 248), RGB(254, 196, 96) };

/* 货币 */
static int   gGold = 0;                     /* 金气：击杀获得 */
static int   gGhost = 0;                    /* 幽能：减速获得 */
static int   gFury = 0;                     /* 愤怒条：每帧 +1，100 满 */
static float gFuryMax = 45.0f;      /* 秒：满一次触发全屏雷击 */
static int   gCharge = 0;                   /* 充能槽 */
static int   gFuryT = 0;                    /* 雷击剩余时长 */


/* 每帧 +1 涨速系数 */

/* 三选一：几何常量与函数原型（实现放在渲染层之前） */
#define CARD_W   256.0f
#define CARD_H   360.0f
#define CARD_GAP 44.0f
#define CARD_Y   150.0f
#define SKIP_Y   532.0f
#define SKIP_H   36.0f
static void draftCardRect(int i, float *x, float *y);
static void applyRelic(int id);
static void drawDraftShapes(HDC dc);
static void drawDraftText(HDC dc);
static int   mMouseX, mMouseY;

/* ---- 窗口缩放 / 全屏 ----
   游戏内部固定按 1000x650 渲染，窗口（尤其全屏）尺寸任意，
   所以呈现时要「等比缩放 + 黑边」，鼠标坐标也要反向映射回游戏坐标系。 */
static HWND   hWndMain   = NULL;
static int    gFullscreen = 0;
static RECT   gSavedRect;
static LONG   gSavedStyle = 0;
static float  gViewScale = 1.0f;                 /* 窗口像素 / 游戏像素 */
static int    gViewOffX  = 0, gViewOffY = 0;     /* 黑边偏移 */

/* ---- 全屏切换的防抖 ----
   按住 F11 / Alt+Enter 不放时，Windows 会持续发送 WM_KEYDOWN（键盘自动重复），
   每一次都会触发一次切换 —— 全屏被反复开关，看起来就是"不停闪烁"。
   两道保险：
     ① 检查 lParam 的 bit30（上一次该键是否已按下）—— 这是自动重复位
     ② 再做 350ms 的时间窗防抖，兜住极端情况 */
static DWORD gLastToggleTick = 0;

static int keyIsRepeat(LPARAM lp)
{
    return (lp & (1L << 30)) != 0;
}

static int toggleAllowed(void)
{
    DWORD now = GetTickCount();
    if (now - gLastToggleTick < 350) return 0;
    gLastToggleTick = now;
    return 1;
}

/* ---- 呈现用的后台缓冲 ----
   原来往窗口 DC 分两步画：先 FillRect 铺满黑边、再 StretchBlt 贴游戏画面。
   两步之间的一瞬间整屏是黑的，而全屏（1920x1080）下 StretchBlt 很慢，
   这一帧黑屏会被看到 —— 表现就是"全屏下不停闪烁"。
   改成先在离屏缓冲里把黑边+画面一起画好，再一次性 BitBlt 上去，
   窗口每帧只收到一次完整更新，没有中间态。
   缓冲只在窗口尺寸变化时重建，不是每帧新建。 */
static HDC     gPresentDC = NULL;
static HBITMAP gPresentBM = NULL;
static int     gPresentW  = 0, gPresentH = 0;

/* 黑边填充：用一个长期存在的画刷，避免每帧 Create/Delete GDI 对象 */
static HBRUSH gBlackBrush = NULL;
static void presentFillBlack(HDC dc, int w, int h)
{
    RECT r;
    if (!gBlackBrush) gBlackBrush = CreateSolidBrush(RGB(0, 0, 0));
    r.left = 0; r.top = 0; r.right = w; r.bottom = h;
    FillRect(dc, &r, gBlackBrush);
}

static void presentEnsure(int w, int h)
{
    HDC scr;
    if (w <= 0 || h <= 0) return;
    if (gPresentDC && gPresentW == w && gPresentH == h) return;   /* 复用 */
    if (!gPresentDC) {
        scr = GetDC(NULL);
        gPresentDC = CreateCompatibleDC(scr);
        ReleaseDC(NULL, scr);
    }
    if (gPresentBM) { DeleteObject(gPresentBM); gPresentBM = NULL; }
    scr = GetDC(NULL);
    gPresentBM = CreateCompatibleBitmap(scr, w, h);
    ReleaseDC(NULL, scr);
    SelectObject(gPresentDC, gPresentBM);
    gPresentW = w; gPresentH = h;
}

static void presentRelease(void)
{
    if (gPresentBM) { DeleteObject(gPresentBM); gPresentBM = NULL; }
    if (gPresentDC) { DeleteDC(gPresentDC); gPresentDC = NULL; }
    gPresentW = gPresentH = 0;
}

/* 窗口尺寸变了（全屏切换 / 拉伸）就把缓冲作废，下一帧重建 */
static void presentInvalidate(void)
{
    gPresentW = gPresentH = 0;
}

static void windowToGame(int wx, int wy, int *gx, int *gy)
{
    if (gViewScale <= 0.0001f) { *gx = wx; *gy = wy; return; }
    *gx = (int)((float)(wx - gViewOffX) / gViewScale);
    *gy = (int)((float)(wy - gViewOffY) / gViewScale);
}
static int   mDown   = 0;
static int   gWave    = 0;          /* 已开始的波数 */
static float waveTimer = 0;
static float skySunTimer;
static float shakeT = 0, shakeMag = 0;
static float flashRed = 0;
static int   gKilled = 0;
static float gElapsed = 0;
static int   gMowerUsed = 0;

static HDC     gWorldDC, gFrameDC, gScreenDC;
static HBITMAP gWorldBM, gFrameBM;
static XFORM   gScaleXF;

/* ======================================================================== */
/*                                  辅助函数                                 */
/* ======================================================================== */
static inline float cellCX(int col) { return LAWN_X + col * CELL_W + CELL_W * 0.5f; }
static inline float cellBaseY(int row) { return LAWN_Y + row * CELL_H + CELL_H - 16.0f; }
static inline float rowCY(int row) { return LAWN_Y + row * CELL_H + CELL_H * 0.5f; }

static Plant *plantAt(int row, int col)
{
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return NULL;
    if (grid[row][col].alive) return &grid[row][col];
    if (gCurMode == MODE_DOUBLE && grid2[row][col].alive) return &grid2[row][col];
    return NULL;
}

/* 双株模式下往哪个槽位种：先填 slot0，再填 slot1 */
static Plant *plantSlotFor(int row, int col)
{
    if (!grid[row][col].alive) return &grid[row][col];
    if (gCurMode == MODE_DOUBLE && !grid2[row][col].alive) return &grid2[row][col];
    return NULL;
}

/* 双株模式：该格是否还能再种一株 */
static int cellHasRoom(int row, int col, int w, int hh)
{
    int rr, cc;
    for (rr = row; rr < row + hh; rr++) for (cc = col; cc < col + w; cc++) {
        if (!grid[rr][cc].alive) continue;
        if (gCurMode == MODE_DOUBLE && !grid2[rr][cc].alive) continue;
        return 0;
    }
    return 1;
}

/* ---------------- 种植模式：合法性 + 多形态尺寸 ---------------- */
/* 每种模式给出"在 (r, c) 位置种一株是否合法"，以及
   "种下的这株要占多大面积（宽/高）"。多列占 2 列宽，塔防占 1 列宽 2 行高。 */
static int cellLegal(int r, int c, int w, int hh)
{
    if (r < 0 || c < 0 || r >= ROWS || c >= COLS) return 0;
    if (c + w > COLS) return 0;
    if (r + hh > ROWS) return 0;
    if (!cellHasRoom(r, c, w, hh)) return 0;
    if (gCurMode == MODE_WATER && c < COLS - 3) return 0;   /* 水中：仅后 3 列 */
    return 1;
}

/* 从鼠标坐标得到 (r, c) 与"多形态下的占位尺寸"。 */
static int mouseToCell(int mx, int my, int *pr, int *pc, int *pw, int *phh)
{
    int r = (my - LAWN_Y) / CELL_H;
    int c = (mx - LAWN_X) / CELL_W;
    int w = 1, hh = 1;
    if (r < 0 || c < 0 || r >= ROWS || c >= COLS) return 0;
    if (gCurMode == MODE_MULTILANE) w = 2;
    if (gCurMode == MODE_TOWER)    hh = 2;
    *pr = r; *pc = c; *pw = w; *phh = hh;
    return 1;
}

/* ---------------- 依赖棋盘布局的条件加成 ---------------- */
/* 羁绊系统已移除。
   实测通关率 100%、羁绊贡献了约 25% 的免费强度（尤其射手 3 档 -28% 射击间隔），
   这是难度上不去的主要原因之一。现在只保留一个空函数，
   让所有调用点不用改，但不再给任何加成。 */
static void traitsRecalc(void)
{
    int i;
    for (i = 0; i < TR_COUNT; i++) { gTraitCount[i] = 0; gTraitTier[i] = -1; }
    gTrShotRate = 1.0f; gTrGarden = 1.0f; gTrBulwark = 1.0f;
    gTrFrost = 1.0f; gTrDemoDmg = 1.0f; gTrMutant = 1.0f;
}

static int countPlants(int type)      /* type < 0 不筛种类 */
{
    int r, c, n = 0;
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
        if (grid[r][c].alive && (type < 0 || grid[r][c].type == type)) n++;
        if (grid2[r][c].alive && (type < 0 || grid2[r][c].type == type)) n++;
    }
    return n;
}

/* 该格植物开火时的额外伤害加成（0.25 = +25%） */
static float fieldDmgBonusAt(int row, int col)
{
    float b = 0.006f * (float)gKillStack;      /* 以战养战：本局累计击杀叠层 */
    int i;

    if (HAS(R_COLUMN)) {                        /* 同列共鸣 */
        int n = 0;
        for (i = 0; i < ROWS; i++) if (plantAt(i, col)) n++;
        if (n >= 3) b += 0.25f;
    }
    if (HAS(R_EMPTY)) {                         /* 留白 */
        int e = 0;
        if (!plantAt(row - 1, col)) e++;
        if (!plantAt(row + 1, col)) e++;
        if (!plantAt(row, col - 1)) e++;
        if (!plantAt(row, col + 1)) e++;
        b += 0.07f * (float)e;
    }
    if (HAS(R_WALLNUT_DOGMA))                   /* 坚果教条 */
        b += 0.09f * (float)countPlants(PT_WALLNUT);
    {                                           /* 骨髓花：邻格 +18% 伤害 */
        int d;
        static const int DR[4] = {-1, 1, 0, 0};
        static const int DC[4] = { 0, 0,-1, 1};
        for (d = 0; d < 4; d++) {
            Plant *q = plantAt(row + DR[d], col + DC[d]);
            if (q && q->type == PT_MARROW) { b += 0.18f; break; }
        }
    }
    if (HAS(R_SYMBIOSIS)) {                     /* 温床：四邻有同类 */
        Plant *self = plantAt(row, col);
        if (self) {
            Plant *nb[4];
            int k;
            nb[0] = plantAt(row - 1, col); nb[1] = plantAt(row + 1, col);
            nb[2] = plantAt(row, col - 1); nb[3] = plantAt(row, col + 1);
            for (k = 0; k < 4; k++)
                if (nb[k] && nb[k]->type == self->type) { b += 0.08f; break; }
        }
    }
    if (gThreeMidMul > 1.0f && row == ROWS / 2) b += (gThreeMidMul - 1.0f);  /* 中路压制 */
    if (HAS(R_VANGUARD))  b += (col == 0) ? 0.40f : -0.10f;   /* 尖兵 */
    if (HAS(R_BACKLINE) && col == COLS - 1) b += 0.60f;        /* 纵深防御 */
    return b;
}

/* 齐射：该行射手 ≥4 株时射击间隔 -30% */
static float rowRateMul(int row)
{
    int c, n = 0;
    if (!HAS(R_VOLLEY)) return 1.0f;
    for (c = 0; c < COLS; c++) {
        Plant *p = plantAt(row, c);
        if (p && (p->type == PT_PEASHOOTER || p->type == PT_SNOWPEA || p->type == PT_REPEATER))
            n++;
    }
    return (n >= 4) ? 0.70f : 1.0f;
}

/* 花海会随向日葵数量变化，生命类遗物也会改数值：统一刷新全场植物 */
static void plantsRefreshHp(void)
{
    int r, c;
    float sea = HAS(R_FLOWER_SEA) ? (1.0f + 0.06f * (float)countPlants(PT_SUNFLOWER)) : 1.0f;
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
        int sl;
        for (sl = 0; sl < 2; sl++) {
        Plant *p = (sl == 0) ? &grid[r][c] : &grid2[r][c];
        float nm;
        if (!p->alive) continue;
        nm = plantHpMax(p->type) * sea;
        if (nm > p->maxhp)      { p->hp += (nm - p->maxhp); p->maxhp = nm; }
        else if (nm < p->maxhp) { p->hp -= (p->maxhp - nm); p->maxhp = nm; if (p->hp < 1.0f) p->hp = 1.0f; }
        }
    }
}

static void addParticle(float x, float y, float vx, float vy, float life,
                        float size, COLORREF c, int grav)
{
    int i;
    for (i = 0; i < MAX_PARTS; i++) {
        if (!parts[i].active) {
            parts[i].active = 1;
            parts[i].x = x; parts[i].y = y; parts[i].vx = vx; parts[i].vy = vy;
            parts[i].life = parts[i].maxlife = life;
            parts[i].size = size; parts[i].col = c; parts[i].grav = grav;
            return;
        }
    }
}

static void addShake(float mag, float dur) { shakeMag = mag; shakeT = dur; }

/* ---------------------------------------------- 天气层：飘落的花瓣/枯叶 */
/* GDI 画不了旋转的图元，所以用「椭圆宽度振荡」伪造翻转 —— 花瓣看起来像在打旋。 */
static void weatherSpawn(Weather *w, int anywhere)
{
    float pick;
    w->active = 1;
    w->x     = rnd(-40.0f, VIEW_W + 40.0f);
    w->y     = anywhere ? rnd(-20.0f, VIEW_H) : rnd(-80.0f, -12.0f);
    w->vy    = rnd(17.0f, 33.0f);
    w->size  = rnd(4.2f, 6.8f);
    w->phase = rnd(0.0f, 6.28f);
    w->sway  = rnd(9.0f, 24.0f);
    w->spin  = rnd(0.8f, 1.9f);
    pick = rnd(0.0f, 1.0f);
    if (pick < 0.50f)      w->col = RGB(255, 203, 221);   /* 粉花瓣 */
    else if (pick < 0.78f) w->col = RGB(252, 231, 172);   /* 黄花瓣 */
    else                   w->col = RGB(164, 180, 102);   /* 枯叶 */
}

static void weatherInit(void)
{
    int i;
    for (i = 0; i < MAX_WEATHER; i++) weatherSpawn(&gWeather[i], 1);
}

static void weatherUpdate(float dt)
{
    int i;
    for (i = 0; i < MAX_WEATHER; i++) {
        Weather *w = &gWeather[i];
        if (!w->active) continue;
        w->phase += dt * w->spin;
        w->y     += w->vy * dt;
        w->x     += sin(w->phase * 1.7f) * w->sway * dt;
        if (w->y > VIEW_H + 26.0f || w->x < -60.0f || w->x > VIEW_W + 60.0f)
            weatherSpawn(w, 0);
    }
}

static void drawWeather(HDC dc)
{
    int i;
    if (!gWeatherOn) return;
    for (i = 0; i < MAX_WEATHER; i++) {
        Weather *w = &gWeather[i];
        float spin, rx, ry;
        if (!w->active) continue;
        spin = cos(w->phase * 2.2f);
        /* 正面：宽椭圆；侧面：细长条 —— 这样才读得出「在翻转」。
           （写反了会变成一个时胖时瘦的圆点，像噪点不像花瓣） */
        rx = w->size * (0.14f + 0.86f * fabs(spin));
        ry = w->size * (0.62f + 0.16f * fabs(spin));
        fillEllipse(dc, w->x, w->y, rx, ry, w->col);
        if (rx > w->size * 0.5f)                       /* 受光面 */
            fillEllipse(dc, w->x - rx * 0.26f, w->y - ry * 0.24f,
                        rx * 0.40f, ry * 0.36f, tint(w->col, 0.34f));
    }
}

/* ------------------------------------------- 余烬：爆炸后残留的火焰地带 */
#define MAX_FIRES 20
typedef struct { int active; float x, y, t; } FireZone;
static FireZone gFires[MAX_FIRES];

static void fireSpawn(float x, float y, float dur)
{
    int i;
    for (i = 0; i < MAX_FIRES; i++) {
        if (!gFires[i].active) {
            gFires[i].active = 1;
            gFires[i].x = x; gFires[i].y = y; gFires[i].t = dur;
            return;
        }
    }
}

static void firesUpdate(float dt)
{
    int i, k;
    for (i = 0; i < MAX_FIRES; i++) {
        FireZone *f = &gFires[i];
        if (!f->active) continue;
        f->t -= dt;
        if (f->t <= 0.0f) { f->active = 0; continue; }
        for (k = 0; k < MAX_ZOMBIES; k++) {
            Zombie *z = &zombies[k];
            if (!z->active || z->dead) continue;
            if (fabs(z->x - f->x) < CELL_W * 0.65f && fabs(z->y - f->y) < CELL_H * 0.85f) {
                z->hp -= 35.0f * dt;
                z->flash = 0.06f;
            }
        }
        if (rnd(0.0f, 1.0f) < dt * 26.0f)
            addParticle(f->x + rnd(-24, 24), f->y - rnd(0, 16), rnd(-16, 16),
                        rnd(-72, -26), rnd(0.25f, 0.5f), rnd(4, 9),
                        (rnd(0.0f, 1.0f) < 0.5f) ? RGB(255, 168, 60) : RGB(255, 220, 120), 1);
    }
}

static void drawFires(HDC dc)
{
    int i, k;
    for (i = 0; i < MAX_FIRES; i++) {
        FireZone *f = &gFires[i];
        float kk;
        if (!f->active) continue;
        kk = CLAMP(f->t, 0.0f, 1.0f);            /* 最后 1 秒开始收小 */
        /* 地面焦痕：让火「贴地」，不然会像浮在半空的橙色团块 */
        fillEllipse(dc, f->x, f->y + 6.0f, 46.0f * (0.4f + 0.6f * kk),
                    13.0f * (0.4f + 0.6f * kk), RGB(56, 42, 30));
        /* 一排向上收窄的火苗，高度各自抖动 */
        for (k = 0; k < 6; k++) {
            float ox = -30.0f + (float)k * 12.0f;
            float ph = sin(gTime * 11.0f + (float)k * 1.7f);
            float hh = (24.0f + 15.0f * ph) * kk;
            float ww = 8.5f + 3.0f * ph;
            fillTriangle(dc, f->x + ox - ww, f->y + 6.0f,
                             f->x + ox + ww, f->y + 6.0f,
                             f->x + ox + ph * 4.0f, f->y + 6.0f - hh, RGB(255, 126, 32));
            fillTriangle(dc, f->x + ox - ww * 0.48f, f->y + 6.0f,
                             f->x + ox + ww * 0.48f, f->y + 6.0f,
                             f->x + ox + ph * 3.0f, f->y + 6.0f - hh * 0.58f, RGB(255, 206, 88));
        }
    }
}

static void spawnSunDrop(float x, float y, float targetY, int value)
{
    int i;
    for (i = 0; i < MAX_SUNS; i++) {
        if (!suns[i].active) {
            suns[i].active = 1;
            suns[i].x = x; suns[i].y = y; suns[i].vy = 42.0f;
            suns[i].life = 11.0f; suns[i].value = (float)value;
            suns[i].flying = 0; suns[i].phase = rnd(0, 6.28f);
            /* 用 fy 记录落地目标 */
            suns[i].fy = targetY;
            return;
        }
    }
}

/* ======================================================================== */
/*                                植物/僵尸绘制                               */
/* ======================================================================== */

/* ======================================================================== */
/*                    精灵资源：加载与绘制（可选，缺失即回退）                 */
/* ======================================================================== */
/* assets/ 下是 32 位 BGRA BMP（alpha 已预乘）。任何一张缺失，对应对象就自动
   回退到后面的程序化绘制 —— 把 assets 目录整个删掉游戏照样能跑。            */

typedef struct {
    int     ok;
    int     w, h;          /* 像素尺寸 = 逻辑尺寸 x SS */
    HDC     dc;
    HBITMAP bmp;
    void   *bits;
} Sprite;

static XFORM   gIdentXF;        /* 单位变换 */
static XFORM   gCurXF;          /* render() 当前使用的世界变换 */
static wchar_t gAssetDir[MAX_PATH];

static Sprite gSprPlant[PT_COUNT];
static Sprite gSprZombie[ZT_COUNT];
static Sprite gSprZombieFlash[ZT_COUNT];   /* 纯白剪影，受击闪白用 */
static Sprite gSprZombieBlue[ZT_COUNT];    /* 冷蓝剪影，减速用 */
static Sprite gSprZombieEat[ZT_COUNT];       /* 第二套姿态：啃食 */
static Sprite gSprZombieEatFlash[ZT_COUNT];
static Sprite gSprZombieEatBlue[ZT_COUNT];
static Sprite gSprZombieDead[ZT_COUNT][5];   /* 倒地动画帧（绕脚底旋转派生） */
static Sprite gSprLawnNight, gSprLawnWater;   /* 关卡草坪变体 */
static Sprite gSprLawnRoof, gSprLawnDesert;
static Sprite gSprWeapon[5];                  /* 5 种武器植物 */
static Sprite gSprShape[3];                   /* 多形态植物: 0=双株 1=多列 2=塔防1x2 */
static Sprite gSprGold, gSprGhost, gSprPumpkin;  /* 货币 / 增援 */
static Sprite gSprCardBack, gSprBurst;           /* 抽卡用 UI */

/* ---- 子弹特效 ----
   以前豌豆是"纯色椭圆"，12 种植物打出来的东西长得一模一样。
   现在每株植物、每种武器模组都有对应的子弹资源。 */
enum { BUL_PEA, BUL_ICE, BUL_KERNEL, BUL_FLAME, BUL_CRYSTAL, BUL_FROST,
       BUL_DOOM, BUL_PIERCE, BUL_SPLASH, BUL_BOUNCE, BUL_BURN,
       BUL_STAR, BUL_SPINE, BUL_SPLIT, BUL_ARC, BUL_BOOMER, BUL_FUME,
       BUL_BEAM, BUL_MELON, BUL_ICEMELON, BUL_GLOOM, BUL_COB, BUL_DART,
       BUL_COUNT };
static Sprite gSprBullet[BUL_COUNT];
static const wchar_t *bulletFile[BUL_COUNT] = {
    L"bullet_pea", L"bullet_ice", L"bullet_kernel", L"bullet_flame",
    L"bullet_crystal", L"bullet_frost", L"bullet_doom",
    L"bullet_pierce", L"bullet_splash", L"bullet_bounce", L"bullet_burn",
    L"bullet_star", L"bullet_spine", L"bullet_split", L"bullet_arc",
    L"bullet_boomer", L"bullet_fume", L"bullet_beam", L"bullet_melon",
    L"bullet_icemelon", L"bullet_gloom", L"bullet_cob", L"bullet_dart"
};

/* 选子弹图：武器模组优先（它覆盖了外观），否则按发射它的植物 */
static Sprite *bulletSpriteOf(const Pea *pe)
{
    switch (pe->wpn) {
    case WPN_PIERCE: return &gSprBullet[BUL_PIERCE];
    case WPN_SPLASH: return &gSprBullet[BUL_SPLASH];
    case WPN_BOUNCE: return &gSprBullet[BUL_BOUNCE];
    case WPN_FREEZE: return &gSprBullet[BUL_ICE];
    case WPN_BURN:   return &gSprBullet[BUL_BURN];
    default: break;
    }
    switch (pe->src) {
    case PT_SNOWPEA:       return &gSprBullet[BUL_ICE];
    case PT_KERNELPULT:    return &gSprBullet[BUL_KERNEL];
    case PT_STARFRUIT:     return &gSprBullet[BUL_STAR];
    case PT_CACTUS:        return &gSprBullet[BUL_SPINE];
    case PT_SPLITPEA:      return &gSprBullet[BUL_SPLIT];
    case PT_REED:          return &gSprBullet[BUL_ARC];
    case PT_BLOOMERANG:    return &gSprBullet[BUL_BOOMER];
    case PT_FUME:          return &gSprBullet[BUL_FUME];
    case PT_LASERBEAN:     return &gSprBullet[BUL_BEAM];
    case PT_MELONPULT:     return &gSprBullet[BUL_MELON];
    case PT_WINTERMELON:   return &gSprBullet[BUL_ICEMELON];
    case PT_GLOOM:         return &gSprBullet[BUL_GLOOM];
    case PT_COBCANNON:     return &gSprBullet[BUL_COB];
    case PT_CATTAIL:       return &gSprBullet[BUL_DART];
    case PT_HERO_FLAME:    return &gSprBullet[BUL_FLAME];
    case PT_HERO_CRYSTAL:  return &gSprBullet[BUL_CRYSTAL];
    case PT_HERO_FROST:    return &gSprBullet[BUL_FROST];
    case PT_HERO_DOOM:     return &gSprBullet[BUL_DOOM];
    default:               return &gSprBullet[BUL_PEA];
    }
}
static Sprite gSprSun;
static Sprite gSprLawn;
static Sprite gSprBackground;
static int    gArtLoaded = 0;   /* 成功载入的资源张数，用于 HUD 提示 */

/* 共享同一个 DIB 的浅拷贝。精灵资源只读，共享句柄安全；
   只在析构时要注意别重复 DeleteObject（用 owner 标记）。 */
static void sprCopy(Sprite *dst, const Sprite *src)
{
    *dst = *src;
}

static void spriteRelease(Sprite *s)
{
    if (s->bmp) { DeleteObject(s->bmp); s->bmp = NULL; }
    if (s->dc) { DeleteDC(s->dc); s->dc = NULL; }
    s->ok = 0;
}

/* 读 32 位 BMP -> DIB 段（保留 alpha 通道，AlphaBlend 需要） */
static int spriteLoad(Sprite *s, const wchar_t *path)
{
    FILE *fp;
    long size;
    unsigned char *buf;
    unsigned int off;
    int w, h, bpp, y;
    BITMAPINFO bi;
    void *bits = NULL;

    memset(s, 0, sizeof(*s));
    fp = _wfopen(path, L"rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END); size = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (size < 54) { fclose(fp); return 0; }
    buf = (unsigned char *)malloc((size_t)size);
    if (!buf) { fclose(fp); return 0; }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) { free(buf); fclose(fp); return 0; }
    fclose(fp);

    if (buf[0] != 'B' || buf[1] != 'M') { free(buf); return 0; }
    off = *(unsigned int *)(buf + 10);
    w   = *(int *)(buf + 18);
    h   = *(int *)(buf + 22);
    bpp = *(unsigned short *)(buf + 28);
    if (w <= 0 || h <= 0 || bpp != 32 ||
        (size_t)off + (size_t)w * h * 4 > (size_t)size) { free(buf); return 0; }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;            /* 负值 = 自上而下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!s->bmp || !bits) { free(buf); return 0; }
    s->dc = CreateCompatibleDC(NULL);
    SelectObject(s->dc, s->bmp);
    for (y = 0; y < h; y++)                     /* BMP 自下而上 -> DIB 自上而下 */
        memcpy((unsigned char *)bits + (size_t)y * w * 4,
               buf + off + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
    free(buf);
    s->bits = bits; s->w = w; s->h = h; s->ok = 1;
    return 1;
}

static int spriteLoadName(Sprite *s, const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%s%s.bmp", gAssetDir, name);
    return spriteLoad(s, path);
}

/* 生成同尺寸纯色剪影（保留 alpha，RGB 按 alpha 预乘），用于闪白 / 染蓝 */
static int spriteTint(Sprite *dst, const Sprite *src, int r, int g, int b)
{
    BITMAPINFO bi;
    void *bits = NULL;
    unsigned char *sp = (unsigned char *)src->bits, *dp;
    int i, n;

    memset(dst, 0, sizeof(*dst));
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = src->w;
    bi.bmiHeader.biHeight      = -src->h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dst->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dst->bmp || !bits) return 0;
    dst->dc = CreateCompatibleDC(NULL);
    SelectObject(dst->dc, dst->bmp);
    dp = (unsigned char *)bits;
    n = src->w * src->h;
    for (i = 0; i < n; i++) {
        unsigned char a = sp[i * 4 + 3];
        dp[i * 4 + 0] = (unsigned char)(b * a / 255);
        dp[i * 4 + 1] = (unsigned char)(g * a / 255);
        dp[i * 4 + 2] = (unsigned char)(r * a / 255);
        dp[i * 4 + 3] = a;
    }
    dst->bits = bits; dst->w = src->w; dst->h = src->h; dst->ok = 1;
    return 1;
}

/* 底部中心锚点绘制。(cx, baseY) 是逻辑坐标，sx/sy 是缩放比例。
   constAlpha < 0 表示只用资源自带 alpha，否则再乘一个全局不透明度。 */
static void spriteBlit(HDC dst, const Sprite *s, float cx, float baseY,
                       float sx, float sy, int constAlpha)
{
    BLENDFUNCTION bf;
    float w, h;
    int dl, dt, dr, db;

    if (!s->ok) return;
    /* 资源的像素尺寸本来就是「逻辑尺寸 x SS」，这里先换算回逻辑尺寸 */
    w = (float)s->w / (float)SS * sx;
    h = (float)s->h / (float)SS * sy;
    dl = (int)((cx - w * 0.5f) * SS + 0.5f);
    dt = (int)((baseY - h) * SS + 0.5f);
    dr = (int)((cx + w * 0.5f) * SS + 0.5f);
    db = (int)(baseY * SS + 0.5f);
    if (dr <= dl || db <= dt) return;

    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = (BYTE)(constAlpha < 0 ? 255 : CLAMP(constAlpha, 0, 255));
    bf.AlphaFormat         = AC_SRC_ALPHA;

    /* AlphaBlend 用的是设备坐标，这里临时把世界变换复位再恢复 */
    SetWorldTransform(dst, &gIdentXF);
    AlphaBlend(dst, dl, dt, dr - dl, db - dt, s->dc, 0, 0, s->w, s->h, bf);
    SetWorldTransform(dst, &gCurXF);
}

static void drawPlantShapeProc(HDC dc, int type, float cx, float baseY, float s,
                               float phase, float hpFrac, int ghost);

/* ======================================================================== */
/*                        鼠标悬停提示（植物介绍）                            */
/* ======================================================================== */
/* 玩家需要知道"这张卡是什么、多少钱、多久冷却、多少血"，
   以前只能靠记忆。这里在悬停时弹一个小面板，卡片栏的卡和草坪上已种的植物都支持。 */

static void putTextWrap(HDC dc, HFONT f, COLORREF col, const wchar_t *s,
                        float x, float y, float maxw, float lh, int maxLines);

#define TIP_NONE (-1)
#define TIP_CARD (0)
#define TIP_LAWN (1)

static int   gTipKind  = TIP_NONE;
static int   gTipPlant = -1;
static float gTipX = 0.0f, gTipY = 0.0f;       /* 面板左上角（世界层坐标） */
static int   gTipShow = 0;

/* 卡片栏总宽度：卡槽多时会收窄，这里必须和绘制处用同一套公式 */
static float cardBarGap(void)
{
    int total = gLoadoutN + gBonusN;
    return (((total > 8) ? 68.0f : 82.0f)) + 4.0f;
}

static void hoverUpdate(void)
{
    gTipKind = TIP_NONE;
    gTipPlant = -1;
    gTipShow = 0;
    if (gState != ST_PLAY && gState != ST_PAUSE) return;

    /* 顶部卡片栏 */
    if (mMouseY < 92 && mMouseX >= 140) {
        float cgap = cardBarGap();
        int slot = (int)floor((mMouseX - 144.0f) / cgap);
        if (slot >= 0 && slot < gLoadoutN + gBonusN) {
            int ci = (slot < gLoadoutN) ? gLoadout[slot] : gBonusPlant[slot - gLoadoutN];
            gTipKind  = TIP_CARD;
            gTipPlant = ci;
            gTipX = 144.0f + (float)slot * cgap + cgap * 0.5f - 148.0f;
            gTipY = 92.0f;
            gTipShow = 1;
            return;
        }
        return;                     /* 卡片栏空白处不显示 */
    }
    /* 草坪上已种的植物 */
    if (mMouseY >= 92 && mMouseY < LAWN_Y + ROWS * CELL_H) {
        int row, col, w, hh;
        if (mouseToCell(mMouseX, mMouseY, &row, &col, &w, &hh)) {
            Plant *q = plantAt(row, col);
            if (q && q->alive) {
                gTipKind  = TIP_LAWN;
                gTipPlant = q->type;
                gTipX = (float)mMouseX + 24.0f;
                gTipY = (float)mMouseY - 56.0f;
                gTipShow = 1;
            }
        }
    }
}

/* 面板几何（世界层坐标，会被 2x 变换放大） */
#define TIP_W 300.0f
#define TIP_H 128.0f

static void drawHoverTipShapes(HDC dc)
{
    float tx = gTipX, ty = gTipY;
    int   hr = -1;
    COLORREF ac;
    if (!gTipShow || gTipPlant < 0) return;
    if (gTipPlant >= PT_HERO_FLAME) hr = (int)plantDefs[gTipPlant].rarity - 1;
    ac = (hr >= 0 && hr < HERO_RARITY_COUNT) ? HERO_RARITY_COL[hr] : RGB(140, 196, 120);

    if (tx + TIP_W > VIEW_W - 6.0f) tx = VIEW_W - 6.0f - TIP_W;
    if (tx < 6.0f) tx = 6.0f;
    if (ty + TIP_H > VIEW_H - 6.0f) ty = VIEW_H - 6.0f - TIP_H;
    if (ty < 6.0f) ty = 6.0f;
    gTipX = tx; gTipY = ty;

    fillRound(dc, tx + 4, ty + 5, tx + TIP_W + 4, ty + TIP_H + 5, 10, RGB(16, 26, 18));
    fillRound(dc, tx, ty, tx + TIP_W, ty + TIP_H, 10, RGB(247, 243, 230));
    strokeRound(dc, tx, ty, tx + TIP_W, ty + TIP_H, 10, ac, hr >= 0 ? 4 : 3);
    if (hr >= 0) fillRound(dc, tx, ty, tx + TIP_W, ty + 7.0f, 5, ac);

    /* 左侧画植物本体，一眼看出是哪一株 */
    if (gSprPlant[gTipPlant].ok)
        spriteBlit(dc, &gSprPlant[gTipPlant], tx + 48.0f, ty + TIP_H - 10.0f, 0.58f, 0.58f, -1);
    else
        fillCircle(dc, tx + 48.0f, ty + TIP_H * 0.5f, 22.0f, RGB(150, 190, 130));

    /* 右上角阳光标 */
    fillRound(dc, tx + TIP_W - 78.0f, ty + 14.0f, tx + TIP_W - 12.0f, ty + 40.0f, 7,
              RGB(238, 214, 130));
    fillCircle(dc, tx + TIP_W - 66.0f, ty + 27.0f, 9.0f, RGB(255, 226, 96));
}

static void drawHoverTipText(HDC dc)
{
    wchar_t buf[192];
    int   hr = -1;
    COLORREF ac;
    float tx = gTipX, ty = gTipY;
    const PlantDef *pd;
    if (!gTipShow || gTipPlant < 0) return;
    pd = &plantDefs[gTipPlant];
    if (gTipPlant >= PT_HERO_FLAME) hr = (int)pd->rarity - 1;
    ac = (hr >= 0 && hr < HERO_RARITY_COUNT) ? HERO_RARITY_COL[hr] : RGB(120, 170, 100);

    /* 名字 */
    putTextS(dc, gF22, RGB(44, 34, 20), RGB(255, 255, 255), pd->name,
             tx + 92.0f, ty + 26.0f, 0);
    /* 稀有度标签（神级才有） */
    if (hr >= 0) {
        wsprintfW(buf, L"神级 · %s", HERO_RARITY_NAME[hr]);
        putTextS(dc, gF15, ac, RGB(255, 255, 255), buf, tx + 92.0f, ty + 46.0f, 0);
    } else {
        putTextS(dc, gF15, RGB(140, 140, 130), RGB(255, 255, 255),
                 pd->rarity == 0 ? L"普通植物" : L"稀有植物", tx + 92.0f, ty + 46.0f, 0);
    }
    /* 数值行：费用 / 冷却 / 生命 */
    wsprintfW(buf, L"阳光 %d    冷却 %.0f 秒    生命 %.0f",
              plantCost(gTipPlant), plantCd(gTipPlant), plantHpMax(gTipPlant));
    putTextS(dc, gF15, RGB(70, 60, 44), RGB(255, 255, 255), buf, tx + 12.0f, ty + 70.0f, 0);
    /* 右上角费用数字 */
    wsprintfW(buf, L"%d", plantCost(gTipPlant));
    putTextS(dc, gF18, RGB(80, 62, 20), RGB(255, 255, 255), buf, tx + TIP_W - 34.0f, ty + 27.0f, 1);

    /* 效果说明：神级植物用它自己的 desc，普通植物给一句通用说明 */
    if (pd->desc) {
        putTextWrap(dc, gF15, RGB(60, 52, 40), pd->desc,
                    tx + 12.0f, ty + 92.0f, TIP_W - 24.0f, 20.0f, 2);
    } else {
        putTextWrap(dc, gF15, RGB(60, 52, 40),
                    (gTipKind == TIP_CARD)
                        ? L"左键选中后点草坪种下；右键取消选择"
                        : L"左键点草坪可铲除不需要的植物",
                    tx + 12.0f, ty + 92.0f, TIP_W - 24.0f, 20.0f, 2);
    }
}

/* 接触阴影：三层同心椭圆做出柔和渐变。
   原来只有一层平椭圆（甚至植物完全没有），角色看起来像"贴"在地面上；
   真实物体在接触点附近最暗、往外快速衰减 —— 三层就足够表达这个梯度，
   比一层多两次 fillEllipse，性能可以忽略。 */
static void drawContactShadow(HDC dc, float x, float y, float rx, float ry)
{
    fillEllipse(dc, x, y,          rx * 1.34f, ry * 1.30f, RGB(66, 100, 56));
    fillEllipse(dc, x, y - ry * 0.06f, rx * 0.98f, ry * 0.94f, RGB(48, 78, 42));
    fillEllipse(dc, x, y - ry * 0.12f, rx * 0.56f, ry * 0.54f, RGB(32, 54, 30));
}

/* ---------------- 场景里的植物绘制 ----------------
   按「形态 > 武器 > 基础」的优先级选图：
     多格形态（多列/塔防）改的是布局，是这一局的核心视觉，优先级最高；
     其次是武器模组（射手装了非默认武器就换图）；
     最后回退到基础植物图。
   多格植物只在锚点格画一次（plantIt 会把同 instance 拷进其他格）。 */
static void drawPlantShape(HDC dc, int type, float cx, float baseY, float s,
                           float phase, float hpFrac, int ghost);

static void drawPlantEntity(HDC dc, Plant *p, float ex)
{
    Sprite *sp = &gSprPlant[p->type];
    int isShooter = (p->type == PT_PEASHOOTER || p->type == PT_SNOWPEA ||
                     p->type == PT_REPEATER);
    int wpn = gHasSubWpn ? gSubWpn : gMainWpn;
    float cx = cellCX(p->col) + ex;
    float by = cellBaseY(p->row + p->hh - 1);
    float br, sy = 1.0f;

    /* 合成混种：先画一圈光环，再把两个亲本各画一层（错开一点看出是融合） */
    if (p->fused) {
        int gg;
        for (gg = 0; gg < 3; gg++)
            fillEllipse(dc, cx, by - 26.0f - (float)gg * 4.0f, 44.0f - (float)gg * 7.0f, 12.0f,
                        (gg & 1) ? RGB(255, 232, 140) : RGB(180, 255, 200));
        if (gSprPlant[p->fuseA].ok)
            spriteBlit(dc, &gSprPlant[p->fuseA], cx - 9.0f, by, 0.94f, sy, -1);
        if (gSprPlant[p->fuseB].ok)
            spriteBlit(dc, &gSprPlant[p->fuseB], cx + 11.0f, by - 8.0f, 0.80f, sy * 0.98f, -1);
        return;
    }
    /* 植物接地影：之前完全没有，是"贴上去"观感的最大来源 */
    if (!p->stack) drawContactShadow(dc, cx, by + 1.0f,
                                     (p->w > 1 ? 34.0f : 22.0f), 7.0f);

    if (p->summon && gSprPumpkin.ok)         sp = &gSprPumpkin;   /* 金气召唤的南瓜 */
    else if (p->w > 1 && gSprShape[1].ok)    sp = &gSprShape[1];
    else if (p->hh > 1 && gSprShape[2].ok)   sp = &gSprShape[2];
    else if (isShooter && wpn > WPN_NORMAL && gSprWeapon[wpn - 1].ok)
        sp = &gSprWeapon[wpn - 1];

    if (p->w > 1) cx += CELL_W * 0.5f * (float)(p->w - 1);   /* 多列：居中到两格中间 */
    if (p->stack) { cx -= 8.0f; by -= 26.0f; }               /* 双株：叠株错开，看得出是两株 */
    if (p->shield > 0.0f) {
        float k = CLAMP(p->shield / 2200.0f, 0.25f, 1.0f);
        strokeRound(dc, cx - 34.0f, by - 86.0f, cx + 34.0f, by + 4.0f, 18,
                    RGB(255, (int)(140 + 80 * k), 40), 3);
    }

    br = sin(p->phase * 2.2f);
    sy = 1.0f + br * 0.022f;
    if (sp->ok && sp != &gSprPlant[p->type]) {
        spriteBlit(dc, sp, cx, by + br * 2.0f, 1.0f, sy, -1);
        return;
    }
    /* 基础植物：走原有函数，保留坚果裂纹 / 土豆雷指示灯等叠加 */
    drawPlantShape(dc, p->type, cx, by, 1.0f, p->phase,
                   p->hp / (p->maxhp > 1.0f ? p->maxhp : 1.0f), 0);
}

/* 通用植物造型。cx=中心x, baseY=贴地y, s=缩放, phase=动画相位, hpFrac=血量比例 */
static void drawPlantShape(HDC dc, int type, float cx, float baseY, float s,
                           float phase, float hpFrac, int ghost)
{
    Sprite *sp = &gSprPlant[type];

    /* ---- 有美术资源：精灵渲染（待机呼吸 + 上下浮动） ---- */
    if (sp->ok && !ghost) {
        float br = sin(phase * 2.2f);
        float u  = s * 0.73f;                    /* 宽度归一，供叠加特效用 */
        spriteBlit(dc, sp, cx, baseY + br * 2.0f * s, s, s * (1.0f + br * 0.022f), -1);
        if (type >= 0 && type < PT_COUNT) {
            /* 南瓜罩护盾：在已有植物外再套一圈橙环 */
        }
        /* 坚果墙裂纹：血量低时叠加，纯程序化（资源是完好的） */
        if (type == PT_WALLNUT) {
            COLORREF cr = RGB(112, 72, 34);
            if (hpFrac < 0.66f) {
                line(dc, cx - 20 * u, baseY - 64 * s, cx - 10 * u, baseY - 46 * s, cr, (int)(2.4f * s + 1));
                line(dc, cx - 10 * u, baseY - 46 * s, cx - 19 * u, baseY - 30 * s, cr, (int)(2.4f * s + 1));
            }
            if (hpFrac < 0.33f) {
                line(dc, cx + 17 * u, baseY - 58 * s, cx + 8 * u, baseY - 42 * s, cr, (int)(2.4f * s + 1));
                line(dc, cx + 8 * u, baseY - 42 * s, cx + 18 * u, baseY - 24 * s, cr, (int)(2.4f * s + 1));
                line(dc, cx - 2 * u, baseY - 16 * s, cx - 9 * u, baseY - 28 * s, cr, (int)(2.4f * s + 1));
            }
        }
        return;
    }

    /* ---- 无资源：回退到程序化绘制 ---- */
    drawPlantShapeProc(dc, type, cx, baseY, s, phase, hpFrac, ghost);
}

static void drawPlantShapeProc(HDC dc, int type, float cx, float baseY, float s,
                           float phase, float hpFrac, int ghost)
{
    float bob = sin(phase * 2.2f) * 2.0f * s;
    float sway = sin(phase * 0.9f) * 0.05f;
    COLORREF up = ghost ? RGB(190, 226, 190) : 0;

/* 便捷宏：把归一化坐标变成屏幕坐标 */
#define PX(v) (cx + (v) * s)
#define PY(v) (baseY + (v) * s)

    switch (type) {
    /* ---------------------------------------------------- 向日葵 */
    case PT_SUNFLOWER: {
        COLORREF st = ghost ? up : RGB(94, 176, 68);
        COLORREF lf = ghost ? up : RGB(74, 156, 54);
        COLORREF pt = ghost ? up : RGB(255, 214, 64);
        COLORREF pc = ghost ? up : RGB(196, 138, 46);
        float hx = PX(sway * 10), hy = PY(-56) + bob;
        /* 茎 */
        line(dc, PX(0), PY(0), hx, hy + 12 * s, st, (int)(7 * s));
        /* 叶 */
        fillEllipse(dc, PX(-20 * 1.0f), PY(-6), 15 * s, 9 * s, lf);
        fillEllipse(dc, PX(20), PY(-2), 13 * s, 8 * s, lf);
        /* 花瓣 */
        {
            int i;
            for (i = 0; i < 12; i++) {
                float a = (float)i / 12.0f * 6.28318f + phase * 0.35f;
                fillEllipse(dc, hx + cos(a) * 24 * s, hy + sin(a) * 24 * s,
                            9 * s, 9 * s, pt);
            }
        }
        fillCircle(dc, hx, hy, 19 * s, pc);
        if (!ghost) {
            fillCircle(dc, hx, hy, 15 * s, RGB(214, 156, 54));
            /* 眼睛 */
            fillCircle(dc, hx - 6 * s, hy - 3 * s, 4.5f * s, RGB(52, 36, 20));
            fillCircle(dc, hx + 6 * s, hy - 3 * s, 4.5f * s, RGB(52, 36, 20));
            fillCircle(dc, hx - 5 * s, hy - 4.5f * s, 1.8f * s, RGB(255, 255, 255));
            fillCircle(dc, hx + 7 * s, hy - 4.5f * s, 1.8f * s, RGB(255, 255, 255));
            /* 嘴 */
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            SelectObject(dc, getPen(RGB(120, 80, 30), (int)(3 * s + 0.5f)));
            Arc(dc, (int)(hx - 9 * s), (int)(hy + 2 * s), (int)(hx + 9 * s), (int)(hy + 14 * s),
                (int)(hx + 9 * s), (int)(hy + 8 * s), (int)(hx - 9 * s), (int)(hy + 8 * s));
        }
        break;
    }
    /* ------------------------------------------- 豌豆射手 / 寒冰 / 双发 */
    case PT_PEASHOOTER:
    case PT_SNOWPEA:
    case PT_REPEATER: {
        int frozen = (type == PT_SNOWPEA);
        int twin   = (type == PT_REPEATER);
        COLORREF st = ghost ? up : RGB(74, 158, 62);
        COLORREF hd = ghost ? up : (frozen ? RGB(126, 206, 230) : RGB(96, 194, 78));
        COLORREF hd2= ghost ? up : (frozen ? RGB(168, 228, 246) : RGB(132, 216, 108));
        float hx = PX(-2 + sway * 8), hy = PY(-58) + bob;
        line(dc, PX(0), PY(0), hx, hy + 14 * s, st, (int)(8 * s));
        fillEllipse(dc, PX(-22), PY(-4), 16 * s, 9 * s, ghost ? up : RGB(66, 148, 54));
        fillEllipse(dc, PX(20), PY(-1), 13 * s, 8 * s, ghost ? up : RGB(66, 148, 54));
        /* 炮管 */
        fillRound(dc, hx + 4 * s, hy - 9 * s, hx + 30 * s, hy + 7 * s, 7 * s, hd);
        if (twin) fillRound(dc, hx + 4 * s, hy - 24 * s, hx + 30 * s, hy - 10 * s, 6 * s, hd);
        fillCircle(dc, hx + 30 * s, hy - 1 * s, 8.5f * s, hd2);
        if (twin) fillCircle(dc, hx + 30 * s, hy - 17 * s, 7.0f * s, hd2);
        fillCircle(dc, hx + 32 * s, hy - 1 * s, 5.0f * s, ghost ? up : (frozen ? RGB(60, 120, 160) : RGB(40, 84, 36)));
        /* 头 */
        fillCircle(dc, hx, hy, 21 * s, hd);
        fillCircle(dc, hx - 3 * s, hy - 4 * s, 13 * s, hd2);
        if (!ghost) {
            fillCircle(dc, hx + 2 * s, hy - 5 * s, 5.0f * s, RGB(255, 255, 255));
            fillCircle(dc, hx + 3.5f * s, hy - 5 * s, 2.8f * s, RGB(30, 30, 30));
            if (frozen) {   /* 霜花 */
                fillCircle(dc, hx - 12 * s, hy - 14 * s, 3.4f * s, RGB(240, 252, 255));
                fillCircle(dc, hx - 17 * s, hy - 7 * s, 2.4f * s, RGB(240, 252, 255));
            }
        }
        break;
    }
    /* ---------------------------------------------------- 坚果墙 */
    case PT_WALLNUT: {
        COLORREF nu = ghost ? up : RGB(196, 146, 84);
        float hy = PY(-40) + bob * 0.5f;
        fillEllipse(dc, PX(0), hy, 32 * s, 40 * s, nu);
        fillEllipse(dc, PX(0), hy - 6 * s, 26 * s, 30 * s, ghost ? up : RGB(216, 170, 106));
        if (!ghost) {
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            SelectObject(dc, getPen(RGB(140, 94, 44), (int)(3 * s + 0.5f)));
            Ellipse(dc, (int)PX(-32), (int)(hy - 40 * s), (int)PX(32), (int)(hy + 40 * s));
            /* 眼睛 + 眉 */
            fillCircle(dc, PX(-11), hy - 8 * s, 6.5f * s, RGB(255, 255, 255));
            fillCircle(dc, PX(11), hy - 8 * s, 6.5f * s, RGB(255, 255, 255));
            fillCircle(dc, PX(-11), hy - 8 * s, 3.2f * s, RGB(40, 30, 20));
            fillCircle(dc, PX(11), hy - 8 * s, 3.2f * s, RGB(40, 30, 20));
            line(dc, PX(-18), hy - 16 * s, PX(-5), hy - 18 * s, RGB(120, 78, 36), (int)(3 * s + 0.5f));
            line(dc, PX(18), hy - 16 * s, PX(5), hy - 18 * s, RGB(120, 78, 36), (int)(3 * s + 0.5f));
            line(dc, PX(-8), hy + 8 * s, PX(8), hy + 8 * s, RGB(120, 78, 36), (int)(3 * s + 0.5f));
            /* 裂纹 */
            if (hpFrac < 0.66f) {
                line(dc, PX(-20), hy - 30 * s, PX(-12), hy - 16 * s, RGB(120, 78, 36), (int)(2.4f * s + 0.5f));
                line(dc, PX(-12), hy - 16 * s, PX(-19), hy - 2 * s, RGB(120, 78, 36), (int)(2.4f * s + 0.5f));
            }
            if (hpFrac < 0.33f) {
                line(dc, PX(18), hy - 26 * s, PX(10), hy - 12 * s, RGB(120, 78, 36), (int)(2.4f * s + 0.5f));
                line(dc, PX(10), hy - 12 * s, PX(20), hy + 4 * s, RGB(120, 78, 36), (int)(2.4f * s + 0.5f));
                line(dc, PX(0), hy + 26 * s, PX(-7), hy + 14 * s, RGB(120, 78, 36), (int)(2.4f * s + 0.5f));
            }
        }
        break;
    }
    /* ---------------------------------------------------- 土豆雷 */
    case PT_POTATOMINE: {
        COLORREF po = ghost ? up : RGB(178, 134, 82);
        COLORREF pd = ghost ? up : RGB(140, 100, 58);
        float hy = PY(-14) + bob * 0.4f;
        /* 土堆 */
        fillEllipse(dc, PX(0), PY(0), 30 * s, 10 * s, ghost ? up : RGB(118, 86, 52));
        fillEllipse(dc, PX(0), hy, 26 * s, 17 * s, po);
        fillEllipse(dc, PX(-6), hy - 4 * s, 14 * s, 9 * s, pd);
        if (!ghost) {
            fillCircle(dc, PX(-9), hy - 2 * s, 3.4f * s, RGB(40, 30, 20));
            fillCircle(dc, PX(6), hy - 2 * s, 3.4f * s, RGB(40, 30, 20));
            /* 天线 + 指示灯（激活后闪烁） */
            {
                int blink = (hpFrac > 0.5f) ? 1 : ((int)(phase * 4.0f) & 1);
                COLORREF lc = blink ? RGB(240, 66, 48) : RGB(120, 40, 34);
                line(dc, PX(0), hy - 14 * s, PX(2), hy - 26 * s, RGB(90, 70, 40), (int)(2.5f * s + 0.5f));
                fillCircle(dc, PX(2), hy - 29 * s, 5.0f * s, lc);
                if (blink) fillCircle(dc, PX(2), hy - 29 * s, 2.4f * s, RGB(255, 220, 140));
            }
        }
        break;
    }
    /* ---------------------------------------------------- 樱桃炸弹 */
    case PT_CHERRY: {
        float pulse = 1.0f + sin(phase * 10.0f) * 0.08f;
        float hy = PY(-26) + bob;
        line(dc, PX(-11), hy - 14 * s, PX(-4), PY(-46) + bob, ghost ? up : RGB(96, 150, 60), (int)(3.5f * s));
        line(dc, PX(11), hy - 14 * s, PX(4), PY(-46) + bob, ghost ? up : RGB(96, 150, 60), (int)(3.5f * s));
        fillCircle(dc, PX(-13), hy, 17 * s * pulse, ghost ? up : RGB(214, 52, 44));
        fillCircle(dc, PX(13), hy, 17 * s * pulse, ghost ? up : RGB(196, 42, 36));
        if (!ghost) {
            fillCircle(dc, PX(-18), hy - 5 * s, 5 * s, RGB(246, 130, 118));
            fillCircle(dc, PX(8), hy - 5 * s, 5 * s, RGB(240, 120, 108));
            line(dc, PX(-20), hy - 12 * s, PX(-8), hy - 6 * s, RGB(70, 26, 22), (int)(3 * s + 0.5f));
            line(dc, PX(6), hy - 12 * s, PX(18), hy - 6 * s, RGB(70, 26, 22), (int)(3 * s + 0.5f));
            fillCircle(dc, PX(-13), hy + 2 * s, 3.0f * s, RGB(255, 255, 255));
            fillCircle(dc, PX(13), hy + 2 * s, 3.0f * s, RGB(255, 255, 255));
        }
        break;
    }
    /* ---------------------------------------------------- 火爆辣椒 */
    case PT_JALAPENO: {
        float hy = PY(-32) + bob;
        line(dc, PX(-4), PY(-40), PX(-14), PY(-56) + bob, ghost ? up : RGB(96, 150, 60), (int)(5 * s));
        fillEllipse(dc, PX(0), hy, 16 * s, 30 * s, ghost ? up : RGB(212, 46, 38));
        fillEllipse(dc, PX(-5), hy - 4 * s, 6 * s, 18 * s, ghost ? up : RGB(240, 108, 90));
        if (!ghost) {
            fillCircle(dc, PX(-4), hy - 8 * s, 4.2f * s, RGB(255, 255, 255));
            fillCircle(dc, PX(-4), hy - 8 * s, 2.2f * s, RGB(60, 20, 16));
            line(dc, PX(-14), hy - 14 * s, PX(-2), hy - 18 * s, RGB(90, 26, 20), (int)(3 * s + 0.5f));
        }
        break;
    }
    }
#undef PX
#undef PY
}

/* 僵尸绘制 */
static void drawZombieProc(HDC dc, Zombie *z);

static void drawZombie(HDC dc, Zombie *z)
{
    Sprite *sp = &gSprZombie[z->type];
    Sprite *spF = &gSprZombieFlash[z->type];
    Sprite *spB = &gSprZombieBlue[z->type];

    /* 倒地中：播放绕脚底旋转的倒地帧，末段淡出 */
    if (z->dead) {
        float t  = 1.0f - CLAMP(z->deadT / DEATH_DUR, 0, 1);
        int   fi = (int)CLAMP(t * 5.0f, 0, 4);
        float al = (t > 0.68f) ? (1.0f - (t - 0.68f) / 0.32f) : 1.0f;
        Sprite *sd = &gSprZombieDead[z->type][fi];
        drawContactShadow(dc, z->x, z->y + 2, 22.0f, 7.5f);
        if (sd->ok) spriteBlit(dc, sd, z->x, z->y, 1.0f, 1.0f,
                               (int)(255.0f * CLAMP(al, 0, 1)));
        else        drawZombieProc(dc, z);
        return;
    }

    /* 啃食时切到第二套姿态 */
    if (z->eating && gSprZombieEat[z->type].ok) {
        sp  = &gSprZombieEat[z->type];
        spF = &gSprZombieEatFlash[z->type];
        spB = &gSprZombieEatBlue[z->type];
    }

    /* ---- 有美术资源：精灵渲染 ---- */
    if (sp->ok) {
        float walk = z->anim * 6.0f;
        float bob  = z->eating ? fabs(sin(z->anim * 15.0f)) * 1.6f
                               : fabs(sin(walk)) * 1.6f;
        float sway = z->eating ? sin(z->anim * 5.0f) * 0.5f : sin(z->anim * 3.0f) * 1.2f;
        float sy   = z->eating ? 0.94f : (1.0f - fabs(sin(walk)) * 0.02f);
        float cx   = z->x + sway - (z->eating ? 6.0f : 0.0f);
        float cy   = z->y;
        float top  = cy - (float)sp->h / SS;

        drawContactShadow(dc, z->x, cy + 2, 22.0f, 7.5f);           /* 三层接触影 */
        spriteBlit(dc, sp, cx, cy - bob, 1.0f, sy, -1);
        /* 减速染蓝 / 受击闪白：叠加同尺寸纯色剪影 */
        if (z->slow > 0 && spB->ok)
            spriteBlit(dc, spB, cx, cy - bob, 1.0f, sy, 96);
        if (z->flash > 0 && spF->ok)
            spriteBlit(dc, spF, cx, cy - bob, 1.0f, sy,
                       (int)(70 + 150 * CLAMP(z->flash / 0.09f, 0, 1)));
        /* 血条 */
        if (z->hp < z->maxhp - 1) {
            float w = 34, hpf = z->hp / z->maxhp;
            float by = top - 8;
            fillRect(dc, z->x - w / 2, by, z->x + w / 2, by + 4, RGB(34, 26, 26));
            fillRect(dc, z->x - w / 2 + 1, by + 1, z->x - w / 2 + 1 + (w - 2) * hpf, by + 3,
                     hpf > 0.5f ? RGB(110, 200, 90) : (hpf > 0.22f ? RGB(226, 178, 60) : RGB(214, 62, 52)));
        }
        return;
    }

    /* ---- 无资源：回退到程序化绘制 ---- */
    drawZombieProc(dc, z);
}

static void drawZombieProc(HDC dc, Zombie *z)
{
    float x = z->x, y = z->y;
    float walk = z->anim * 6.0f;
    float bob  = z->eating ? sin(z->anim * 14.0f) * 1.2f : fabs(sin(walk)) * 1.6f;
    float armS = z->eating ? sin(z->anim * 14.0f) * 2.0f : sin(walk) * 2.6f;
    COLORREF skin = RGB(158, 184, 132);
    COLORREF skinD= RGB(128, 154, 104);
    COLORREF cloth= RGB(94, 104, 122);
    COLORREF clothD=RGB(72, 80, 96);
    COLORREF hair = RGB(58, 50, 52);
    float f = 0;

    if (z->flash > 0) f = 0.55f;
    if (z->slow > 0) { skin = RGB(146, 186, 200); skinD = RGB(116, 156, 176);
                       cloth = RGB(96, 120, 142); clothD = RGB(74, 96, 116); hair = RGB(70, 72, 88); }
    if (f > 0) { skin = tint(skin, f); skinD = tint(skinD, f); cloth = tint(cloth, f);
                 clothD = tint(clothD, f); hair = tint(hair, f); }

    /* 影子 */
    fillEllipse(dc, x, y + 2, 22, 7, RGB(46, 92, 40));

    /* 腿 */
    {
        float l1 = sin(walk) * 5.0f, l2 = -l1;
        SelectObject(dc, getPen(clothD, 7));
        MoveToEx(dc, (int)(x - 2), (int)(y - 20), NULL); LineTo(dc, (int)(x - 2 + l1), (int)y);
        MoveToEx(dc, (int)(x + 3), (int)(y - 20), NULL); LineTo(dc, (int)(x + 3 + l2), (int)y);
    }
    /* 躯干 */
    {
        float ty = y - 46 - bob;
        fillRound(dc, x - 13, ty, x + 13, y - 18, 6, cloth);
        fillRound(dc, x - 13, ty + 3, x + 5, y - 20, 5, clothD);
        fillCircle(dc, x + 6, ty + 16, 3.4f, tint(cloth, -0.25f));   /* 破洞 */
    }
    /* 手臂（前伸） */
    {
        float ay = y - 40 - bob;
        SelectObject(dc, getPen(skin, 7));
        MoveToEx(dc, (int)(x - 5), (int)ay, NULL);
        LineTo(dc, (int)(x - 25), (int)(ay + 3 + armS * 0.5f));
        MoveToEx(dc, (int)(x - 4), (int)(ay + 4), NULL);
        LineTo(dc, (int)(x - 23), (int)(ay + 10));
        fillCircle(dc, x - 27, ay + 3 + armS * 0.5f, 4.2f, skinD);
        fillCircle(dc, x - 25, ay + 10, 4.2f, skinD);
    }
    /* 头 */
    {
        float hy = y - 58 - bob + (z->eating ? 2.0f : 0);
        float hx = x - 2;
        fillCircle(dc, hx, hy, 13, skin);
        fillCircle(dc, hx - 5, hy - 2, 9, tint(skin, 0.06f));
        /* 头发 */
        {
            int i;
            for (i = 0; i < 5; i++) {
                float a = -2.5f + i * 0.42f;
                line(dc, hx + cos(a) * 9, hy - 6 + sin(a) * 6,
                        hx + cos(a) * 16, hy - 11 + sin(a) * 9, hair, 3);
            }
        }
        /* 眼睛 */
        fillEllipse(dc, hx - 6, hy - 1, 3.8f, 3.2f, RGB(240, 240, 232));
        fillEllipse(dc, hx + 1, hy - 1, 3.4f, 3.0f, RGB(240, 240, 232));
        fillCircle(dc, hx - 7.0f, hy - 1, 1.8f, RGB(30, 26, 26));
        fillCircle(dc, hx + 0.3f, hy - 1, 1.7f, RGB(30, 26, 26));
        /* 嘴 */
        {
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            SelectObject(dc, getPen(RGB(70, 40, 40), 2));
            MoveToEx(dc, (int)(hx - 8), (int)(hy + 6), NULL);
            LineTo(dc, (int)(hx + 5), (int)(hy + 6));
            line(dc, hx - 5, hy + 6, hx - 5, hy + 9, RGB(240, 240, 232), 2);
            line(dc, hx - 1, hy + 6, hx - 1, hy + 9, RGB(240, 240, 232), 2);
            line(dc, hx + 3, hy + 6, hx + 3, hy + 9, RGB(240, 240, 232), 2);
        }
        /* 头饰 */
        if (z->type == ZT_CONE) {
            float ch = hy - 8;
            COLORREF cc = (z->flash > 0) ? tint(RGB(212, 122, 58), 0.5f) : RGB(212, 122, 58);
            POINT p[3];
            p[0].x = (int)(hx - 14); p[0].y = (int)ch;
            p[1].x = (int)(hx + 14); p[1].y = (int)ch;
            p[2].x = (int)(hx + 1);  p[2].y = (int)(ch - 22);
            poly(dc, p, 3, cc, 0, 0, 0);
            fillRect(dc, hx - 15, ch, hx + 15, ch + 4, tint(cc, -0.22f));
            line(dc, hx - 7, ch - 2, hx - 1, ch - 16, tint(cc, -0.15f), 3);
        }
        else if (z->type == ZT_BUCKET) {
            float ch = hy - 9;
            COLORREF mc = (z->flash > 0) ? tint(RGB(176, 182, 190), 0.5f) : RGB(176, 182, 190);
            fillRound(dc, hx - 13, ch - 4, hx + 13, ch + 12, 4, mc);
            fillRect(dc, hx - 15, ch + 8, hx + 15, ch + 12, tint(mc, -0.22f));
            fillEllipse(dc, hx - 9, ch + 1, 3, 9, tint(mc, 0.25f));
            fillEllipse(dc, hx + 8, ch, 3, 4.5f, tint(mc, -0.18f));   /* 凹痕 */
        }
        else if (z->type == ZT_FLAG) {
            /* 旗杆 + 红旗 */
            line(dc, x + 15, y - 26, x + 20, y - 98, RGB(120, 96, 70), 3);
            {
                POINT p[3];
                p[0].x = (int)(x + 20); p[0].y = (int)(y - 98);
                p[1].x = (int)(x + 48 + sin(z->anim * 4) * 3); p[1].y = (int)(y - 90);
                p[2].x = (int)(x + 20); p[2].y = (int)(y - 78);
                poly(dc, p, 3, RGB(198, 44, 40), 0, 0, 0);
            }
        }
    }
    /* 血条 */
    if (z->hp < z->maxhp - 1) {
        float w = 34, hpf = z->hp / z->maxhp;
        fillRect(dc, x - w / 2, y - 96, x + w / 2, y - 92, RGB(40, 30, 30));
        fillRect(dc, x - w / 2 + 1, y - 95, x - w / 2 + 1 + (w - 2) * hpf, y - 93,
                 hpf > 0.5f ? RGB(110, 200, 90) : (hpf > 0.22f ? RGB(226, 178, 60) : RGB(214, 62, 52)));
    }
}

/* 卡片上的植物缩略图 */
static void drawIcon(HDC dc, int type, float cx, float cy, float sz)
{
    float s = sz / 100.0f;
    drawPlantShape(dc, type, cx, cy + 24 * s, s, 0, 1.0f, 0);
}

/* ======================================================================== */
/*                                  场景绘制                                  */
/* ======================================================================== */
static const COLORREF GRASS_A = RGB(124, 198, 76);
static const COLORREF GRASS_B = RGB(104, 172, 62);
static const COLORREF HUD_BG  = RGB(104, 70, 42);
static const COLORREF HUD_DK  = RGB(74, 48, 28);
static const COLORREF HUD_LT  = RGB(146, 104, 62);

static void drawBackground(HDC dc)
{
    int r, c;
    /* 场景背景：有资源就用整张后院背景图，否则退回程序化色块 */
    if (gSprBackground.ok) {
        spriteBlit(dc, &gSprBackground, VIEW_W * 0.5f, (float)VIEW_H, 1.0f, 1.0f, -1);
    } else {
        fillRect(dc, 0, 0, VIEW_W, VIEW_H, RGB(66, 112, 74));
        {
            int y;
            for (y = 0; y < VIEW_H; y += 26) {
                fillRect(dc, 0, y, LAWN_X, y + 24, RGB(150, 96, 68));
                fillRect(dc, 0, y + 24, LAWN_X, y + 26, RGB(112, 68, 46));
            }
            for (y = 0; y < VIEW_H; y += 52)
                fillRect(dc, LAWN_X * 0.5f - 13, y, LAWN_X * 0.5f - 11, y + 26, RGB(112, 68, 46));
        }
        fillRect(dc, LAWN_X + COLS * CELL_W, 0, VIEW_W, VIEW_H, RGB(132, 100, 62));
        fillRect(dc, LAWN_X + COLS * CELL_W, 0, LAWN_X + COLS * CELL_W + 8, VIEW_H, RGB(100, 74, 46));
    }

    /* 草坪：模式决定用哪张图（标准/夜间/水中），无变体时回退到主草坪 */
    {
        /* 关卡定基调（屋顶/沙漠/夜间/泳池），模式可以在其之上再覆盖成夜间或水中 */
        Sprite *ls = &gSprLawn;
        switch (gLawnVariant) {
        case 1: if (gSprLawnNight.ok)  ls = &gSprLawnNight;  break;
        case 2: if (gSprLawnWater.ok)  ls = &gSprLawnWater;  break;
        case 3: if (gSprLawnRoof.ok)   ls = &gSprLawnRoof;   break;
        case 4: if (gSprLawnDesert.ok) ls = &gSprLawnDesert; break;
        default: break;
        }
        if (gCurMode == MODE_NIGHT && gSprLawnNight.ok) ls = &gSprLawnNight;
        if (gCurMode == MODE_WATER && gSprLawnWater.ok) ls = &gSprLawnWater;
        if (ls->ok) {
            spriteBlit(dc, ls, LAWN_X + COLS * CELL_W * 0.5f,
                       LAWN_Y + ROWS * CELL_H, 1.0f, 1.0f, -1);
        } else {
            for (r = 0; r < ROWS; r++)
                for (c = 0; c < COLS; c++) {
                    COLORREF g = ((r + c) & 1) ? GRASS_A : GRASS_B;
                    fillRect(dc, LAWN_X + c * CELL_W, LAWN_Y + r * CELL_H,
                             LAWN_X + (c + 1) * CELL_W, LAWN_Y + (r + 1) * CELL_H, g);
                }
            for (r = 0; r < ROWS; r++)
                fillRect(dc, LAWN_X, LAWN_Y + (r + 1) * CELL_H - 2,
                         LAWN_X + COLS * CELL_W, LAWN_Y + (r + 1) * CELL_H, RGB(100, 168, 60));
            fillRect(dc, LAWN_X, LAWN_Y, LAWN_X + 3, LAWN_Y + ROWS * CELL_H, RGB(100, 168, 60));
        }
    }
}

/* 顶部 HUD */
static void drawHUDShapes(HDC dc)
{
    int i;
    /* 木质背景条 */
    fillRect(dc, 0, 0, VIEW_W, 92, HUD_BG);
    fillRect(dc, 0, 88, VIEW_W, 92, HUD_DK);
    fillRect(dc, 0, 0, VIEW_W, 5, HUD_LT);

    /* 阳光面板 */
    strokeRound(dc, 8, 8, 132, 80, 8, HUD_LT, 3);
    fillRound(dc, 10, 10, 130, 78, 7, RGB(222, 200, 152));
    fillCircle(dc, 36, 44, 20, RGB(255, 220, 70));
    fillCircle(dc, 36, 44, 15, RGB(255, 236, 128));
    {
        int k;
        for (k = 0; k < 8; k++) {
            float a = k * 0.7854f;
            fillTriangle(dc, 36 + cos(a) * 21, 44 + sin(a) * 21,
                             36 + cos(a + 0.35f) * 21, 44 + sin(a + 0.35f) * 21,
                             36 + cos(a + 0.175f) * 30, 44 + sin(a + 0.175f) * 30,
                             RGB(255, 214, 60));
        }
    }

    /* 卡片：只画本局出战编组里的植物（大厅里选的，最多 gCardSlots 张） */
    {
    int   cardTotal = gLoadoutN + gBonusN;
    float cw = (cardTotal > 8) ? 68.0f : 82.0f;      /* 卡槽多时自动收窄 */
    float cgap = cw + 4.0f;
    for (i = 0; i < gLoadoutN + gBonusN; i++) {
        int   ci = (i < gLoadoutN) ? gLoadout[i] : gBonusPlant[i - gLoadoutN];
        float x = 144 + (float)i * cgap, y = 8;
        float w = cw - 4.0f, h = 76;
        int   ready = (cardCD[ci] <= 0.0f) && (gSun >= plantCost(ci));
        /* 卡座 */
        strokeRound(dc, x - 3, y - 3, x + w + 3, y + h + 3, 7, HUD_LT, 3);
        /* 卡面用材质而不是纯色：纯色圆角矩形是"塑料感"的主要来源 */
        fillRound(dc, x, y, x + w, y + h, 6, ready ? RGB(238, 224, 178) : RGB(196, 186, 162));
        {
            int ty;
            for (ty = (int)y + 3; ty < (int)(y + h) - 3; ty += 4)
                fillRect(dc, x + 2, ty, x + w - 2, ty + 1,
                         ready ? RGB(228, 212, 164) : RGB(186, 176, 152));
        }
        /* 选中高亮 */
        if (gSel == ci && !gShovel) strokeRound(dc, x - 5, y - 5, x + w + 5, y + h + 5, 8, RGB(255, 240, 120), 4);
        /* 图标 */
        drawIcon(dc, ci, x + w * 0.5f, y + h * 0.5f - 2, 56);
        /* 冷却遮罩 */
        if (cardCD[ci] > 0.0f) {
            float frac = cardCD[ci] / plantCd(ci);
            float cover = h * CLAMP(frac, 0, 1);
            fillRect(dc, x, y, x + w, y + cover, RGB(120, 120, 130));
        }
        /* 阳光不足 */
        if (gSun < plantCost(ci)) {
            HBRUSH hb = CreateHatchBrush(HS_DIAGCROSS, RGB(70, 70, 80));
            RECT rc; rc.left = (int)x; rc.top = (int)y; rc.right = (int)(x + w); rc.bottom = (int)(y + h);
            FillRect(dc, &rc, hb);
            DeleteObject(hb);
        }
    }
    /* 铲子卡 */
    {
        float x = 144 + (float)cardTotal * cgap, y = 8, w = 78, h = 76;
        strokeRound(dc, x - 3, y - 3, x + w + 3, y + h + 3, 7, HUD_LT, 3);
        fillRound(dc, x, y, x + w, y + h, 6, RGB(238, 224, 178));
        if (gShovel) strokeRound(dc, x - 5, y - 5, x + w + 5, y + h + 5, 8, RGB(255, 240, 120), 4);
        /* 铲子造型 */
        line(dc, x + 24, y + 16, x + 50, y + 44, RGB(150, 108, 62), 8);
        {
            POINT p[4];
            p[0].x = (int)(x + 44); p[0].y = (int)(y + 36);
            p[1].x = (int)(x + 64); p[1].y = (int)(y + 56);
            p[2].x = (int)(x + 50); p[2].y = (int)(y + 68);
            p[3].x = (int)(x + 32); p[3].y = (int)(y + 50);
            poly(dc, p, 4, RGB(196, 200, 208), 1, RGB(140, 146, 156), 2);
        }
    }
    }
}

/* 草坪网格高亮 */
static void drawGridHighlight(HDC dc)
{
    int col = (int)floor((mMouseX - LAWN_X) / (float)CELL_W);
    int row = (int)floor((mMouseY - LAWN_Y) / (float)CELL_H);
    int valid;
    if (gSel < 0 && !gShovel) return;
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return;

    if (gShovel) {
        int ok = (plantAt(row, col) != NULL);
        strokeRect(dc, LAWN_X + col * CELL_W + 2, LAWN_Y + row * CELL_H + 2,
                   LAWN_X + (col + 1) * CELL_W - 2, LAWN_Y + (row + 1) * CELL_H - 2,
                   ok ? RGB(255, 120, 90) : RGB(160, 160, 160), 3);
        return;
    }
    valid = (plantAt(row, col) == NULL);
    strokeRect(dc, LAWN_X + col * CELL_W + 2, LAWN_Y + row * CELL_H + 2,
               LAWN_X + (col + 1) * CELL_W - 2, LAWN_Y + (row + 1) * CELL_H - 2,
               valid ? RGB(255, 255, 210) : RGB(220, 90, 80), 3);
    if (valid)
        drawPlantShape(dc, gSel, cellCX(col), cellBaseY(row), 1.0f,
                       gTime, 1.0f, 0);
}

/* 阳光 */
static void drawSun(HDC dc, SunDrop *s)
{
    float fade = 1.0f;
    float r;
    int i;
    if (s->life < 2.0f) fade = s->life / 2.0f;
    if (fade < 0.55f && ((int)(gTime * 12) & 1)) fade = 1.0f - fade + 0.2f;

    /* ---- 有美术资源：直接用带发光渐变的精灵 ---- */
    if (gSprSun.ok) {
        float k = (0.94f + sin(s->phase * 3.0f) * 0.06f)
                  * (0.70f + 0.30f * CLAMP(fade, 0, 1))
                  * (s->flying ? 0.85f : 1.0f);
        spriteBlit(dc, &gSprSun, s->x, s->y + (float)gSprSun.h * k * 0.5f / SS, k, k, -1);
        return;
    }

    r = 22.0f * (0.94f + sin(s->phase * 3.0f) * 0.06f) * (s->flying ? 0.8f : 1.0f);
    if (fade < 0.55f) {  /* 快消失时缩小闪烁 */
        if (((int)(gTime * 12) & 1)) fade = 1.0f - fade + 0.2f;
    }
    r *= (0.7f + 0.3f * CLAMP(fade, 0, 1));
    for (i = 0; i < 8; i++) {
        float a = i * 0.7854f + s->phase * 0.8f;
        fillTriangle(dc, s->x + cos(a) * r * 0.9f, s->y + sin(a) * r * 0.9f,
                         s->x + cos(a + 0.4f) * r * 0.9f, s->y + sin(a + 0.4f) * r * 0.9f,
                         s->x + cos(a + 0.2f) * r * 1.45f, s->y + sin(a + 0.2f) * r * 1.45f,
                         RGB(255, 212, 56));
    }
    fillCircle(dc, s->x, s->y, r, RGB(255, 220, 70));
    fillCircle(dc, s->x, s->y, r * 0.74f, RGB(255, 240, 140));
    fillCircle(dc, s->x - r * 0.25f, s->y - r * 0.25f, r * 0.3f, RGB(255, 252, 210));
}

/* ======================================================================== */
/*                                 游戏逻辑                                  */
/* ======================================================================== */

static void spawnZombieAt(int type, int row, float atX)
{
    int i;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        if (!zombies[i].active) {
            Zombie *z = &zombies[i];
            memset(z, 0, sizeof(*z));
            z->active = 1;
            z->type = type;
            z->row = row;
            z->x = atX;
            z->y = cellBaseY(row) + 4;
            z->hp = z->maxhp = zombieHpMax(type) * levelDefs[gCurLevel].hpMul;
            z->speed = zSpeed[type] * gZombieSpdMul * gZombieSpdUp
                     * levelDefs[gCurLevel].spdMul;
            if (type == ZT_ZAMBONI) z->speed *= gZamboniSlow;      /* 破冰 */
            z->anim = rnd(0, 6.28f);
            z->shield = (type == ZT_SCREENDOOR) ? 1 : 0;
            if (type == ZT_DIGGER) {
                /* 挖掘僵尸不是从屏幕外走进来，而是直接从草坪最右列钻出来 ——
                   这逼你在后排也留防御，而不是把火力全堆在左侧。 */
                z->x = LAWN_X + COLS * CELL_W - 26.0f;
                { int q; for (q = 0; q < 20; q++)
                    addParticle(z->x + rnd(-20, 20), z->y - rnd(0, 24), rnd(-60, 60),
                                rnd(-140, -40), 0.6f, rnd(3, 7), RGB(150, 120, 80), 1); }
                addShake(3.0f, 0.15f);
            }
            return;
        }
    }
}

/* 原签名保留：从屏幕右外侧生成 */
static void spawnZombie(int type, int row)
{
    spawnZombieAt(type, row, VIEW_W + rnd(30.0f, 130.0f));
}

static void zombieDie(Zombie *z, int byMower)
{
    COLORREF c1 = RGB(158, 184, 132), c2 = RGB(94, 104, 122);
    int i;
    if (z->dead) return;                 /* 已经在倒地了 */
    z->dead   = 1;
    z->deadT  = DEATH_DUR;
    z->eating = 0;
    z->slow   = 0;
    gKilled++;
    if (HAS(R_KILL_STACK) && gKillStack < 100) gKillStack++;
    if (gKillSun > 0) gSun += (int)((float)gKillSun * gKillSunMul);
    if (gBanker) gGold += byMower ? (int)(4.0f * gWeekGoldMul) : (int)(1.0f * gWeekGoldMul);
    if (gStarHunter && (rand() % 100) < 3) gSave.coins += 1;   /* 猎星者 */
    if (byMower) {                       /* 被小推车碾的：立刻碎掉，不播倒地 */
        for (i = 0; i < 14; i++)
            addParticle(z->x + rnd(-14, 14), z->y - rnd(10, 80), rnd(-70, 70),
                        rnd(-130, -30), rnd(0.5f, 1.0f), rnd(3, 7),
                        (i & 1) ? c1 : c2, 1);
        z->active = 0;
        z->dead = 0;
        addShake(4, 0.15f);
    }
}

/* 爆炸伤害：mode 0 = 圆形范围（3x3）；mode 1 = 整行 */
static int gChainDepth = 0;

static void explode(float cx, float cy, float radius, float dmg, int mode, int row)
{
    int i, chain[8], nchain = 0;
    dmg    *= gExplodeDmgMul * gTrDemoDmg;      /* 爆破羁绊 */
    radius *= gExplodeRadMul;
    /* 充能槽消费点：有充能时这次爆炸伤害 +60%、范围 +30%，并消耗一格 */
    if (gCharge > 0) { gCharge--; dmg *= 1.60f; radius *= 1.30f; }
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        float dx, dy;
        if (!z->active || z->dead) continue;
        if (mode == 1) {
            if (z->row == row) { z->hp -= dmg; z->slow = 0.0f; z->rooted = 0.35f; }
        } else {
            dx = fabs(z->x - cx);
            dy = fabs(z->y - cy);
            if (dx < radius && dy < CELL_H * 1.5f) {
                z->hp -= dmg;
                z->rooted = 0.35f;              /* 爆心震荡：短暂定身 */
                z->x += (z->x >= cx ? 1.0f : -1.0f) * 12.0f; /* 爆炸击退 */
            }
        }
        if (z->hp <= 0.0f && nchain < 8) chain[nchain++] = i;
    }
    if (gEmber > 0) fireSpawn(cx, cy + 12.0f, (float)gEmber);   /* 余烬（贴地） */
    /* 连锁引爆：被炸死的僵尸原地再爆一次（限深度，防无限递归） */
    if (gChainBoom && gChainDepth < 2) {
        gChainDepth++;
        for (i = 0; i < nchain && i < 4; i++)
            explode(zombies[chain[i]].x, zombies[chain[i]].y - 22.0f,
                    CELL_W * 0.72f, dmg * 0.45f, 0, 0);
        gChainDepth--;
    }
}

static void doExplosionFX(float x, float y, float power, COLORREF c1, COLORREF c2)
{
    int i, n = (int)(28 * power);
    for (i = 0; i < n; i++) {
        float a = rnd(0, 6.28318f), sp = rnd(60, 270) * power;
        addParticle(x, y, cos(a) * sp, sin(a) * sp * 0.7f - 40,
                    rnd(0.35f, 0.8f), rnd(5, 16), (i & 3) ? c1 : c2, 1);
    }
}

/* ------------------------------------------------ 波次系统 */
static void startWave(int idx)
{
    int budget, flag, tries;
    int row;
    float t0;

    if (idx >= levelDefs[gCurLevel].waves) return;
    flag = (idx == 9 || idx == levelDefs[gCurLevel].waves - 1);
    /* 难度曲线：加了遗物成长后必须改陡，否则中后期会被碾平。
       线性 2+idx 在肉鸽下会变得毫无压力。 */
    /* 难度曲线（第三版）
       第一版 2+idx 线性        -> 实测真人 100% 通关
       第二版 加平方项          -> 仍然太松
       第三版 移除羁绊后重调：线性项 + 平方项 + 立方项一起上，
       后期波次密度陡增，逼玩家在资源和防守之间做真正的取舍。 */
    budget = 2 + (int)((float)idx * 1.9f + (float)idx * (float)idx * 0.95f
                       + (float)idx * (float)idx * (float)idx * 0.030f);
    if (flag) budget = (int)(budget * 1.55f);

    if (gInterest > 0) gSun += gSun * gInterest / 100;   /* 利滚利：上一波结算 */
    t0 = gTime;
    /* 后期僵尸按关卡进度解锁 */
    if (idx >= 5)  zCost[ZT_DANCER]  = 4;
    if (idx >= 8)  zCost[ZT_BALLOON] = 4;
    if (idx >= 10) zCost[ZT_ZAMBONI] = 6;
    /* 第二批反制型僵尸：每种都在"玩家刚形成某个套路"的时候登场 */
    if (idx >= 4)  zCost[ZT_VAULTER]    = 3;   /* 撑杆：克制前排坚果 */
    if (idx >= 6)  zCost[ZT_NEWSPAPER]  = 3;   /* 读报：打断慢磨节奏 */
    if (idx >= 8)  zCost[ZT_SCREENDOOR] = 4;   /* 铁门：逼你上磁力菇/爆炸 */
    if (idx >= 11) zCost[ZT_DIGGER]     = 5;   /* 挖掘：逼你守后排 */
    if (idx >= 13) zCost[ZT_FOOTBALL]   = 6;   /* 橄榄球：逼你堆真实 DPS */
    if (idx >= 16) zCost[ZT_GIANT]      = 12;  /* 巨人 BOSS */
    /* 旗帜僵尸领队 */
    if (flag) {
        int r = rndi(0, ROWS);
        if (spawnQN < MAX_SPAWNS) { spawnQ[spawnQN].type = ZT_FLAG; spawnQ[spawnQN].row = r; spawnQ[spawnQN].t = t0; spawnQN++; }
        budget -= 1;
    }
    tries = 0;
    while (budget > 0 && tries < 600 && spawnQN < MAX_SPAWNS) {
        int pool[3], pn = 0, pick, type;
        tries++;
        pool[pn++] = ZT_NORMAL;
        if (idx >= 2) pool[pn++] = ZT_CONE;
        if (idx >= 5) pool[pn++] = ZT_BUCKET;
        pick = pool[rndi(0, pn)];
        if (zCost[pick] > budget) {
            if (zCost[ZT_NORMAL] > budget) break;
            pick = ZT_NORMAL;
        }
        type = pick;
        row = rndi(0, ROWS);
        spawnQ[spawnQN].type = type;
        spawnQ[spawnQN].row = row;
        /* 同一波内的散布窗口：越到后期压得越紧。
           波次预算早就超过出怪上限（160），继续加预算没用，
           "同样数量的僵尸在更短时间内涌出"才是真正加难度的杠杆。 */
        {
            float win = 10.0f - (float)idx * 0.27f;
            if (win < 3.5f) win = 3.5f;
            spawnQ[spawnQN].t = t0 + rnd(0, win) * (flag ? 0.62f : 1.0f);
        }
        spawnQN++;
        budget -= zCost[type];
    }
    gWave = idx + 1;
    if (flag) { addShake(6, 0.55f); flashRed = 0.5f; }
    waveTimer = flag ? 23.5f : 17.2f;   /* 波间喘息也压缩，整体节奏更紧 */
}

static int zombiesAlive(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_ZOMBIES; i++)
        if (zombies[i].active && !zombies[i].dead) n++;
    return n;
}

static int spawnPeaAt(float x, float y, float vx, float vy, float dmg,
                      int row, int src, int frozen)
{
    int k;
    for (k = 0; k < MAX_PEAS; k++) if (!peas[k].active) {
        memset(&peas[k], 0, sizeof(Pea));
        peas[k].active = 1;
        peas[k].x = x; peas[k].y = y;
        peas[k].vx = vx; peas[k].vy = vy;
        peas[k].dmg = dmg; peas[k].row = row;
        peas[k].src = src; peas[k].frozen = frozen;
        peas[k].root = 0; peas[k].knockback = 0; peas[k].homing = 0;
        return k;
    }
    return -1;
}

/* ------------------------------------------------ 放置植物 */
static void plantIt(int row, int col, int type)
{
    int w = 1, hh = 1, rr, cc;
    Plant *p;
    if (gCurMode == MODE_MULTILANE) w = 2;
    if (gCurMode == MODE_TOWER)    hh = 2;
    if (!cellLegal(row, col, w, hh)) return;
    /* 双株模式：先填 slot0，再填 slot1；叠加的那株带 stack 标记（画在上方） */
    p = plantSlotFor(row, col);
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->alive = 1; p->type = type; p->row = row; p->col = col;
    p->w = w; p->hh = hh;
    p->stack = (p != &grid[row][col]) ? 1 : 0;   /* 双株：叠在上面的那株 */
    p->fuseMul = 1.0f;
    p->hp = p->maxhp = plantHpMax(type);
    p->phase = rnd(0, 6.28f);
    if (gBloodPact) p->hp = p->maxhp * 0.70f;   /* 血祭 */
    switch (type) {
    case PT_SUNFLOWER:  p->timer = 6.0f; break;
    case PT_PEASHOOTER: p->timer = 0.4f; break;
    case PT_SNOWPEA:    p->timer = 0.4f; break;
    case PT_REPEATER:   p->timer = 0.4f; break;
    case PT_CHERRY:     p->fuse = 1.0f;  break;
    case PT_JALAPENO:   p->fuse = 1.0f;  break;
    case PT_ICESHROOM:  p->fuse = 1.0f;  break;
    case PT_STARFRUIT: case PT_CACTUS: case PT_SPLITPEA: case PT_REED:
    case PT_BLOOMERANG: case PT_FUME: case PT_LASERBEAN: case PT_MELONPULT:
    case PT_WINTERMELON: case PT_GLOOM: case PT_CATTAIL:
        p->timer = 0.4f; break;
    case PT_TWINFLOWER: case PT_GOLDMAGNET:
        p->timer = 5.0f; break;
    case PT_COBCANNON:
        p->timer = 8.0f; break;
    default: break;
    }
    /* 多形态：把同 instance 的其他格子指向同一 Plant（共享血量/动画） */
    for (rr = row; rr < row + hh; rr++) for (cc = col; cc < col + w; cc++) {
        if (rr == row && cc == col) continue;
        if (p == &grid[row][col]) grid[rr][cc] = *p;      /* 拷贝：共享所有字段 */
        else                      grid2[rr][cc] = *p;
        if (p == &grid[row][col]) { grid[rr][cc].row = rr;  grid[rr][cc].col = cc; }
        else                      { grid2[rr][cc].row = rr; grid2[rr][cc].col = cc; }
    }
}

static void removePlantAt(int row, int col)
{
    Plant *p = plantAt(row, col);
    int w, hh, rr, cc;
    if (!p) return;
    addParticle(cellCX(col), cellBaseY(row) - 24, 0, -40, 0.4f, 10, RGB(120, 190, 90), 1);
    w = p->w; hh = p->hh;
    for (rr = p->row; rr < p->row + hh; rr++) for (cc = p->col; cc < p->col + w; cc++) {
        if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) continue;
        if (&grid[p->row][p->col] == p) memset(&grid[rr][cc], 0, sizeof(Plant));
        else                            memset(&grid2[rr][cc], 0, sizeof(Plant));
    }
}/* ======================================================================== */
/*                                 游戏逻辑更新                               */
/* ======================================================================== */

static void resetGame(void)
{
    int i, r;
    memset(grid,   0, sizeof(grid));
    memset(grid2,  0, sizeof(grid2));
    memset(zombies,0, sizeof(zombies));
    memset(peas,   0, sizeof(peas));
    memset(suns,   0, sizeof(suns));
    memset(parts,  0, sizeof(parts));
    memset(spawnQ, 0, sizeof(spawnQ));
    spawnQN = 0;
    gDailyMode = 0;              /* 默认非每日；仅每日营地入口会在调用本函数前置 1 */

    memset(gRelic, 0, sizeof(gRelic));
    gRelicCount = 0; gKillStack = 0; gKillStackDrawn = -1;
    gMedicT = 12.0f;
    relicsRecalc();
    memset(gFires, 0, sizeof(gFires));
    gSun = levelDefs[gCurLevel].startSun + gStartSunAdd;
    if (gWeekSunAdd) gSun += gWeekSunAdd;         /* 星期主题开局加成 */
    if (gDailyMode)  gSun -= 25;                  /* 每日挑战：开局比平时紧张一点 */
    gTime = 0; gWave = 0; gSel = -1; gShovel = 0;
    waveTimer = 14.0f; skySunTimer = 5.0f;
    gKilled = 0; gElapsed = 0;
    shakeT = 0; shakeMag = 0; flashRed = 0; gMowerUsed = 0;
    loadoutBuild();
    /* 本局的神级植物必须每局从零开始随机 —— 上一局带到下一局会毁掉随机性 */
    memset(gBonusPlant, 0, sizeof(gBonusPlant));
    gBonusN = 0;
    for (i = 0; i < PT_COUNT; i++) cardCD[i] = 0.0f;
    for (r = 0; r < ROWS; r++) {
        mowers[r].active = gNoMower ? 0 : 1;
        mowers[r].running = 0; mowers[r].recharge = 0.0f;
        mowers[r].row = r; mowers[r].x = LAWN_X - 34.0f; mowers[r].speed = 430.0f;
    }
    /* 模式 / 武器 / 货币 重置 */
    memset(gUnlockedMode, 0, sizeof(gUnlockedMode));
    gUnlockedMode[MODE_STANDARD] = 1;
    gCurMode = MODE_STANDARD;
    gMainWpn = WPN_NORMAL; gSubWpn = WPN_NORMAL; gHasSubWpn = 0;
    gGold = 0; gGhost = 0; gFury = 0; gCharge = 0;
    gFuryT = 0; gBanker = 0; gNecromancer = 0;
    if (HAS(R_WATER_MODE)) { gUnlockedMode[MODE_WATER] = 1; gCurMode = MODE_WATER; }
    if (HAS(R_NIGHT_MODE)) { gUnlockedMode[MODE_NIGHT] = 1; gCurMode = MODE_NIGHT; }
    weatherInit();
    if (gDailyMode) srand((unsigned)gDailySeed);  /* 每日挑战：固定种子，当天每次开局都一样 */
    else            srand((unsigned)time(NULL));
}

/* 开启一局每日挑战：目标波数与三条规则都由当天日期唯一决定 ——
   当天怎么打都是同一关，第二天自动换新。这就是「明天再来」的理由。 */
static void dailyStart(void)
{
    gDailySeed = todayYMD() * 31 + 17;
    gCurLevel = 5;                                /* 固定无尽关底 */
    gLawnVariant = 0;
    gDailyHpMul  = 1.0f + 0.15f * (float)((gDailySeed / 7)  % 3);   /* 1.00 / 1.15 / 1.30 */
    gDailySpdMul = 1.0f + 0.05f * (float)((gDailySeed / 11) % 3);   /* 1.00 / 1.05 / 1.10 */
    gDailySunMul = 1.0f + 0.20f * (float)((gDailySeed / 13) % 2);   /* 1.00 / 1.20（更少阳光） */
    gDailyTarget = 20 + (gDailySeed / 17) % 15;                     /* 20 ~ 34 波 */
    resetGame();                                  /* 先按普通方式重置（会清 gDailyMode） */
    gDailyMode = 1;                               /* 再标成每日挑战 */
    relicsRecalc();                               /* 重算修正，把每日规则叠上 */
    gSun -= 25;                                   /* 开局收紧 */
    srand((unsigned)gDailySeed);                  /* 固定种子：当天每次开局都一样 */
    gState = ST_PLAY;
}

static void updateGame(float dt)
{
    int i, r, c, sl;

    gElapsed += dt;
    gTime    += dt;
    weatherUpdate(dt);
    if (shakeT > 0)   { shakeT -= dt;  if (shakeT <= 0) shakeMag = 0; }
    if (flashRed > 0)   flashRed -= dt;

    /* ------------------------- 卡片冷却 ------------------------- */
    for (i = 0; i < PT_COUNT; i++) {
        if (cardCD[i] > 0) { cardCD[i] -= dt; if (cardCD[i] < 0) cardCD[i] = 0; }
    }

    /* ------------------------- 天空阳光 ------------------------- */
    skySunTimer -= dt;
    if (skySunTimer <= 0) {
        spawnSunDrop(rnd(LAWN_X + 50, LAWN_X + COLS * CELL_W - 50), LAWN_Y - 40,
                     rnd(LAWN_Y + 70, LAWN_Y + ROWS * CELL_H - 40), 25);
        skySunTimer = rnd(6.0f, 10.0f) * gSkySunMul * levelDefs[gCurLevel].sunMul;
    }

    traitsRecalc();          /* 羁绊已移除，保留空实现 */
    hoverUpdate();           /* 鼠标悬停提示 */

    /* 小推车存活数（背水遗物要看） */
    { int mv = 0; for (i = 0; i < ROWS; i++) if (mowers[i].active) mv++;
      gMowerAlive = mv; }

    /* ------------------------- 遗物：持续效果 ------------------------- */
    if (gState == ST_PLAY) {
        /* 按时间涨，不是按帧 —— 原来写成每帧 +1，60fps 下 1.67 秒就满，
           等于全屏雷击每 1.67 秒来一次，而且开局就有。 */
        gFury += dt;
        if (gFury >= gFuryMax) {
            int r, t; gFury = 0;
            for (r = 0; r < ROWS; r++) for (t = 0; t < 6; t++) {
                float cx = LAWN_X + t * (COLS * CELL_W / 5.0f) + rnd(-12, 12);
                addParticle(cx, LAWN_Y + 4, rnd(-30, 30), rnd(80, 160), 0.3f, rnd(4, 7),
                            (t & 1) ? RGB(255, 244, 200) : RGB(255, 188, 80), 1);
            }
            flashRed = 0.35f; addShake(6, 0.3f);
        }
    }
    firesUpdate(dt);
    if (gSpareTire) {
        for (i = 0; i < ROWS; i++) {
            Mower *m = &mowers[i];
            if (!m->active && m->running == 0 && m->recharge > 0.0f) {
                m->recharge -= dt;
                if (m->recharge <= 0.0f) {
                    m->active = 1; m->x = LAWN_X - 34.0f; m->running = 0;
                    for (r = 0; r < 8; r++)
                        addParticle(m->x + rnd(-14, 14), cellBaseY(i) - rnd(4, 26),
                                    rnd(-60, 60), rnd(-90, -20), 0.5f, rnd(3, 6),
                                    RGB(200, 206, 216), 1);
                }
            }
        }
    }
    if (gMedic) {
        gMedicT -= dt;
        if (gMedicT <= 0.0f) {
            int br = -1, bc = -1;
            float worst = 1.0f;
            gMedicT = 12.0f;
            for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
                Plant *q = &grid[r][c];
                float f;
                if (!q->alive || q->maxhp <= 1.0f) continue;
                f = q->hp / q->maxhp;
                if (f < worst) { worst = f; br = r; bc = c; }
            }
            if (br >= 0) {
                Plant *q = &grid[br][bc];
                q->hp = (q->hp + 120.0f > q->maxhp) ? q->maxhp : q->hp + 120.0f;
                for (r = 0; r < 10; r++)
                    addParticle(cellCX(bc) + rnd(-18, 18), cellBaseY(br) - rnd(10, 60),
                                rnd(-30, 30), rnd(-70, -20), 0.6f, rnd(3, 6),
                                RGB(150, 240, 170), 1);
            }
        }
    }

    /* ------------------------- 波次控制 ------------------------- */
    /* 每日挑战：达到当日目标波数即算完成（无尽关不会走下面的通关结算） */
    if (gDailyMode && gWave >= gDailyTarget) {
        gainXp(gKilled + 30);
        if (gKilled > gSave.bestKills)                { gSave.bestKills = gKilled;          gRecNew = 1; }
        if (gGold   > gSave.bestGold)                 { gSave.bestGold  = gGold;            gRecNew = 1; }
        if (gSave.dailyDoneDate != todayYMD()) {
            gSave.dailyDoneDate = todayYMD();         /* 每日挑战每天只发一次奖 */
            gSave.stars += 3;
            gSave.coins += 5;
            if (gDailyTarget > gSave.dailyBestWave) gSave.dailyBestWave = gDailyTarget;
        }
        saveFlush();
        gState = ST_WIN;
        return;
    }
    if (gWave < levelDefs[gCurLevel].waves) {
        waveTimer -= dt;
        if (waveTimer <= 0.0f) {
            /* 每 2 波给一次三选一奖励（在开波前弹出，选完立刻能用上） */
            if (gWave > 0 && (gWave % 2) == 0 && gLastDraft != gWave) {
                gLastDraft = gWave;
                openDraft();
                return;
            }
            startWave(gWave);
        } else if (gWave > 0 && zombiesAlive() == 0 && spawnQN == 0 && waveTimer > 2.0f) {
            waveTimer = 2.0f;      /* 提前清场则加速下一波 */
        }
    } else if (spawnQN == 0 && zombiesAlive() == 0) {
        /* ---- 通关结算：给星 / 解锁下一关 / 解锁成就 / 落盘 ---- */
        {
            int st = STAR_PER_WIN;
            if (gMowerUsed == 0) st++;
            if (st > gSave.lvStars[gCurLevel]) gSave.lvStars[gCurLevel] = st;
            gSave.stars += st;
            gSave.coins += 3 + st;
            if (gCurLevel + 1 < LV_COUNT) gSave.lvUnlocked[gCurLevel + 1] = 1;
            gSave.totalKills += gKilled;
            if (gCurLevel > 0)   achUnlock(0);
            if (gKilled > 100)   achUnlock(1);
            if (gSave.totalKills >= 1000) achUnlock(2);
            if (gMowerUsed == 0) achUnlock(3);
            if (gRelicCount >= 12) achUnlock(6);
            if (gSun >= 3000)    achUnlock(7);
            switch (gCurLevel) {
            case 1: achUnlock(12); break;
            case 2: achUnlock(11); break;
            case 3: achUnlock(13); break;
            case 4: achUnlock(14); break;
            }
            if (gCurLevel == 5 && gWave >= 30) achUnlock(15);
            /* 留存：等级经验 + 个人纪录 */
            gainXp(gKilled + 20);
            if (gKilled > gSave.bestKills)                             { gSave.bestKills  = gKilled;      gRecNew = 1; }
            if (gGold   > gSave.bestGold)                              { gSave.bestGold   = gGold;        gRecNew = 1; }
            if (gSave.bestTimeSec == 0 || (int)gElapsed < gSave.bestTimeSec) { gSave.bestTimeSec = (int)gElapsed; gRecNew = 1; }
            if (gCurLevel > gSave.bestWave)                            { gSave.bestWave   = gCurLevel;    gRecNew = 1; }
            saveFlush();
        }
        gState = ST_WIN;
        return;
    }

    /* ------------------------- 出怪队列 ------------------------- */
    for (i = 0; i < spawnQN; ) {
        if (spawnQ[i].t <= gTime) {
            spawnZombie(spawnQ[i].type, spawnQ[i].row);
            spawnQ[i] = spawnQ[--spawnQN];
        } else i++;
    }

    /* ------------------------- 植物 ------------------------- */
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) for (sl = 0; sl < 2; sl++) {
        Plant *p = (sl == 0) ? &grid[r][c] : &grid2[r][c];
        if (!p->alive) continue;
        if (p->row != r || p->col != c) continue;   /* 多格植物只跑一次 */
        p->phase += dt;
        if (p->recoil > 0) p->recoil -= dt * 5.0f;
        if (p->eaten  > 0) p->eaten  -= dt * 3.0f;
        {
            float cx = cellCX(c), by = cellBaseY(r);
            /* 武器模组：主射 / 副射【交替】开火。
               之前写成"有副射就完全覆盖主射"，导致「主射升级」这个遗物永远看不到效果。 */
            int wpn = gMainWpn, sub = 0;
            if (gHasSubWpn) {
                sub = p->burst;                  /* 借 burst 当交替开关（射手本来就每发翻转） */
                wpn = sub ? gSubWpn : gMainWpn;
            }
            {
            float py = by - 62;

            switch (p->type) {
            /* ---------------- 向日葵 ---------------- */
            case PT_SUNFLOWER:
            case PT_HERO_SUNGOD: {
                int god = (p->type == PT_HERO_SUNGOD);
                p->timer -= dt;
                if (p->timer <= 0.0f) {
                    p->timer = god ? rnd(9.0f, 12.0f) : rnd(17.0f, 23.0f);
                    spawnSunDrop(cx + rnd(-12, 12), by - 72, by - 6,
                                 (int)(((god ? 180.0f : 25.0f) + gSunflowerAdd) * gTrGarden));
                    if (gCharge < gChargeMax) gCharge += 1;
                }
                break;
            }

            /* ---------------- 钢铁坚果（稀有）：被啃时每秒反伤 60 ---------------- */
            case PT_HERO_IRONNUT: {
                float icx = cellCX(c);
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    if (!z->active || z->dead || z->row != r) continue;
                    if (fabsf(z->x - icx) < 62.0f) {
                        z->hp -= 60.0f * dt;
                        z->flash = 0.05f;
                        if (rnd(0.0f, 1.0f) < dt * 10.0f)
                            addParticle(z->x + rnd(-14, 14), z->y - rnd(10, 50),
                                        rnd(-40, 40), rnd(-70, -10), 0.3f, rnd(2, 5),
                                        RGB(255, 214, 120), 1);
                    }
                }
                break;
            }

            /* ---------------- 射手类 ---------------- */
            case PT_PEASHOOTER:
            case PT_SNOWPEA:
            case PT_REPEATER:
            case PT_THREEPEATER:
            case PT_KERNELPULT:
            case PT_HERO_FLAME:
            case PT_HERO_CRYSTAL:
            case PT_HERO_FROST:
            case PT_HERO_DOOM: {
                int hasTarget = 0;
                int three = (p->type == PT_THREEPEATER || p->type == PT_HERO_CRYSTAL);
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    int rowOk = three ? (z->row >= r - 1 && z->row <= r + 1) : (z->row == r);
                    if (z->active && !z->dead && rowOk && z->x > cx - 30 && z->x < VIEW_W + 80) { hasTarget = 1; break; }
                }
                if (hasTarget) {
                    p->timer -= dt;
                    if (p->timer <= 0.0f) {
                        int frozen = (p->type == PT_SNOWPEA);
                        float py = by - 62;
                        int k;
                        for (k = 0; k < MAX_PEAS; k++) {
                            if (!peas[k].active) {
                                peas[k].active = 1;
                                peas[k].x = cx + 28; peas[k].y = py;
                                peas[k].vx = 340; peas[k].vy = 0;
                                peas[k].dmg = peaBaseDmg() * (1.0f + fieldDmgBonusAt(r, c));
                                peas[k].row = r; peas[k].frozen = frozen;
                                peas[k].wpn = wpn;
                                peas[k].splash = 0; peas[k].bounces = 0;
                                peas[k].freeze = 0; peas[k].bouncesBase = 0;
                                peas[k].sub = sub;
                                peas[k].src = p->type;
                                switch (wpn) {
                                case WPN_PIERCE: peas[k].pierceLeft = 3; break;
                                case WPN_SPLASH: peas[k].splash = 3;   break;
                                case WPN_BOUNCE: peas[k].bouncesBase = peas[k].bounces = 1; break;
                                case WPN_FREEZE: peas[k].freeze = 1000; peas[k].dmg *= 0.55f; break;
                                case WPN_BURN:   peas[k].dmg *= 0.70f; break;
                                }
                                break;
                            }
                        }
                        /* 三线射手：同一次射击向上下三行各发一颗 */
                        if (three) {
                            int rr2;
                            for (rr2 = r - 1; rr2 <= r + 1; rr2++) {
                                if (rr2 == r || rr2 < 0 || rr2 >= ROWS) continue;
                                float tmul = gThreeUpFull ? 1.0f : 0.75f;   /* 三线齐鸣 */
                                for (k = 0; k < MAX_PEAS; k++) {
                                    if (!peas[k].active) {
                                        peas[k].active = 1;
                                        peas[k].x = cx + 28; peas[k].y = cellBaseY(rr2) - 62;
                                        peas[k].vx = 340; peas[k].vy = 0;
                                        peas[k].dmg = peaBaseDmg() * (1.0f + fieldDmgBonusAt(rr2, c)) * tmul;
                                        peas[k].row = rr2; peas[k].frozen = 0;
                                        peas[k].wpn = wpn; peas[k].splash = 0;
                                        peas[k].bounces = 0; peas[k].freeze = 0;
                                        peas[k].bouncesBase = 0; peas[k].sub = sub;
                                        peas[k].src = p->type;
                                        break;
                                    }
                                }
                            }
                        }
                        /* ---- 神级射手：统一在这里覆写伤害与特性 ---- */
                        switch (p->type) {
                        case PT_HERO_FLAME:                       /* 稀有：烈焰射手 */
                            for (k = 0; k < MAX_PEAS; k++)
                                if (peas[k].active && peas[k].row == r &&
                                    fabsf(peas[k].x - (cx + 28)) < 1.0f) {
                                    peas[k].dmg = 58.0f * gDmgMul;
                                    peas[k].wpn = WPN_BURN;       /* 留火焰 */
                                    peas[k].pierceLeft = 2;       /* 穿透 2 个 */
                                    peas[k].knockback = 12;       /* 烈焰冲击 */
                                    break;
                                }
                            break;
                        case PT_HERO_CRYSTAL:                     /* 史诗：晶簇射手 */
                            for (k = 0; k < MAX_PEAS; k++)
                                if (peas[k].active && peas[k].row == r &&
                                    fabsf(peas[k].x - (cx + 28)) < 1.0f) {
                                    peas[k].dmg = 55.0f * gDmgMul;
                                    peas[k].wpn = WPN_PIERCE;
                                    peas[k].pierceLeft = 2;
                                    peas[k].root = 450;           /* 晶化定身 0.45 秒 */
                                    break;
                                }
                            break;
                        case PT_HERO_FROST:                       /* 传奇：霜之哀伤 */
                            for (k = 0; k < MAX_PEAS; k++)
                                if (peas[k].active && peas[k].row == r &&
                                    fabsf(peas[k].x - (cx + 28)) < 1.0f) {
                                    peas[k].dmg = 90.0f * gDmgMul;
                                    peas[k].frozen = 1;
                                    peas[k].freeze = 900;
                                    peas[k].root = 600;           /* 冰冻+定身 */
                                    break;
                                }
                            break;
                        case PT_HERO_DOOM: {                      /* 神话：末日花（追踪弹） */
                            int best = -1;
                            float bd = 1e9f;
                            for (k = 0; k < MAX_ZOMBIES; k++) {
                                Zombie *tz = &zombies[k];
                                float dx, dy;
                                if (!tz->active || tz->dead) continue;
                                dx = tz->x - cx; dy = tz->y - by;
                                if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = k; }
                            }
                            for (k = 0; k < MAX_PEAS; k++)
                                if (peas[k].active && fabsf(peas[k].x - (cx + 28)) < 1.0f) {
                                    peas[k].dmg = 280.0f * gDmgMul;   /* 单发 280，追踪 */
                                    peas[k].wpn  = WPN_NORMAL;
                                    peas[k].homing = 1;
                                    peas[k].knockback = 25;           /* 毁灭冲击 */
                                    if (best >= 0) {                  /* 朝目标方向飞 */
                                        float tx = zombies[best].x - cx;
                                        float ty = (zombies[best].y - 40.0f) - by;
                                        float len = sqrtf(tx * tx + ty * ty) + 1e-4f;
                                        peas[k].vx = 430.0f * tx / len;
                                        peas[k].vy = 430.0f * ty / len;
                                        peas[k].row = zombies[best].row;   /* 追踪弹跨行 */
                                    }
                                    break;
                                }
                            break;
                        }
                        default: break;
                        }

                        /* 玉米投手：投掷，命中带眩晕 + 小幅击退 */
                        if (p->type == PT_KERNELPULT) {
                            for (k = 0; k < MAX_PEAS; k++) if (peas[k].active &&
                                fabsf(peas[k].x - (cx + 28)) < 1.0f && peas[k].row == r) {
                                peas[k].dmg *= 1.35f * gCornDmgMul;              /* 爆米花 */
                                peas[k].freeze = (int)(600.0f * gCornStunMul * gTrMutant);  /* 黄油+异种 */
                                peas[k].knockback = 8;                           /* 小幅击退 */
                                break;
                            }
                        }
                        /* 荆棘藤蔓/缠绕海草：命中带定身（若作为发射植物） */
                        if (p->type == PT_HERO_THORNVINE || p->type == PT_TANGLEKELP) {
                            for (k = 0; k < MAX_PEAS; k++) if (peas[k].active &&
                                fabsf(peas[k].x - (cx + 28)) < 1.0f && peas[k].row == r) {
                                peas[k].root = 800;   /* 定身 0.8 秒 */
                                break;
                            }
                        }
                        p->recoil = 1.0f;
                        for (k = 0; k < 4; k++)
                            addParticle(cx + 30, py + rnd(-4, 4), rnd(40, 110), rnd(-30, 30),
                                        0.2f, rnd(2, 5),
                                        frozen ? RGB(200, 244, 255) : RGB(210, 240, 160), 0);
                        if (p->type == PT_REPEATER && !p->burst) {
                            p->burst = 1; p->timer = 0.16f;
                        } else {
                            p->burst = 0; p->timer = shootGap() * rowRateMul(r);
                        }
                    }
                } else if (p->timer < 0.35f) {
                    p->timer = 0.35f;
                }
                break;
            }

            /* ---------------- 土豆雷 ---------------- */
            case PT_POTATOMINE:
                if (!p->armed) {
                    if (p->phase > 14.0f) {
                        p->armed = 1;
                        for (i = 0; i < 10; i++)
                            addParticle(cx, by - 26, rnd(-40, 40), rnd(-60, -10), 0.4f,
                                        rnd(2, 5), RGB(190, 200, 210), 1);
                    }
                } else {
                    for (i = 0; i < MAX_ZOMBIES; i++) {
                        Zombie *z = &zombies[i];
                        if (!z->active || z->dead || z->row != r) continue;
                        if (z->x - 24 <= cx + CELL_W * 0.5f && z->x > cx - CELL_W * 0.6f) {
                            doExplosionFX(cx, by - 18, 1.25f, RGB(255, 190, 80), RGB(120, 90, 60));
                            explode(cx, by - 18, CELL_W * 0.9f, 1800.0f, 0, 0);
                            addShake(7, 0.3f);
                            p->alive = 0;
                            break;
                        }
                    }
                }
                break;

            /* ---------------- 樱桃炸弹 ---------------- */
            case PT_CHERRY:
                p->fuse -= dt;
                if (p->fuse <= 0.0f) {
                    doExplosionFX(cx, by - 26, 1.7f, RGB(255, 120, 60), RGB(255, 220, 120));
                    explode(cx, by - 26, CELL_W * 1.55f, 1800.0f, 0, 0);
                    addShake(11, 0.45f);
                    p->alive = 0;
                }
                break;

            /* ---------------- 火爆辣椒 ---------------- */
            case PT_JALAPENO:
                p->fuse -= dt;
                if (p->fuse <= 0.0f) {
                    int k;
                    for (k = 0; k < 60; k++)
                        addParticle(rnd(LAWN_X, LAWN_X + COLS * CELL_W), by + rnd(-30, 24),
                                    rnd(-30, 30), rnd(-160, -40), rnd(0.4f, 0.9f),
                                    rnd(6, 20), (k & 1) ? RGB(255, 150, 50) : RGB(255, 220, 90), 1);
                    explode(0, 0, 0, 1800.0f, 1, r);
                    addShake(10, 0.4f); flashRed = 0.35f;
                    p->alive = 0;
                }
                break;

            case PT_STARFRUIT: {
                int hasT = 0, kk;
                for (kk = 0; kk < MAX_ZOMBIES; kk++)
                    if (zombies[kk].active && !zombies[kk].dead &&
                        zombies[kk].x > cx - 40) { hasT = 1; break; }
                if (!hasT) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 1.55f * gRateMul;
                {
                    float dmg = 22.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c));
                    int id;
                    id = spawnPeaAt(cx + 18, py, 300, 0, dmg, r, PT_STARFRUIT, 0);
                    if (id >= 0) peas[id].knockback = 5;  /* 星星轻微击退 */
                    id = spawnPeaAt(cx + 10, py, 210, -180, dmg * 0.64f, r - 1, PT_STARFRUIT, 0);
                    if (id >= 0 && r > 0) peas[id].knockback = 5;
                    id = spawnPeaAt(cx + 10, py, 210, 180, dmg * 0.64f, r + 1, PT_STARFRUIT, 0);
                    if (id >= 0 && r + 1 < ROWS) peas[id].knockback = 5;
                    spawnPeaAt(cx - 8, py, -80, -240, dmg * 0.50f, r, PT_STARFRUIT, 0);
                    spawnPeaAt(cx - 8, py, -80, 240, dmg * 0.50f, r, PT_STARFRUIT, 0);
                }
                p->recoil = 1.0f;
                break;
            }
            case PT_CACTUS: {
                int hasT = 0, kk;
                for (kk = 0; kk < MAX_ZOMBIES; kk++)
                    if (zombies[kk].active && !zombies[kk].dead &&
                        zombies[kk].row == r && zombies[kk].x > cx - 20) { hasT = 1; break; }
                if (!hasT) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 1.35f * gRateMul;
                { int id = spawnPeaAt(cx + 26, py, 380, 0,
                           28.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)),
                           r, PT_CACTUS, 0);
                  if (id >= 0) peas[id].pierceLeft = 2; }  /* 仙人掌针刺穿透 */
                p->recoil = 1.0f;
                break;
            }
            case PT_SPLITPEA: {
                int front = 0, back = 0, kk;
                for (kk = 0; kk < MAX_ZOMBIES; kk++) {
                    Zombie *z = &zombies[kk];
                    if (!z->active || z->dead || z->row != r) continue;
                    if (z->x > cx) front = 1; else back = 1;
                }
                if (!front && !back) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 1.45f * gRateMul;
                if (front) spawnPeaAt(cx + 26, py, 340, 0, 24.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)), r, PT_SPLITPEA, 0);
                if (back)  spawnPeaAt(cx - 26, py, -300, 0, 18.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)), r, PT_SPLITPEA, 0);
                p->recoil = 1.0f;
                break;
            }
            case PT_REED: {
                int hasT = 0, kk;
                for (kk = 0; kk < MAX_ZOMBIES; kk++)
                    if (zombies[kk].active && !zombies[kk].dead &&
                        abs(zombies[kk].row - r) <= 1 && zombies[kk].x > cx - 20) { hasT = 1; break; }
                if (!hasT) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 1.25f * gRateMul;
                { int id = spawnPeaAt(cx + 20, py, 300, 0, 18.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)), r, PT_REED, 0);
                  if (id >= 0) { peas[id].pierceLeft = 3; peas[id].knockback = 15; } }  /* 穿刺+击退 */
                p->recoil = 1.0f;
                break;
            }
            case PT_BLOOMERANG: {
                int hasT = 0, kk;
                for (kk = 0; kk < MAX_ZOMBIES; kk++)
                    if (zombies[kk].active && !zombies[kk].dead &&
                        zombies[kk].row == r && zombies[kk].x > cx) { hasT = 1; break; }
                if (!hasT) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 1.70f * gRateMul;
                { int id = spawnPeaAt(cx + 22, py, 280, 0, 20.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)), r, PT_BLOOMERANG, 0);
                  if (id >= 0) peas[id].bounces = 1; }
                p->recoil = 1.0f;
                break;
            }
            case PT_FUME: {
                int hasT = 0, kk;
                for (kk = 0; kk < MAX_ZOMBIES; kk++)
                    if (zombies[kk].active && !zombies[kk].dead &&
                        zombies[kk].row == r && zombies[kk].x > cx && zombies[kk].x < cx + 280) { hasT = 1; break; }
                if (!hasT) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 1.40f * gRateMul;
                { int id = spawnPeaAt(cx + 18, py, 220, 0, 16.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)), r, PT_FUME, 0);
                  if (id >= 0) peas[id].pierceLeft = 6; }
                p->recoil = 1.0f;
                break;
            }
            case PT_LASERBEAN: {
                int hasT = 0, kk;
                for (kk = 0; kk < MAX_ZOMBIES; kk++)
                    if (zombies[kk].active && !zombies[kk].dead &&
                        zombies[kk].row == r && zombies[kk].x > cx) { hasT = 1; break; }
                if (!hasT) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 2.40f * gRateMul;
                { int id = spawnPeaAt(cx + 20, py, 900, 0, 40.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)), r, PT_LASERBEAN, 0);
                  if (id >= 0) peas[id].pierceLeft = 12; }
                p->recoil = 1.0f;
                break;
            }
            case PT_MELONPULT:
            case PT_WINTERMELON: {
                int hasT = 0, kk, ice = (p->type == PT_WINTERMELON);
                for (kk = 0; kk < MAX_ZOMBIES; kk++)
                    if (zombies[kk].active && !zombies[kk].dead &&
                        zombies[kk].row == r && zombies[kk].x > cx) { hasT = 1; break; }
                if (!hasT) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = (ice ? 2.20f : 1.90f) * gRateMul;
                { int id = spawnPeaAt(cx + 16, py, 260, -40,
                                      70.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)),
                                      r, p->type, ice);
                  if (id >= 0) { peas[id].splash = 2; peas[id].wpn = WPN_SPLASH; if (ice) peas[id].freeze = 2200; } }
                p->recoil = 1.0f;
                break;
            }
            case PT_GLOOM: {
                int hasT = 0, kk, dr, dc2;
                for (kk = 0; kk < MAX_ZOMBIES; kk++) {
                    Zombie *z = &zombies[kk];
                    if (!z->active || z->dead) continue;
                    if (abs(z->row - r) <= 1 && fabsf(z->x - cx) < CELL_W * 1.6f) { hasT = 1; break; }
                }
                if (!hasT) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 1.60f * gRateMul;
                for (dr = -1; dr <= 1; dr++) for (dc2 = -1; dc2 <= 1; dc2++) {
                    int rr2 = r + dr;
                    float vx = (dc2 == 0 && dr == 0) ? 40.0f : (float)dc2 * 180.0f;
                    float vy = (float)dr * 140.0f;
                    if (rr2 < 0 || rr2 >= ROWS) continue;
                    spawnPeaAt(cx, py, vx, vy, 22.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)), rr2, PT_GLOOM, 0);
                }
                p->recoil = 1.0f;
                break;
            }
            case PT_TWINFLOWER: {
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = rnd(16.0f, 21.0f);
                spawnSunDrop(cx - 10, by - 72, by - 6, (int)((25.0f + gSunflowerAdd) * gTrGarden));
                spawnSunDrop(cx + 12, by - 72, by - 6, (int)((25.0f + gSunflowerAdd) * gTrGarden));
                break;
            }
            case PT_GOLDMAGNET: {
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 9.0f;
                spawnSunDrop(cx, by - 60, by - 8, 15);
                gGold += 2;
                break;
            }
            case PT_COBCANNON: {
                int hasT = 0, kk, best = -1;
                float bd = 1e9f;
                for (kk = 0; kk < MAX_ZOMBIES; kk++) {
                    Zombie *z = &zombies[kk];
                    float dx, dy;
                    if (!z->active || z->dead) continue;
                    dx = z->x - cx; dy = z->y - by;
                    if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = kk; hasT = 1; }
                }
                if (!hasT) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 18.0f;
                { int id = spawnPeaAt(cx + 20, py, 220, 0, 520.0f * gDmgMul, r, PT_COBCANNON, 0);
                  if (id >= 0 && best >= 0) {
                      float tx = zombies[best].x - cx, ty = zombies[best].y - 40.0f - by;
                      float len = sqrtf(tx * tx + ty * ty) + 1e-4f;
                      peas[id].vx = 280.0f * tx / len;
                      peas[id].vy = 280.0f * ty / len;
                      peas[id].row = zombies[best].row;
                      peas[id].splash = 3; peas[id].wpn = WPN_SPLASH;
                  } }
                p->recoil = 1.0f; addShake(5, 0.25f);
                break;
            }
            case PT_CATTAIL: {
                int best = -1, kk; float bd = 1e9f;
                for (kk = 0; kk < MAX_ZOMBIES; kk++) {
                    Zombie *z = &zombies[kk];
                    float dx, dy, d;
                    if (!z->active || z->dead) continue;
                    dx = z->x - cx; dy = z->y - by; d = dx * dx + dy * dy;
                    if (z->type == ZT_BALLOON) d *= 0.15f;
                    if (d < bd) { bd = d; best = kk; }
                }
                if (best < 0) break;
                p->timer -= dt;
                if (p->timer > 0) break;
                p->timer = 1.15f * gRateMul;
                { int id = spawnPeaAt(cx + 16, py, 360, 0, 26.0f * gDmgMul * (1.0f + fieldDmgBonusAt(r, c)), r, PT_CATTAIL, 0);
                  if (id >= 0) {
                      float tx = zombies[best].x - cx, ty = zombies[best].y - 40.0f - by;
                      float len = sqrtf(tx * tx + ty * ty) + 1e-4f;
                      peas[id].vx = 420.0f * tx / len;
                      peas[id].vy = 420.0f * ty / len;
                      peas[id].row = zombies[best].row;
                  } }
                p->recoil = 1.0f;
                break;
            }
            case PT_ICESHROOM:
                p->fuse -= dt;
                if (p->fuse <= 0.0f) {
                    for (i = 0; i < MAX_ZOMBIES; i++) {
                        Zombie *z = &zombies[i];
                        if (!z->active || z->dead) continue;
                        if (z->slow < 4.0f) z->slow = 4.0f;
                        z->flash = 0.14f;
                    }
                    addShake(4, 0.25f);
                    p->alive = 0;
                }
                break;
            case PT_TANGLEKELP: {
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    if (!z->active || z->dead || z->row != r) continue;
                    if (z->type == ZT_BALLOON) continue;
                    if (fabsf(z->x - cx) < 42.0f) {
                        zombieDie(z, 1);
                        p->alive = 0;
                        break;
                    }
                }
                break;
            }

            default: break;
            }
            } /* py */
        }
    }

    /* ------------------------- 豌豆 ------------------------- */
    for (i = 0; i < MAX_PEAS; i++) {
        Pea *pe = &peas[i];
        if (!pe->active) continue;
        pe->x += pe->vx * dt;
        /* 追踪弹显式跨行判定；不再用伤害数值猜测弹道类型 */
        if (pe->homing) pe->row = pe->row;
        if (pe->src == PT_BLOOMERANG && pe->bounces == 1 && pe->x > VIEW_W - 80) {
            pe->vx = -pe->vx; pe->bounces = 0;
        }
        if (pe->x > VIEW_W + 40 || pe->x < LAWN_X - 40) { pe->active = 0; continue; }
        /* 冰冻：直接冻结沿途目标（不需命中） */
        if (pe->freeze > 0) {
            int kk;
            for (kk = 0; kk < MAX_ZOMBIES; kk++) {
                Zombie *z = &zombies[kk];
                if (z->active && !z->dead && z->row == pe->row &&
                    z->x >= pe->x - 30 && z->x <= pe->x + 8) {
                    if ((float)pe->freeze / 1000.0f > z->slow) z->slow = (float)pe->freeze / 1000.0f;
                    z->flash = 0.08f;
                }
            }
            pe->freeze -= (int)(dt * 1000.0f);
        }
        /* 反弹：到屏幕顶部或底部时反弹 vy */
        if (pe->wpn == WPN_BOUNCE && pe->bounces > 0) {
            if (pe->y < LAWN_Y + 10)       { pe->y = LAWN_Y + 10; pe->vy =  fabs(pe->vy) + 60.0f; pe->bounces--; }
            if (pe->y > LAWN_Y + ROWS * CELL_H - 8) { pe->y = LAWN_Y + ROWS * CELL_H - 8; pe->vy = -fabs(pe->vy) - 60.0f; pe->bounces--; }
        }
        pe->y += pe->vy * dt;
        {
            int hit = -1;
            float best = 1e9f;
            int k;
            for (k = 0; k < MAX_ZOMBIES; k++) {
                Zombie *z = &zombies[k];
                /* 末日花的追踪弹会跨行，用 y 距离判定而不是严格同排 */
                if (!z->active || z->dead) continue;
                if (pe->homing) {
                    if (fabsf(z->y - 40.0f - pe->y) > 46.0f) continue;
                } else if (z->row != pe->row) continue;
                if (pe->x >= z->x - 26 && pe->x <= z->x + 22 && z->x < best) { best = z->x; hit = k; }
            }
            if (hit >= 0) {
                Zombie *z = &zombies[hit];
                float pd = pe->dmg;
                if (gArmorPierce && (z->type == ZT_CONE || z->type == ZT_BUCKET)) pd *= 1.60f;
                if (z->type == ZT_SCREENDOOR && z->shield) pd *= 0.45f;  /* 铁门挡掉大部分正面伤害 */
                if (pe->src == PT_FUME && z->type == ZT_SCREENDOOR && z->shield) pd /= 0.45f;
                if (z->type == ZT_BALLOON) pd *= gBalloonMul;   /* 飞镖 */
                if (pe->src == PT_CACTUS && z->type == ZT_BALLOON) pd *= 3.0f;
                if (HAS(R_FROST_CORE) && z->slow > 0.0f) pd *= 1.35f;
                z->hp -= pd;
                z->flash = 0.09f;
                if (pe->frozen) z->slow = slowDur();
                if (z->type == ZT_FOOTBALL) z->slow *= 0.40f;   /* 橄榄球：减速抗性 */
                if (gNecromancer && z->slow > 0.0f) gGhost += 1;
                {
                    int k2;
                    for (k2 = 0; k2 < 5; k2++)
                        addParticle(pe->x, pe->y, rnd(-50, 70), rnd(-70, 20), 0.3f, rnd(2, 5),
                                    pe->frozen ? RGB(200, 240, 255) : WPN_COL[pe->wpn], 1);
                }

                /* 灼烧：留火焰 */
                if (pe->wpn == WPN_BURN) fireSpawn(pe->x, cellBaseY(pe->row) + 6.0f, 3.0f);
                /* 冰冻：冻结 1 秒 */
                if (pe->wpn == WPN_FREEZE) if (1.0f > z->slow) z->slow = 1.0f;
                /* 定身（root）：使僵尸无法移动 */
                if (pe->root > 0) {
                    float rootSec = (float)pe->root / 1000.0f;
                    if (rootSec > z->rooted) z->rooted = rootSec;
                }
                /* 击退（knockback）：推离僵尸 */
                if (pe->knockback > 0) {
                    z->x -= (float)pe->knockback;  /* 向左推 */
                    if (z->x < LAWN_X - 10) z->x = LAWN_X - 10;  /* 不推出边界 */
                }
                /* 溅射：分裂成 3 个小弹（每弹 60% 伤害） */
                if (pe->wpn == WPN_SPLASH && pe->splash > 0) {
                    int n = pe->splash, kk;
                    pe->splash = 0;           /* 母弹不重复分裂 */
                    for (kk = 0; kk < n; kk++) {
                        int m;
                        for (m = 0; m < MAX_PEAS; m++) {
                            if (!peas[m].active) {
                                peas[m] = *pe;
                                peas[m].splash = 0;
                                peas[m].wpn = WPN_NORMAL;
                                peas[m].dmg = pe->dmg * 0.60f;
                                peas[m].vx = 220.0f;
                                peas[m].vy = (kk - 1) * 90.0f;   /* 扇形散开 */
                                peas[m].x = pe->x + 4.0f;
                                peas[m].y = pe->y;
                                break;
                            }
                        }
                    }
                }

                /* 穿透弹：gPierceChance 是普通遗物；WPN_PIERCE 是武器内置 */
                if (pe->wpn == WPN_PIERCE && pe->pierceLeft > 0) {
                    pe->pierceLeft--;
                    pe->dmg *= 0.85f;
                    pe->x = z->x + 26.0f;
                } else if (gPierceChance > 0 && (rand() % 100) < gPierceChance) {
                    pe->dmg *= 0.72f;
                    pe->x = z->x + 26.0f;
                } else {
                    pe->active = 0;
                }
            }
        }
    }

    /* ------------------------- 神级植物的持续效果 ------------------------- */
    {
        int hc = 0;                                   /* 场上神级植物数量（图鉴成就用）*/
        for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) for (sl = 0; sl < 2; sl++) {
            Plant *p = (sl == 0) ? &grid[r][c] : &grid2[r][c];
            if (!p->alive || p->row != r || p->col != c) continue;
            if (p->type < PT_HERO_FLAME) continue;
            hc++;

            if (p->type == PT_HERO_WORLDTREE) {
                /* 世界树（神话）：3x3 范围每秒 200 伤害 + 治疗范围内植物 */
                int dr, dc2;
                for (dr = -1; dr <= 1; dr++) for (dc2 = -1; dc2 <= 1; dc2++) {
                    int r2 = r + dr, c2 = c + dc2;
                    Plant *q;
                    if (r2 < 0 || r2 >= ROWS || c2 < 0 || c2 >= COLS) continue;
                    q = plantAt(r2, c2);
                    if (q && q->hp < q->maxhp) {
                        q->hp += 90.0f * dt;
                        if (q->hp > q->maxhp) q->hp = q->maxhp;
                    }
                }
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    if (!z->active || z->dead) continue;
                    if (fabsf(z->x - cellCX(c)) < CELL_W * 1.6f &&
                        abs(z->row - r) <= 1) {
                        z->hp -= 200.0f * dt;
                        if (rnd(0.0f, 1.0f) < dt * 14.0f)
                            addParticle(z->x + rnd(-20, 20), z->y - rnd(10, 70),
                                        rnd(-60, 60), rnd(-110, -20), 0.5f, rnd(3, 7),
                                        (rnd(0.0f, 1.0f) < 0.5f) ? RGB(120, 230, 140) : RGB(255, 236, 150), 1);
                    }
                }
            } else if (p->type == PT_HERO_FROST) {
                /* 霜之哀伤（传奇）：每 4.5 秒冻结全屏僵尸 3 秒 */
                p->timer -= dt;
                if (p->timer <= 0.0f) {
                    p->timer = 4.5f;
                    for (i = 0; i < MAX_ZOMBIES; i++) {
                        Zombie *z = &zombies[i];
                        if (!z->active || z->dead) continue;
                        if (z->type == ZT_FOOTBALL) { if (z->slow < 1.6f) z->slow = 1.6f; }
                        else if (z->slow < 3.0f) z->slow = 3.0f;
                        z->flash = 0.12f;
                    }
                    flashRed = 0.0f;
                    addShake(3.0f, 0.18f);
                    { int q; for (q = 0; q < 26; q++)
                        addParticle(rnd(LAWN_X, LAWN_X + COLS * CELL_W), rnd((float)LAWN_Y, (float)(LAWN_Y + 80)),
                                    rnd(-40, 40), rnd(60, 150), 0.9f, rnd(3, 7), RGB(180, 240, 255), 1); }
                }
            } else if (p->type == PT_HERO_SUNGOD) {
                /* 太阳神花（传奇）：产阳光 + 每 8 秒治疗全体 */
                p->fuse -= dt;                       /* fuse 对非爆炸植物是空闲字段 */
                if (p->fuse <= 0.0f) {
                    p->fuse = 8.0f;
                    int r2, c2;
                    for (r2 = 0; r2 < ROWS; r2++) for (c2 = 0; c2 < COLS; c2++) {
                        Plant *q = plantAt(r2, c2);
                        if (q && q->hp < q->maxhp) {
                        q->hp += 400.0f;
                        if (q->hp > q->maxhp) q->hp = q->maxhp;
                    }
                    }
                    { int q; for (q = 0; q < 20; q++)
                        addParticle(rnd(LAWN_X, LAWN_X + COLS * CELL_W), rnd((float)LAWN_Y, (float)(LAWN_Y + 300)),
                                    rnd(-30, 30), rnd(-110, -40), 0.9f, rnd(3, 7), RGB(255, 240, 160), 1); }
                }
            }
        }
        if (hc > 0 && gKilled >= 1) { /* 占位：神级植物相关成就后续接入 */ }
    }

    /* ------------------------- 地刺 / 磁力菇（接触型） ------------------------- */
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) for (sl = 0; sl < 2; sl++) {
        Plant *p = (sl == 0) ? &grid[r][c] : &grid2[r][c];
        if (!p->alive || p->row != r || p->col != c) continue;
        if (p->type == PT_HERO_THORNVINE) {
            /* 荆棘藤蔓（史诗）：覆盖所在行全部 9 格，每秒 130 伤害 */
            for (i = 0; i < MAX_ZOMBIES; i++) {
                Zombie *z = &zombies[i];
                if (!z->active || z->dead || z->row != r) continue;
                if (z->type == ZT_BALLOON) continue;
                z->hp -= 130.0f * dt;
                if (rnd(0.0f, 1.0f) < dt * 10.0f)
                    addParticle(z->x + rnd(-10, 10), z->y - 6, rnd(-30, 30), rnd(-60, -10),
                                0.3f, rnd(2, 5), RGB(120, 200, 90), 1);
            }
        } else if (p->type == PT_SPIKEWEED) {
            /* 地刺：踩在上面的僵尸持续受伤。僵尸踩过不啃食，所以会一路挨着走 */
            float cx0 = cellCX(c);
            float lo = cx0 - CELL_W * 0.5f, hi = cx0 + CELL_W * 0.5f;
            int   rr2;
            for (rr2 = gSpikeWide ? r - 1 : r; rr2 <= (gSpikeWide ? r + 1 : r); rr2++) {
                if (rr2 < 0 || rr2 >= ROWS) continue;
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    if (!z->active || z->dead || z->row != rr2) continue;
                    if (z->x < lo || z->x > hi) continue;
                    if (z->type == ZT_BALLOON) continue;   /* 气球飞在空中，踩不到 */
                    z->hp -= 45.0f * gSpikeDmgMul * gTrMutant * dt;   /* 异种羁绊 */
                    if (rnd(0.0f, 1.0f) < dt * 8.0f)
                        addParticle(z->x + rnd(-8, 8), z->y - 6, rnd(-20, 30), rnd(-50, -10),
                                    0.25f, rnd(2, 4), RGB(198, 206, 216), 1);
                }
            }
        } else if (p->type == PT_MAGNET) {
            /* 磁力菇：每 2 秒吸掉范围内一个僵尸的护具（路障/铁桶血量直接砍掉护具部分） */
            p->timer -= dt;
            if (p->timer <= 0.0f) {
                int best = -1;
                float bd = 1e9f;
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    float dx, dy;
                    if (!z->active || z->dead) continue;
                    if (z->type != ZT_CONE && z->type != ZT_BUCKET &&
                        !(z->type == ZT_SCREENDOOR && z->shield)) continue;
                    dx = z->x - cellCX(c); dy = z->y - cellBaseY(r) + 40.0f;
                    if (dx * dx + dy * dy < 200.0f * 200.0f && dx * dx < bd) { bd = dx * dx; best = i; }
                }
                if (best >= 0) {
                    Zombie *z = &zombies[best];
                    float armor = (z->type == ZT_BUCKET) ? 900.0f
                                : (z->type == ZT_SCREENDOOR ? 0.0f : 360.0f);
                    p->timer = 2.0f * gMagnetCdMul / gTrMutant;      /* 异种羁绊 */
                    if (gMagnetGold) gGold += 15;          /* 磁化：吸走护具掉金气 */
                    if (z->type == ZT_SCREENDOOR) {
                        z->shield = 0;                   /* 铁门被吸走，不再是盾 */
                    } else {
                        if (z->hp > armor) z->hp -= armor; else z->hp = 1.0f;
                        z->type = ZT_NORMAL;             /* 护具被吸走，退化成普通僵尸 */
                    }
                    z->flash = 0.14f;
                    {
                        int q;
                        for (q = 0; q < 14; q++)
                            addParticle(z->x + rnd(-18, 18), z->y - rnd(20, 70),
                                        rnd(-90, 90), rnd(-90, 20), 0.5f, rnd(3, 6),
                                        (q & 1) ? RGB(198, 206, 216) : RGB(150, 120, 220), 1);
                    }
                    addShake(2.5f, 0.12f);
                } else {
                    p->timer = 0.4f * gMagnetCdMul;
                }
            }
        }
    }

    /* ------------------------- 僵尸 ------------------------- */
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        if (!z->active) continue;

        /* 倒地动画：播完碎成粒子消失。期间不移动、不被命中 */
        if (z->dead) {
            int k;
            z->deadT -= dt;
            z->anim  += dt * 0.4f;
            if (z->deadT > 0.0f) continue;
            for (k = 0; k < 16; k++)
                addParticle(z->x + rnd(-16, 16), z->y - rnd(6, 46), rnd(-80, 80),
                            rnd(-140, -20), rnd(0.45f, 0.95f), rnd(3, 7),
                            (k & 1) ? RGB(158, 184, 132) : RGB(94, 104, 122), 1);
            z->active = 0;
            z->dead   = 0;
            continue;
        }

        z->anim += dt;
        if (z->flash > 0) z->flash -= dt;
        if (z->slow  > 0) z->slow  -= dt;

        if (z->hp <= 0.0f) { zombieDie(z, 0); continue; }

        /* ---- 铁门僵尸：血量掉到 55% 以下时铁门被打烂 ---- */
        if (z->type == ZT_SCREENDOOR && z->shield && z->hp <= z->maxhp * 0.55f) {
            z->shield = 0;
            z->flash = 0.14f;
            addShake(3.0f, 0.14f);
            { int q; for (q = 0; q < 14; q++)
                addParticle(z->x + rnd(-20, 20), z->y - rnd(10, 60), rnd(-100, 100),
                            rnd(-110, 10), 0.6f, rnd(3, 7), RGB(170, 170, 165), 1); }
        }

        /* ---- 读报僵尸：报纸被打破后暴走 ---- */
        if (z->type == ZT_NEWSPAPER) {
            if (z->hp <= z->maxhp * 0.42f) {
                if (z->speed < zSpeed[ZT_NEWSPAPER] * 2.6f) {
                    z->speed = zSpeed[ZT_NEWSPAPER] * 2.6f;   /* 报纸没了 -> 暴走 */
                    z->flash = 0.16f;
                    { int q; for (q = 0; q < 12; q++)
                        addParticle(z->x + rnd(-16, 16), z->y - rnd(30, 80), rnd(-110, 110),
                                    rnd(-90, 10), 0.6f, rnd(3, 6), RGB(230, 230, 220), 1); }
                }
            }
        }
        /* ---- 巨人僵尸：走到植物前直接砸碎（不吃，一击碎） ---- */
        if (z->type == ZT_GIANT) {
            int gc = (int)((z->x - 24.0f - LAWN_X) / CELL_W);
            Plant *gp = (gc >= 0 && gc < COLS) ? plantAt(z->row, gc) : NULL;
            if (z->atkCd > 0.0f) z->atkCd -= dt;
            if (gp && gp->row == z->row && gp->col == gc) {
                if (z->atkCd <= 0.0f) {
                    z->atkCd = 2.6f;
                    z->anim += 1.0f;
                    addShake(9.0f, 0.30f); flashRed = 0.25f;
                    { int q; for (q = 0; q < 22; q++)
                        addParticle(cellCX(gc) + rnd(-26, 26), cellBaseY(z->row) - rnd(0, 50),
                                    rnd(-140, 140), rnd(-160, 20), 0.8f, rnd(4, 9),
                                    (q & 1) ? RGB(140, 110, 70) : RGB(190, 190, 180), 1); }
                    removePlantAt(gp->row, gp->col);
                    z->eating = 0;
                }
            }
        }
        /* ---- 舞王僵尸：每 6 秒召唤一只伴舞僵尸（同排靠后） ---- */
        if (z->type == ZT_DANCER && !gDancerSilence) {   /* 静音：不召唤伴舞 */
            z->eatBob += dt;
            if (z->eatBob >= 14.0f) {
                z->eatBob = 0.0f;
                if (z->x < VIEW_W - 40) spawnZombieAt(ZT_NORMAL, z->row, z->x + 70.0f);
            }
        }
        /* ---- 冰车僵尸：碾过植物（不啃食，直接压碎） ---- */
        if (z->type == ZT_ZAMBONI) {
            int tc = (int)((z->x - LAWN_X) / CELL_W);
            Plant *tp = plantAt(z->row, tc);
            if (tp && tp->row == z->row && tp->col == tc) {
                if (tp->type != PT_SPIKEWEED) {          /* 地刺能扎破冰车 */
                    removePlantAt(tp->row, tp->col);
                    addShake(4.0f, 0.16f);
                    { int q; for (q = 0; q < 12; q++)
                        addParticle(z->x, z->y - rnd(0, 30), rnd(-70, 40), rnd(-110, -20),
                                    0.5f, rnd(3, 6), RGB(150, 200, 230), 1); }
                } else {
                    z->hp -= 60.0f * dt;                 /* 地刺反伤冰车 */
                }
            }
        }
        /* ---- 气球僵尸：飞在空中，不受地面植物阻挡，被攻击 3 次后落地 ---- */
        if (z->type == ZT_BALLOON) {
            if (z->maxhp - z->hp >= z->maxhp * 0.34f) {
                z->type = ZT_NORMAL;                     /* 气球破了 -> 落地变普通僵尸 */
                z->speed = zSpeed[ZT_NORMAL];
                addShake(2.0f, 0.12f);
                { int q; for (q = 0; q < 16; q++)
                    addParticle(z->x + rnd(-16, 16), z->y - rnd(80, 140), rnd(-80, 80),
                                rnd(-40, 60), 0.7f, rnd(3, 7), RGB(228, 96, 110), 1); }
            }
        }

        {
            if (z->slow > 0.0f) z->slow -= dt;
            if (z->rooted > 0.0f) z->rooted -= dt;
            {
            float sp   = z->speed * (z->rooted > 0.0f ? 0.0f : (z->slow > 0 ? gSlowFactor : 1.0f));
            float bite = z->x - 24.0f;
            int   col  = (int)floor((bite - LAWN_X) / (float)CELL_W);
            Plant *tg  = (col >= 0 && col < COLS) ? plantAt(z->row, col) : NULL;

            if (tg) {
                if (z->type == ZT_BALLOON) { z->eating = 0; tg = NULL; }   /* 飞在空中，不啃植物 */
                if (z->type == ZT_SCREENDOOR && z->shield) { z->eating = 0; tg = NULL; }
                if (z->type == ZT_GIANT) { z->eating = 0; tg = NULL; }     /* 巨人不啃，直接砸 */
                /* 撑杆僵尸：在"即将啃食"这一刻改成跳过去。
                   注意判定必须挂在啃食点上 —— 原来用更近的距离阈值，
                   僵尸会先停下来啃食，永远轮不到跳跃。 */
                if (z->type == ZT_VAULTER && !z->vaulted) {
                    z->vaulted = 1;
                    z->eating = 0;
                    z->x -= 96.0f;
                    z->anim += 0.6f;
                    addShake(2.2f, 0.12f);
                    { int q; for (q = 0; q < 14; q++)
                        addParticle(z->x + rnd(-10, 50), z->y - rnd(6, 60), rnd(-90, 30),
                                    rnd(-90, 20), 0.5f, rnd(3, 6), RGB(220, 200, 170), 1); }
                    tg = NULL;
                }
            }
            if (tg) {
                z->eating = 1;
                z->eatBob += dt;
                if (tg->type == PT_GARLIC) {
                    int nr = z->row + ((z->row <= 0) ? 1 : ((z->row >= ROWS - 1) ? -1 : ((rand() & 1) ? 1 : -1)));
                    z->row = nr;
                    z->y = cellBaseY(nr);
                    z->eating = 0;
                    tg->hp -= 40.0f;
                    tg->eaten = 1.0f;
                } else if (tg->type == PT_HYPNOSHROOM) {
                    zombieDie(z, 1);
                    spawnSunDrop(z->x, z->y - 40, z->y - 8, 50);
                    tg->alive = 0;
                } else {
                    if (tg->shield > 0.0f) {
                        tg->shield -= 80.0f * dt;
                        if (tg->shield < 0.0f) tg->shield = 0.0f;
                    } else {
                        tg->hp   -= 80.0f * dt;
                    }
                    if (gThorns > 0) z->hp -= (float)gThorns * dt;
                    tg->eaten = 1.0f;
                    if (tg->hp <= 0.0f) {
                        int k;
                        for (k = 0; k < 12; k++)
                            addParticle(cellCX(col) + rnd(-16, 16), cellBaseY(z->row) - rnd(4, 50),
                                        rnd(-70, 70), rnd(-120, -20), rnd(0.4f, 0.8f),
                                        rnd(3, 7), RGB(110, 180, 80), 1);
                        tg->alive = 0;
                    }
                }
            } else {
                z->eating = 0;
                z->x -= sp * dt;
            }

            /* 抵达房屋 */
            if (bite < LAWN_X - 2.0f) {
                if (mowers[z->row].active && !mowers[z->row].running) {
                    mowers[z->row].running = 1;
                } else if (!mowers[z->row].active) {
                    /* 失败也结算：经验参与奖 + 纪录 */
                    gainXp(gKilled / 4 + 5);
                    if (gKilled > gSave.bestKills) { gSave.bestKills = gKilled; gRecNew = 1; }
                    if (gGold   > gSave.bestGold)  { gSave.bestGold  = gGold;   gRecNew = 1; }
                    saveFlush();
                    gState = ST_LOSE;
                    flashRed = 1.0f;
                    addShake(12, 0.6f);
                    return;
                }
            }
        }
        }
    }

    /* ------------------------- 小推车 ------------------------- */
    for (r = 0; r < ROWS; r++) {
        Mower *m = &mowers[r];
        if (!m->active || !m->running) continue;
        m->x += m->speed * dt;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r) continue;
            if (fabs(z->x - m->x) < 48.0f) zombieDie(z, 1);
        }
        if (m->x > VIEW_W + 80.0f) {
            m->active = 0; m->running = 0; gMowerUsed++;
            m->recharge = 30.0f;          /* 备用轮胎会用这个计时补回来 */
        }
    }

    /* ------------------------- 阳光 ------------------------- */
    for (i = 0; i < MAX_SUNS; i++) {
        SunDrop *s = &suns[i];
        if (!s->active) continue;
        s->phase += dt;
        /* 自动拾取：出现短暂停留后自动飞向计数器（仍可手动点击立即拾取） */
        if (gAutoSun && !s->flying && (11.0f - s->life) > 0.45f) s->flying = 1;
        if (s->flying) {
            float dx = 36.0f - s->x, dy = 44.0f - s->y;
            float d  = sqrt(dx * dx + dy * dy);
            if (d < 26.0f) {
                gSun += (int)(s->value * gSunGainMul + 0.5f);
                s->active = 0;
            } else {
                float k = 1000.0f * dt / d;
                s->x += dx * k; s->y += dy * k;
            }
        } else {
            if (s->y < s->fy) {
                s->y += s->vy * dt;
                if (s->y > s->fy) s->y = s->fy;
            }
            s->life -= dt;
            if (s->life <= 0.0f) s->active = 0;
        }
    }

    /* ------------------------- 粒子 ------------------------- */
    for (i = 0; i < MAX_PARTS; i++) {
        Particle *pt = &parts[i];
        if (!pt->active) continue;
        pt->life -= dt;
        if (pt->life <= 0.0f) { pt->active = 0; continue; }
        pt->x += pt->vx * dt;
        pt->y += pt->vy * dt;
        if (pt->grav) pt->vy += 430.0f * dt;
    }
}

/* ======================================================================== */
/*                                   输入                                    */
/* ======================================================================== */
static void onClick(int mx, int my)
{
    int i;

    if (gState == ST_MENU) {
        /* 菜单按钮：第一排 开始/编组/天赋/成就/抽卡，第二排 每日营地 */
        float by = 330.0f;
        if (my >= (int)by && my <= (int)(by + 46)) {
            if (mx >= 40  && mx <= 300)  { gState = ST_LEVELS;  return; }
            if (mx >= 320 && mx <= 450)  { gState = ST_LOADOUT; return; }
            if (mx >= 470 && mx <= 600)  { gState = ST_TALENTS; return; }
            if (mx >= 620 && mx <= 750)  { gState = ST_ACH;     return; }
            if (mx >= 770 && mx <= 960)  { gState = ST_GACHA;   return; }
        }
        if (my >= 396 && my <= 442 && mx >= 370 && mx <= 630) { gState = ST_DAILY; return; }
        gState = ST_LEVELS;
        return;
    }
    if (gState == ST_WIN || gState == ST_LOSE) {
        rewardPrepare();            /* 每局打完都给一次抽卡 */
        gState = ST_REWARD;
        return;
    }

    /* ---- 元界面交互 ---- */
    if (gState == ST_LEVELS) {
        int lv = metaHitLevel(mx, my);
        if (lv >= 0 && levelPlayable(lv)) {
            gCurLevel = lv;
            gLawnVariant = levelDefs[lv].lawnVariant;
            resetGame();
            gState = ST_PLAY;
        }
        return;
    }
    if (gState == ST_TALENTS) {
        int t = metaHitTalent(mx, my);
        if (t >= 0 && !gSave.talents[t]) {
            int tier = t % 4;
            if (tier == 0 || gSave.talents[t - 1]) {      /* 必须按顺序 */
                if (gSave.stars >= talCost[t]) {
                    gSave.stars -= talCost[t];
                    gSave.talents[t] = 1;
                    relicsRecalc();                        /* 天赋也走同一套修正 */
                    achUnlock(17);
                    { int q, all = 1; for (q = 0; q < TAL_COUNT; q++) if (!gSave.talents[q]) all = 0;
                      if (all) achUnlock(18); }
                    saveFlush();
                }
            }
        }
        return;
    }
    if (gState == ST_ACH) return;

    if (gState == ST_LOADOUT) {
        int t = loHit(mx, my);
        if (t >= 0) loadoutToggle(t);
        return;
    }
    if (gState == ST_REWARD) {
        if (gRewardPick[0] < 0) { gState = ST_MENU; resetGame(); return; }
        if (gRewardChosen < 0) {
            int c = rwHit(mx, my);
            if (c >= 0) rewardChoose(c);
        } else {
            gState = ST_MENU;
        }
        return;
    }
    if (gState == ST_GACHA) {
        float bx = VIEW_W * 0.5f - 150.0f;
        if ((float)mx >= bx && (float)mx <= bx + 300.0f &&
            (float)my >= GACHA_BTN_Y && (float)my <= GACHA_BTN_Y + 64.0f)
            doGacha();
        return;
    }
    if (gState == ST_DAILY) {
        /* 每日营地：签到面板 / 每日挑战面板 */
        if (my >= 280 && my <= 370 && mx >= 370 && mx <= 630) { doCheckIn(); return; }
        if (my >= 400 && my <= 500 && mx >= 370 && mx <= 630) { dailyStart(); return; }
        return;
    }
    if (gState == ST_DRAFT) {
        int k;
        for (k = 0; k < DRAFT_N; k++) {
            float cx0, cy0;
            draftCardRect(k, &cx0, &cy0);
            if (mx >= cx0 && mx <= cx0 + CARD_W && my >= cy0 && my <= cy0 + CARD_H) {
                applyRelic(gDraft[k]);
                gState = ST_PLAY;
                return;
            }
        }
        if (my >= SKIP_Y && my <= SKIP_Y + SKIP_H &&
            mx >= VIEW_W * 0.5f - 200.0f && mx <= VIEW_W * 0.5f + 200.0f) {
            gSun += DRAFT_SKIP;
            gState = ST_PLAY;
        }
        return;
    }
    if (gState != ST_PLAY) return;

    /* 1) 收阳光 */
    for (i = 0; i < MAX_SUNS; i++) {
        SunDrop *s = &suns[i];
        if (s->active && !s->flying) {
            float dx = mx - s->x, dy = my - s->y;
            if (dx * dx + dy * dy < 34.0f * 34.0f) { s->flying = 1; return; }
        }
    }

    /* 2) 顶部卡槽：卡位 i 对应编组里的第 i 株 */
    if (my < 92) {
        /* 必须和绘制处用同一间距（含奖励植物）——之前这里只看 gLoadoutN，
           编组 4 株 + 奖励 5 株时卡片按 72 画、点击按 86 算，全部点偏 */
        float cgap = cardBarGap();
        int slot, idx;
        if (mx < 140) return;
        slot = (int)floor((mx - 144.0f) / cgap);
        if (slot >= 0 && slot < gLoadoutN + gBonusN) {
            idx = (slot < gLoadoutN) ? gLoadout[slot] : gBonusPlant[slot - gLoadoutN];
            if (cardCD[idx] <= 0.0f && gSun >= plantCost(idx)) { gSel = idx; gShovel = 0; }
            else gSel = -1;
            return;
        }
        if (slot == gLoadoutN + gBonusN) { gShovel = !gShovel; gSel = -1; return; }
        return;
    }

    /* 3) 草坪 */
    {
        int row, col, w, hh;
        if (!mouseToCell(mx, my, &row, &col, &w, &hh)) { gSel = -1; gShovel = 0; return; }

        if (gShovel) {
            if (plantAt(row, col)) { removePlantAt(row, col); gShovel = 0; }
            return;
        }
        if (gSel == PT_PUMPKIN) {
            Plant *ex = plantAt(row, col);
            if (ex && ex->type != PT_PUMPKIN) {
                ex->shield = 2200.0f;
                gSun -= plantCost(gSel);
                if (!HAS(R_NINJA)) cardCD[gSel] = plantCd(gSel);
                gSel = -1;
                return;
            }
        }
        /* 注意：这里不能用 plantAt()==NULL 判断 —— 双株模式下第二株要种进 slot1，
           而 plantAt 会返回已经存在的 slot0，导致第二株永远种不下去。
           能不能种，cellLegal() 里的 cellHasRoom() 已经判过了。 */
        if (gSel >= 0 && cellLegal(row, col, w, hh)) {
            plantIt(row, col, gSel);
            gSun -= plantCost(gSel);
            /* 隐忍者：种下不结算冷却，下一次种才扣 */
            if (!HAS(R_NINJA)) cardCD[gSel] = plantCd(gSel);
            for (i = 0; i < 10; i++)
                addParticle(cellCX(col) + rnd(-24, 24), cellBaseY(row) - rnd(0, 20),
                            rnd(-70, 70), rnd(-110, -25), 0.45f, rnd(3, 7),
                            RGB(186, 154, 108), 1);
        }
    }
}
/* ======================================================================== */
/*                         三选一抽卡（肉鸽奖励）                             */
/* ======================================================================== */
static const COLORREF CAT_COL[6] = {
    RGB(232, 188,  66),   /* 经济 */
    RGB(214,  96,  70),   /* 火力 */
    RGB(110, 178,  92),   /* 生存 */
    RGB( 92, 156, 214),   /* 控制 */
    RGB(150, 122, 214),   /* 构筑 */
    RGB(190,  90, 110),   /* 风险 */
};
static const COLORREF RAR_COL[3] = {
    RGB(158, 156, 146),   /* 普通 */
    RGB( 72, 140, 226),   /* 稀有 */
    RGB(228, 174,  50),   /* 传说 */
};
static const wchar_t *RAR_NAME[3] = { L"普通", L"稀有", L"传说" };
static const wchar_t *CAT_NAME[6] = { L"经济", L"火力", L"生存", L"控制", L"构筑", L"风险" };

static void draftCardRect(int i, float *x, float *y)
{
    float total = DRAFT_N * CARD_W + (DRAFT_N - 1) * CARD_GAP;
    *x = (VIEW_W - total) * 0.5f + (float)i * (CARD_W + CARD_GAP);
    *y = CARD_Y;
}

/* 按稀有度权重从池子里抽一个；目标档没有就逐级退化 */
static int draftPickOne(const int *pool, int n, int forceRare)
{
    int target, roll, i, cand[R_COUNT], m = 0;
    roll = rand() % 100;
    if (forceRare)      target = (roll < 30) ? 2 : 1;
    else if (roll < 13) target = 2;
    else if (roll < 45) target = 1;
    else                target = 0;
    for (i = 0; i < n; i++) if (relicDefs[pool[i]].rarity == target) cand[m++] = pool[i];
    if (m == 0) for (i = 0; i < n; i++) if (relicDefs[pool[i]].rarity == 0) cand[m++] = pool[i];
    if (m == 0) for (i = 0; i < n; i++) cand[m++] = pool[i];
    return cand[rand() % m];
}

/* 三选一卡片编码：
     >= 0            -> 遗物 id
     <  0            -> 神级植物，植物 id = (-v) - 1
   这样一套 UI 就能同时画遗物卡和植物卡。 */
#define DRAFT_PLANT(p)   (-((p) + 1))
#define DRAFT_IS_PLANT(v) ((v) < 0)
#define DRAFT_PLANT_ID(v) ((-(v)) - 1)
/* 全图鉴兜底：植物精华卡。编码成"植物 id = PT_COUNT"，和真实植物区分开 */
#define DRAFT_ESSENCE      DRAFT_PLANT(PT_COUNT)
#define DRAFT_ESSENCE_SUN  250
#define DRAFT_ESSENCE_GOLD  25

static void openDraft(void)
{
    int i, t, pool[R_COUNT], n = 0;
    int rareLeft = (gWave % 5 == 0) ? 1 : 0;      /* 旗标波保底一张稀有以上 */
    int plantIdx = rand() % DRAFT_N;              /* 植物卡固定占一个位置 */
    int heroPlant, normalPlant = -1;

    for (i = 1; i < R_COUNT; i++) if (!HAS(i)) pool[n++] = i;

    /* ---------------- 硬保证：三张里【有且仅有】一张植物卡 ----------------
       旧实现先掷概率（32%~55%）再决定要不要放植物卡，而且"某一档的两株都拿过"
       就直接放弃，于是经常三个选项全是遗物。现在改成：
         ① 本局还没拿到的神级植物（按稀有度权重抽，只在本局生效）
         ② 本局神级卡槽满 / 8 株都拿过 -> 永久解锁一株还没解锁的普通植物
         ③ 连普通植物也全解锁 -> 植物精华卡（阳光 + 金气）
       三种必然命中其一，所以"出植物卡"的概率恒为 100%，且只会占一个位置。 */
    gDraftPlantPerm = 0;
    heroPlant = rollHeroPlant();
    if (heroPlant < 0) {
        normalPlant = rollNormalPlant();
        if (normalPlant >= 0) gDraftPlantPerm = 1;
    }
    /* 遗物也拿光了（44 项全满）才可能出现凑不满三个选项的退化态 */
    if (n == 0 && heroPlant < 0 && normalPlant < 0) { gState = ST_PLAY; return; }

    for (i = 0; i < DRAFT_N; i++) {
        int pick;
        if (i == plantIdx) {                     /* 植物卡：占据且仅占这一个位置 */
            if (heroPlant >= 0)        gDraft[i] = DRAFT_PLANT(heroPlant);
            else if (normalPlant >= 0) gDraft[i] = DRAFT_PLANT(normalPlant);
            else                       gDraft[i] = DRAFT_ESSENCE;
            continue;
        }
        if (n == 0) { gDraft[i] = DRAFT_ESSENCE; continue; }   /* 退化态兜底 */
        pick = draftPickOne(pool, n, rareLeft);
        if (relicDefs[pick].rarity >= 1) rareLeft = 0;
        gDraft[i] = pick;
        for (t = 0; t < n; t++) if (pool[t] == pick) { pool[t] = pool[n - 1]; n--; break; }
    }
    gDraftHover = -1;
    gSel = -1; gShovel = 0;
    gState = ST_DRAFT;
}

/* 拿到神级植物：只加进【本局】的额外卡槽。
   注意这里绝对不能写 gSave.plantOwned —— 那是永久解锁，
   上一版就是写了它导致"上一局拿到的神级植物下一局还能用"，
   把三选一的随机性完全破坏掉了。也不要落盘。 */
static void applyHeroPlant(int pid)
{
    int i;
    if (pid < 0 || pid >= PT_COUNT) return;
    if (gBonusN < MAX_BONUS_PLANT) {
        for (i = 0; i < gBonusN; i++) if (gBonusPlant[i] == pid) return;   /* 本局已有 */
        gBonusPlant[gBonusN++] = pid;
    }
    cardCD[pid] = 0.0f;
}

static void applyRelic(int id)
{
    if (DRAFT_IS_PLANT(id)) {
        int pid = DRAFT_PLANT_ID(id);
        if (pid >= PT_COUNT) {                   /* 全图鉴兜底：植物精华 */
            gSun += DRAFT_ESSENCE_SUN;
            gGold += DRAFT_ESSENCE_GOLD;
            return;
        }
        if (gDraftPlantPerm) {                   /* 普通植物卡：永久解锁 */
            if (pid >= 0 && pid < PT_HERO_FLAME && !gSave.plantOwned[pid]) {
                int c = 0, k;
                gSave.plantOwned[pid] = 1;
                for (k = 0; k < PT_COUNT; k++) if (gSave.loadout[k]) c++;
                if (c < gCardSlots) gSave.loadout[pid] = 1;   /* 编组有空位就顺手带上 */
                saveFlush();
            }
            return;
        }
        applyHeroPlant(pid);
        return;
    }
    if (id <= 0 || id >= R_COUNT) return;
    if (!gRelic[id]) { gRelic[id] = 1; gRelicCount++; }
    relicsRecalc();
    plantsRefreshHp();
    if (HAS(R_NO_MOWER)) {                        /* 背水一战：立刻撤掉所有小推车 */
        int r;
        for (r = 0; r < ROWS; r++) { mowers[r].active = 0; mowers[r].running = 0; }
    }
    gDraftCount++;
    switch (id) {
    case R_BANKER:           gBanker = 1; break;
    case R_NECROMANCER:      gNecromancer = 1; break;
    case R_WEAPON_RACK:      applyWeaponRack(1); break;
    case R_MAIN_UPGRADE:     applyMainUpgrade(); break;
    case R_WEAPON_MASTER:    gHasSubWpn = 1; break;
    case R_MODE_SHIFT: {
        int m;
        for (m = 0; m < MODE_COUNT; m++) if (!gUnlockedMode[m]) break;
        if (m < MODE_COUNT) gUnlockedMode[m] = 1;
        break;
    }
    case R_MUTANT_SEED: {
        int m;
        for (m = 0; m < MODE_COUNT; m++) if (!gUnlockedMode[m]) break;
        if (m < MODE_COUNT) { gUnlockedMode[m] = 1; gCurMode = m; }
        break;
    }
    case R_CHARGED:          gChargeMax += 1; break;
    default: break;
    }
}

/* 武器架：每株植物解锁副射槽 + 随机给一种非默认武器 */
static void applyWeaponRack(int sub)
{
    gHasSubWpn = 1;
    if (sub) {
        int w = (rand() % (WPN_COUNT - 1)) + 1;   /* 不给默认直射 */
        gSubWpn = w;
    }
}
static void applyMainUpgrade(void)
{
    gMainWpn = (rand() % (WPN_COUNT - 1)) + 1;
}

/* ---- 遗物图标：没有美术资源，用几何符号按类别区分 ---- */
static void drawRelicIcon(HDC dc, int cat, COLORREF col, float cx, float cy, float r)
{
    COLORREF dk = tint(col, -0.62f);
    COLORREF lt = tint(col, 0.62f);
    fillRound(dc, cx - r, cy - r, cx + r, cy + r, r * 0.30f, dk);
    fillRound(dc, cx - r * 0.87f, cy - r * 0.87f, cx + r * 0.87f, cy + r * 0.87f, r * 0.26f, col);
    switch (cat) {
    case 0:                                   /* 经济：硬币 */
        fillCircle(dc, cx, cy, r * 0.50f, lt);
        fillRect(dc, cx - r * 0.07f, cy - r * 0.30f, cx + r * 0.07f, cy + r * 0.30f, dk);
        break;
    case 1:                                   /* 火力：向上箭头 */
        fillTriangle(dc, cx, cy - r * 0.52f, cx + r * 0.50f, cy + r * 0.34f,
                     cx - r * 0.50f, cy + r * 0.34f, lt);
        fillRect(dc, cx - r * 0.13f, cy + r * 0.30f, cx + r * 0.13f, cy + r * 0.56f, lt);
        break;
    case 2:                                   /* 生存：盾形方块 */
        fillRound(dc, cx - r * 0.46f, cy - r * 0.50f, cx + r * 0.46f, cy + r * 0.50f,
                  r * 0.20f, lt);
        break;
    case 3: {                                 /* 控制：菱形 */
        POINT p[4];
        p[0].x = (int)cx;            p[0].y = (int)(cy - r * 0.58f);
        p[1].x = (int)(cx + r * 0.50f); p[1].y = (int)cy;
        p[2].x = (int)cx;            p[2].y = (int)(cy + r * 0.58f);
        p[3].x = (int)(cx - r * 0.50f); p[3].y = (int)cy;
        poly(dc, p, 4, lt, 0, 0, 0);
        break;
    }
    case 4:                                   /* 构筑：三环相扣 */
        fillCircle(dc, cx, cy - r * 0.30f, r * 0.24f, lt);
        fillCircle(dc, cx - r * 0.30f, cy + r * 0.26f, r * 0.24f, lt);
        fillCircle(dc, cx + r * 0.30f, cy + r * 0.26f, r * 0.24f, lt);
        break;
    default:                                  /* 风险：叉 */
        line(dc, cx - r * 0.42f, cy - r * 0.42f, cx + r * 0.42f, cy + r * 0.42f,
             lt, (int)(r * 0.20f) + 1);
        line(dc, cx + r * 0.42f, cy - r * 0.42f, cx - r * 0.42f, cy + r * 0.42f,
             lt, (int)(r * 0.20f) + 1);
        break;
    }
}

/* 世界层：卡片底板、色条、图标（跟随 2x 超采样，边缘平滑） */
static void drawDraftShapes(HDC dc)
{
    int i;
    for (i = 0; i < DRAFT_N; i++) {
        float x, y;
        int rid = gDraft[i];
        int hov = (gDraftHover == i);
        int isPlant = DRAFT_IS_PLANT(rid);
        COLORREF rc, cc;
        draftCardRect(i, &x, &y);
        if (isPlant) {
            int pid = DRAFT_PLANT_ID(rid);
            if (pid >= PT_COUNT) {                 /* 植物精华兜底卡 */
                rc = cc = RGB(240, 198, 92);
            } else if (gDraftPlantPerm) {          /* 普通植物：永久解锁，绿色区分 */
                rc = cc = RGB(148, 216, 118);
            } else {
                int hr = plantDefs[pid].rarity - 1;
                if (hr < 0) hr = 0;
                if (hr >= HERO_RARITY_COUNT) hr = HERO_RARITY_COUNT - 1;
                rc = HERO_RARITY_COL[hr];          /* 稀有度色条 */
                cc = rc;
            }
        } else {
            rc = RAR_COL[relicDefs[rid].rarity];
            cc = CAT_COL[relicDefs[rid].cat];
        }
        fillRound(dc, x + 5, y + 8, x + CARD_W + 5, y + CARD_H + 8, 18, RGB(22, 36, 26));
        /* 植物卡的卡面用带金边的深色底，和遗物卡一眼区分 */
        fillRound(dc, x, y, x + CARD_W, y + CARD_H, 18,
                  isPlant ? (hov ? RGB(46, 38, 62) : RGB(34, 28, 48))
                          : (hov ? RGB(255, 252, 240) : RGB(245, 240, 224)));
        fillRound(dc, x, y, x + CARD_W, y + 18, 18, rc);
        fillRect(dc, x, y + 9, x + CARD_W, y + 18, rc);
        strokeRound(dc, x, y, x + CARD_W, y + CARD_H, 18,
                    hov ? rc : tint(rc, -0.28f), hov ? 5 : 3);
        if (isPlant) {
            /* 神级植物：直接画植物本体 + 一圈稀有度光环 */
            int pid = DRAFT_PLANT_ID(rid);
            int g2;
            for (g2 = 0; g2 < 3; g2++)
                fillEllipse(dc, x + CARD_W * 0.5f, y + 210.0f - (float)g2 * 5.0f,
                            62.0f - (float)g2 * 12.0f, 17.0f - (float)g2 * 3.5f,
                            (g2 & 1) ? tint(rc, 0.45f) : tint(rc, -0.15f));
            if (pid >= PT_COUNT) {                 /* 植物精华：画一颗太阳 */
                if (gSprSun.ok)
                    spriteBlit(dc, &gSprSun, x + CARD_W * 0.5f, y + 246.0f, 0.82f, 0.82f, -1);
                else
                    drawRelicIcon(dc, 4, cc, x + CARD_W * 0.5f, y + 196.0f, 52.0f);
            } else if (gSprPlant[pid].ok) {
                spriteBlit(dc, &gSprPlant[pid], x + CARD_W * 0.5f, y + 246.0f, 0.82f, 0.82f, -1);
            } else {
                drawRelicIcon(dc, 4, cc, x + CARD_W * 0.5f, y + 196.0f, 52.0f);
            }
        } else {
            drawRelicIcon(dc, relicDefs[rid].cat, cc, x + CARD_W * 0.5f, y + 196.0f, 52.0f);
        }
        if (hov) strokeRound(dc, x + 8, y + 8, x + CARD_W - 8, y + CARD_H - 8, 12,
                             tint(rc, 0.45f), 2);
    }
    /* 跳过条 */
    {
        float sx = VIEW_W * 0.5f - 200.0f, hover = (mMouseY >= SKIP_Y && mMouseY <= SKIP_Y + SKIP_H &&
                                                    mMouseX >= sx && mMouseX <= sx + 400.0f);
        fillRound(dc, sx, SKIP_Y, sx + 400.0f, SKIP_Y + SKIP_H, 10,
                  hover ? RGB(96, 122, 92) : RGB(62, 82, 60));
        strokeRound(dc, sx, SKIP_Y, sx + 400.0f, SKIP_Y + SKIP_H, 10, RGB(150, 196, 150), 2);
    }
    /* 已获得遗物一览 */
    {
        int k, n = 0;
        float total, x0;
        for (k = 1; k < R_COUNT; k++) if (HAS(k)) n++;
        total = (float)n * 38.0f;
        x0 = VIEW_W * 0.5f - total * 0.5f + 19.0f;
        for (k = 1; k < R_COUNT; k++) {
            if (!HAS(k)) continue;
            drawRelicIcon(dc, relicDefs[k].cat, CAT_COL[relicDefs[k].cat], x0, 598.0f, 15.0f);
            x0 += 38.0f;
        }
    }
}

/* 自动换行（中文按字符宽度实测，不能按字数估） */
static void putTextWrap(HDC dc, HFONT f, COLORREF c, const wchar_t *s,
                        float x, float y, float maxw, float lh, int maxlines)
{
    wchar_t buf[512];
    int len = (int)wcslen(s), i = 0, lines = 0;
    SIZE sz;
    SelectObject(dc, f);
    while (i < len && lines < maxlines) {
        int j = i + 1, best = i + 1;
        while (j <= len) {
            int k;
            for (k = i; k < j; k++) buf[k - i] = s[k];
            buf[j - i] = 0;
            GetTextExtentPoint32W(dc, buf, j - i, &sz);
            if ((float)sz.cx > maxw) break;
            best = j;
            j++;
        }
        /* 不要把连续的数字/英文拆到两行（「+20%」被切成「+2」/「0%」很难看） */
        while (best > i + 1 && best < len &&
               iswalnum(s[best]) && iswalnum(s[best - 1])) best--;
        for (j = i; j < best; j++) buf[j - i] = s[j];
        buf[best - i] = 0;
        if (lines == maxlines - 1 && best < len) {
            int bl = (int)wcslen(buf);
            if (bl >= 2) { buf[bl - 2] = L'…'; buf[bl - 1] = 0; }
        }
        putText(dc, f, c, buf, x, y + (float)lines * lh, 0);
        i = best;
        lines++;
    }
}

/* 文字层（1x，清晰） */
static void drawDraftText(HDC dc)
{
    wchar_t buf[256];
    int i, n = 0;
    for (i = 1; i < R_COUNT; i++) if (HAS(i)) n++;
    wsprintfW(buf, L"第 %d 波 · 选择一项强化", gWave);
    putTextCS(dc, gF30, RGB(255, 246, 200), RGB(18, 32, 20), buf, VIEW_W * 0.5f, 100.0f);

    for (i = 0; i < DRAFT_N; i++) {
        float x, y;
        int rid = gDraft[i];
        COLORREF rc;
        draftCardRect(i, &x, &y);
        if (DRAFT_IS_PLANT(rid)) {
            int pid = DRAFT_PLANT_ID(rid);
            /* 注意：植物卡的 rid 是负数，绝对不能用 relicDefs[rid] 取色（越界读） */
            if (pid >= PT_COUNT) {
                rc = RGB(240, 198, 92);
                putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), L"奖励",
                         x + 12.0f, y + 2.0f, 0);
                putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), L"植物",
                         x + CARD_W - 14.0f, y + 2.0f, 2);
                putTextCS(dc, gF22, RGB(255, 244, 214), RGB(40, 26, 60), L"植物精华",
                          x + CARD_W * 0.5f, y + 54.0f);
                putTextWrap(dc, gF15, RGB(212, 204, 232),
                            L"全部植物都已解锁，转化为资源",
                            x + 22.0f, y + 268.0f, CARD_W - 44.0f, 22.0f, 3);
                wsprintfW(buf, L"阳光 +%d　金气 +%d", DRAFT_ESSENCE_SUN, DRAFT_ESSENCE_GOLD);
                putTextCS(dc, gF15, tint(rc, 0.35f), RGB(40, 26, 60), buf,
                          x + CARD_W * 0.5f, y + 264.0f);
            } else if (gDraftPlantPerm) {
                rc = RGB(148, 216, 118);
                putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), L"新植物",
                         x + 12.0f, y + 2.0f, 0);
                putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), L"永久解锁",
                         x + CARD_W - 14.0f, y + 2.0f, 2);
                putTextCS(dc, gF22, RGB(255, 244, 214), RGB(40, 26, 60), plantDefs[pid].name,
                          x + CARD_W * 0.5f, y + 54.0f);
                putTextWrap(dc, gF15, RGB(212, 204, 232),
                            L"永久解锁：编组和抽卡池里都会出现",
                            x + 22.0f, y + 268.0f, CARD_W - 44.0f, 22.0f, 3);
                putTextCS(dc, gF15, tint(rc, 0.35f), RGB(40, 26, 60),
                          L"选它 = 永久解锁", x + CARD_W * 0.5f, y + 264.0f);
            } else {
                int hr = plantDefs[pid].rarity - 1;
                const wchar_t *pdesc = plantDefs[pid].desc;
                if (hr < 0) hr = 0;
                if (hr >= HERO_RARITY_COUNT) hr = HERO_RARITY_COUNT - 1;
                rc = HERO_RARITY_COL[hr];
                wsprintfW(buf, L"神级 · %s", HERO_RARITY_NAME[hr]);
                putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), buf, x + 12.0f, y + 2.0f, 0);
                putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), L"植物",
                         x + CARD_W - 14.0f, y + 2.0f, 2);
                putTextCS(dc, gF22, RGB(255, 244, 214), RGB(40, 26, 60), plantDefs[pid].name,
                          x + CARD_W * 0.5f, y + 54.0f);
                putTextWrap(dc, gF15, RGB(212, 204, 232), pdesc ? pdesc : L"",
                            x + 22.0f, y + 268.0f, CARD_W - 44.0f, 22.0f, 4);
                putTextCS(dc, gF15, tint(rc, 0.35f), RGB(40, 26, 60),
                          L"选它 = 本局多一个卡槽", x + CARD_W * 0.5f, y + 264.0f);
            }
        } else {
            rc = RAR_COL[relicDefs[rid].rarity];
            wsprintfW(buf, L"%s", RAR_NAME[relicDefs[rid].rarity]);
            putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), buf, x + 14.0f, y + 2.0f, 0);
            wsprintfW(buf, L"%s", CAT_NAME[relicDefs[rid].cat]);
            putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), buf, x + CARD_W - 14.0f, y + 2.0f, 2);
            putTextCS(dc, gF22, RGB(46, 34, 22), RGB(255, 255, 255), relicDefs[rid].name,
                      x + CARD_W * 0.5f, y + 54.0f);
            putTextWrap(dc, gF15, RGB(78, 66, 50), relicDefs[rid].desc,
                        x + 24.0f, y + 268.0f, CARD_W - 48.0f, 22.0f, 3);
        }
    }
    wsprintfW(buf, L"跳过奖励  →  +%d 阳光　（按 S 或点这里）", DRAFT_SKIP);
    putTextCS(dc, gF18, RGB(236, 246, 236), RGB(18, 32, 20), buf, VIEW_W * 0.5f,
              SKIP_Y + SKIP_H * 0.5f);
    wsprintfW(buf, L"已获得 %d 项强化", n);
    putTextCS(dc, gF15, RGB(200, 220, 200), RGB(18, 32, 20), buf, VIEW_W * 0.5f, 574.0f);
}

/* ======================================================================== */
/*                    元界面：选关 / 天赋 / 成就 / 抽卡                        */
/* ======================================================================== */
static void metaLayoutLevel(int i, float *x, float *y)
{
    *x = LV_COL_X0 + (float)(i % 3) * (LV_CARD_W + LV_GAP_X);
    *y = LV_ROW_Y0 + (float)(i / 3) * (LV_CARD_H + LV_GAP_Y);
}

static void metaLayoutTalent(int i, float *x, float *y)
{
    int cat = talCat[i];              /* 0 园艺 1 战斗 2 坚韧 */
    int tier = i % 4;
    *x = TAL_X0 + (float)tier * (TAL_NODE_W + TAL_GAP_X);
    *y = TAL_Y0 + (float)cat * (TAL_NODE_H + TAL_GAP_Y);
}

static int metaHitLevel(int mx, int my)
{
    int i;
    for (i = 0; i < LV_COUNT; i++) {
        float x, y;
        metaLayoutLevel(i, &x, &y);
        if ((float)mx >= x && (float)mx <= x + LV_CARD_W &&
            (float)my >= y && (float)my <= y + LV_CARD_H) return i;
    }
    return -1;
}

static int metaHitTalent(int mx, int my)
{
    int i;
    for (i = 0; i < TAL_COUNT; i++) {
        float x, y;
        metaLayoutTalent(i, &x, &y);
        if ((float)mx >= x && (float)mx <= x + TAL_NODE_W &&
            (float)my >= y && (float)my <= y + TAL_NODE_H) return i;
    }
    return -1;
}

static void drawMetaShapes(HDC dc)
{
    int i;
    int yy;
    /* 元界面先完整覆盖游戏世界，避免草坪 / 卡槽 / 僵尸透出；再叠扫描线 */
    fillRect(dc, 0, 0, VIEW_W, VIEW_H, RGB(16, 26, 20));
    /* 暗幕（隔行扫描线，和抽卡界面保持一致的观感） */
    for (yy = 0; yy < VIEW_H; yy += 2)
        fillRect(dc, 0, yy, VIEW_W, yy + 1, RGB(16, 26, 20));

    if (gState == ST_LEVELS) {
        for (i = 0; i < LV_COUNT; i++) {
            float x, y;
            int ok = levelPlayable(i);
            int done = gSave.lvStars[i];
            int hov = (mMouseX >= (int)x && 0);
            metaLayoutLevel(i, &x, &y);
            hov = ((float)mMouseX >= x && (float)mMouseX <= x + LV_CARD_W &&
                   (float)mMouseY >= y && (float)mMouseY <= y + LV_CARD_H);
            fillRound(dc, x, y, x + LV_CARD_W, y + LV_CARD_H, 12,
                      ok ? (hov ? RGB(66, 96, 58) : RGB(46, 70, 42)) : RGB(38, 40, 38));
            strokeRound(dc, x, y, x + LV_CARD_W, y + LV_CARD_H, 12,
                        ok ? RGB(150, 210, 130) : RGB(80, 84, 80), hov ? 4 : 2);
            /* 星级 */
            { int s; for (s = 0; s < 3; s++)
                fillCircle(dc, x + LV_CARD_W - 26.0f - (float)s * 22.0f, y + 22.0f,
                           7.0f, s < done ? RGB(255, 208, 72) : RGB(74, 78, 74)); }
            if (!ok)   /* 锁 */
                fillRound(dc, x + LV_CARD_W - 44.0f, y + LV_CARD_H - 36.0f,
                          x + LV_CARD_W - 22.0f, y + LV_CARD_H - 20.0f, 3, RGB(150, 150, 150));
        }
    } else if (gState == ST_TALENTS) {
        for (i = 0; i < TAL_COUNT; i++) {
            float x, y;
            int owned = gSave.talents[i];
            int can = (!owned && gSave.stars >= talCost[i]);
            int tier = i % 4;
            int blocked = (tier > 0 && !gSave.talents[i - 1]);   /* 必须按顺序点 */
            metaLayoutTalent(i, &x, &y);
            {
                COLORREF bg = owned ? RGB(58, 104, 52)
                            : (can && !blocked) ? RGB(56, 68, 48) : RGB(42, 44, 42);
                fillRound(dc, x, y, x + TAL_NODE_W, y + TAL_NODE_H, 10, bg);
                strokeRound(dc, x, y, x + TAL_NODE_W, y + TAL_NODE_H, 10,
                            owned ? RGB(170, 236, 140)
                                  : (can && !blocked ? RGB(150, 200, 120) : RGB(78, 80, 78)), 2);
            }
        }
    } else if (gState == ST_ACH) {
        for (i = 0; i < ACH_COUNT; i++) {
            float x = 52.0f + (float)(i % 2) * 468.0f;
            float y = 108.0f + (float)(i / 2) * 50.0f;
            int got = gSave.ach[i];
            fillRound(dc, x, y, x + 448.0f, y + 42.0f, 8,
                      got ? RGB(52, 88, 48) : RGB(42, 44, 42));
            strokeRound(dc, x, y, x + 448.0f, y + 42.0f, 8,
                        got ? RGB(170, 236, 140) : RGB(78, 80, 78), 2);
            fillCircle(dc, x + 22.0f, y + 21.0f, 11.0f, got ? RGB(255, 208, 72) : RGB(64, 66, 64));
        }
    } else if (gState == ST_GACHA) {
        float bx = VIEW_W * 0.5f - 150.0f;
        int can = (gSave.coins >= 1);
        fillRound(dc, bx, GACHA_BTN_Y, bx + 300.0f, GACHA_BTN_Y + 64.0f, 14,
                  can ? RGB(92, 66, 130) : RGB(56, 54, 58));
        strokeRound(dc, bx, GACHA_BTN_Y, bx + 300.0f, GACHA_BTN_Y + 64.0f, 14,
                    can ? RGB(212, 170, 255) : RGB(90, 88, 92), 3);
        /* 三个稀有度示意球 */
        { int k; for (k = 0; k < 3; k++)
            fillCircle(dc, VIEW_W * 0.5f - 120.0f + (float)k * 120.0f, 186.0f, 34.0f,
                       k == 0 ? RGB(158, 156, 146)
                              : (k == 1 ? RGB(72, 140, 226) : RGB(228, 174, 50))); }
    } else if (gState == ST_LOADOUT) {
        for (i = 0; i < PT_HERO_FLAME; i++) {
            float x, y;
            int own = gSave.plantOwned[i];
            int sel = gSave.loadout[i];
            int hov = (loHit(mMouseX, mMouseY) == i);
            loLayout(i, &x, &y);
            fillRound(dc, x, y, x + LO_CARD_W, y + LO_CARD_H, 12,
                      !own ? RGB(44, 44, 42)
                           : (sel ? (hov ? RGB(92, 132, 72) : RGB(70, 104, 58))
                                  : (hov ? RGB(62, 68, 60) : RGB(48, 54, 48))));
            strokeRound(dc, x, y, x + LO_CARD_W, y + LO_CARD_H, 12,
                        !own ? RGB(72, 72, 70)
                             : (sel ? RGB(190, 244, 150) : RGB(96, 104, 96)),
                        sel ? 5 : 2);
            if (own && sel)
                fillCircle(dc, x + LO_CARD_W - 20.0f, y + 20.0f, 13.0f, RGB(150, 220, 110));
        }
    } else if (gState == ST_REWARD) {
        for (i = 0; i < 3; i++) {
            float x, y;
            int flipped = (gRewardChosen == i);
            int dim = (gRewardChosen >= 0 && !flipped);
            rwLayout(i, &x, &y);
            if (!flipped) {
                if (gSprCardBack.ok)
                    spriteBlit(dc, &gSprCardBack, x + RW_CARD_W * 0.5f, y + RW_CARD_H,
                               1.0f, 1.0f, dim ? 88 : 255);
                else
                    fillRound(dc, x, y, x + RW_CARD_W, y + RW_CARD_H, 14,
                              dim ? RGB(40, 56, 40) : RGB(56, 82, 54));
            } else {
                if (gSprBurst.ok)
                    spriteBlit(dc, &gSprBurst, x + RW_CARD_W * 0.5f,
                               y + RW_CARD_H * 0.5f + 46.0f, 0.80f, 0.80f, -1);
                fillRound(dc, x, y, x + RW_CARD_W, y + RW_CARD_H, 14, RGB(250, 244, 226));
                strokeRound(dc, x, y, x + RW_CARD_W, y + RW_CARD_H, 14, RGB(240, 196, 90), 5);
            }
        }
    } else if (gState == ST_DAILY) {
        /* 每日营地：签到面板 / 每日挑战面板 / 纪录墙 */
        int signedIn = (gSave.lastCheckIn == todayYMD());
        int dailyDone = (gSave.dailyDoneDate == todayYMD());
        /* 签到面板（金色） */
        fillRound(dc, 370.0f, 280.0f, 630.0f, 370.0f, 14,
                  signedIn ? RGB(70, 92, 60) : RGB(118, 100, 50));
        strokeRound(dc, 370.0f, 280.0f, 630.0f, 370.0f, 14,
                    signedIn ? RGB(150, 210, 130) : RGB(255, 226, 130), 3);
        /* 每日挑战面板（蓝色） */
        fillRound(dc, 370.0f, 400.0f, 630.0f, 500.0f, 14,
                  dailyDone ? RGB(60, 86, 104) : RGB(52, 84, 118));
        strokeRound(dc, 370.0f, 400.0f, 630.0f, 500.0f, 14,
                    dailyDone ? RGB(140, 200, 230) : RGB(150, 200, 255), 3);
        /* 纪录墙（深色面板） */
        fillRound(dc, 120.0f, 530.0f, 880.0f, 640.0f, 14, RGB(40, 52, 44));
        strokeRound(dc, 120.0f, 530.0f, 880.0f, 640.0f, 14, RGB(120, 160, 130), 2);
    }
}

static void drawMetaText(HDC dc)
{
    wchar_t buf[256];
    int i, n;
    putTextCS(dc, gF30, RGB(255, 246, 200), RGB(18, 32, 20),
              gState == ST_LEVELS ? L"选择关卡" :
              gState == ST_TALENTS ? L"天赋树"   :
              gState == ST_ACH     ? L"成就"     :
              gState == ST_DAILY   ? L"每日营地" : L"植物抽卡",
              VIEW_W * 0.5f, META_TITLE_Y);
    wsprintfW(buf, L"星星 %d　金币 %d", gSave.stars, gSave.coins);
    putTextCS(dc, gF18, RGB(255, 226, 150), RGB(18, 32, 20), buf, VIEW_W * 0.5f, 90.0f);

    if (gState == ST_LEVELS) {
        for (i = 0; i < LV_COUNT; i++) {
            float x, y;
            int ok = levelPlayable(i);
            metaLayoutLevel(i, &x, &y);
            putTextS(dc, gF18, ok ? RGB(240, 255, 230) : RGB(140, 140, 138),
                     RGB(18, 32, 20), levelDefs[i].name, x + 16.0f, y + 26.0f, 0);
            putTextS(dc, gF15, ok ? RGB(190, 220, 180) : RGB(120, 120, 118),
                     RGB(18, 32, 20), levelDefs[i].desc, x + 16.0f, y + 50.0f, 0);
            wsprintfW(buf, L"%d 波", levelDefs[i].waves > 900 ? L"∞" : L"", 0);
            wsprintfW(buf, levelDefs[i].waves > 900 ? L"无尽" : L"%d 波",
                      levelDefs[i].waves);
            putTextS(dc, gF15, RGB(220, 210, 160), RGB(18, 32, 20), buf, x + 16.0f, y + 74.0f, 0);
            if (!ok) {
                wsprintfW(buf, L"需要 %d 星解锁", levelDefs[i].starReq);
                putTextS(dc, gF15, RGB(255, 180, 140), RGB(18, 32, 20), buf,
                         x + LV_CARD_W - 16.0f, y + 74.0f, 2);
            }
        }
    } else if (gState == ST_TALENTS) {
        for (i = 0; i < TAL_COUNT; i++) {
            float x, y;
            int owned = gSave.talents[i];
            int tier = i % 4;
            int blocked = (tier > 0 && !gSave.talents[i - 1]);
            metaLayoutTalent(i, &x, &y);
            putTextS(dc, gF15, owned ? RGB(200, 255, 180) : RGB(230, 230, 220),
                     RGB(18, 32, 20), talName[i], x + 10.0f, y + 20.0f, 0);
            putTextWrap(dc, gF15, owned ? RGB(170, 230, 150) : RGB(196, 196, 188),
                        talDesc[i], x + 10.0f, y + 40.0f, TAL_NODE_W - 20.0f, 17.0f, 2);
            if (!owned) {
                wsprintfW(buf, L"%d 星", talCost[i]);
                putTextS(dc, gF15,
                         (!blocked && gSave.stars >= talCost[i]) ? RGB(255, 216, 120)
                                                                 : RGB(180, 110, 110),
                         RGB(18, 32, 20), buf, x + 10.0f, y + 70.0f, 0);
            } else {
                putTextS(dc, gF15, RGB(150, 220, 130), RGB(18, 32, 20),
                         L"已点亮", x + 10.0f, y + 70.0f, 0);
            }
        }
    } else if (gState == ST_ACH) {
        n = 0;
        for (i = 0; i < ACH_COUNT; i++) {
            float x = 52.0f + (float)(i % 2) * 468.0f;
            float y = 108.0f + (float)(i / 2) * 50.0f;
            if (gSave.ach[i]) n++;
            putTextS(dc, gF15, gSave.ach[i] ? RGB(220, 255, 200) : RGB(180, 180, 174),
                     RGB(18, 32, 20), achName[i], x + 42.0f, y + 15.0f, 0);
            putTextS(dc, gF15, gSave.ach[i] ? RGB(170, 220, 150) : RGB(140, 140, 134),
                     RGB(18, 32, 20), achDesc[i], x + 42.0f, y + 32.0f, 0);
        }
        wsprintfW(buf, L"已完成 %d / %d", n, ACH_COUNT);
        putTextS(dc, gF15, RGB(255, 226, 150), RGB(18, 32, 20), buf, 52.0f, 636.0f, 0);
    } else if (gState == ST_LOADOUT) {
        putTextCS(dc, gF18, RGB(220, 240, 210), RGB(18, 32, 20),
                  L"点一下切换候选　（准备 5 株，开局携带其中 4 株）", VIEW_W * 0.5f, 92.0f);
        {
            int sel = 0;
            for (i = 0; i < PT_COUNT; i++) if (gSave.loadout[i]) sel++;
            wsprintfW(buf, L"已选候选 %d / %d　实战携带 %d 株", sel, gCandidateSlots, gCardSlots);
            putTextCS(dc, gF22, sel >= gCandidateSlots ? RGB(180, 255, 150) : RGB(255, 226, 150),
                      RGB(18, 32, 20), buf, VIEW_W * 0.5f, 640.0f);
        }
        for (i = 0; i < PT_HERO_FLAME; i++) {
            float x, y;
            int own = gSave.plantOwned[i];
            loLayout(i, &x, &y);
            putTextS(dc, gF15, own ? RGB(240, 255, 230) : RGB(150, 150, 148),
                     RGB(18, 32, 20), plantDefs[i].name, x + LO_CARD_W * 0.5f, y + 92.0f, 1);
            if (!own)
                putTextS(dc, gF15, RGB(180, 180, 176), RGB(18, 32, 20),
                         L"未解锁", x + LO_CARD_W * 0.5f, y + 108.0f, 1);
        }
    } else if (gState == ST_REWARD) {
        int allOwned = (gRewardPick[0] < 0);
        putTextCS(dc, gF30, RGB(255, 246, 200), RGB(18, 32, 20),
                  gState == ST_REWARD && gRewardChosen < 0 ? L"通关奖励 · 选一张翻开"
                                                           : L"通关奖励",
                  VIEW_W * 0.5f, 62.0f);
        if (allOwned) {
            wsprintfW(buf, L"植物已全部解锁，改为奖励 %d 金币", gRewardCoins);
            putTextCS(dc, gF22, RGB(255, 226, 150), RGB(18, 32, 20), buf,
                      VIEW_W * 0.5f, 330.0f);
            putTextCS(dc, gF18, RGB(200, 220, 200), RGB(18, 32, 20),
                      L"点击任意处继续", VIEW_W * 0.5f, 520.0f);
        } else if (gRewardChosen < 0) {
            putTextCS(dc, gF18, RGB(210, 230, 210), RGB(18, 32, 20),
                      L"三张盲盒，翻开一张解锁一株新植物", VIEW_W * 0.5f, 118.0f);
            for (i = 0; i < 3; i++) {
                float x, y;
                rwLayout(i, &x, &y);
                putTextS(dc, gF22, RGB(255, 244, 200), RGB(40, 30, 10),
                         L"?", x + RW_CARD_W * 0.5f, y + RW_CARD_H * 0.5f, 1);
            }
        } else {
            int pid = gRewardPick[gRewardChosen];
            if (pid >= 0) {
                putTextCS(dc, gF22, RGB(80, 66, 40), RGB(255, 255, 255),
                          L"解锁新植物！", VIEW_W * 0.5f, RW_Y + RW_CARD_H + 34.0f);
                putTextCS(dc, gF30, RGB(52, 40, 22), RGB(255, 255, 255),
                          plantDefs[pid].name, VIEW_W * 0.5f, RW_Y + RW_CARD_H + 74.0f);
                wsprintfW(buf, L"%s　已加入出战编组",
                          plantDefs[pid].rarity == 0 ? L"普通" :
                          (plantDefs[pid].rarity == 1 ? L"稀有" : L"传说"));
                putTextCS(dc, gF18, RGB(120, 100, 60), RGB(255, 255, 255), buf,
                          VIEW_W * 0.5f, RW_Y + RW_CARD_H + 108.0f);
            }
            putTextCS(dc, gF18, RGB(200, 220, 200), RGB(18, 32, 20),
                      L"点击任意处继续", VIEW_W * 0.5f, 620.0f);
        }
    } else if (gState == ST_GACHA) {
        putTextCS(dc, gF18, RGB(220, 200, 250), RGB(18, 32, 20),
                  L"消耗 1 金币，随机解锁一株植物", VIEW_W * 0.5f, 250.0f);
        putTextCS(dc, gF22, RGB(255, 255, 255), RGB(40, 24, 60),
                  (gSave.coins >= 1) ? L"抽 一 次" : L"金币不足", VIEW_W * 0.5f,
                  GACHA_BTN_Y + 32.0f);
        {
            int owned = 0;
            for (i = 0; i < PT_COUNT; i++) if (gSave.plantOwned[i]) owned++;
            wsprintfW(buf, L"已解锁植物 %d / %d", owned, PT_COUNT);
            putTextCS(dc, gF18, RGB(200, 220, 250), RGB(18, 32, 20), buf, VIEW_W * 0.5f, 400.0f);
            putTextCS(dc, gF15, RGB(150, 150, 160), RGB(18, 32, 20),
                      L"普通 60%　稀有 32%　传说 8%", VIEW_W * 0.5f, 430.0f);
        }
    } else if (gState == ST_DAILY) {
        /* 星期主题（今天打什么规则，先摆出来） */
        if (gWeekDesc[0])
            putTextCS(dc, gF15, RGB(255, 226, 150), RGB(18, 32, 20),
                      gWeekDesc, VIEW_W * 0.5f, 252.0f);
        /* 签到面板 / 每日挑战面板 / 纪录墙 */
        {
            int signedIn = (gSave.lastCheckIn == todayYMD());
            int dailyDone = (gSave.dailyDoneDate == todayYMD());
            int dailySeed = todayYMD() * 31 + 17;
            int target = 20 + (dailySeed / 17) % 15;
            /* 签到面板 */
            putTextCS(dc, gF22, RGB(255, 244, 210), RGB(20, 32, 18),
                      L"每日签到", 500.0f, 302.0f);
            if (signedIn) {
                wsprintfW(buf, L"已签到 · 连续 %d 天", gSave.checkStreak);
                putTextCS(dc, gF18, RGB(200, 255, 180), RGB(20, 32, 18), buf, 500.0f, 332.0f);
            } else {
                if (gSave.lastCheckIn == ymdDaysAgo(1) && gSave.checkStreak + 1 >= 7)
                    putTextCS(dc, gF18, RGB(255, 226, 150), RGB(20, 32, 18),
                              L"点此签到 · 明天就满 7 连大奖！", 500.0f, 332.0f);
                else
                    putTextCS(dc, gF18, RGB(255, 226, 150), RGB(20, 32, 18),
                              L"点此签到 · 星星 +1 金币 +2 经验 +15", 500.0f, 332.0f);
                wsprintfW(buf, L"连续 7 天额外 +3 星 +5 金币 · 当前 %d 天",
                          (gSave.lastCheckIn == ymdDaysAgo(1)) ? gSave.checkStreak : 0);
                putTextCS(dc, gF15, RGB(220, 200, 160), RGB(20, 32, 18), buf, 500.0f, 352.0f);
            }
            /* 每日挑战面板 */
            putTextCS(dc, gF22, RGB(240, 248, 255), RGB(18, 30, 42),
                      L"每日挑战 · 撑过 N 波", 500.0f, 420.0f);
            wsprintfW(buf, L"今日目标：无尽关撑过 %d 波", target);
            putTextCS(dc, gF18, RGB(230, 240, 255), RGB(18, 30, 42), buf, 500.0f, 448.0f);
            if ((dailySeed / 13) % 2)
                wsprintfW(buf, L"僵尸生命 x%.2f · 速度 x%.2f · 阳光 稀少",
                          1.0f + 0.15f * (float)((dailySeed / 7)  % 3),
                          1.0f + 0.05f * (float)((dailySeed / 11) % 3));
            else
                wsprintfW(buf, L"僵尸生命 x%.2f · 速度 x%.2f · 阳光 正常",
                          1.0f + 0.15f * (float)((dailySeed / 7)  % 3),
                          1.0f + 0.05f * (float)((dailySeed / 11) % 3));
            putTextCS(dc, gF15, RGB(190, 210, 235), RGB(18, 30, 42), buf, 500.0f, 472.0f);
            putTextCS(dc, gF15,
                      dailyDone ? RGB(180, 240, 180) : RGB(255, 226, 150),
                      RGB(18, 30, 42),
                      dailyDone ? L"今日已完成 ✓（奖励已领）" : L"完成奖励：星星 +3 金币 +5",
                      500.0f, 486.0f);
            /* 纪录墙 */
            putTextCS(dc, gF18, RGB(255, 226, 150), RGB(18, 32, 20),
                      L"个 人 纪 录", 500.0f, 546.0f);
            wsprintfW(buf, L"最高关卡 第%d关 · 单局最高击杀 %d · 单局最高金气 %d",
                      gSave.bestWave + 1, gSave.bestKills, gSave.bestGold);
            putTextCS(dc, gF18, RGB(220, 240, 220), RGB(18, 32, 20), buf, 500.0f, 576.0f);
            wsprintfW(buf, L"最快通关 %d:%02d · 每日挑战最高 %d 波 · 连胜 %d 天",
                      gSave.bestTimeSec / 60, gSave.bestTimeSec % 60,
                      gSave.dailyBestWave, gSave.checkStreak);
            putTextCS(dc, gF18, RGB(220, 240, 220), RGB(18, 32, 20), buf, 500.0f, 604.0f);
            if (gRecNew) {
                putTextCS(dc, gF15, RGB(255, 180, 120), RGB(18, 32, 20),
                          L"★ 新纪录！", 780.0f, 546.0f);
            }
        }
    }
    putTextCS(dc, gF15, RGB(180, 200, 180), RGB(18, 32, 20),
              L"右键 / Esc 返回", VIEW_W * 0.5f, 620.0f);
}

/* ---- 大厅：卡组编组（12 株植物里选 8 张带上场） ---- */
static void loLayout(int i, float *x, float *y)
{
    *x = LO_X0 + (float)(i % LO_COLS) * (LO_CARD_W + LO_GAP_X);
    *y = LO_Y0 + (float)(i / LO_COLS) * (LO_CARD_H + LO_GAP_Y);
}
/* 编组界面只列普通植物：神级植物是每局随机产物，不属于可编组的池子 */
static int loHit(int mx, int my)
{
    int i;
    for (i = 0; i < PT_HERO_FLAME; i++) {
        float x, y;
        loLayout(i, &x, &y);
        if ((float)mx >= x && (float)mx <= x + LO_CARD_W &&
            (float)my >= y && (float)my <= y + LO_CARD_H) return i;
    }
    return -1;
}

/* ---- 每局结算抽卡：三张盲盒选一张翻开 ---- */
static void rwLayout(int i, float *x, float *y)
{
    float total = 3 * RW_CARD_W + 2 * RW_GAP;
    *x = (VIEW_W - total) * 0.5f + (float)i * (RW_CARD_W + RW_GAP);
    *y = RW_Y;
}
static int rwHit(int mx, int my)
{
    int i;
    for (i = 0; i < 3; i++) {
        float x, y;
        rwLayout(i, &x, &y);
        if ((float)mx >= x && (float)mx <= x + RW_CARD_W &&
            (float)my >= y && (float)my <= y + RW_CARD_H) return i;
    }
    return -1;
}

/* 开局准备：三个盲盒里各放一株"还没解锁的植物"；如果全解锁了就给金币 */
static void rewardPrepare(void)
{
    int pool[PT_COUNT], n = 0, i, j;
    gRewardChosen = -1;
    gRewardT = 0.0f;
    gRewardCoins = 0;
    /* 只从普通植物里抽：神级植物不能被永久解锁 */
    for (i = 0; i < PT_HERO_FLAME; i++) if (!gSave.plantOwned[i]) pool[n++] = i;
    if (n == 0) {                       /* 植物全解锁：补偿金币 */
        gRewardCoins = 2 + gSave.lvStars[gCurLevel];
        gSave.coins += gRewardCoins;
        for (i = 0; i < 3; i++) gRewardPick[i] = -1;
        saveFlush();
        return;
    }
    for (i = 0; i < 3; i++) {
        /* 尽量不重复，但池子不够时允许重复 */
        int c = pool[rand() % n];
        for (j = 0; j < i; j++) if (gRewardPick[j] == c && n > i) { c = pool[(rand() + 1) % n]; }
        gRewardPick[i] = c;
    }
}

static void rewardChoose(int idx)
{
    if (gRewardChosen >= 0) return;      /* 只能翻一次 */
    gRewardChosen = idx;
    gRewardT = 0.0f;
    if (gRewardPick[idx] >= 0) {
        gSave.plantOwned[gRewardPick[idx]] = 1;
        gSave.loadout[gRewardPick[idx]] = 1;     /* 新植物默认加入编组 */
        { int i, owned = 0; for (i = 0; i < PT_COUNT; i++) if (gSave.plantOwned[i]) owned++;
          if (owned >= PT_COUNT) achUnlock(5); }
        loadoutBuild();
    }
    saveFlush();
}

/* 抽卡：抽一株还没解锁的植物 */
static void doGacha(void)
{
    int pool[PT_COUNT], n = 0, i, target, roll, picked = -1, guard;
    if (gSave.coins < 1) return;
    for (i = 0; i < PT_HERO_FLAME; i++) if (!gSave.plantOwned[i]) pool[n++] = i;
    if (n == 0) return;                       /* 全解锁了 */
    gSave.coins--;
    gSave.gachaCount++;
    if (gSave.gachaCount >= 10) achUnlock(19);
    roll = rand() % 100;
    target = (roll < 60) ? 0 : (roll < 92 ? 1 : 2);
    for (guard = 0; guard < 200; guard++) {
        int c = pool[rand() % n];
        if (plantDefs[c].rarity == target) { picked = c; break; }
    }
    if (picked < 0) picked = pool[rand() % n];
    gSave.plantOwned[picked] = 1;
    { int owned = 0; for (i = 0; i < PT_COUNT; i++) if (gSave.plantOwned[i]) owned++;
      if (owned >= PT_COUNT) achUnlock(5); }
    saveFlush();
}

/* ======================================================================== */
/*                                   渲染                                    */
/* ======================================================================== */
#define BAR_X 178.0f
#define BAR_Y 620.0f
#define BAR_W 644.0f
#define BAR_H 20.0f

/* 危险预警：屏幕四边红色渐变光晕（用递减密度的扫描线模拟透明度） */
static void drawDangerFlash(HDC dc, float k)
{
    int i, n = 3 + (int)(5.0f * CLAMP(k, 0, 1.0f));
    float base = 3.0f;
    for (i = 0; i < n; i++) {
        int band = 6 + i * 4;
        int step = 2 + i;
        int j;
        COLORREF c = tint(RGB(198, 36, 30), -0.12f + i * 0.045f);
        for (j = 0; j < band; j += step) {
            fillRect(dc, 0, base + j, VIEW_W, base + j + 1, c);
            fillRect(dc, 0, VIEW_H - base - j - 1, VIEW_W, VIEW_H - base - j, c);
            fillRect(dc, base + j, 0, base + j + 1, VIEW_H, c);
            fillRect(dc, VIEW_W - base - j - 1, 0, VIEW_W - base - j, VIEW_H, c);
        }
        base += band;
    }
}

static void drawProgressBar(HDC dc){
    int   mw = levelDefs[gCurLevel].waves;
    float pr = (mw > 900) ? (float)(gWave % 20) / 20.0f : (float)gWave / (float)mw;
    int i;
    strokeRound(dc, BAR_X - 3, BAR_Y - 3, BAR_X + BAR_W + 3, BAR_Y + BAR_H + 3, 6, HUD_LT, 3);
    fillRound(dc, BAR_X, BAR_Y, BAR_X + BAR_W, BAR_Y + BAR_H, 5, RGB(52, 40, 30));
    if (pr > 0.003f)
        fillRound(dc, BAR_X + 2, BAR_Y + 2, BAR_X + 2 + (BAR_W - 4) * pr, BAR_Y + BAR_H - 2,
                  3, RGB(118, 194, 76));
    for (i = 0; i < mw; i++) {
        if (i == 9 || i == MAX_WAVES - 1) {
            float x = BAR_X + BAR_W * (float)(i + 1) / (float)mw;
            POINT p[3];
            line(dc, x, BAR_Y + 1, x, BAR_Y + BAR_H - 1, RGB(40, 30, 24), 2);
            p[0].x = (int)x;             p[0].y = (int)(BAR_Y - 13);
            p[1].x = (int)(x + 17);      p[1].y = (int)(BAR_Y - 8);
            p[2].x = (int)x;             p[2].y = (int)(BAR_Y - 3);
            poly(dc, p, 3, RGB(200, 46, 42), 1, RGB(120, 26, 24), 2);
        }
    }
    if (pr > 0.001f) {
        float x = BAR_X + BAR_W * pr, y = BAR_Y + BAR_H * 0.5f;
        fillCircle(dc, x, y, 11, RGB(146, 176, 120));
        fillCircle(dc, x + 2, y - 1, 7, RGB(176, 202, 150));
        fillCircle(dc, x - 1, y - 2, 2.4f, RGB(30, 26, 26));
        fillCircle(dc, x + 5, y - 2, 2.2f, RGB(30, 26, 26));
    }
}

static void drawMowers(HDC dc)
{
    int r;
    for (r = 0; r < ROWS; r++) {
        Mower *m = &mowers[r];
        float y;
        if (!m->active) continue;
        y = cellBaseY(r) + 2;
        fillEllipse(dc, m->x, y, 22, 7, RGB(46, 92, 40));
        fillRound(dc, m->x - 17, y - 22, m->x + 17, y - 4, 6, RGB(198, 62, 52));
        fillRound(dc, m->x - 14, y - 20, m->x + 14, y - 13, 4, RGB(234, 104, 88));
        fillRound(dc, m->x - 20, y - 30, m->x - 6, y - 6, 4, RGB(160, 48, 42));
        fillCircle(dc, m->x - 10, y - 4, 7, RGB(48, 48, 56));
        fillCircle(dc, m->x + 9, y - 4, 7, RGB(48, 48, 56));
        fillCircle(dc, m->x - 10, y - 4, 3, RGB(150, 150, 160));
        fillCircle(dc, m->x + 9, y - 4, 3, RGB(150, 150, 160));
        line(dc, m->x + 14, y - 26, m->x + 26, y - 36, RGB(130, 130, 138), 4);
    }
}

static void render(HDC out)
{
    XFORM id, xf;
    wchar_t buf[192];
    int i, r, c;

    /* ================= 世界层（2x 超采样） ================= */
    id.eM11 = 1; id.eM12 = 0; id.eM21 = 0; id.eM22 = 1; id.eDx = 0; id.eDy = 0;
    SetWorldTransform(gWorldDC, &id);
    fillRect(gWorldDC, 0, 0, (float)(VIEW_W * SS), (float)(VIEW_H * SS), RGB(22, 34, 26));

    xf = gScaleXF;
    if (shakeT > 0) {
        xf.eDx = cos(gTime * 62.0f) * shakeMag * (float)SS;
        xf.eDy = sin(gTime * 79.0f) * shakeMag * (float)SS;
    }
    gCurXF = xf;                      /* 精灵绘制要先复位变换再做 AlphaBlend */
    SetWorldTransform(gWorldDC, &xf);

    drawBackground(gWorldDC);
    if (gState == ST_PLAY || gState == ST_PAUSE) drawGridHighlight(gWorldDC);
    drawMowers(gWorldDC);

    /* 植物 */
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
        Plant *p = &grid[r][c];
        float ex;
        if (!p->alive) continue;
        if (p->row != r || p->col != c) continue;   /* 多格植物只在锚点格画一次 */
        ex = sin(p->phase * 22.0f) * p->eaten * 2.4f + p->recoil * 3.0f;
        drawPlantEntity(gWorldDC, p, ex);
        /* 土豆雷激活后的闪烁指示灯（资源是未激活状态，这里叠加） */
        if (p->type == PT_POTATOMINE && p->armed) {
            float bl = 0.55f + 0.45f * sin(p->phase * 7.0f);
            float ly = cellBaseY(r) - 50.0f;
            fillCircle(gWorldDC, cellCX(c), ly, 5.0f * bl, RGB(198, 48, 36));
            fillCircle(gWorldDC, cellCX(c), ly, 2.4f * bl, RGB(255, 226, 170));
        }
    }

    /* 僵尸（按行从远到近） */
    for (r = 0; r < ROWS; r++)
        for (i = 0; i < MAX_ZOMBIES; i++)
            if (zombies[i].active && zombies[i].row == r) drawZombie(gWorldDC, &zombies[i]);

    /* 豌豆 */
    for (i = 0; i < MAX_PEAS; i++) {
        Pea *pe = &peas[i];
        COLORREF c, d;
        if (!pe->active) continue;
        {
            Sprite *bs = bulletSpriteOf(pe);
            COLORREF gc = WPN_COL[pe->wpn];
            if (pe->src == PT_SNOWPEA) gc = RGB(150, 226, 250);
            if (pe->src == PT_HERO_FLAME)   gc = RGB(255, 140, 50);
            if (pe->src == PT_HERO_CRYSTAL) gc = RGB(180, 120, 250);
            if (pe->src == PT_HERO_FROST)   gc = RGB(160, 235, 255);
            if (pe->src == PT_HERO_DOOM)    gc = RGB(210, 90, 230);
            if (pe->src == PT_KERNELPULT)   gc = RGB(250, 214, 90);
            if (pe->src == PT_STARFRUIT)    gc = RGB(255, 214, 70);
            if (pe->src == PT_CACTUS)       gc = RGB(170, 220, 130);
            if (pe->src == PT_SPLITPEA)     gc = RGB(190, 220, 80);
            if (pe->src == PT_REED)         gc = RGB(90, 230, 255);
            if (pe->src == PT_BLOOMERANG)   gc = RGB(255, 140, 90);
            if (pe->src == PT_FUME)         gc = RGB(170, 90, 210);
            if (pe->src == PT_LASERBEAN)    gc = RGB(255, 70, 80);
            if (pe->src == PT_MELONPULT)    gc = RGB(70, 150, 70);
            if (pe->src == PT_WINTERMELON)  gc = RGB(170, 230, 240);
            if (pe->src == PT_GLOOM)        gc = RGB(120, 60, 160);
            if (pe->src == PT_COBCANNON)    gc = RGB(240, 200, 70);
            if (pe->src == PT_CATTAIL)      gc = RGB(160, 110, 60);
            /* 速度拖尾：越快的弹拖得越长，读得出"在飞" */
            fillEllipse(gWorldDC, pe->x - 7.0f, pe->y, 11.0f, 5.0f, tint(gc, -0.25f));
            fillEllipse(gWorldDC, pe->x - 3.5f, pe->y, 7.5f, 3.6f, tint(gc, 0.20f));
            if (bs->ok) {
                float sc = 0.55f;
                if (pe->dmg > 200.0f) sc = 0.85f;          /* 末日弹更大 */
                else if (pe->wpn != WPN_NORMAL) sc = 0.62f;
                spriteBlit(gWorldDC, bs, pe->x, pe->y + 11.0f, sc, sc, -1);
            } else {
                /* 资源缺失时的回退：仍然是可读的圆弹 */
                c = pe->frozen ? RGB(158, 226, 246) : RGB(126, 204, 80);
                d = pe->frozen ? RGB(104, 176, 200) : RGB(74, 154, 46);
                fillCircle(gWorldDC, pe->x, pe->y, 8, d);
                fillCircle(gWorldDC, pe->x, pe->y, 7, c);
                fillCircle(gWorldDC, pe->x - 2.5f, pe->y - 2.5f, 2.4f, tint(c, 0.55f));
            }
        }
    }

    /* 粒子 */
    for (i = 0; i < MAX_PARTS; i++) {
        Particle *pt = &parts[i];
        float k;
        if (!pt->active) continue;
        k = pt->life / pt->maxlife;
        fillCircle(gWorldDC, pt->x, pt->y, pt->size * (0.3f + 0.7f * k), pt->col);
    }

    /* 雷击（全屏 + 每行 6 颗垂直雷柱） */
    if (gFuryT > 0) {
        int r, t;
        gFuryT = 0;
        for (r = 0; r < ROWS; r++) for (t = 0; t < 6; t++) {
            float cx = LAWN_X + t * (COLS * CELL_W / 5.0f) + rnd(-12, 12);
            addParticle(cx, LAWN_Y + 4, rnd(-30, 30), rnd(80, 160), 0.3f, rnd(4, 7),
                        (t & 1) ? RGB(255, 244, 200) : RGB(255, 188, 80), 1);
        }
        flashRed = 0.35f; addShake(6, 0.3f);
    }

    /* 余烬火焰 */
    drawFires(gWorldDC);

    /* 天气层：飘落的花瓣（画在僵尸之后 = 从近处飘过，但在阳光之前避免挡住可点目标） */
    drawWeather(gWorldDC);

    /* ---- 右下角状态栏：模式 / 武器 / 货币 / 愤怒条 / 充能 ----
       全部竖排在右侧，避开左侧的遗物图标条，彼此也不再重叠。 */
    {
        float bx = 812.0f, bw = 182.0f, yy = 94.0f;
        fillRound(gFrameDC, bx, yy, bx + bw, yy + 24, 7, MODE_COL[gCurMode]);
        wsprintfW(buf, L"%s模式  [M]", MODE_NAME[gCurMode]);
        putTextS(gFrameDC, gF15, RGB(255, 255, 255), tint(MODE_COL[gCurMode], -0.45f),
                 buf, bx + 12, yy + 12, 0);
        for (i = 0; i < MODE_COUNT; i++) {
            float lx = bx + bw - 12.0f - (float)(MODE_COUNT - i) * 13.0f;
            fillRound(gFrameDC, lx, yy + 7, lx + 10, yy + 17, 3,
                      gUnlockedMode[i] ? MODE_COL[i] : RGB(72, 78, 72));
        }
        yy += 30.0f;
        if (gHasSubWpn) {
            fillRound(gFrameDC, bx, yy, bx + bw, yy + 24, 7, WPN_COL[gSubWpn]);
            wsprintfW(buf, L"主 %s 副 %s", WPN_NAME[gMainWpn], WPN_NAME[gSubWpn]);
            putTextS(gFrameDC, gF15, RGB(255, 255, 255), tint(WPN_COL[gSubWpn], -0.5f),
                     buf, bx + 12, yy + 12, 0);
            yy += 30.0f;
        }
        if (gBanker) {
            if (gSprGold.ok)
                spriteBlit(gFrameDC, &gSprGold, bx + 14.0f, yy + 22.0f, 0.44f, 0.44f, -1);
            else
                fillRound(gFrameDC, bx + 3, yy + 3, bx + 25, yy + 25, 5, RGB(204, 156, 64));
            wsprintfW(buf, L"%d   按 G 召唤南瓜（10）", gGold);
            putTextS(gFrameDC, gF15, RGB(255, 244, 214), RGB(72, 58, 26),
                     buf, bx + 32, yy + 12, 0);
            yy += 28.0f;
        }
        if (gNecromancer) {
            if (gSprGhost.ok)
                spriteBlit(gFrameDC, &gSprGhost, bx + 14.0f, yy + 22.0f, 0.44f, 0.44f, -1);
            else
                fillRound(gFrameDC, bx + 3, yy + 3, bx + 25, yy + 25, 5, RGB(118, 174, 232));
            wsprintfW(buf, L"%d   按 H 全屏减速（20）", gGhost);
            putTextS(gFrameDC, gF15, RGB(224, 240, 252), RGB(40, 60, 82),
                     buf, bx + 32, yy + 12, 0);
            yy += 28.0f;
        }
        if (gChargeMax > 0) {
            for (i = 0; i < gChargeMax; i++)
                fillRound(gFrameDC, bx + 4.0f + (float)i * 24.0f, yy + 6.0f,
                          bx + 22.0f + (float)i * 24.0f, yy + 20.0f, 4,
                          i < gCharge ? RGB(255, 168, 56) : RGB(84, 84, 84));
            wsprintfW(buf, L"充能：下次爆炸 +60%%");
            putTextS(gFrameDC, gF15, RGB(210, 210, 200), RGB(20, 30, 20),
                     buf, bx + 82, yy + 13, 0);
            yy += 26.0f;
        }
        {
            float pct = gFury / gFuryMax;
            if (pct > 1.0f) pct = 1.0f;
            fillRound(gFrameDC, bx, yy + 4, bx + bw, yy + 20, 5, RGB(52, 40, 40));
            fillRound(gFrameDC, bx + 2, yy + 6, bx + 2 + (bw - 4) * pct, yy + 18, 4,
                      RGB(236, 108, 56));
            putTextS(gFrameDC, gF15, RGB(255, 226, 200), RGB(40, 20, 14),
                     L"愤怒条", bx + 10, yy + 12, 0);
        }
    }


    /* 已获得遗物（左下角，2 行 x 8 个） */
    if (gState == ST_PLAY || gState == ST_PAUSE) {
        int k, n = 0;
        for (k = 1; k < R_COUNT; k++) {
            if (!HAS(k)) continue;
            drawRelicIcon(gWorldDC, relicDefs[k].cat, CAT_COL[relicDefs[k].cat],
                          16.0f + (float)(n % 8) * 20.0f,
                          610.0f + (float)(n / 8) * 19.0f, 8.5f);
            n++;
        }
    }

    /* 阳光 */
    for (i = 0; i < MAX_SUNS; i++) if (suns[i].active) drawSun(gWorldDC, &suns[i]);

    /* 危险预警红晕 */
    if (flashRed > 0 && gState == ST_PLAY) drawDangerFlash(gWorldDC, flashRed);

    drawHoverTipShapes(gWorldDC);   /* 悬停提示（画在最上层） */
    drawHUDShapes(gWorldDC);
    drawProgressBar(gWorldDC);

    /* 三选一：暗幕 + 卡片。GDI 没有 alpha，用隔行扫描线把画面压暗，
       既能把背景压下去，又保留了「游戏还在后面」的临场感。 */
    if (gState == ST_DRAFT) {
        int yy;
        for (yy = 0; yy < VIEW_H; yy += 2)
            fillRect(gWorldDC, 0, yy, VIEW_W, yy + 1, RGB(14, 24, 16));
        drawDraftShapes(gWorldDC);
    }
    /* 元界面（选关 / 天赋 / 成就 / 抽卡） */
    if (gState == ST_LEVELS || gState == ST_TALENTS ||
        gState == ST_ACH || gState == ST_GACHA ||
        gState == ST_LOADOUT || gState == ST_REWARD ||
        gState == ST_DAILY) {
        drawMetaShapes(gWorldDC);
    }

    /* 菜单的底色与植物图标必须画在「世界层」——精灵绘制依赖 2x 变换，
       直接画到 1x 的文字层坐标会翻倍跑出画面。 */
    if (gState == ST_MENU) {
        float by = 330.0f;
        fillRect(gWorldDC, 0, 0, VIEW_W, VIEW_H, RGB(20, 44, 26));
        fillRect(gWorldDC, 0, 0, VIEW_W, 8, RGB(150, 104, 62));
        for (i = 0; i < gLoadoutN; i++)
            drawIcon(gWorldDC, gLoadout[i], 130 + i * 82.0f, 480, 58);
        /* 四个入口按钮 */
        { int b;
          const float bx[5] = { 40.0f, 320.0f, 470.0f, 620.0f, 770.0f };
          const float bw[5] = { 260.0f, 130.0f, 130.0f, 130.0f, 190.0f };
          for (b = 0; b < 5; b++) {
              int hov = (mMouseX >= (int)bx[b] && mMouseX <= (int)(bx[b] + bw[b]) &&
                         mMouseY >= (int)by && mMouseY <= (int)(by + 46));
              fillRound(gWorldDC, bx[b], by, bx[b] + bw[b], by + 46.0f, 12,
                        b == 0 ? (hov ? RGB(110, 170, 90) : RGB(78, 132, 66))
                               : (hov ? RGB(70, 92, 74) : RGB(50, 68, 52)));
              strokeRound(gWorldDC, bx[b], by, bx[b] + bw[b], by + 46.0f, 12,
                          b == 0 ? RGB(190, 240, 150) : RGB(130, 170, 130), 3);
          }
        }
        /* 第二排：每日营地（签到 + 每日挑战） */
        { int by2 = 396;
          int hov = (mMouseX >= 370 && mMouseX <= 630 && mMouseY >= by2 && mMouseY <= by2 + 46);
          fillRound(gWorldDC, 370.0f, (float)by2, 630.0f, (float)(by2 + 46), 12,
                    hov ? RGB(140, 120, 60) : RGB(118, 100, 50));
          strokeRound(gWorldDC, 370.0f, (float)by2, 630.0f, (float)(by2 + 46), 12,
                      RGB(255, 226, 130), 3);
        }
    }

    fillRound(gFrameDC, 820, 200, 980, 224, 7, RGB(255, 0, 255));
    putTextCS(gFrameDC, gF18, RGB(0,0,0), RGB(0,0,0), L"PRE", 900, 212);
    /* ================= 降采样到 1x ================= */    fillRound(gFrameDC, 820, 250, 980, 274, 7, RGB(0, 255, 0));
    putTextCS(gFrameDC, gF18, RGB(0,0,0), RGB(0,0,0), L"POST", 900, 262);

    SetWorldTransform(gWorldDC, &id);
    SetStretchBltMode(gFrameDC, HALFTONE);
    SetBrushOrgEx(gFrameDC, 0, 0, NULL);
    StretchBlt(gFrameDC, 0, 0, VIEW_W, VIEW_H, gWorldDC, 0, 0, VIEW_W * SS, VIEW_H * SS, SRCCOPY);

    /* ================= 文字层（1x，清晰） ================= */
    if (gState == ST_MENU) {
        float cx = VIEW_W * 0.5f;
        putTextCS(gFrameDC, gF72, RGB(255, 236, 120), RGB(20, 40, 20), L"植物大战僵尸", cx, 200);
        putTextCS(gFrameDC, gF22, RGB(190, 235, 190), RGB(16, 34, 16), L"C 语言 · Win32 GDI 复刻版", cx, 268);
        /* 菜单四个入口按钮的文字 */
        {
            const wchar_t *nm[5] = { L"开始游戏", L"编组", L"天赋", L"成就", L"抽卡" };
            const float bx[5] = { 40.0f, 320.0f, 470.0f, 620.0f, 770.0f };
            const float bw[5] = { 260.0f, 130.0f, 130.0f, 130.0f, 190.0f };
            int b;
            for (b = 0; b < 5; b++)
                putTextCS(gFrameDC, gF22, b == 0 ? RGB(255, 255, 235) : RGB(226, 240, 226),
                          RGB(20, 40, 20), nm[b], bx[b] + bw[b] * 0.5f, 353.0f);
            putTextCS(gFrameDC, gF22, RGB(255, 244, 210), RGB(20, 40, 20),
                      L"每日营地", 500.0f, 419.0f);
            /* 等级 / 经验 / 收集度 / 货币（一行，下面挂经验条） */
            wsprintfW(buf, L"Lv.%d　经验 %d/%d　星星 %d　金币 %d　植物 %d/%d",
                      gSave.level, gSave.xp, xpNeed(gSave.level),
                      gSave.stars, gSave.coins, plantOwnedCount(), PT_COUNT);
            putTextCS(gFrameDC, gF15, RGB(255, 226, 150), RGB(20, 40, 20), buf, cx, 296.0f);
            { float x0 = cx - 130.0f, w = 260.0f, f = (float)gSave.xp / (float)xpNeed(gSave.level);
              if (f > 1.0f) f = 1.0f;
              fillRound(gFrameDC, x0, 306.0f, x0 + w, 320.0f, 7, RGB(30, 48, 30));
              strokeRound(gFrameDC, x0, 306.0f, x0 + w, 320.0f, 7, RGB(150, 200, 130), 2);
              fillRound(gFrameDC, x0 + 3.0f, 309.0f, x0 + 3.0f + (w - 6.0f) * f, 317.0f, 4,
                        RGB(120, 220, 90)); }
        }
        putTextCS(gFrameDC, gF15, RGB(160, 200, 160), RGB(16, 34, 16),
                  L"左键选卡片 → 左键点草坪种植  |  右键 / Esc 取消选择  |  P 暂停  |  R 重开", cx, 596);
        putTextCS(gFrameDC, gF15, RGB(160, 200, 160), RGB(16, 34, 16),
                  L"阳光自动拾取（A 键开关） · 飘落特效（W 键开关） · 别让僵尸走到最左边", cx, 622);
    } else if (gState == ST_PLAY || gState == ST_PAUSE ||
               gState == ST_WIN || gState == ST_LOSE || gState == ST_DRAFT) {
        /* ---- HUD 文字 ---- */
        /* 阳光数字：四周描一圈深色再压主色，任何底色上都读得清 */
        wsprintfW(buf, L"%d", gSun);
        putTextS(gFrameDC, gF22, RGB(38, 24, 10), RGB(38, 24, 10), buf, 123, 29, 2);
        putTextS(gFrameDC, gF22, RGB(38, 24, 10), RGB(38, 24, 10), buf, 125, 31, 2);
        putTextS(gFrameDC, gF22, RGB(255, 252, 236), RGB(56, 38, 20), buf, 124, 30, 2);

        /* 价格只画本局编组里的卡（槽位 i → 编组第 i 株），
           间距必须和卡片绘制一致 —— 之前遍历全部 PT_COUNT 种、
           硬编码 82px，把没上场的植物价格也刷在了顶栏上 */
        {
            int   cardTotal = gLoadoutN + gBonusN;
            float cgap = cardBarGap();
            for (i = 0; i < cardTotal; i++) {
                int   ci = (i < gLoadoutN) ? gLoadout[i] : gBonusPlant[i - gLoadoutN];
                float bx = 144 + (float)i * cgap;
                int   afford = (gSun >= plantCost(ci));
                wsprintfW(buf, L"%d", plantCost(ci));
                putTextCS(gFrameDC, gF15, afford ? RGB(255, 255, 255) : RGB(255, 140, 140),
                          RGB(28, 18, 10), buf, bx + (cgap - 8.0f) * 0.5f, 8 + 76 - 12);
            }
        }

        /* 卡片悬停提示：槽位 → 编组植物（之前把槽位当植物类型索引，
           悬停空卡位会报出根本没上场的植物名和价格） */
        if (mMouseY < 92 && mMouseX >= 144) {
            float cgap = cardBarGap();
            int   total = gLoadoutN + gBonusN;
            int   slot  = (int)floor((mMouseX - 144.0f) / cgap);
            if (slot >= 0 && slot < total) {
                int ci = (slot < gLoadoutN) ? gLoadout[slot] : gBonusPlant[slot - gLoadoutN];
                wsprintfW(buf, L"%s   %d 阳光", plantDefs[ci].name, plantCost(ci));
                putTextCS(gFrameDC, gF18, RGB(255, 248, 210), RGB(20, 30, 20), buf, (float)mMouseX, 100);
            } else if (slot == total) {
                putTextCS(gFrameDC, gF18, RGB(255, 248, 210), RGB(20, 30, 20),
                          L"铲子 · 点植物铲除", (float)mMouseX, 100);
            }
        }

        /* 波次 */
        wsprintfW(buf, L"第 %d / %d 波", gWave, levelDefs[gCurLevel].waves);
        putTextCS(gFrameDC, gF15, RGB(255, 250, 220), RGB(28, 22, 14), buf,
                  BAR_X + BAR_W * 0.5f, BAR_Y + BAR_H * 0.5f);
        /* 底部状态条：草坪底边 604 到窗口底 650 只有 46px，中间还压着
           波次进度条（含描边与外发光三角，占据 x 175~825 / y 607~643）。
           所以只能用左右两侧的空档：左侧放击杀数，右侧放两个开关提示。
           原版把三行全塞在右下角（611 / 626 / 629），后两行只差 3px，
           "击杀"被两个开关提示夹住糊成一团。 */
        /* 坐标是按真实行高算出来的，不是拍脑袋：微软雅黑 15px 的
           GetTextExtentPoint32 返回 cy=20（不是 15），18px 返回 cy=25。
           条带只有 604..650，所以两行 20px 正好占 40px，居中从 607 起。 */
        wsprintfW(buf, L"击杀 %d", gKilled);
        putTextS(gFrameDC, gF18, RGB(232, 228, 202), RGB(20, 30, 20), buf, 18, 614, 0);

        wsprintfW(buf, L"自动拾取 [A] %s", gAutoSun ? L"开" : L"关");
        putTextS(gFrameDC, gF15, gAutoSun ? RGB(178, 240, 150) : RGB(206, 206, 196),
                 RGB(20, 30, 20), buf, 982, 607, 2);
        wsprintfW(buf, L"飘落特效 [W] %s", gWeatherOn ? L"开" : L"关");
        putTextS(gFrameDC, gF15, gWeatherOn ? RGB(178, 240, 150) : RGB(206, 206, 196),
                 RGB(20, 30, 20), buf, 982, 627, 2);

        /* 三选一界面文字 */
        if (gState == ST_DRAFT) drawDraftText(gFrameDC);
        drawHoverTipText(gFrameDC);   /* 悬停提示文字 */

        /* 元界面文字 */
        if (gState == ST_LEVELS || gState == ST_TALENTS ||
            gState == ST_ACH || gState == ST_GACHA ||
            gState == ST_LOADOUT || gState == ST_REWARD ||
            gState == ST_DAILY) drawMetaText(gFrameDC);

        if (gState == ST_PLAY && gWave == 0)
            putTextCS(gFrameDC, gF30, RGB(255, 244, 180), RGB(24, 46, 24),
                      L"准备迎战僵尸…", LAWN_X + COLS * CELL_W * 0.5f, LAWN_Y + 46);

        if (gState == ST_PAUSE) {
            fillRect(gFrameDC, 0, 92, VIEW_W, VIEW_H, RGB(16, 26, 18));
            putTextCS(gFrameDC, gF54, RGB(255, 236, 120), RGB(20, 40, 20), L"已暂停", VIEW_W * 0.5f, 320);
            putTextCS(gFrameDC, gF22, RGB(220, 255, 220), RGB(16, 34, 16),
                      L"按 P 或空格继续", VIEW_W * 0.5f, 390);
        }
        if (gState == ST_WIN || gState == ST_LOSE) {
            int win = (gState == ST_WIN);
            fillRect(gFrameDC, 0, 92, VIEW_W, VIEW_H, RGB(14, 24, 16));
            strokeRound(gFrameDC, 240, 190, 760, 470, 14, win ? RGB(150, 210, 100) : RGB(200, 80, 70), 5);
            fillRound(gFrameDC, 242, 192, 758, 468, 13, win ? RGB(34, 66, 38) : RGB(60, 30, 28));
            putTextCS(gFrameDC, gF54, win ? RGB(190, 255, 150) : RGB(255, 150, 140),
                      RGB(12, 20, 12), win ? L"守住了草坪！" : L"僵尸吃掉了你的脑子…", 500, 250);
            wsprintfW(buf, L"击杀 %d 只僵尸   用时 %d 秒", gKilled, (int)gElapsed);
            putTextCS(gFrameDC, gF22, RGB(255, 244, 200), RGB(12, 20, 12), buf, 500, 330);
            putTextCS(gFrameDC, gF18, RGB(210, 240, 210), RGB(12, 20, 12),
                      L"点击屏幕 / 按 R 再玩一局", 500, 400);
        }
    }

    /* 元界面文字独立绘制，避免把选关/每日营地误当成游戏 HUD */
    if (gState == ST_LEVELS || gState == ST_TALENTS ||
        gState == ST_ACH || gState == ST_GACHA ||
        gState == ST_LOADOUT || gState == ST_REWARD ||
        gState == ST_DAILY) drawMetaText(gFrameDC);

    /* 升级横幅（菜单 / 结算都显示） */
    if (gLvUpT > 0.0f) {
        wsprintfW(buf, L"升级！Lv.%d", gLvUpNew);
        putTextCS(gFrameDC, gF30, RGB(255, 236, 120), RGB(40, 24, 8), buf, VIEW_W * 0.5f, 96.0f);
        putTextCS(gFrameDC, gF15, RGB(255, 240, 190), RGB(40, 24, 8),
                  L"星星 +2 · 金币 +3", VIEW_W * 0.5f, 128.0f);
    }

    /* ================= 输出到窗口 =================
       窗口不一定正好是 1000x650（全屏时更大），做等比缩放 + 黑边，
       并把缩放参数记下来给鼠标坐标反算用。 */
    {
        RECT rc;
        int  cw, ch, dw, dh, dx, dy;
        HWND wnd = hWndMain ? hWndMain : GetActiveWindow();
        if (wnd) GetClientRect(wnd, &rc);
        else { rc.left = rc.top = 0; rc.right = VIEW_W; rc.bottom = VIEW_H; }
        cw = rc.right - rc.left; ch = rc.bottom - rc.top;
        if (cw <= 0 || ch <= 0) { cw = VIEW_W; ch = VIEW_H; }
        {
            float sx = (float)cw / (float)VIEW_W;
            float sy = (float)ch / (float)VIEW_H;
            if (sy < sx) sx = sy;
            gViewScale = sx;
            dw = (int)((float)VIEW_W * sx);
            dh = (int)((float)VIEW_H * sx);
        }
        dx = (cw - dw) / 2; dy = (ch - dh) / 2;
        gViewOffX = dx; gViewOffY = dy;

        if (dw == VIEW_W && dh == VIEW_H && dx == 0 && dy == 0) {
            /* 窗口正好 1:1：直接贴，最快，也没有中间态 */
            BitBlt(out, 0, 0, VIEW_W, VIEW_H, gFrameDC, 0, 0, SRCCOPY);
        } else {
            /* 先在离屏把「黑边 + 缩放后的画面」合成完，再一次性贴到窗口。
               这样窗口每帧只被写一次，不会出现"先全黑再出画面"的闪烁。 */
            presentEnsure(cw, ch);
            if (gPresentDC) {
                presentFillBlack(gPresentDC, cw, ch);
                /* 用 COLORONCOLOR（最近邻）而不是 HALFTONE：
                   ① 快很多（全屏下 HALFTONE 是主要的掉帧来源）
                   ② 卡通描边的资源放大后保持锐利，HALFTONE 反而把轮廓糊掉 */
                SetStretchBltMode(gPresentDC, COLORONCOLOR);
                StretchBlt(gPresentDC, dx, dy, dw, dh, gFrameDC, 0, 0, VIEW_W, VIEW_H, SRCCOPY);
                BitBlt(out, 0, 0, cw, ch, gPresentDC, 0, 0, SRCCOPY);
            } else {
                BitBlt(out, 0, 0, VIEW_W, VIEW_H, gFrameDC, 0, 0, SRCCOPY);
            }
        }
    }
}

/* ======================================================================== */
/*                                窗口与主循环                                */
static void toggleFullscreen(void)
{
    static DWORD busyUntil = 0;
    DWORD now = GetTickCount();
    if (!hWndMain) return;
    /* 切换本身要走 SetWindowLong + SetWindowPos，需要几十毫秒。
       在这期间如果又来一次请求，会切成"半全屏"的中间态。
       这里直接挡掉窗口期内的重复调用。 */
    if (now < busyUntil) return;
    busyUntil = now + 400;
    if (!gFullscreen) {
        HMONITOR hm = MonitorFromWindow(hWndMain, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hm, &mi)) return;
        GetWindowRect(hWndMain, &gSavedRect);
        gSavedStyle = GetWindowLongW(hWndMain, GWL_STYLE);
        /* 无边框窗口铺满显示器，而不是 ChangeDisplaySettings 真全屏：
           后者会改用户的显示分辨率，切出去还要黑屏恢复，体验很差。 */
        SetWindowLongW(hWndMain, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hWndMain, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        gFullscreen = 1;
        presentInvalidate();          /* 尺寸变了，后台缓冲要重建 */
    } else {
        SetWindowLongW(hWndMain, GWL_STYLE, gSavedStyle);
        SetWindowPos(hWndMain, HWND_TOP,
                     gSavedRect.left, gSavedRect.top,
                     gSavedRect.right - gSavedRect.left,
                     gSavedRect.bottom - gSavedRect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        gFullscreen = 0;
        presentInvalidate();
    }
}

/* ======================================================================== */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    /* 主循环自己每帧画，这里只需要"认领"这块更新区域，
       否则 Windows 会一直重发 WM_PAINT（表现为持续闪）。 */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_SIZE:
        /* 尺寸变了（全屏切换 / 拉伸）：后台缓冲必须重建 */
        presentInvalidate();
        return 0;
    case WM_MOUSEMOVE:
        windowToGame((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &mMouseX, &mMouseY);
        if (gState == ST_DRAFT) {
            int k;
            gDraftHover = -1;
            for (k = 0; k < DRAFT_N; k++) {
                float cx0, cy0;
                draftCardRect(k, &cx0, &cy0);
                if (mMouseX >= cx0 && mMouseX <= cx0 + CARD_W &&
                    mMouseY >= cy0 && mMouseY <= cy0 + CARD_H) gDraftHover = k;
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
        mDown = 1;
        windowToGame((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &mMouseX, &mMouseY);
        onClick(mMouseX, mMouseY);
        return 0;
    case WM_LBUTTONUP: mDown = 0; return 0;
    case WM_SYSKEYDOWN:                      /* Alt+Enter 也切全屏 */
        if (wp == VK_RETURN) {
            if (!keyIsRepeat(lp) && toggleAllowed()) toggleFullscreen();
            return 0;                        /* 吞掉，免得触发系统菜单 */
        }
        break;
    case WM_RBUTTONDOWN:
        if (gState == ST_LEVELS || gState == ST_TALENTS ||
            gState == ST_ACH || gState == ST_GACHA ||
            gState == ST_LOADOUT || gState == ST_REWARD ||
            gState == ST_DAILY) { gState = ST_MENU; return 0; } gSel = -1; gShovel = 0; return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11) {
            if (!keyIsRepeat(lp) && toggleAllowed()) toggleFullscreen();
            return 0;
        }
        else if (wp == VK_ESCAPE) { gSel = -1; gShovel = 0; }
        else if (wp == 'A') { gAutoSun = !gAutoSun; }
        else if (wp == 'W') { gWeatherOn = !gWeatherOn; }
        else if (wp == 'S' && gState == ST_DRAFT) { gSun += DRAFT_SKIP; gState = ST_PLAY; }
        else if (wp == 'G' && gState == ST_PLAY && gBanker && gGold >= 10) {
            /* 金气消费点：消耗 10 金气，在鼠标所在格免费召唤一个南瓜炸弹 */
            int row, col, w, hh;
            if (mouseToCell(mMouseX, mMouseY, &row, &col, &w, &hh) && cellLegal(row, col, 1, 1)) {
                gGold -= 10;
                plantIt(row, col, PT_CHERRY);
                if (grid[row][col].alive) {
                    grid[row][col].summon = 1;      /* 用南瓜贴图 */
                    grid[row][col].fuse = 0.9f;
                }
                gSel = -1;
            }
        }
        else if (wp == 'H' && gState == ST_PLAY && gNecromancer && gGhost >= 20) {
            /* 幽能消费点：消耗 20 幽能，全屏僵尸减速 6 秒 */
            int k;
            gGhost -= 20;
            for (k = 0; k < MAX_ZOMBIES; k++) {
                Zombie *z = &zombies[k];
                if (!z->active || z->dead) continue;
                if (z->slow < 6.0f) z->slow = 6.0f;
                z->flash = 0.10f;
            }
            flashRed = 0.22f;
            for (k = 0; k < 30; k++)
                addParticle(rnd(LAWN_X, LAWN_X + COLS * CELL_W), rnd(LAWN_Y, LAWN_Y + 60),
                            rnd(-40, 40), rnd(60, 140), 0.7f, rnd(3, 6), RGB(150, 220, 255), 1);
        }
        else if ((wp == 'M') && (gState == ST_LEVELS || gState == ST_TALENTS ||
                                 gState == ST_ACH || gState == ST_GACHA ||
                                 gState == ST_LOADOUT || gState == ST_REWARD ||
                                 gState == ST_DAILY)) {
            gState = ST_MENU;
        }
        else if (wp == 'F' && gState == ST_PLAY) {
            /* 合成植物：把鼠标格与"右邻格"的两株植物融成一株混种。
               混种不新增美术 —— 画面上叠两层亲本精灵 + 一圈光环，读起来就是"合体了"。 */
            int row, col, w, hh;
            if (mouseToCell(mMouseX, mMouseY, &row, &col, &w, &hh)) {
                Plant *a = plantAt(row, col);
                Plant *b = plantAt(row, col + 1);
                if (a && b) {
                    int ta = a->type, tb = b->type;
                    float hx = cellCX(col), hy = cellBaseY(row);
                    /* 混种取两者能力之和：类型取左，右邻的植物消失并把特性并进来 */
                    removePlantAt(row, col + 1);
                    a = plantAt(row, col);
                    if (a) {
                        a->fused  = 1;
                        a->fuseA  = ta;
                        a->fuseB  = tb;
                        a->hp = a->maxhp = (plantHpMax(ta) + plantHpMax(tb)) * 0.5f;
                        a->fuseMul = HAS(R_FUSION_MASTER) ? 1.35f : 1.0f;
                        a->timer = 0.5f;
                    }
                    gSave.fusionCount++;
                    achUnlock(16);
                    {
                        int q;
                        for (q = 0; q < 26; q++)
                            addParticle(hx + rnd(-40, 40), hy - rnd(10, 90),
                                        rnd(-140, 140), rnd(-160, 30), 0.8f, rnd(3, 8),
                                        (q & 1) ? RGB(255, 220, 120) : RGB(180, 255, 200), 1);
                    }
                    addShake(5.0f, 0.25f);
                    saveFlush();
                }
            }
        }
        else if (wp == 'M' && gState == ST_PLAY) {
            int n = 0, ids[MODE_COUNT], m;
            for (m = 0; m < MODE_COUNT; m++) if (gUnlockedMode[m]) ids[n++] = m;
            if (n > 1) {
                int next = -1, i;
                for (i = 0; i < n; i++) if (ids[i] == gCurMode) { next = ids[(i + 1) % n]; break; }
                if (next >= 0) gCurMode = next;
            }
        }
        else if (wp == 'P' || wp == VK_SPACE) {
            if (gState == ST_PLAY) gState = ST_PAUSE;
            else if (gState == ST_PAUSE) gState = ST_PLAY;
        } else if (wp == 'R') {
            resetGame();
            gState = ST_PLAY;
        } else if (wp >= '1' && wp <= '9') {
            /* 数字键 = 卡槽位（和鼠标点击同一映射），不是植物类型。
               之前用 wp < '1'+PT_COUNT，PT_COUNT=33 时把 '1'..'Q' 的键全吞了，
               而且选的是植物类型而非编组里的卡 */
            int slot = (int)(wp - '1');
            if (slot < gLoadoutN + gBonusN) {
                int idx = (slot < gLoadoutN) ? gLoadout[slot] : gBonusPlant[slot - gLoadoutN];
                if (cardCD[idx] <= 0.0f && gSun >= plantCost(idx)) { gSel = idx; gShovel = 0; }
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    WNDCLASSEXW wc;
    HWND hWnd;
    MSG msg;
    RECT r;
    DWORD style;
    DWORD prev;
    int running = 1;

    (void)hPrev; (void)cmdLine;

    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszMenuName  = NULL;
    wc.lpszClassName = L"PvZClassC";
    wc.hIconSm       = NULL;
    if (!RegisterClassExW(&wc)) return 1;

    style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    r.left = 0; r.top = 0; r.right = VIEW_W; r.bottom = VIEW_H;
    AdjustWindowRect(&r, style, FALSE);

    hWnd = CreateWindowExW(0, L"PvZClassC", L"植物大战僵尸 — C 语言复刻版",
                           style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, hInst, NULL);
    if (!hWnd) return 1;
    hWndMain = hWnd;

    /* ---------- 建立分层缓冲区 ---------- */
    gScreenDC = GetDC(hWnd);
    gFrameDC  = CreateCompatibleDC(gScreenDC);
    gFrameBM  = CreateCompatibleBitmap(gScreenDC, VIEW_W, VIEW_H);
    SelectObject(gFrameDC, gFrameBM);
    gWorldDC  = CreateCompatibleDC(gScreenDC);
    gWorldBM  = CreateCompatibleBitmap(gScreenDC, VIEW_W * SS, VIEW_H * SS);
    SelectObject(gWorldDC, gWorldBM);
    ReleaseDC(hWnd, gScreenDC);

    SetGraphicsMode(gWorldDC, GM_ADVANCED);
    gScaleXF.eM11 = (FLOAT)SS; gScaleXF.eM12 = 0;
    gScaleXF.eM21 = 0;         gScaleXF.eM22 = (FLOAT)SS;
    gScaleXF.eDx  = 0;         gScaleXF.eDy  = 0;

    /* ---------- 字体 ---------- */
    gF15 = mkFont(15, 0, L"微软雅黑");
    gF18 = mkFont(18, 1, L"微软雅黑");
    gF22 = mkFont(22, 1, L"微软雅黑");
    gF30 = mkFont(30, 1, L"微软雅黑");
    gF54 = mkFont(54, 1, L"微软雅黑");
    gF72 = mkFont(72, 1, L"微软雅黑");

    /* ---------- 载入美术资源（任何一张缺失就自动回退程序化绘制） ---------- */
    gIdentXF.eM11 = 1; gIdentXF.eM12 = 0; gIdentXF.eM21 = 0;
    gIdentXF.eM22 = 1; gIdentXF.eDx = 0;  gIdentXF.eDy = 0;
    gCurXF = gScaleXF;
    {
        static const wchar_t *plantFile[PT_COUNT] = {
            L"sunflower", L"peashooter", L"wallnut", L"potatomine",
            L"snowpea", L"repeater", L"cherrybomb", L"jalapeno",
            L"plant_threepeater", L"plant_spikeweed", L"plant_magnet",
            L"plant_kernelpult",
            L"starfruit", L"cactus", L"splitpea", L"lightningreed", L"bloomerang",
            L"fume", L"laserbean", L"melonpult", L"wintermelon", L"gloom",
            L"goldmagnet", L"twinflower", L"marrow", L"pumpkin", L"garlic",
            L"hypnoshroom", L"iceshroom", L"tanglekelp", L"cobcannon", L"cattail",
            L"hero_flame", L"hero_ironnut", L"hero_crystal", L"hero_thornvine",
            L"hero_sungod", L"hero_frost", L"hero_worldtree", L"hero_doom"
        };
        static const wchar_t *zombieFile[ZT_COUNT] = {
            L"zombie_normal", L"zombie_cone", L"zombie_bucket", L"zombie_flag",
            L"zombie_dancer", L"zombie_zamboni", L"zombie_balloon",
            L"zombie_vaulter", L"zombie_screendoor", L"zombie_football",
            L"zombie_newspaper", L"zombie_digger", L"zombie_giant"
        };
        static const wchar_t *zombieEatFile[ZT_COUNT] = {
            L"zombie_normal_eat", L"zombie_cone_eat",
            L"zombie_bucket_eat", L"zombie_flag_eat",
            L"zombie_dancer", L"zombie_zamboni", L"zombie_balloon",
            L"zombie_vaulter", L"zombie_screendoor", L"zombie_football",
            L"zombie_newspaper", L"zombie_digger", L"zombie_giant"
        };
        wchar_t *p;
        int i, nArt = 0;
        GetModuleFileNameW(NULL, gAssetDir, MAX_PATH);
        p = wcsrchr(gAssetDir, L'\\');
        if (p) p[1] = 0; else gAssetDir[0] = 0;
        wcscat(gAssetDir, L"assets\\");

        for (i = 0; i < PT_COUNT; i++) nArt += spriteLoadName(&gSprPlant[i], plantFile[i]);
        for (i = 0; i < ZT_COUNT; i++) {
            if (spriteLoadName(&gSprZombie[i], zombieFile[i])) {
                spriteTint(&gSprZombieFlash[i], &gSprZombie[i], 255, 255, 255);
                spriteTint(&gSprZombieBlue[i],  &gSprZombie[i], 110, 200, 255);
                nArt++;
            }
            if (!gSprZombieEat[i].ok) {            /* 新僵尸没有独立啃食图：退化成行走图 */
                sprCopy(&gSprZombieEat[i], &gSprZombie[i]);
                gSprZombieEat[i].ok = gSprZombie[i].ok;
            }
            spriteLoadName(&gSprZombieEat[i], zombieEatFile[i]);
            if (gSprZombieEat[i].ok) {   /* 啃食姿态 */
                spriteTint(&gSprZombieEatFlash[i], &gSprZombieEat[i], 255, 255, 255);
                spriteTint(&gSprZombieEatBlue[i],  &gSprZombieEat[i], 110, 200, 255);
                nArt++;
            }
            {   /* 倒地动画帧（由行走姿态离线派生） */
                int k;
                for (k = 0; k < 5; k++) {
                    wchar_t nm[64];
                    wsprintfW(nm, L"%s_dead%d", zombieFile[i], k);
                    nArt += spriteLoadName(&gSprZombieDead[i][k], nm);
                }
            }
        }
        nArt += spriteLoadName(&gSprSun,  L"sun");
        nArt += spriteLoadName(&gSprLawn, L"lawn");
        nArt += spriteLoadName(&gSprBackground, L"background");
        nArt += spriteLoadName(&gSprLawnNight, L"lawn_night");
        nArt += spriteLoadName(&gSprLawnWater, L"lawn_water");
        nArt += spriteLoadName(&gSprLawnRoof, L"lawn_roof");
        nArt += spriteLoadName(&gSprLawnDesert, L"lawn_desert");
        { int k;
          static const wchar_t *wn[WPN_COUNT-1] = {L"plant_pierce", L"plant_splash",
              L"plant_bounce", L"plant_freeze", L"plant_burn"};
          static const wchar_t *sn[3] = {L"plant_doublestack", L"plant_multilane", L"plant_tall"};
          for (k = 0; k < WPN_COUNT - 1; k++) nArt += spriteLoadName(&gSprWeapon[k], wn[k]);
          for (k = 0; k < 3; k++) nArt += spriteLoadName(&gSprShape[k], sn[k]);
          { int b; for (b = 0; b < BUL_COUNT; b++) nArt += spriteLoadName(&gSprBullet[b], bulletFile[b]); }
        nArt += spriteLoadName(&gSprCardBack, L"ui_cardback");
        nArt += spriteLoadName(&gSprBurst,    L"ui_burst");
        nArt += spriteLoadName(&gSprGold,    L"relic_gold");
          nArt += spriteLoadName(&gSprGhost,   L"relic_ghost");
          nArt += spriteLoadName(&gSprPumpkin, L"relic_pumpkin");
        }
        gArtLoaded = nArt;
        saveLoad();                                   /* 读存档（首次运行会建初始档） */
    }

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    resetGame();
    gState = ST_MENU;

    prev = GetTickCount();
    while (running) {
        float dt, used;
        DWORD t0;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        {
            DWORD now = GetTickCount();
            dt = (float)((DWORD)(now - prev)) / 1000.0f;
            prev = now;
        }
        if (dt > 0.05f) dt = 0.05f;
        if (dt < 0.0005f) dt = 0.0005f;

        if (gState == ST_PLAY) updateGame(dt);
        else { gTime += dt; weatherUpdate(dt); if (gLvUpT > 0.0f) gLvUpT -= dt; }   /* 菜单/结算界面保持动画 */

        t0 = GetTickCount();
        {
            HDC hdc = GetDC(hWnd);
            render(hdc);
            ReleaseDC(hWnd, hdc);
        }

        used = (float)((DWORD)(GetTickCount() - t0)) / 1000.0f;
        {
            float rest = 1.0f / 60.0f - used;
            if (rest > 0.001f) Sleep((DWORD)(rest * 1000.0f));
            else if (rest > 0.0f) Sleep(1);
        }
    }

    /* ---------- 清理 ---------- */
    {
        int i, k;
        for (i = 0; i < PT_COUNT; i++) spriteRelease(&gSprPlant[i]);
        for (i = 0; i < ZT_COUNT; i++) {
            spriteRelease(&gSprZombie[i]);
            spriteRelease(&gSprZombieFlash[i]);
            spriteRelease(&gSprZombieBlue[i]);
            spriteRelease(&gSprZombieEat[i]);
            spriteRelease(&gSprZombieEatFlash[i]);
            spriteRelease(&gSprZombieEatBlue[i]);
            for (k = 0; k < 5; k++) spriteRelease(&gSprZombieDead[i][k]);
        }
        spriteRelease(&gSprSun);
        spriteRelease(&gSprLawn);
        spriteRelease(&gSprBackground);
        spriteRelease(&gSprLawnNight);
        spriteRelease(&gSprLawnWater);
        spriteRelease(&gSprLawnRoof);
        spriteRelease(&gSprLawnDesert);
        { int k;
          for (k = 0; k < WPN_COUNT - 1; k++) spriteRelease(&gSprWeapon[k]);
          for (k = 0; k < 3; k++) spriteRelease(&gSprShape[k]);
          { int b; for (b = 0; b < BUL_COUNT; b++) spriteRelease(&gSprBullet[b]); }
          spriteRelease(&gSprCardBack);
          spriteRelease(&gSprBurst);
          spriteRelease(&gSprGold);
          spriteRelease(&gSprGhost);
          spriteRelease(&gSprPumpkin);
        }
    }
    DeleteObject(gWorldBM); DeleteDC(gWorldDC);
    presentRelease();
    if (gBlackBrush) { DeleteObject(gBlackBrush); gBlackBrush = NULL; }
    DeleteObject(gFrameBM); DeleteDC(gFrameDC);
    DeleteObject(gF15); DeleteObject(gF18); DeleteObject(gF22);
    DeleteObject(gF30); DeleteObject(gF54); DeleteObject(gF72);
    return 0;
}
