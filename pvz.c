/* =========================================================================
   Plants vs Zombies  ——  C 语言复刻版
   -------------------------------------------------------------------------
   纯 C + Win32 GDI，零第三方依赖，单文件。
   渲染采用 2 倍超采样后降采样，获得抗锯齿效果。

   编译：
       gcc -O2 -mwindows -o pvz.exe pvz.c -lgdi32 -luser32 -lm
   ========================================================================= */

/* ---- 平台层 ----
   所有 Win32 依赖都收在 plat.h 背后：_WIN32 下它只是把 windows.h / mmsystem.h
   引进来（直通，行为一个字节不变）；iOS 下它提供同名类型与函数声明，
   由 plat_ios.m 用 CoreGraphics / CoreText / AVFoundation 实现。
   游戏逻辑与本文件其余部分**两端共用同一份源码**。 */
#include "plat.h"

/* ---- 图像解码：stb_image（单头文件，公共领域）----
   资源已从 32bpp BMP 改为 PNG（带透明）/ JPEG（整屏不透明），
   实测 259 MB → 45 MB。BMP 是未压缩格式，795 张贴图占了整个包的大头。
   不要 stdio：调用点是自己把文件读进内存再解（这样走得到宽字符路径）。

   ★ BMP 分支**必须保留**（2026-09-21 踩过，代价是 50 张新卡全部不出图）：
     打包流程是 `_build_units.py`（出 BMP 母版）→ `_pack_assets.py`（转 PNG/JPEG）。
     两步之间的状态是只有 BMP 的。上一版把 stb 限制成 STBI_ONLY_PNG|JPEG、
     且 spriteLoadName 只试 .png/.jpg，于是**只跑了第一步**的新素材会静默加载失败：
     不报错、不崩溃，只是卡片上一片空白。
     新素材要立刻可用，解码器就得认识母版格式。
     留 BMP 分支的代价只有几百行解码代码（体积可忽略），
     换来的是"漏跑打包步骤"不会变成"素材白做"。
   ⚠️ stb_image 在 -Wextra 下会产生告警，用 pragma 压掉以保持"零告警"基线。 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_image.h"
#pragma GCC diagnostic pop
#define COBJMACROS
#ifdef PLAT_PORTABLE
/* 网页 / iOS 侧不碰 D3D：画面靠软件光栅 + canvas 上传。
   这里换成桩头 —— Direct3DCreate9 返回 NULL → gpuInit 失败 →
   引擎自动走**既有的** GDI 回退路径（PVZ_GDI=1 走的就是那条，久经使用）。
   为什么用桩而不是把 D3D 代码切掉：那 200 行要动 6 个函数，
   切割风险远大于留一个必然失败的桩。见 plat_d3d_stub.h。 */
#include "plat_d3d_stub.h"
#else
#include <d3d9.h>       /* GPU 呈现后端。编译需加 -ld3d9 */
#endif
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
/* 诊断用：渲染耗时与累计帧数，用来核对"性能是否稳定" */
static float   gDbgRenderMs = 0.0f;
static int     gDbgFrameCnt = 0;
static DWORD   gDbgPerfT0   = 0;
static double  gDbgSumRender = 0.0;
static int     gDbgPerfN    = 0;

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

/* --------------------------------------------------- 视觉升级层（P4-B）
   木纹 / 渐变卡面 / 圆角裁剪 / 全屏暗角。
   全部用 GDI 现成能力（GradientFill、Region 裁剪、AlphaBlend）实现，
   不依赖新的美术资源，也不引入新窗口或新线程。 */

/* 圆角裁剪进出栈：把之后的绘制限制在圆角矩形内。
   GetClipRgn 返回 -1 表示当前没有裁剪区，这种时候恢复要用 NULL。 */
static HRGN gClipSav = NULL;
static int  gClipHad = 0;

static void pushRoundClip(HDC dc, float l, float t, float r, float b, float rad)
{
    HRGN rg;
    float w = r - l, h = b - t;
    gClipHad = 0;
    if (w < 2.0f || h < 2.0f) return;
    if (rad > w * 0.5f) rad = w * 0.5f;
    if (rad > h * 0.5f) rad = h * 0.5f;
    if (!gClipSav) gClipSav = CreateRectRgn(0, 0, 1, 1);
    if (!gClipSav) return;
    gClipHad = (GetClipRgn(dc, gClipSav) == 1);
    rg = CreateRoundRectRgn((int)l, (int)t, (int)r + 1, (int)b + 1,
                            (int)(rad * 2.0f), (int)(rad * 2.0f));
    if (rg) { SelectClipRgn(dc, rg); DeleteObject(rg); }
}
static void popClip(HDC dc)
{
    if (gClipHad) { SelectClipRgn(dc, gClipSav); gClipHad = 0; }
    else          { SelectClipRgn(dc, NULL); }
}

/* 圆角 + 纵向渐变。GradientFill 在 msimg32 里，构建时早就链上了。 */
static void fillRoundGradV(HDC dc, float l, float t, float r, float b, float rad,
                           COLORREF top, COLORREF bot)
{
    TRIVERTEX v[2];
    GRADIENT_RECT gr;
    if (r - l < 2.0f || b - t < 2.0f) return;
    v[0].x = (LONG)l;   v[0].y = (LONG)t;
    v[0].Red   = (COLOR16)(GetRValue(top) << 8);
    v[0].Green = (COLOR16)(GetGValue(top) << 8);
    v[0].Blue  = (COLOR16)(GetBValue(top) << 8);
    v[0].Alpha = 0xFF00;
    v[1].x = (LONG)r;   v[1].y = (LONG)b;
    v[1].Red   = (COLOR16)(GetRValue(bot) << 8);
    v[1].Green = (COLOR16)(GetGValue(bot) << 8);
    v[1].Blue  = (COLOR16)(GetBValue(bot) << 8);
    v[1].Alpha = 0xFF00;
    gr.UpperLeft = 0; gr.LowerRight = 1;
    pushRoundClip(dc, l, t, r, b, rad);
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    popClip(dc);
}

/* 木纹：在渐变之上叠 7 条带正弦扰动的细线。
   每条线的颜色按它所在高度先把底色估出来、再按比例压暗，
   所以木纹不会和下面的渐变"打架"，从上到下自然变深。 */
static void woodGrain(HDC dc, float l, float t, float r, float b, float rad,
                      COLORREF base, float strength)
{
    int i, k;
    float h = b - t, w = r - l;
    POINT pt[26];
    if (h < 10.0f || w < 12.0f || strength <= 0.0f) return;
    pushRoundClip(dc, l, t, r, b, rad);
    for (i = 0; i < 7; i++) {
        float yy  = t + h * (0.07f + 0.142f * (float)i);
        float amp = 1.5f + (float)(i % 3) * 1.1f;
        float ph  = (float)i * 2.1f;
        float fy  = (yy - t) / h;                             /* 0..1 高度比例 */
        COLORREF rowC = tint(base, (0.5f - fy) * 0.30f);      /* 近似该行的渐变底色 */
        COLORREF gc   = tint(rowC, -(0.05f + 0.030f * (float)(i % 4)) * strength);
        for (k = 0; k < 26; k++) {
            float u = (float)k / 25.0f;
            pt[k].x = (LONG)(l + w * u);
            pt[k].y = (LONG)(yy + sin(ph + u * 3.4f) * amp);
        }
        SelectObject(dc, getPen(gc, (i % 3) ? 1 : 2));
        Polyline(dc, pt, 26);
    }
    popClip(dc);
}

/* 卡面 = 渐变 +（可选）木纹 + 顶部高光 / 底部内阴影（这两条线负责"厚度感"） */
static void cardFaceV(HDC dc, float l, float t, float r, float b, float rad,
                      COLORREF top, COLORREF bot, float grain)
{
    fillRoundGradV(dc, l, t, r, b, rad, top, bot);
    if (grain > 0.0f) woodGrain(dc, l, t, r, b, rad, bot, grain);
    pushRoundClip(dc, l, t, r, b, rad);
    line(dc, l + rad * 0.6f, t + 1.5f, r - rad * 0.6f, t + 1.5f, tint(top, 0.30f), 2);
    line(dc, l + rad * 0.6f, b - 2.0f, r - rad * 0.6f, b - 2.0f, tint(bot, -0.34f), 2);
    popClip(dc);
}

/* 全屏暗角：预先算好一张 1x 的 32 位 ARGB 图，之后每帧只做一次 AlphaBlend。
   用椭圆距离场——中心全透明、四周逐渐压暗，把注意力收在草坪中央。
   预乘 alpha 的纯黑正好等于「只压暗、不改变色相」。

   只作用于草坪那一条横带（VIG_TOP..VIG_BOT），带外 alpha 直接写 0：
   顶部 HUD（阳光/卡槽/波次）和底部进度条不能被压暗，否则信息读不清。
   这是个「零成本」的取舍——反正带外像素本来就是透明的，
   顺带把每帧 AlphaBlend 的像素量砍掉三成。 */
#define VIG_TOP   96.0f
#define VIG_BOT  606.0f

static HDC     gVigDC  = NULL;
static HBITMAP gVigBmp = NULL;

static void buildVignette(void)
{
    BITMAPINFO bi;
    void *bits = NULL;
    unsigned char *p;
    int x, y;
    float cx = VIEW_W * 0.5f;                 /* 横轴：整屏居中 */
    float cy = (VIG_TOP + VIG_BOT) * 0.5f;    /* 纵轴：草坪带居中 */
    float hh = (VIG_BOT - VIG_TOP) * 0.5f;
    int   y0 = (int)VIG_TOP, y1 = (int)VIG_BOT;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = VIEW_W;
    bi.bmiHeader.biHeight      = -VIEW_H;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    gVigBmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!gVigBmp || !bits) return;
    gVigDC = CreateCompatibleDC(NULL);
    if (!gVigDC) return;
    SelectObject(gVigDC, gVigBmp);
    p = (unsigned char *)bits;
    for (y = 0; y < VIEW_H; y++) {
        for (x = 0; x < VIEW_W; x++) {
            unsigned char a8 = 0;
            if (y >= y0 && y < y1) {
                float nx = ((float)x - cx) / cx;
                float ny = ((float)y - cy) / hh;
                /* 纵向系数略小于横向：草坪上下两端是刷怪/进攻方向，
                   压得太狠会让玩家看不清远处的僵尸 */
                float d  = sqrtf(nx * nx * 0.82f + ny * ny * 0.92f);
                float a  = (d - 0.55f) / 0.62f;
                if (a < 0.0f) a = 0.0f;
                if (a > 1.0f) a = 1.0f;
                a = a * a * 0.55f;                        /* 平方衰减 + 上限 */
                a8 = (unsigned char)(a * 255.0f);
            }
            p[0] = 0; p[1] = 0; p[2] = 0;
            p[3] = a8;
            p += 4;
        }
    }
}
static void drawVignette(HDC dc, int targetScale)
{
    BLENDFUNCTION bf;
    XFORM oldXF, identXF;
    int dstY, dstH;
    if (!gVigDC) buildVignette();
    if (!gVigDC) return;
    if (targetScale < 1) targetScale = 1;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    /* AlphaBlend 使用设备坐标，不遵循世界变换。高分辨率渲染时临时复位变换，
       直接把 1x 暗角遮罩缩放到目标缓冲，避免先降到 1x 再放大造成模糊。 */
    GetWorldTransform(dc, &oldXF);
    identXF.eM11 = 1; identXF.eM12 = 0; identXF.eM21 = 0;
    identXF.eM22 = 1; identXF.eDx = 0; identXF.eDy = 0;
    SetWorldTransform(dc, &identXF);
    dstY = (int)VIG_TOP * targetScale;
    dstH = (int)(VIG_BOT - VIG_TOP) * targetScale;
    AlphaBlend(dc, 0, dstY, VIEW_W * targetScale, dstH,
               gVigDC, 0, (int)VIG_TOP, VIEW_W, (int)(VIG_BOT - VIG_TOP), bf);
    SetWorldTransform(dc, &oldXF);
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
        PT_PIXELFARMER, PT_STARSHOOTER, PT_MUSHROOMSCOUT, PT_FIREIMP, PT_FROSTPITCHER,
        PT_COINSPROUT, PT_WOODGUARDIAN, PT_CACTUSDART, PT_HEALPETAL, PT_THUNDERMAGE,
        PT_CHRONODISTORTER, PT_DRAGONBREATHVINE, PT_SHADOWASSASSIN, PT_SUMMONERSHAMAN, PT_BOUNCETHORN,
        PT_FLASHBERRY, PT_MINERRADISH, PT_HOLOGRAMPROJECTOR, PT_GRAVITYCONTROLLER, PT_VOIDDEVOURER,
        PT_THUNDEROVERLORD, PT_PHENIXFLAME, PT_FROSTQUEEN, PT_TIMEHUNTER, PT_SOULREAPER,
        PT_SILVERMOONASSASSIN, PT_DEMOLITIONEXPERT, PT_MOONLIGHTPRIESTESS, PT_LUCKYROULETTE, PT_SPEEDYPEA,
        PT_BABYDRAGONGRASS, PT_MAGESPUMPKIN, PT_ENGINEERWALNUT, PT_THORFLOWER, PT_MIRRORSUNFLOWER,
        PT_CRYSTALSHOOTER, PT_ZOMBIESLAYER, PT_SEDUCTIONQUEEN, PT_PIXELTHROWER, PT_EAGLEEYESHOOTER,
        PT_SPORETANK, PT_STARDIVINER, PT_POISNOVINE, PT_LOCKONPEA, PT_ABSOLUTEZERO,
        PT_KALEIDOSCOPEFLOWER, PT_WEREWOLFGRASS, PT_GENESISFLOWER, PT_BOOMERANGPEA, PT_GOLDENPEA,
        PT_BUBBLELOTUS, PT_ECHOBAMBOO, PT_DREAMCAP, PT_SPOREORCHESTRA, PT_RUNECACTUS,
        PT_CLOCKSPROUT, PT_MIRRORFERN, PT_COMETCLOVER, PT_HONEYDRIPPER, PT_TESLAPUMPKIN,
        PT_BUBBLECANNON, PT_SONICBLOOM, PT_NEBULABEET, PT_LANTERNMELON, PT_CORALGUARD,
        PT_GEARFLOWER, PT_SNAILSHROOM, PT_PRISMWILLOW, PT_PAPERCRANE, PT_MONSOONREED,
        PT_CANDLESPROUT, PT_QUARTZKERNEL, PT_BELLFLOWER, PT_VORTEXTURNIP, PT_PAINTBRUSH,
        PT_DUSKORCHID, PT_STEAMPEPPER, PT_ORBITALMOSS, PT_LULLABYONION, PT_AURORAPEA,
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

/* 【重要】PT_COUNT 含 8 株神级植物，它们是"局内三选一"的临时产物，
   不进抽卡池、不参与永久解锁（plantOwned[] 永远是 0）。
   凡是要统计"收集度 / 已解锁植物 / 是否全收集"的地方，分母都必须用
   PT_COLLECTIBLE —— 用 PT_COUNT 会让分子最多到 32 而分母是 40，
   进度永远打不满，"收藏家"成就也永远解不开。 */
#define PT_COLLECTIBLE PT_HERO_FLAME

/* ================= 已下架植物（软移除） =================
   ------------------------------------------------------------------------
   ⚠️ 为什么一张表也不许从 enum 里删：
      gSave.plantOwned[] / gSave.loadout[] 都是按 PT_* 下标落盘的，
      从中间挪走一株，老存档里所有排在它后面的植物解锁状态会整体错一位 ——
      玩家会看到"没解锁的植物变亮、已解锁的反而变灰"。
      和 R_LOADOUT_PLUS 那次是同一类约束：**枚举位一旦落盘就是存档格式**。

   所以"下架"= 保留枚举位 + 在下面每一个「发牌入口」把它过滤掉：
     抽卡池 / 奖励池 / 神级兜底池 / 编组界面 / 初見赠送 / 收集度统计。
   存档二进制布局一个字节都不动，且随时可逆 —— 改这一个数组即可恢复。

   挑这 4 株的理由（都是普通档、非核心、且被别的植物完整上位替代）：
     像素农夫 PT_PIXELFARMER  自动种植采阳光 -> 经济位已多次补强，功能冗余
     木质守护 PT_WOODGUARDIAN  减伤坦        -> 坚果墙与南瓜头完全覆盖
     蜗牛菇   PT_SNAILSHROOM   黏液减速      -> 与寒冰射手等减速控制严重重叠
     烛芯芽   PT_CANDLESPROUT  廉价火苗射手   -> 豌豆射手及众多火系上位替代 */
static int plantRetired(int pid)
{
    static const int RET[] = {
        PT_PIXELFARMER, PT_WOODGUARDIAN, PT_SNAILSHROOM, PT_CANDLESPROUT
    };
    int i;
    for (i = 0; i < (int)(sizeof(RET) / sizeof(RET[0])); i++)
        if (pid == RET[i]) return 1;
    return 0;
}

/* 收集度分母：PT_COLLECTIBLE 再扣掉已下架的株数。
   直接拿 PT_COLLECTIBLE 当分母会让"收藏家"成就永远解不开 ——
   下架的 4 株再也不会出现在任何池子里，owned 永远追不上分母。
   这里用运行时统计而不是写死常量，免得以后调整 RET[] 忘了同步改数字。 */
static int plantPoolTotal(void)
{
    int i, n = 0;
    for (i = 0; i < PT_COLLECTIBLE; i++) if (!plantRetired(i)) n++;
    return n;
}

/* 编组界面的「可见格位」表。
   那个界面是按格位铺的网格，直接跳过某些 id 会在网格里留下空洞，
   所以先把还在显示的植物压实成 slot -> pid 的映射，界面只认 slot。
   这张表只依赖 RET[]（编译期常量），所以算一次就够；用 gLoN==0 做惰性标记。 */
static int gLoId[PT_COLLECTIBLE];
static int gLoN = 0;
static void loBuild(void)
{
    int i;
    if (gLoN) return;
    for (i = 0; i < PT_COLLECTIBLE; i++)
        if (!plantRetired(i)) gLoId[gLoN++] = i;
}
static int loId(int slot) { loBuild(); return (slot >= 0 && slot < gLoN) ? gLoId[slot] : -1; }
static int loCount(void)  { loBuild(); return gLoN; }

/* 僵尸类型 */
enum { ZT_NORMAL, ZT_CONE, ZT_BUCKET, ZT_FLAG,
       ZT_DANCER, ZT_ZAMBONI, ZT_BALLOON,
       /* 第二批：每种针对一种玩家策略做反制，用兵种相克加难度 */
       ZT_VAULTER,     /* 撑杆：快速跃进到第一株植物前 */
       ZT_SCREENDOOR,  /* 铁门：正面伤害减半 -> 逼你上磁力菇或爆炸 */
       ZT_FOOTBALL,    /* 橄榄球：高血高速 + 减速抗性 -> 逼你堆真实 DPS */
       ZT_NEWSPAPER,   /* 读报：破报后暴走 -> 打断"慢慢磨"的节奏 */
       ZT_DIGGER,      /* 挖掘：潜地快速接近当前前排 */
       ZT_GIANT,       /* 巨人 BOSS：一击砸碎植物 -> 逼你集火 */
       /* ---- 新五关专属僵尸（第 6~10 关）：每只对应它那一关的规则 ---- */
       ZT_FROSTFANG,   /* 冰锥：受击时向相邻行溅射冰冻（霜牙隘口） */
       ZT_GLACIER,     /* 冰川巨尸：高血量 + 免疫击退，破解"靠击退拖" */
       ZT_MAGMAW,      /* 熔心：免疫裂隙伤害，堵死"靠裂隙清怪"的退路 */
       ZT_ASHWALKER,   /* 灰烬行者：死亡留灰烬云遮蔽视线 */
       ZT_PHANTOM,     /* 幽影：在迷雾中移速 +40% */
       ZT_BLINKER,     /* 闪现：周期性快速接近当前前排 */
       ZT_COGWORK,     /* 机械：受击反弹伤害给攻击者植物 */
       ZT_SABOTEUR,    /* 短路：接触植物时使该行断电 */
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
    /* ---- 新增 50 株永久植物 ---- */
    { L"像素农夫",      80,  5.0f,  300,  0, 0, L"自动种植种子并采集阳光" },
    { L"星之射手",     100,  7.5f,  250,  1, 0, L"星光穿透弹，命中回血" },
    { L"蘑菇侦察兵",    50,  5.0f,  200,  0, 0, L"夜间毒气，白天休眠" },
    { L"火焰小鬼",     100,  8.0f,  280,  1, 0, L"火焰弹链传并燃烧" },
    { L"寒冰投手",     100,  8.0f,  280,  1, 0, L"冰球减速并留下冰面" },
    { L"小金币",       50, 15.0f,  150,  0, 0, L"每10秒产阳光，周边增益" },
    { L"木质守护",     50,  7.0f,  800,  0, 0, L"保护身后植物减伤" },
    { L"仙人掌飞镖",  125,  7.5f,  300,  1, 0, L"高速刺针穿透并眩晕" },
    { L"治愈花瓣",     100, 15.0f,  200,  1, 0, L"范围治疗并解除负面" },
    { L"雷电法师",    150, 10.0f,  250,  1, 0, L"闪电链传并麻痹" },
    { L"时空扭曲者",  200, 30.0f,  200,  2, 0, L"时停并提升攻击速度" },
    { L"龙息藤蔓",    200, 12.0f,  400,  2, 0, L"穿透龙息燃烧" },
    { L"暗影刺客",    175, 15.0f,  180,  2, 0, L"瞬移暴击眩晕" },
    { L"召唤师萨满",  175, 20.0f,  250,  2, 0, L"召唤小植物单位" },
    { L"反弹荆棘",    125,  8.0f,  600,  1, 0, L"反弹投射物" },
    { L"闪光草莓",    150, 15.0f,  220,  1, 0, L"致盲并丢失目标" },
    { L"矿工萝卜",    125, 20.0f,  200,  1, 0, L"埋设地雷击飞" },
    { L"全息投影",    175, 25.0f,  150,  2, 0, L"嘲讽诱饵吸引僵尸" },
    { L"重力控制",    200, 30.0f,  250,  2, 0, L"强制落地飞行单位" },
    { L"虚空吞噬者",  200, 30.0f,  350,  3, 0, L"黑洞吸附吞噬回血" },
    { L"雷电领主",    175, 20.0f,  400,  2, 0, L"全范围雷电链" },
    { L"凤凰火焰",    200, 45.0f,  300,  3, 0, L"燃烧并复活一次" },
    { L"寒冰女王",    200, 30.0f,  400,  3, 0, L"冰封并暴击" },
    { L"时间猎手",    175, 25.0f,  300,  2, 0, L"加速后减速" },
    { L"灵魂收割者",  200, 20.0f,  280,  2, 0, L"高伤吸血" },
    { L"银月刺客",    175, 12.0f,  220,  2, 0, L"月光突刺穿透" },
    { L"爆破专家",    150, 18.0f,  260,  1, 0, L"定时爆炸范围" },
    { L"月光祭司",    150, 15.0f,  240,  2, 0, L"光环净化治疗" },
    { L"幸运轮盘",    125, 20.0f,  220,  1, 0, L"随机增益" },
    { L"极速豌豆",    125,  5.0f,  250,  1, 0, L"极高射速" },
    { L"幼龙草",      150, 10.0f,  350,  2, 0, L"成长型伤害递增" },
    { L"魔导师南瓜",  150, 25.0f,  300,  2, 0, L"魔法护盾" },
    { L"工程兵坚果",  100, 30.0f,  5000,0, 0, L"自带修理" },
    { L"雷神花",      175, 12.0f,  280,  2, 0, L"雷电眩晕" },
    { L"镜像向日葵",  150,  8.0f,  280,  1, 0, L"复制阳光" },
    { L"水晶射手",    175,  8.0f,  300,  2, 0, L"水晶弹分裂" },
    { L"丧尸克星",    200, 10.0f,  320,  2, 0, L"对僵尸额外伤害" },
    { L"魅惑女王",    175, 20.0f,  240,  2, 0, L"魅惑僵尸" },
    { L"像素投手",    100,  7.5f,  280,  0, 0, L"像素风格射击" },
    { L"鹰眼射手",    150,  7.5f,  260,  1, 0, L"远距离狙击" },
    { L"孢子坦克",    175, 15.0f,  600,  2, 0, L"高血量孢子喷射" },
    { L"星辰占卜师",  175, 25.0f,  240,  2, 0, L"预言暴击" },
    { L"毒藤蔓",      125,  8.0f,  300,  1, 0, L"持续毒伤" },
    { L"锁定豌豆",    150,  8.0f,  280,  1, 0, L"锁定攻击" },
    { L"绝对零度",    200, 35.0f,  300,  3, 0, L"全屏冻结" },
    { L"万花筒花",    150, 12.0f,  260,  2, 0, L"多形态攻击" },
    { L"狼人草",      150, 10.0f,  350,  2, 0, L"夜战变身" },
    { L"创世之花",    200, 30.0f,  500,  4, 0, L"终极治疗增益" },
    { L"回旋豌豆",    125,  9.0f,  300,  1, 0, L"回旋镖攻击" },
    { L"金色豌豆",    125,  7.5f,  300,  1, 0, L"高价值阳光" },
    /* ---- 第三批：30 株机制型永久植物 ---- */
    { L"泡泡莲",      100,  8.0f,  300,  0, 0, L"泡泡弹定身并击退，命中后小范围溅射" },
    { L"回声竹",      125,  8.0f,  280,  0, 0, L"声波穿透整行，命中后会回响弹跳" },
    { L"梦境菇",       75, 10.0f,  230,  0, 0, L"周期性催眠前方僵尸，造成小伤害与长减速" },
    { L"孢子乐团",    125, 11.0f,  260,  1, 0, L"向三行扩散孢子，群体减速并持续压血" },
    { L"符文仙人掌",  150,  8.0f,  300,  1, 0, L"符文刺高穿透，短暂晶化定身目标" },
    { L"时钟芽",      150, 16.0f,  260,  1, 0, L"缩短全卡组冷却，并让本行僵尸短暂停滞" },
    { L"镜蕨",        125, 14.0f,  420,  1, 0, L"给邻近植物反射护盾，被逼近时反击" },
    { L"彗星草",      175, 10.0f,  280,  2, 0, L"发射跨行追踪彗星，命中带小爆炸" },
    { L"蜜滴花",      150, 12.0f,  260,  1, 0, L"产出蜜阳光并治疗周围植物" },
    { L"特斯拉南瓜",  200, 18.0f, 2600,  2, 0, L"高耐久护体，周期链电附近僵尸" },
    { L"泡泡炮",      225, 14.0f,  420,  2, 0, L"重型泡泡炮弹，大击退、大定身、大溅射" },
    { L"声波花",      175, 12.0f,  300,  1, 0, L"整行声波冲击，群体击退" },
    { L"星云甜菜",    225, 18.0f,  520,  2, 0, L"制造重力井，对聚集僵尸造成范围伤害和定身" },
    { L"灯笼瓜",      250, 16.0f,  420,  2, 0, L"抛投燃烧灯笼，爆开后留下火焰" },
    { L"珊瑚卫士",    150, 24.0f, 3800,  1, 0, L"持续为十字邻格叠护盾，守住阵线" },
    { L"齿轮花",      175,  9.0f,  300,  1, 0, L"连续作战会越转越快，发射弹跳齿轮" },
    { L"蜗牛菇",      100, 12.0f,  260,  0, 0, L"释放黏液场，让本行僵尸大幅减速" },
    { L"棱镜柳",      225, 14.0f,  340,  2, 0, L"棱镜激光照射三行，穿透密集尸群" },
    { L"纸鹤草",      125,  7.5f,  240,  1, 0, L"高速追踪纸鹤，优先处理跨行威胁" },
    { L"季风芦苇",    175, 12.0f,  300,  1, 0, L"季风弹穿透整行，强击退并减速" },
    { L"烛芯芽",      100,  7.5f,  240,  0, 0, L"廉价火苗射手，命中后点燃地面" },
    { L"石英玉米",    175, 13.0f,  320,  1, 0, L"晶化玉米粒，单体高控制" },
    { L"铃兰钟",      200, 18.0f,  300,  2, 0, L"全场钟声微眩晕，拖住尸潮节奏" },
    { L"涡旋萝卜",    225, 20.0f,  480,  2, 0, L"制造涡旋，把本行僵尸卷回去并造成伤害" },
    { L"彩绘笔刷",    150, 10.0f,  260,  1, 0, L"给脚下画出花蔓增益地块，同时发射颜料弹" },
    { L"暮光兰",      225, 18.0f,  320,  2, 0, L"在治疗与暗影打击之间交替" },
    { L"蒸汽辣椒",    250, 22.0f,  360,  2, 0, L"周期喷出整行蒸汽，燃烧并强击退" },
    { L"轨道苔藓",    300, 24.0f,  360,  3, 0, L"锁定最密集尸群，呼叫轨道孢子打击" },
    { L"催眠洋葱",    200, 20.0f,  360,  2, 0, L"释放催眠气味，群体长减速与定身" },
    { L"极光豌豆",    300, 14.0f,  340,  3, 0, L"极光寒冰弹，穿透、连锁、冻结三合一" },
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

/* 僵尸出场费：0 = 尚未解锁。zCost 是运行期可变的（随波次逐级解锁），
   所以另存一份「0 波基准值」，每局 resetGame 还原 ——
   否则打过后期关卡后重开第 1 关，第 0 波就会刷出舞王/巨人，早期关卡被直接打穿。 */
static const int   zCostBase[ZT_COUNT] = { 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0, 0, 0, 0, 0 };
static int         zCost[ZT_COUNT];
/* 出怪稀有度权重（相对值，不是百分比）：值越大越常见。
   普通/路障/铁桶构成基本盘；反制型僵尸出现得足够频繁；
   巨人 BOSS 权重 1，只在它解锁后的波次里偶尔冒一头。 */
static const int   zWeight[ZT_COUNT] = { 6, 4, 3, 0,   /* 普通 路障 铁桶 旗帜(不入池) */
                                         2, 2, 2,      /* 舞王 冰车 气球 */
                                         3, 3, 2,      /* 撑杆 铁门 橄榄球 */
                                         3, 2, 1,      /* 读报 挖掘 巨人 */
                                         /* 新五关专属：中等频率，让规则压力持续存在
                                            但不至于一次涌出一堆 */
                                         2, 1, 2, 2, 2, 2, 2, 2 };
/* 血量整体上调 30%：移除羁绊后玩家 DPS 下降，僵尸血量同步补上强度 */
static const float zHp[ZT_COUNT]    = { 260.0f, 730.0f, 1430.0f, 260.0f,
                                         420.0f, 1760.0f, 220.0f,
                                         340.0f, 620.0f, 1950.0f,
                                         390.0f, 550.0f, 5460.0f,
                                         /* 冰锥 冰川 熔心 灰烬 幽影 闪现 机械 短路 */
                                         680.0f, 3200.0f, 1600.0f, 520.0f,
                                         600.0f, 460.0f, 2100.0f, 740.0f };
/* 速度上调 12%：整体节奏更紧 */
static const float zSpeed[ZT_COUNT] = { 15.7f, 15.7f, 15.7f, 21.3f,
                                         13.4f, 24.6f, 16.8f,
                                         20.2f, 12.3f, 26.9f,
                                         11.2f, 15.7f, 7.8f,
                                         /* 冰锥 冰川 熔心 灰烬 幽影 闪现 机械 短路 */
                                         14.2f,  9.6f, 12.8f, 17.4f,
                                         16.2f, 19.0f, 13.6f, 15.0f };

/* ======================================================================== */
/*              肉鸽系统：品质档位 / 能力位掩码 / 植物与僵尸数据表              */
/* ======================================================================== */
/* 设计要点
   -------------------------------------------------------------------------
   1. 这条线【独立于】永久卡池（plantDefs / PT_COUNT / 存档格式），
      所以加它不会碰存档二进制布局，也不会让老存档读不出来。
   2. 每株肉鸽植物有一个「基座」base（PT_*）：贴图、种植合法性、大部分
      行为都复用基座，肉鸽层只负责「数值倍率 + 能力位」。
      这样一来 50 株新植物不需要 50 套贴图，也不会出现缺失资源的黑块。
   3. 数值全部由品质档位推导，不手填 —— 用户要求
      「杀伤力 / 频率 / 杀伤范围 按照等级来分」，用函数算才不会调歪。      */

/* ---- 品质档位：普通/稀有/史诗/传说/神级/传奇/殿堂 ----
   ------------------------------------------------------------------------
   ⚠️ 改 RGQ_COUNT 时必须同步扩下面 8 个数组，漏一个就是越界读：
      RGQ_NAME / RGQ_COL / RGQ_DMG / RGQ_RATE / RGQ_RANGE /
      RGQ_DRAFT_W / RGQ_COST（最后一个在下方「阳光定价」处）。
      RGQ_W 是肉鸽僵尸用的，改植物权重不要去动它。
      它们全部 assert 不了，编译器也不会报 —— 只能靠手动对齐。     */
enum { RGQ_COMMON, RGQ_RARE, RGQ_EPIC, RGQ_LEGEND, RGQ_MYTH, RGQ_ULTRA,
       RGQ_HALL,
       RGQ_PRIMORDIAL,          /* 始祖：全表唯三，用户定义的"最强等级" */
       RGQ_COUNT };

static const wchar_t *RGQ_NAME[RGQ_COUNT] = {
    L"普通", L"稀有", L"史诗", L"传说", L"神级", L"传奇", L"殿堂", L"始祖" };

static const COLORREF RGQ_COL[RGQ_COUNT] = {
    RGB(176, 182, 176),   /* 普通 灰 */
    RGB( 96, 168, 232),   /* 稀有 蓝 */
    RGB(168, 108, 232),   /* 史诗 紫 */
    RGB(240, 168,  56),   /* 传说 金 */
    RGB(244,  88, 132),   /* 神级 洋红 */
    RGB(255,  64,  48),   /* 传奇 赤红 */
    RGB(255, 246, 214),   /* 殿堂 炽白金 */
    RGB(255, 236, 255),   /* 始祖 太初紫白（比殿堂更亮，是全表最高亮度档） */
};

/* 杀伤力倍率：普通 2× → 传奇 20× → 殿堂 100× → 始祖 1000×
   殿堂那一档是用户指定的「传奇伤害的 5 倍」；始祖是用户指定的
   「其他植物的 1000 倍」，所以直接给 1000×（殿堂的 10 倍）。
   频率与范围仍然刻意不再放大（停在 1.60 / 3.00）：
   1000× 已经是"一击清场"的量级，再把射速与范围一起乘上去只会变成
   纯数字膨胀，反而看不出差异。 */
/* 伤害倍率。
   ★ 始祖（末位）从 1000 拉到 4000 —— 用户要求"与其他植物强度不匹配、需要过强"。
     这一步是安全的：RGQ_DMG 只被 rgDmgMul（植物）与 rgZombieHpMul（僵尸）
     两处消费，而**没有任何僵尸使用 RGQ_PRIMORDIAL 档位**（已核对全表），
     所以抬高末位不会连带把僵尸血量也放大 1680 倍。
     rgZombieHpMul 另外加了一道保险，见那里的注释。 */
static const float RGQ_DMG[RGQ_COUNT]  = { 2.0f, 4.0f, 7.0f, 11.0f, 15.0f, 20.0f, 100.0f, 4000.0f };
/* 发射频率倍率（越大越快，作用在冷却上取倒数） */
/* 发射频率倍率（越大越快，作用在冷却上取倒数）。
   始祖 1.60 → 3.00：脉冲周期 2.8/3.0 ≈ 0.93 秒，比殿堂快将近一倍。 */
static const float RGQ_RATE[RGQ_COUNT] = { 1.00f, 1.00f, 1.15f, 1.30f, 1.45f, 1.60f, 1.60f, 3.00f };
/* 杀伤范围倍率（溅射半径 / 覆盖格数 / 弹幕数量） */
/* 杀伤范围倍率（溅射半径 / 覆盖格数 / 弹幕数量）。
   始祖 3.00 → 8.00：地刺铺满半场、齐射覆盖全屏。 */
static const float RGQ_RANGE[RGQ_COUNT]= { 1.0f, 1.20f, 1.50f, 1.90f, 2.40f, 3.00f, 3.00f, 8.00f };

/* ---- 伤害倍率的整数镜像 + 用户硬要求的编译期断言 ----
   用户明确要求：**始祖级伤害至少为殿堂级的 5 倍**。
   C 里 `static const float` 不是常量表达式，没法直接拿 RGQ_DMG 做数组断言，
   所以把这两个数各镜像一份整数下来，用 _Static_assert 钉死。
   ⚠️ 镜像一旦和 RGQ_DMG 不一致就失去意义 —— _test_card_tiers 会逐项核对
      （断言 + 运行时核对，两道一起才可靠）。 */
enum {
    RG_DMGX_HALL       = 100,
    RG_DMGX_PRIMORDIAL = 4000,
    RG_PRIM_MIN_RATIO  = 5      /* 始祖 ≥ 5 × 殿堂 */
};
_Static_assert(RG_DMGX_PRIMORDIAL >= RG_PRIM_MIN_RATIO * RG_DMGX_HALL,
               "始祖级伤害必须至少是殿堂级的 5 倍");
_Static_assert(RG_DMGX_PRIMORDIAL / RG_DMGX_HALL >= RG_PRIM_MIN_RATIO,
               "始祖级伤害必须至少是殿堂级的 5 倍（比值）");
/* （这里原来有一张 RGQ_TRAITN[RGQ_COUNT]，是"该档位典型能力条数"。
   2026-09-21 删掉了 —— 卡片上现在直接用 rgPlantTraitCount() 对 traits 掩码
   做 popcount，报的是**这张卡实际开几位**。留着档位代表值只会出现"卡片写 7 条、
   实际开了 8 条"的少报，而且它本身就是"改 RGQ_COUNT 必须同步扩的数组"之一，
   删掉一个就少一个漏扩的机会。 */
/* 出现权重：越稀有越难抽到（这也是「通关靠随机到好三选一」的来源） */
/* 抽卡权重（不是百分比 —— 抽出前会按"当前可选池"归一化）。
   ⚠️ 这个数组**只服务于肉鸽僵尸**（rgPickZombie），不参与三选一植物抽卡。
      它和植物的权重曾经共用一个数组，改植物的概率会连带改僵尸的出现构成 ——
      那属于"改一个地方、动两套平衡"的隐患，所以 2026-09-20 拆成了两个数组：
        肉鸽僵尸 -> RGQ_W（本数组，未受植物改动影响）
        三选一植物 -> RGQ_DRAFT_W（见下）
   殿堂只有 5 株且权重压到 2，单波抽中概率约 0.8%：它是"这一局彻底起飞"
   的标志性时刻，不该在 20 波里稳定出现。 */
static const int   RGQ_W[RGQ_COUNT]      = { 30, 26, 20, 15, 10, 6, 2, 0 };

/* ---- 三选一植物池专用权重（rgOpenDraft）----
   用户指定的目标（**单卡位**的整档概率）：
       始祖 1%、神级 10%、传奇 8%、殿堂 5%，其余三档吸收剩余的 76%，
       并保持"越稀有越低"的降序不变。

   ⚠️ 三件必须理解的事：

   ① 档位概率 = 该档株数 × 权重 / 总权重，各档株数差别极大
      （史诗 26 株、神级 13 株、传奇/殿堂各 5 株、始祖只有 3 株）。
      所以这个数组**故意不是单调递减的** —— 想让 3 株的始祖只占 1%、
      5 株的传奇占 8%，它们的单株权重反而要**大于**株数多的档位。
      当前：稀有114 史诗86 传说70 神级63 传奇131 殿堂81 始祖27。
      这不是写错，是"目标锚在档位概率而不是权重"的必然结果。

   ② 总权重被**刻意凑成 8192 = 2^13**：
      抽取用 rndi(0, tot) = `rand() % tot`，MSVCRT 的 RAND_MAX 只有 32767。
      tot 不能整除 32768 时会留下余数段，那一段取值多拿一次 ——
      tot=2602 时低段偏重 8.3%，实测把殿堂从设计 5.00% 压到 4.73%；
      8192 能整除 32768（32768/8192 = 4 整），余数段长度为 0，偏差归零。
      **改权重必须继续保证总和恰好是 2 的幂**（2048/4096/8192 都行，
      但别越过 RAND_MAX/2 = 16383，否则后半段候选永远抽不到）。
      校验式：24wR+26wE+18wL+13wM+5wU+5wH+3wP == 8192

   ③ 实际落点（合格池 94 株、总权重 8192）：
      稀有 33.40% / 史诗 27.29% / 传说 15.38% / 神级 10.00% /
      传奇 8.00% / 殿堂 4.94% / 始祖 0.99%    合计 100.00%
      硬指标偏差 ≤0.06pp。

   索引 0（普通）填 30 只是保留历史值 —— 当前普通档被
   RG_DRAFT_MIN_TIER 挡在牌桌外，不参与归一化。 */
/* ⚠️ 这个数组**只服务于植物三选一**（僵尸用 RGQ_W，两者已拆开）。
   2026-09-21 新增 20 张殿堂 + 30 张始祖后**重算**过：
   株数从（24/26/18/13/5/5/3）变成（24/26/18/13/5/25/33），
   权重不跟着改的话殿堂与始祖的占比会翻好几倍、稀有档被挤没。

   目标概率（每卡位）与实测：
     稀有 30.0% → 29.96%   史诗 26.0% → 26.03%   传说 16.0% → 15.99%
     神级 11.0% → 10.99%   传奇  7.0% →  7.02%   殿堂  8.0% →  8.01%
     始祖  2.0% →  2.01%
   总权重 = 24·410 + 26·328 + 18·310 + 13·278 + 5·461 + 25·87 + 33·22
          = 32768 = 2^15（整除 32768 → rndi 取模无偏；见 rgOpenDraft 的注释）
   ★ 这组数是**在"每档总概率严格降序"的硬约束下**解出来的，不是拍出来的：
     光按目标概率取整会让殿堂(25 张)的总概率反超传奇(5 张) ——
     表现为"殿堂比传奇还常见"，违反"越稀有出现率越低"。
     所以把殿堂权重从 105 压到 87（总概率 6.64% < 传奇 7.03%），
     并把差额补给传说 / 神级 / 始祖。求解器见本文件的注释块与
     _test_cards50.c 的"权重解算"小节。
   ⚠️ 注意：**每张卡**的概率并不严格降序，传奇(5 张)每张 1.41% 高于
     神级(13 张)每张 0.85%。这是"张数悬殊"的必然结果（传奇只有 5 张卡）。
     要让它也降序，只能把传奇压到 4% 以下 —— 那样这个档位基本抽不到，
     与"传奇是强力档"的设计冲突。所以这里只保证**档位级**降序。
   ⚠️⚠️ **索引 0（普通）必须留一个占位值**。
      第一版我写成 `{ 409, 328, ... }`（把普通省掉了），C 会把 7 个初值塞进 0..6、
      下标 7 补 0 —— 于是整表**前移一位**，始祖档权重变成 0、**永远抽不出来**，
      而且总权重也不再是 2 的幂。这个错不报错、不崩溃，只是悄悄少了一整档。
      普通档被 RG_DRAFT_MIN_TIER 挡在牌桌外，所以它的值不参与归一化，
      填 30 只是保留历史值。_test_cards50 会把这条钉住。 */
static const int   RGQ_DRAFT_W[RGQ_COUNT] = { 30, 410, 328, 310, 278, 461, 87, 22 };

/* ---- 能力位掩码（32 位，一株植物最多 6 条，传奇 = 全开） ---- */
#define RG_T_MULTILANE   (1u <<  0)   /* 多线齐射 */
#define RG_T_TIMEFREEZE  (1u <<  1)   /* 命中/产出时冻结时间 */
#define RG_T_CORRODE     (1u <<  2)   /* 持续腐蚀护甲（穿甲） */
#define RG_T_DEVOURBUL   (1u <<  3)   /* 吞噬子弹转化阳光 */
#define RG_T_MIRROR      (1u <<  4)   /* 镜像分身分摊伤害 */
#define RG_T_GROWEAT     (1u <<  5)   /* 吃僵尸后成长 */
#define RG_T_CHAINBOOM   (1u <<  6)   /* 延时链式爆炸 */
#define RG_T_GAMBLEDMG   (1u <<  7)   /* 概率十倍伤害 */
#define RG_T_SHARESUN    (1u <<  8)   /* 感染邻近植物共享阳光 */
#define RG_T_GRAVITY     (1u <<  9)   /* 坠落造成范围眩晕 */
#define RG_T_DROPCARD    (1u << 10)   /* 爆炸随机掉落植物卡 */
#define RG_T_LOAN        (1u << 11)   /* 血量翻倍，但结束时扣阳光 */
#define RG_T_WEBSLOW     (1u << 12)   /* 减速并生成蛛网陷阱 */
#define RG_T_ROOFWALK    (1u << 13)   /* 沿格移动射击 */
#define RG_T_REWIND      (1u << 14)   /* 命中后回到数秒前位置 */
#define RG_T_DOUBLEBODY  (1u << 15)   /* 分身夹击敌后 */
#define RG_T_CONFUSE     (1u << 16)   /* 使僵尸混乱走位 */
#define RG_T_ICERESOURCE (1u << 17)   /* 冰冻的僵尸转化为资源 */
#define RG_T_SELFDESTRUCT (1u << 18)  /* 低血自爆并链式引燃 */
#define RG_T_MEMESPREAD  (1u << 19)   /* 被触碰时传播阳光与减速 */
#define RG_T_LASERCUT    (1u << 20)   /* 激光切割，无视护甲 */
#define RG_T_GAMBLERISK  (1u << 21)   /* 每次发射赌博：翻倍或归零 */
#define RG_T_STEPMOVE    (1u << 22)   /* 每次攻击后移动一格 */
#define RG_T_DIGITRAIN   (1u << 23)   /* 数字雨防御层 */
#define RG_T_RHYTHM      (1u << 24)   /* 随节拍增强伤害 */
#define RG_T_PIXELCRUSH  (1u << 25)   /* 概率像素化即死 */
#define RG_T_HIJACK      (1u << 26)   /* 劫持僵尸武器反打 */
#define RG_T_FUSEPLANT   (1u << 27)   /* 吞噬其他植物融合能力 */
#define RG_T_BLINK       (1u << 28)   /* 随机传送换位 */
#define RG_T_PETALBARRAGE (1u << 29)  /* 多方向花瓣弹幕 */
#define RG_T_LIFESTEAL   (1u << 30)   /* 吸取僵尸血量回复自身 */
#define RG_T_CURSE       (1u << 31)   /* 诅咒：持续掉血 */

/* 植物能力位 · 第二字（第 33~48 位）—— 32 位不够放，所以 traits 用 64 位 */
#define RG_T_RECOMBINE   (1ull << 32) /* 炸开后重组再爆 */
#define RG_T_INFECTTURN  (1ull << 33) /* 感染僵尸反击同类 */
#define RG_T_DODGE       (1ull << 34) /* 概率免疫伤害 */
#define RG_T_EATSUN      (1ull << 35) /* 吸收阳光加速 */
#define RG_T_BOUNCE5     (1ull << 36) /* 子弹反弹 5 次 */
#define RG_T_MAZEWALL    (1ull << 37) /* 生成迷宫路障 */
#define RG_T_WEATHER     (1ull << 38) /* 随机天气 */
#define RG_T_DRAWCARD    (1ull << 39) /* 发射抽卡 */
#define RG_T_ASSASSIN    (1ull << 40) /* 隐身蓄力必杀 */
#define RG_T_LULLABY     (1ull << 41) /* 催眠沉睡 */
#define RG_T_CHI         (1ull << 42) /* 内力护盾 */
#define RG_T_PLAGUE      (1ull << 43) /* 中毒扩散 */
#define RG_T_BLACKHOLE   (1ull << 44) /* 吞噬加速 */
#define RG_T_FAKEHP      (1ull << 45) /* 假血欺骗 */
#define RG_T_AURABUFF    (1ull << 46) /* 光环增伤 */
#define RG_T_COPYEAT     (1ull << 47) /* 吞噬复制能力 */

/* ---- 第二批机制位：48~63，正好是 64 位掩码的最后 16 个空位 ----
   设计取材（用户要求"参考市面成功游戏的机制"）：
     ARMORBREAK / MARK   ← 杀戮尖塔的易伤(Vulnerable)与标记
     CRIT                ← 土豆兄弟 / 以撒的暴击与吸血
     KNOCKBACK / PULL    ← 皇室战争、明日方舟的推拉位移
     SUMMON              ← 泰拉瑞亚哨兵 / 云顶之弈召唤使
     ORBITAL             ← 吸血鬼幸存者的天降系武器
     EXECUTE             ← MOBA 的斩杀阈值
     SHIELDALLY          ← 云顶护卫羁绊
     ENERGYGAIN          ← 经济类辅助（阳光是这游戏唯一的货币）
     SLOWFIELD           ← 明日方舟的减速辅助
     OVERLOAD            ← 土豆兄弟的攻速堆叠
     CONVERT             ← 明日方舟的技力回复
     PURGE               ← 驱散类辅助
     LASTSTAND           ← 背水一战被动
     TIMEHEAL            ← 时序回溯式持续治疗
   ⚠️ 这 16 个一旦用满就没有空位了。再加机制必须换成第二个掩码字
   （参考僵尸侧的 RZ2_* 写法），不要在这里硬塞。 */
#define RG_T_ARMORBREAK  (1ull << 48) /* 破甲：命中后目标防御 -50% */
#define RG_T_MARK        (1ull << 49) /* 标记：被标记者受到伤害 +35% */
#define RG_T_CRIT        (1ull << 50) /* 暴击：25% 概率 3 倍伤害 */
#define RG_T_KNOCKBACK   (1ull << 51) /* 击退：命中把僵尸推开 */
#define RG_T_PULL        (1ull << 52) /* 拉拽：把僵尸拉向自己 */
#define RG_T_SUMMON      (1ull << 53) /* 召唤：定期放出仆从参战 */
#define RG_T_ORBITAL     (1ull << 54) /* 轨道轰炸：卫星定点打击 */
#define RG_T_EXECUTE     (1ull << 55) /* 处决：血量低于阈值直接斩杀 */
#define RG_T_SHIELDALLY  (1ull << 56) /* 护盾光环：范围内全队减伤 */
#define RG_T_ENERGYGAIN  (1ull << 57) /* 阳光光环：定期额外产出阳光 */
#define RG_T_SLOWFIELD   (1ull << 58) /* 减速场：半径内僵尸持续变慢 */
#define RG_T_OVERLOAD    (1ull << 59) /* 过载：连续命中叠加攻速 */
#define RG_T_CONVERT     (1ull << 60) /* 吸伤转能量：受伤转化为增伤 */
#define RG_T_PURGE       (1ull << 61) /* 净化：免疫减益并驱散邻格 */
#define RG_T_LASTSTAND   (1ull << 62) /* 濒死狂暴：血量越低伤害越高 */
#define RG_T_TIMEHEAL    (1ull << 63) /* 时序回溯：定期回复友军血量 */

/* 数据表里用的简写别名（保持表体可读，避免一行 200 字符） */
#define RG_T_RECOMBINE_SAFE  RG_T_RECOMBINE
#define RG_T_DODGE_SAFE      RG_T_DODGE
#define RG_T_EATSUN_SAFE     RG_T_EATSUN
#define RG_T_BOUNCE_SAFE     RG_T_BOUNCE5
#define RG_T_MAZEWALL_SAFE   RG_T_MAZEWALL
#define RG_T_WEATHER_SAFE    RG_T_WEATHER
#define RG_T_DRAWCARD_SAFE   RG_T_DRAWCARD
#define RG_T_ASSASSIN_SAFE   RG_T_ASSASSIN
#define RG_T_LULLABY_SAFE    RG_T_LULLABY
#define RG_T_CHI_SAFE        RG_T_CHI
#define RG_T_PLAGUE_SAFE     RG_T_PLAGUE
#define RG_T_BLACKHOLE_SAFE  RG_T_BLACKHOLE

#define RG_ALL_TRAITS    (0xFFFFFFFFFFFFFFFFull)

/* ---- 肉鸽植物：158 株 ----
      00~49   首批
      50~99   第二批创意扩展
      100~104 殿堂五尊（首批）
      105~107 始祖三尊（首批）
      108~127 殿堂二十尊（第三批，2026-09-21）
      128~157 始祖三十尊（第三批，2026-09-21） ----
   ⚠️ RG_PLANT_N 一改，下面三处会跟着变，必须一起确认：
      ① gSprRgPlant[RG_PLANT_N] 贴图槽位（新槽按 rgplant_NN 命名，
         缺图会静默退回基座贴图，不会报错）；
      ② rgOwned() 是本局已解锁判定，跟着 NR 走，无需改；
      ③ 抽卡权重归一化用 RGQ_DRAFT_W + 实际数量，也自动跟着走。 */
#define RG_PLANT_N 158

/* ---- 三选一最低出牌档位（低于它的肉鸽植物不上牌） ----
   用户要求：把「普通」档从三选一里去掉，让其他档位的概率整体上升。

   ⚠️ 实现方式是**过滤发牌入口**，不是把条目从 rgPlants[] 里删掉：
      删掉 14 条会让后面所有索引整体前移，而贴图是按下标命名的
      （rgplant_NN.bmp，见 gSprRgPlant 的加载循环），
      索引一错，剩下的 91 株全部会贴上错误的图。
      （肉鸽植物不落盘，所以这里没有存档格式约束，只有贴图索引约束。）
   所以改这一个常量即可随时恢复或调整档位门槛。

   改这里的同时要改的只有一处：rgOpenDraft() 的候选池构造 —— 
   它是三选一唯一的取牌口。若要连遗物赠卡（rgGrantStarterCard）一起禁，
   在那边也加同一个判定即可，见该函数注释。 */
#define RG_DRAFT_MIN_TIER  RGQ_RARE

typedef struct {
    const wchar_t *name;
    unsigned char  tier;      /* RGQ_* */
    unsigned char  base;      /* PT_*：行为与贴图基座 */
    unsigned long long traits; /* RG_T_* 位掩码（64 位两字） */
    const wchar_t *desc;
    /* 阳光售价。**故意放在最后**：新增字段不会让上面 100 条
       老初始化式失效（C 里漏掉的成员自动置 0），省掉成片无意义改动。
       cost == 0 表示"没单独定价"，由 rgPlantCost() 按品级取 RGQ_COST 基准。
       ⚠️ 原来这里是 unsigned char（上限 255）：給殿堂五尊填 500 会被静默
          截断成 244（-Woverflow 有报但不致命），所以拓宽到 unsigned short。
          这个字段上限必须 ≥ rgPlantCost() 里 CLAMP 的上界（现为 10000），
          否则手填一个高价会悄无声息地变成另一个数。 */
    unsigned short cost;
    /* 单卡专属强度系数（百分比，100 = 基准；**0 视为 100**）。
       为什么要有它：`tier` 只能整档一起调（改 RGQ_DMG 会影响该档全部卡），
       而"30 张始祖彼此也该有强弱梯度"这件事只能落在单卡上。
       ⚠️ 2 亿字节的坑要避开：这个字段放**最后**，于是上面 108 条老初始化式
          一个都不用改（C 会把遗漏成员置 0），而 0 被解释成"基准"。
       只作用于植物侧：rgPlantAtkMul 用它，rgZombieHpMul 不碰它。 */
    unsigned short aspect;
} RgPlantDef;

/* ⚠️ 新增 aspect 字段后，老条目（没写这个字段）会触发
   -Wmissing-field-initializers。C 的语义是"遗漏成员置 0"，
   而我们把 0 定义成"基准强度"，所以这里的告警是**误报**。
   用 pragma 压掉，保住本项目的"零告警"基线（不然真告警会被淹没）。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const RgPlantDef rgPlants[RG_PLANT_N] = {
/* 00 */ { L"量子豌豆射手", RGQ_COMMON, PT_PEASHOOTER, RG_T_MULTILANE,
           L"同时在三条线路齐射，单发 2 倍基础伤害", 30 },
/* 01 */ { L"时停向日葵",   RGQ_RARE,   PT_SUNFLOWER,  RG_T_TIMEFREEZE,
           L"产出阳光时冻结本行僵尸，产量 4 倍", 55 },
/* 02 */ { L"酸蚀藤蔓",     RGQ_RARE,   PT_SPIKEWEED,  RG_T_CORRODE,
           L"覆盖整行，持续腐蚀护甲，无视 2 倍防御", 55 },
/* 03 */ { L"黑洞磁力菇",   RGQ_EPIC,   PT_MAGNET,     RG_T_DEVOURBUL,
           L"吃掉飞来的子弹转化为阳光，同时自产少量阳光", 85 },
/* 04 */ { L"镜像墙坚果",   RGQ_RARE,   PT_WALLNUT,    RG_T_MIRROR,
           L"血量 4 倍，并持续自我修复（每脉冲回 5%）", 55 },
/* 05 */ { L"贪吃蛇大嘴花", RGQ_EPIC,   PT_TANGLEKELP, RG_T_GROWEAT,
           L"吞掉附近的僵尸后体型变大（上限 2.5 倍）", 85 },
/* 06 */ { L"区块链土豆雷", RGQ_LEGEND, PT_POTATOMINE, RG_T_CHAINBOOM,
           L"延时引爆，并向邻格分叉链式反应", 120 },
/* 07 */ { L"概率坩埚",     RGQ_MYTH,   PT_PEASHOOTER, RG_T_GAMBLEDMG,
           L"50% 概率打出 10 倍伤害，否则只有 1 倍", 155 },
/* 08 */ { L"病毒向日葵",   RGQ_RARE,   PT_SUNFLOWER,  RG_T_SHARESUN,
           L"感染相邻植物，全行共享阳光产出", 55 },
/* 09 */ { L"重力玉米",     RGQ_EPIC,   PT_KERNELPULT, RG_T_GRAVITY,
           L"子弹坠落造成范围眩晕，半径 1.5 倍", 85 },
/* 10 */ { L"抽卡樱桃",     RGQ_MYTH,   PT_CHERRY,     RG_T_DROPCARD,
           L"爆炸后额外掉落大量阳光与金气", 155 },
/* 11 */ { L"贷款高坚果",   RGQ_COMMON, PT_WALLNUT,    RG_T_LOAN,
           L"血量 2 倍，但每波结束扣 25 阳光", 30 },
/* 12 */ { L"蜘蛛丝缠绕",   RGQ_RARE,   PT_SPIKEWEED,  RG_T_WEBSLOW,
           L"大幅减速，并在原位留下蛛网陷阱", 55 },
/* 13 */ { L"爬墙仙人掌",   RGQ_EPIC,   PT_CACTUS,     RG_T_ROOFWALK,
           L"沿屋顶逐格移动，穿刺针连射", 85 },
/* 14 */ { L"时间回溯豌豆", RGQ_MYTH,   PT_PEASHOOTER, RG_T_REWIND,
           L"命中后把目标拉回 3 秒前的位置", 155 },
/* 15 */ { L"量子纠缠双胞", RGQ_LEGEND, PT_REPEATER,   RG_T_DOUBLEBODY,
           L"在敌后生成纠缠分身，前后夹击", 120 },
/* 16 */ { L"噪音向日葵",   RGQ_EPIC,   PT_SUNFLOWER,  RG_T_CONFUSE,
           L"嘈杂声使范围内僵尸混乱走位", 85 },
/* 17 */ { L"暗物质冰冻",   RGQ_LEGEND, PT_SNOWPEA,    RG_T_ICERESOURCE,
           L"被冻住的僵尸会碎裂成阳光资源", 120 },
/* 18 */ { L"自爆蘑菇",     RGQ_EPIC,   PT_ICESHROOM,  RG_T_SELFDESTRUCT,
           L"血量低于 30% 时自爆并引燃邻格", 85 },
/* 19 */ { L"模因传播葵",   RGQ_RARE,   PT_SUNFLOWER,  RG_T_MEMESPREAD,
           L"被僵尸触碰时传播阳光并造成减速", 55 },
/* 20 */ { L"跨界外星射手", RGQ_LEGEND, PT_LASERBEAN,  RG_T_LASERCUT,
           L"激光切割整行，完全无视护甲", 120 },
/* 21 */ { L"赌徒豌豆",     RGQ_MYTH,   PT_PEASHOOTER, RG_T_GAMBLERISK,
           L"每发射一次赌博：伤害翻倍或归零", 155 },
/* 22 */ { L"爬格子藤蔓",   RGQ_RARE,   PT_SPIKEWEED,  RG_T_STEPMOVE,
           L"每次攻击后向前移动一格", 55 },
/* 23 */ { L"代码向日葵",   RGQ_EPIC,   PT_SUNFLOWER,  RG_T_DIGITRAIN,
           L"生成数字雨屏障，抵挡并反弹伤害", 85 },
/* 24 */ { L"音乐节拍豌豆", RGQ_RARE,   PT_REPEATER,   RG_T_RHYTHM,
           L"随节拍不断增强伤害，最高 3 倍", 55 },
/* 25 */ { L"像素崩坏射手", RGQ_MYTH,   PT_PEASHOOTER, RG_T_PIXELCRUSH,
           L"命中时 20% 概率使僵尸像素化即死", 155 },
/* 26 */ { L"黑客磁力菇",   RGQ_LEGEND, PT_MAGNET,     RG_T_HIJACK,
           L"劫持僵尸武器，转而攻击同类", 120 },
/* 27 */ { L"塔防合成人",   RGQ_ULTRA,  PT_MELONPULT,  RG_T_FUSEPLANT,
           L"吞噬相邻的那株植物，一次性强化自身（上限 2.5 倍）", 190 },
/* 28 */ { L"随机传送茄",   RGQ_COMMON, PT_STARFRUIT,  RG_T_BLINK,
           L"随机传送到任意一格，星弹五向散射", 30 },
/* 29 */ { L"弹幕向日葵",   RGQ_EPIC,   PT_SUNFLOWER,  RG_T_PETALBARRAGE,
           L"产出阳光的同时向多个方向发射花瓣雨", 85 },
/* 30 */ { L"贪婪豌豆",     RGQ_LEGEND, PT_PEASHOOTER, RG_T_LIFESTEAL,
           L"吸取僵尸血量回复自身与同格植物", 120 },
/* 31 */ { L"诅咒樱桃",     RGQ_EPIC,   PT_CHERRY,     RG_T_CURSE,
           L"爆炸后留下诅咒区域，持续掉血", 85 },
/* 32 */ { L"镜像大嘴",     RGQ_LEGEND, PT_TANGLEKELP, RG_T_MIRROR | RG_T_GROWEAT | RG_T_LIFESTEAL,
           L"吞噬后复制该僵尸的能力与体型", 120 },
/* 33 */ { L"虚拟现实坚果", RGQ_RARE,   PT_WALLNUT,    RG_T_MIRROR | RG_T_DIGITRAIN,
           L"假血量欺骗僵尸，被啃穿后重算一次", 55 },
/* 34 */ { L"烈焰舞王葵",   RGQ_EPIC,   PT_SUNFLOWER,  RG_T_RHYTHM | RG_T_SHARESUN | RG_T_TIMEFREEZE,
           L"阳光伴随舞蹈光环，范围增伤并加速", 85 },
/* 35 */ { L"量子纠缠樱桃", RGQ_MYTH,   PT_CHERRY,     RG_T_BLINK | RG_T_CHAINBOOM | RG_T_RECOMBINE_SAFE,
           L"炸开后传送并在别处重新聚合爆炸", 155 },
/* 36 */ { L"生化豌豆",     RGQ_MYTH,   PT_PEASHOOTER, RG_T_INFECTTURN | RG_T_PETALBARRAGE | RG_T_GRAVITY,
           L"感染僵尸，使其转而攻击同类", 155 },
/* 37 */ { L"逆向时间菇",   RGQ_LEGEND, PT_SUNFLOWER,  RG_T_LIFESTEAL | RG_T_MIRROR | RG_T_LOAN | RG_T_DIGITRAIN,
           L"持续恢复范围内全部植物的血量", 120 },
/* 38 */ { L"概率墙",       RGQ_RARE,   PT_WALLNUT,    RG_T_MIRROR | RG_T_DODGE_SAFE,
           L"高血量并持续自我修复，硬吃正面伤害", 55 },
/* 39 */ { L"贪吃葵",       RGQ_COMMON, PT_SUNFLOWER,  RG_T_EATSUN_SAFE,
           L"吸收场上的僵尸掉落与阳光，加速产出", 30 },
/* 40 */ { L"弹跳玉米",     RGQ_RARE,   PT_KERNELPULT, RG_T_BOUNCE_SAFE,
           L"子弹在僵尸之间反弹 5 次，每次 80% 伤害", 55 },
/* 41 */ { L"迷宫坚果",     RGQ_EPIC,   PT_WALLNUT,    RG_T_MAZEWALL_SAFE,
           L"高血量城墙，持续自我修复，挡在最前面", 85 },
/* 42 */ { L"随机天气葵",   RGQ_MYTH,   PT_SUNFLOWER,  RG_T_WEATHER_SAFE | RG_T_PETALBARRAGE | RG_T_SHARESUN,
           L"持续飘落随机天气特效，改变场景氛围", 155 },
/* 43 */ { L"卡牌抽取射手", RGQ_EPIC,   PT_PEASHOOTER, RG_T_DRAWCARD_SAFE | RG_T_GAMBLEDMG,
           L"每次脉冲有概率额外产出大量阳光", 85 },
/* 44 */ { L"暗杀豌豆",     RGQ_MYTH,   PT_PEASHOOTER, RG_T_ASSASSIN_SAFE | RG_T_LASERCUT,
           L"潜行蓄力，脉冲伤害提升到 18 倍", 155 },
/* 45 */ { L"音乐盒樱桃",   RGQ_LEGEND, PT_CHERRY,     RG_T_LULLABY_SAFE | RG_T_CONFUSE,
           L"爆炸播放摇篮曲，使范围内僵尸沉睡", 120 },
/* 46 */ { L"跨服联动功夫葵", RGQ_RARE, PT_SUNFLOWER,  RG_T_CHI_SAFE,
           L"阳光化为内力，为相邻植物提供护盾", 55 },
/* 47 */ { L"病原体豌豆",   RGQ_EPIC,   PT_PEASHOOTER, RG_T_PLAGUE_SAFE | RG_T_CORRODE,
           L"使僵尸中毒，毒素在僵尸之间扩散", 85 },
/* 48 */ { L"吞噬黑洞葵",   RGQ_LEGEND, PT_SUNFLOWER,  RG_T_BLACKHOLE_SAFE | RG_T_DEVOURBUL,
           L"吞噬阳光与来弹，大幅加速自身产出", 120 },
/* 49 */ { L"终极奇葩",     RGQ_ULTRA,  PT_MELONPULT,  RG_ALL_TRAITS,
           L"同时拥有以上全部 99 株植物的所有能力", 200 },

/* ========================================================================
   第二批 · 50 株创意植物（索引 50~99）
   ------------------------------------------------------------------------
   用户要求：参考市面成功游戏机制大幅丰富能力，并为**每株**明确设定
   品级 / 价格 / 伤害数值 / 伤害机制 / 攻击范围 / 攻速频率 / 增益 / 减益。
   每条顶上的注释块就是那份完整属性表（code 侧的 desc 只留一句玩家看得懂的话，
   卡面空间有限）。
   数值基准（普通植物 = 1.0）：品质倍率
     伤害 2/4/7/11/15/20× · 攻速 1.0/1.0/1.15/1.3/1.45/1.6× · 范围 1.0/1.2/1.5/1.9/2.4/3.0×
   所以注释里的"伤害"是基座基础伤害 × 品质倍率后的落点。
   机制取材：杀戮尖塔（易伤/标记）、土豆兄弟（暴击/攻速堆叠）、
   皇室战争与明日方舟（推拉位移/减速辅助/技力）、泰拉瑞亚（哨兵召唤）、
   云顶之弈（羁绊光环）、吸血鬼幸存者（天降系）、MOBA（斩杀阈值）。
   ======================================================================== */

/* ── 位移控制系（推拉改节奏）── */
/* 50 弹簧豆 / 伤害 90 / 机制 落地弹跳把踏入者顶退 / 范围 单格触发+击退0.6格
   攻速 2.4s 复位 / 增益 无 / 减益 强制位移 */
/* 50 */ { L"弹簧豆",       RGQ_COMMON, PT_POTATOMINE, RG_T_KNOCKBACK,
           L"触发弹跳，把踏入的僵尸顶退半格", 28 },
/* 51 铁爪藤 / 伤害 268 / 机制 钩住最近目标拉近并持续绞杀 / 范围 前方3格
   攻速 1.9s / 增益 无 / 减益 拉拽位移+定身0.4s */
/* 51 */ { L"铁爪藤",       RGQ_RARE,   PT_TANGLEKELP, RG_T_PULL | RG_T_WEBSLOW,
           L"钩住最前僵尸往回拖，拖回一步再绞杀", 62 },
/* 52 磁力绞盘 / 伤害 686 / 机制 磁场拉拽+卸甲 / 范围 本行2.4倍
   攻速 1.7s / 增益 无 / 减益 拉拽+防御-50% */
/* 52 */ { L"磁力绞盘",     RGQ_EPIC,   PT_MAGNET,     RG_T_PULL | RG_T_ARMORBREAK,
           L"磁力把僵尸吸近，同时剥掉它一半护甲", 98 },
/* 53 冲压活塞 / 伤害 308 / 机制 蓄力冲压，命中暴击并击退 / 范围 单格
   攻速 2.2s / 增益 暴击率25% / 减益 击退1格 */
/* 53 */ { L"冲压活塞",     RGQ_RARE,   PT_SPIKEWEED,  RG_T_KNOCKBACK | RG_T_CRIT,
           L"蓄力冲压，暴击时把僵尸整格推开", 72 },

/* ── 破甲 / 标记系（杀戮尖塔的易伤）── */
/* 54 酸液喷头 / 伤害 140 / 机制 喷射酸雾，持续破甲 / 范围 前方2格扇形
   攻速 1.6s / 增益 无 / 减益 防御-50% 持续4s */
/* 54 */ { L"酸液喷头",     RGQ_COMMON, PT_FUME,       RG_T_ARMORBREAK | RG_T_CORRODE,
           L"喷酸破甲，被打中的僵尸护甲减半", 35 },
/* 55 猎手指针 / 伤害 264 / 机制 标记目标，全队对其增伤 / 范围 全行
   攻速 1.5s / 增益 全队对该目标+35%伤害 / 减益 被标记4s */
/* 55 */ { L"猎手指针",     RGQ_RARE,   PT_CACTUS,     RG_T_MARK | RG_T_CRIT,
           L"标记最痛的目标，全队对它多打三成", 66 },
/* 56 审判天平 / 伤害 1650 / 机制 标记+处决低血目标 / 范围 全屏2.4倍
   攻速 2.0s / 增益 处决阈值22% / 减益 标记4s */
/* 56 */ { L"审判天平",     RGQ_LEGEND, PT_STARFRUIT,  RG_T_MARK | RG_T_EXECUTE,
           L"给残血僵尸定罪，血量低于两成立即处决", 132 },
/* 57 剥离之刃 / 伤害 714 / 机制 破甲后追加标记，越打越痛 / 范围 本行穿透
   攻速 1.45s / 增益 连续命中叠伤 / 减益 破甲+标记 */
/* 57 */ { L"剥离之刃",     RGQ_EPIC,   PT_SPLITPEA,   RG_T_ARMORBREAK | RG_T_MARK,
           L"先剥甲再标记，团队输出跟着一起涨", 102 },

/* ── 暴击 / 斩杀系（MOBA 阈值）── */
/* 58 幸运四叶草 / 伤害 160 / 机制 高暴击率单发 / 范围 单行
   攻速 1.0s / 增益 暴击率40% / 减益 无 */
/* 58 */ { L"幸运四叶草",   RGQ_RARE,   PT_PEASHOOTER, RG_T_CRIT | RG_T_GAMBLEDMG,
           L"四叶草加持，暴击率高到离谱", 58 },
/* 59 处刑者 / 伤害 1650 / 机制 处决低血+暴击叠加 / 范围 全行2.4倍
   攻速 1.6s / 增益 处决阈值25% / 减益 无 */
/* 59 */ { L"处刑者",       RGQ_LEGEND, PT_CACTUS,     RG_T_EXECUTE | RG_T_CRIT,
           L"专治残血，四分之一血以下一击带走", 138 },
/* 60 断头台 / 伤害 3000 / 机制 破甲+标记+处决三件套 / 范围 全屏3倍
   攻速 1.45s / 增益 处决阈值30% / 减益 破甲+标记全程挂满 */
/* 60 */ { L"断头台",       RGQ_MYTH,   PT_MELONPULT,  RG_T_EXECUTE | RG_T_ARMORBREAK | RG_T_CRIT,
           L"破甲、标记、斩首一条龙，残血一律不留", 168 },

/* ── 光环 / 辅助系（云顶羁绊）── */
/* 61 共鸣花 / 伤害 60 / 机制 无攻击，为相邻植物增伤 / 范围 周围8格
   攻速 持续光环 / 增益 邻近植物+30%伤害 / 减益 无 */
/* 61 */ { L"共鸣花",       RGQ_COMMON, PT_SUNFLOWER,  RG_T_AURABUFF,
           L"自己不打架，让身边每一株都打得更重", 30 },
/* 62 守望藤 / 伤害 40 / 机制 无攻击，为范围内友军提供减伤护盾 / 范围 半径2格
   攻速 持续光环 / 增益 友军减伤30% / 减益 无 */
/* 62 */ { L"守望藤",       RGQ_RARE,   PT_WALLNUT,    RG_T_SHIELDALLY,
           L"展开护盾结界，范围内植物受到的伤害减少", 60 },
/* 63 圣殿钟 / 伤害 70 / 机制 无攻击，护盾+持续回血 / 范围 半径2.5格
   攻速 恢复脉冲 1.8s / 增益 减伤+每秒回血 / 减益 无 */
/* 63 */ { L"圣殿钟",       RGQ_EPIC,   PT_TWINFLOWER, RG_T_SHIELDALLY | RG_T_TIMEHEAL,
           L"钟声一响，周围植物回血又减伤", 108 },
/* 64 丰收之角 / 伤害 75 / 机制 无攻击，定期额外产阳光 / 范围 自身
   攻速 产出 4.5s / 增益 额外+50阳光/次 / 减益 无 */
/* 64 */ { L"丰收之角",     RGQ_RARE,   PT_SUNFLOWER,  RG_T_ENERGYGAIN,
           L"稳定额外产出阳光，滚经济就靠它", 52 },
/* 65 黄金律 / 伤害 55 / 机制 无攻击，产阳光并给全队增伤 / 范围 全场
   攻速 产出 4.0s / 增益 全局+20%伤害 / 减益 无 */
/* 65 */ { L"黄金律",       RGQ_LEGEND, PT_GOLDMAGNET, RG_T_ENERGYGAIN | RG_T_AURABUFF,
           L"每产一次阳光，全场植物强一分", 128 },
/* 66 净化之泉 / 伤害 85 / 机制 无攻击，清除邻格减益并回血 / 范围 半径2格
   攻速 净化脉冲 2.4s / 增益 免疫减益+回血 / 减益 无 */
/* 66 */ { L"净化之泉",     RGQ_EPIC,   PT_SUNFLOWER,  RG_T_PURGE | RG_T_TIMEHEAL,
           L"泉水洗掉队友身上的所有负面状态", 105 },

/* ── 减速场所系（明日方舟减速辅助）── */
/* 67 寒霜地衣 / 伤害 80 / 机制 无攻击，地面持续减速 / 范围 自身格+邻格
   攻速 持续场 / 增益 无 / 减益 移速-35% */
/* 67 */ { L"寒霜地衣",     RGQ_COMMON, PT_SPIKEWEED,  RG_T_SLOWFIELD,
           L"铺一层霜，踩上来的僵尸全程走不快", 32 },
/* 68 沼泽苔 / 伤害 264 / 机制 减速场+蛛网定身 / 范围 半径1.2格
   攻速 持续场 / 增益 无 / 减益 移速-45%+周期定身 */
/* 68 */ { L"沼泽苔",       RGQ_RARE,   PT_SPIKEWEED,  RG_T_SLOWFIELD | RG_T_WEBSLOW,
           L"又黏又滑，僵尸在这儿迈不动腿", 56 },
/* 69 极地苔原 / 伤害 1650 / 机制 大范围减速+冰冻转资源 / 范围 半径1.9格
   攻速 持续场 / 增益 冰冻僵尸掉落阳光 / 减益 移速-60% */
/* 69 */ { L"极地苔原",     RGQ_LEGEND, PT_WINTERMELON, RG_T_SLOWFIELD | RG_T_ICERESOURCE,
           L"冻住的一片地，僵尸碎了还能拣阳光", 142 },
/* 70 停滞力场 / 伤害 3000 / 机制 力场停滞+重力压制 / 范围 半径2.4格
   攻速 持续场 / 增益 无 / 减益 几乎无法移动 */
/* 70 */ { L"停滞力场",     RGQ_MYTH,   PT_GLOOM,      RG_T_SLOWFIELD | RG_T_GRAVITY,
           L"范围内僵尸近乎凝固，还走不稳", 162 },

/* ── 攻速 / 过载系（土豆兄弟）── */
/* 71 齿轮豌豆 / 伤害 60 / 机制 连续命中逐发加速 / 范围 单行
   攻速 1.0s 起，每层+12%，最多5层 / 增益 攻速叠加 / 减益 无 */
/* 71 */ { L"齿轮豌豆",     RGQ_COMMON, PT_REPEATER,   RG_T_OVERLOAD,
           L"越打越快，停火后转速才慢慢回落", 36 },
/* 72 蜂鸟射手 / 伤害 714 / 机制 超高速连射+暴击 / 范围 单行
   攻速 1.15s 起，叠满2.5倍 / 增益 攻速+暴击 / 减益 无 */
/* 72 */ { L"蜂鸟射手",     RGQ_EPIC,   PT_THREEPEATER, RG_T_OVERLOAD | RG_T_CRIT,
           L"翅膀不停，射速叠起来像下雨", 108 },
/* 73 超频核心 / 伤害 2400 / 机制 过载+吸伤转化+暴击 / 范围 全行
   攻速 1.45s 起，叠满3倍 / 增益 攻速+受伤增伤 / 减益 无 */
/* 73 */ { L"超频核心",     RGQ_MYTH,   PT_REPEATER,   RG_T_OVERLOAD | RG_T_CONVERT | RG_T_CRIT,
           L"被打得越狠，转得越快、打得越疼", 172 },

/* ── 召唤系（泰拉瑞亚哨兵 / 云顶召唤使）── */
/* 74 蜂巢塔 / 伤害 80 / 机制 定期放出小蜜蜂扑咬 / 范围 全行游走
   攻速 召唤 3.0s，蜂群独立结算 / 增益 场上同时3只蜂 / 减益 无 */
/* 74 */ { L"蜂巢塔",       RGQ_COMMON, PT_PEASHOOTER, RG_T_SUMMON,
           L"每隔几秒放出一只小蜜蜂去咬人", 42 },
/* 75 蚁群巢 / 伤害 264 / 机制 召唤啃咬型蚁群+持续腐蚀 / 范围 全场
   攻速 召唤 2.6s / 增益 蚁群可叠加 / 减益 护甲腐蚀 */
/* 75 */ { L"蚁群巢",       RGQ_RARE,   PT_SPIKEWEED,  RG_T_SUMMON | RG_T_CORRODE,
           L"蚁群铺出去啃，还会啃烂护甲", 72 },
/* 76 蛛后 / 伤害 770 / 机制 召唤蛛群+织网定身 / 范围 全场
   攻速 召唤 2.4s / 增益 蛛群独立作战 / 减益 蛛网定身 */
/* 76 */ { L"蛛后",         RGQ_EPIC,   PT_TANGLEKELP, RG_T_SUMMON | RG_T_WEBSLOW,
           L"不断产崽，蛛网把僵尸黏在原地", 112 },
/* 77 亡灵花园 / 伤害 1650 / 机制 召唤亡灵仆从，击杀后感染同类 / 范围 全场
   攻速 召唤 2.2s / 增益 仆从越杀越多 / 减益 感染扩散 */
/* 77 */ { L"亡灵花园",     RGQ_LEGEND, PT_HYPNOSHROOM, RG_T_SUMMON | RG_T_INFECTTURN,
           L"仆从打死的僵尸会爬起来替你打", 148 },

/* ── 轨道 / 天降系（吸血鬼幸存者）── */
/* 78 星轨信标 / 伤害 300 / 机制 标记坐标后卫星定点轰炸 / 范围 全场3格
   攻速 1.8s 一次轨道打击 / 增益 无 / 减益 落点范围伤害 */
/* 78 */ { L"星轨信标",     RGQ_RARE,   PT_STARFRUIT,  RG_T_ORBITAL,
           L"锁定最密的僵尸群，从天而降一发", 78 },
/* 79 陨星召唤 / 伤害 805 / 机制 轨道陨石+落点重力眩晕 / 范围 全场2.4格
   攻速 2.0s / 增益 无 / 减益 范围眩晕 */
/* 79 */ { L"陨星召唤",     RGQ_EPIC,   PT_MELONPULT,  RG_T_ORBITAL | RG_T_GRAVITY,
           L"陨石砸下来，砸中的还要晕半天", 118 },
/* 80 天穹之眼 / 伤害 3000 / 机制 轨道齐射+标记+暴击 / 范围 全场3格
   攻速 1.6s / 增益 暴击率25% / 减益 被标记者增伤 */
/* 80 */ { L"天穹之眼",     RGQ_MYTH,   PT_CATTAIL,    RG_T_ORBITAL | RG_T_MARK | RG_T_CRIT,
           L"卫星扫描标记，然后一整轮齐射", 178 },

/* ── 吸血 / 成长系（以撒 / 土豆兄弟）── */
/* 81 血蔷薇 / 伤害 160 / 机制 造成伤害按比例回血 / 范围 单行
   攻速 1.0s / 增益 回复所造成伤害的12% / 减益 无 */
/* 81 */ { L"血蔷薇",       RGQ_COMMON, PT_SPIKEWEED,  RG_T_LIFESTEAL,
           L"靠伤害养自己，越打越不容易死", 42 },
/* 82 食人花圃 / 伤害 272 / 机制 吞噬僵尸永久成长+吸血 / 范围 单格
   攻速 吞噬 2.5s / 增益 每次吞噬体型与伤害+8% / 减益 无 */
/* 82 */ { L"食人花圃",     RGQ_RARE,   PT_TANGLEKELP, RG_T_LIFESTEAL | RG_T_GROWEAT,
           L"每吃一只就长大一点，还顺带回血", 68 },
/* 83 永生藤 / 伤害 770 / 机制 吸血+吃成长+持续自愈 / 范围 前方2格
   攻速 1.5s / 增益 三重回血 / 减益 无 */
/* 83 */ { L"永生藤",       RGQ_EPIC,   PT_TANGLEKELP, RG_T_LIFESTEAL | RG_T_TIMEHEAL | RG_T_GROWEAT,
           L"打不死的藤，边啃边长边回血", 112 },

/* ── 经济系 ── */
/* 84 阳光熔炉 / 伤害 60 / 机制 无攻击，阳光产出与邻格共享 / 范围 同排
   攻速 产出 5.0s / 增益 同排共享产出 / 减益 无 */
/* 84 */ { L"阳光熔炉",     RGQ_COMMON, PT_SUNFLOWER,  RG_T_SHARESUN,
           L"产出的阳光同排植物一起分", 26 },
/* 85 光能汇聚器 / 伤害 85 / 机制 无攻击，共享产出+额外阳光 / 范围 全场
   攻速 产出 4.2s / 增益 共享+额外产出 / 减益 无 */
/* 85 */ { L"光能汇聚器",   RGQ_EPIC,   PT_TWINFLOWER, RG_T_SHARESUN | RG_T_ENERGYGAIN,
           L"把全场的阳光收拢再放大发回去", 98 },
/* 86 阳光银行 / 伤害 65 / 机制 无攻击，高额产出但每波结算扣手续费 / 范围 全场
   攻速 产出 3.5s / 增益 产出最高 / 减益 每波-25 阳光 */
/* 86 */ { L"阳光银行",     RGQ_LEGEND, PT_GOLDMAGNET, RG_T_ENERGYGAIN | RG_T_LOAN | RG_T_SHARESUN,
           L"存得多取得多，就是每波要交点利息", 122 },

/* ── 陷阱 / 地形系 ── */
/* 87 尖刺陷阱 / 伤害 160 / 机制 地面持续刺伤+腐蚀 / 范围 自身格
   攻速 持续 / 增益 无 / 减益 破甲 */
/* 87 */ { L"尖刺陷阱",     RGQ_COMMON, PT_SPIKEWEED,  RG_T_CORRODE,
           L"简单直接，站上去就一直掉血", 30 },
/* 88 焦油坑 / 伤害 272 / 机制 减速+诅咒持续掉血 / 范围 自身格+邻格
   攻速 持续 / 增益 无 / 减益 移速-40%+诅咒 */
/* 88 */ { L"焦油坑",       RGQ_RARE,   PT_SPIKEWEED,  RG_T_SLOWFIELD | RG_T_CURSE,
           L"踩进来就陷住，还带一身诅咒", 60 },
/* 89 熔岩裂隙 / 伤害 770 / 机制 地面灼烧+诅咒+破甲 / 范围 半径1.5格
   攻速 持续 / 增益 无 / 减益 三重重伤 */
/* 89 */ { L"熔岩裂隙",     RGQ_EPIC,   PT_SPIKEWEED,  RG_T_CURSE | RG_T_CORRODE,
           L"地缝里冒岩浆，烫伤还带破甲", 100 },

/* ── 弹幕 / 散射系 ── */
/* 90 散弹向日葵 / 伤害 90 / 机制 一次打出多方向花瓣弹 / 范围 五向散射
   攻速 1.2s / 增益 无 / 减益 无 */
/* 90 */ { L"散弹向日葵",   RGQ_COMMON, PT_STARFRUIT,  RG_T_PETALBARRAGE,
           L"一发五瓣，打散抱团的僵尸", 46 },
/* 91 回旋镖藤 / 伤害 272 / 机制 子弹来回弹射 5 次 / 范围 全行
   攻速 1.3s / 增益 无 / 减益 无 */
/* 91 */ { L"回旋镖藤",     RGQ_RARE,   PT_BLOOMERANG, RG_T_BOUNCE5,
           L"丢出去还会回来，一路撞到底", 66 },
/* 92 弹跳弹珠 / 伤害 805 / 机制 散射+弹射双重叠加 / 范围 全场
   攻速 1.15s / 增益 无 / 减益 无 */
/* 92 */ { L"弹跳弹珠",     RGQ_EPIC,   PT_STARFRUIT,  RG_T_BOUNCE5 | RG_T_PETALBARRAGE,
           L"弹珠在场上来回乱窜，专治扎堆", 106 },

/* ── 特殊机制系 ── */
/* 93 反射棱镜 / 伤害 805 / 机制 激光穿透+折射弹射 / 范围 全行穿透
   攻速 1.3s / 增益 无 / 减益 无视护甲 */
/* 93 */ { L"反射棱镜",     RGQ_EPIC,   PT_LASERBEAN,  RG_T_LASERCUT | RG_T_BOUNCE5,
           L"激光折来折去，一条线穿到最深处", 118 },
/* 94 相位花 / 伤害 700 / 机制 概率闪避+镜像分身分担 / 范围 自身
   攻速 被动 / 增益 闪避35% / 减益 无 */
/* 94 */ { L"相位花",       RGQ_EPIC,   PT_SUNFLOWER,  RG_T_DODGE | RG_T_MIRROR,
           L"相位闪烁：持续自我修复，极难被击倒", 108 },
/* 95 荆棘王冠 / 伤害 1650 / 机制 高血量反伤+荆棘反弹 / 范围 自身格
   攻速 触发式 / 增益 受击反伤 / 减益 无 */
/* 95 */ { L"荆棘王冠",     RGQ_LEGEND, PT_WALLNUT,    RG_T_DODGE | RG_T_LIFESTEAL | RG_T_MIRROR,
           L"谁敢啃它就等着挨扎，血还越回越多", 132 },
/* 96 濒死之花 / 伤害 1650 / 机制 血量越低伤害越高+护盾光环 / 范围 半径1.9格
   攻速 1.6s / 增益 残血时伤害翻数倍 / 减益 无 */
/* 96 */ { L"濒死之花",     RGQ_LEGEND, PT_WALLNUT,    RG_T_LASTSTAND | RG_T_SHIELDALLY,
           L"越接近死亡打得越狠，全队还跟着硬", 125 },
/* 97 时间领主 / 伤害 3000 / 机制 时停+回溯+持续回复 / 范围 全场
   攻速 时停脉冲 2.4s / 增益 全场冻结+回血 / 减益 无 */
/* 97 */ { L"时间领主",     RGQ_ULTRA,  PT_SUNFLOWER,  RG_T_TIMEFREEZE | RG_T_REWIND | RG_T_TIMEHEAL,
           L"冻结整条时间线，队友趁机把血回满", 195 },
/* 98 创世树 / 伤害 2200 / 机制 吞噬邻格植物融合能力+成长+共享产出 / 范围 全场
   攻速 2.0s / 增益 融合全部能力 / 减益 无 */
/* 98 */ { L"创世树",       RGQ_ULTRA,  PT_SUNFLOWER,  RG_T_FUSEPLANT | RG_T_GROWEAT | RG_T_SHARESUN | RG_T_AURABUFF,
           L"吞掉身边的一株植物，立即强化自身（上限 2.5 倍）", 198 },
/* 99 万源归一 / 伤害 4000 / 机制 集齐第二批全部机制 / 范围 全场三倍
   攻速 1.6s / 增益 全部增益叠加 / 减益 全部减益叠加 */
/* 99 */ { L"万源归一",     RGQ_ULTRA,  PT_MELONPULT,  RG_ALL_TRAITS,
           L"第二批的所有机制集于一身", 200 },

/* ---- 殿堂五尊（100~104）：新的最高档 RGQ_HALL ----
   伤害 100×（传奇 20× 的 5 倍），频率与范围刻意停在传奇的 1.60 / 3.00
   不动 —— 三项再一起放大会从「5 倍」膨胀到 25 倍以上。
   能力位刻意避开会让植株乱跑或自伤友军的几条（FUSEPLANT / STEPMOVE /
   ROOFWALK / BLINK / LOAN / GAMBLERISK / SELFDESTRUCT）：
   殿堂是「站定输出的核心」，不是整活道具。5 株基座各不相同，
   好让 5 张专属贴图和行为手感的差异真的看得出来。 */
/* 100 */ { L"终末裁决", RGQ_HALL, PT_COBCANNON,
            RG_T_MULTILANE | RG_T_ORBITAL | RG_T_EXECUTE | RG_T_ARMORBREAK |
            RG_T_CRIT | RG_T_GRAVITY | RG_T_KNOCKBACK | RG_T_AURABUFF |
            RG_T_OVERLOAD | RG_T_PIXELCRUSH,
            L"每发炮弹即是一次处决判定；落点轨道轰炸，命中破甲并击退，全员共享增伤光环",
            500 },
/* 101 */ { L"永恒霜狱", RGQ_HALL, PT_WINTERMELON,
            RG_T_MULTILANE | RG_T_SLOWFIELD | RG_T_ICERESOURCE | RG_T_TIMEFREEZE |
            RG_T_CORRODE | RG_T_WEBSLOW | RG_T_SHIELDALLY | RG_T_CURSE |
            RG_T_LASTSTAND | RG_T_CHI,
            L"半径内僵尸持续减速并被蛛网粘滞，冰冻单位转化为资源，全队获得减伤护盾",
            500 },
/* 102 */ { L"周天星斗", RGQ_HALL, PT_STARFRUIT,
            RG_T_MULTILANE | RG_T_PETALBARRAGE | RG_T_ORBITAL | RG_T_RHYTHM |
            RG_T_GAMBLEDMG | RG_T_LIFESTEAL | RG_T_BLACKHOLE | RG_T_SUMMON |
            RG_T_PURGE | RG_T_TIMEHEAL,
            L"五个方向同时倾泻花瓣弹幕，随节拍递增伤害，吸血续航并定期召唤星仆",
            500 },
/* 103 */ { L"曦光裁决", RGQ_HALL, PT_LASERBEAN,
            RG_T_MULTILANE | RG_T_LASERCUT | RG_T_ARMORBREAK | RG_T_PIXELCRUSH |
            RG_T_EXECUTE | RG_T_CRIT | RG_T_MARK | RG_T_OVERLOAD |
            RG_T_CONVERT | RG_T_PURGE,
            L"贯穿整行的光矛无视护甲，标记的猎物受到额外伤害，连续命中不断叠加攻速",
            500 },
/* 104 */ { L"天罗慑魔", RGQ_HALL, PT_CATTAIL,
            RG_T_MULTILANE | RG_T_EXECUTE | RG_T_MARK | RG_T_ARMORBREAK |
            RG_T_CRIT | RG_T_LIFESTEAL | RG_T_SUMMON | RG_T_LASTSTAND |
            RG_T_AURABUFF | RG_T_SHIELDALLY | RG_T_ENERGYGAIN | RG_T_TIMEHEAL,
            L"全屏无死角追踪弹，越是濒死伤害越高，同时给全场提供增伤、护盾与阳光三重光环",
            500 },

/* ---- 始祖三尊（105~107）：RGQ_PRIMORDIAL，全表唯三 ----
   用户定义：地刺 / 坚果 / 豌豆炮各一，伤害 1000×（其他植物的 1000 倍）、
   多种能力超级叠加、售价 10000 阳光，且**出现时必定改写背景与音乐**。

   · 能力"超级叠加"直接用 RG_ALL_TRAITS（64 位全开），
     不再逐个 OR —— 以后再加新人能力位也不会漏掉这三尊。
   · cost 字段是 unsigned short（上限 65535），10000 放得下；
     但 rgPlantCost() 的 CLAMP 上界必须一起抬到 10000，
     否则会被**静默**夹成 500（殿堂那次 uchar 截断是同一类坑）。
   · 三尊各踩一条极值，让"基座差异"看得出来：
     地刺吃范围（铺满半场的棘刺领域）、坚果吃体质、豌豆炮吃齐射密度。 */
/* 105 */ { L"始源棘原", RGQ_PRIMORDIAL, PT_SPIKEWEED, RG_ALL_TRAITS,
            L"4000 倍伤害。棘刺领域铺满半场，踏入者被持续撕裂；始祖在场时全场僵尸持续失血",
            10000 },
/* 106 */ { L"太初坚壁", RGQ_PRIMORDIAL, PT_WALLNUT, RG_ALL_TRAITS,
            L"4000 倍血量。承受一切并全额奉还，被摧毁时连根重生；始祖在场时全场僵尸持续失血",
            10000 },
/* 107 */ { L"混沌齐射", RGQ_PRIMORDIAL, PT_PEASHOOTER, RG_ALL_TRAITS,
            L"4000 倍伤害 × 3 倍攻速 × 8 倍范围。向所有方向倾泻创世种子，命中即湮灭整行",
            10000 },

    /* ================= 第三批：20 张殿堂 + 30 张始祖（2026-09-21）=================
       设计见 art/_cards_new.py（单一数据源）与 MECHANISMS.md 的卡牌体系章节。
       殿堂 = 流派核心，每张 8 条能力位构成一套构筑骨架；
       始祖 = 流派终局，RG_ALL_TRAITS（64 位全开）+ 各自专属 aspect（100~150）。
       ⚠️ 追加在**表尾**而不是插在中间：贴图按 rgplant_NN 下标命名，
          插中间会让后面所有索引前移、全员贴错图。 */
/* 108 */ { L"裂空弹幕", RGQ_HALL, PT_STARSHOOTER,
            RG_T_MULTILANE | RG_T_PETALBARRAGE | RG_T_BOUNCE5 | RG_T_CRIT | RG_T_MARK | RG_T_OVERLOAD | RG_T_RHYTHM | RG_T_KNOCKBACK,
            L"九向弹幕铺满整行，命中不断叠加攻速；暴击与标记让后续每一发都更痛", 500, 116 },
/* 109 */ { L"万钧重奏", RGQ_HALL, PT_QUARTZKERNEL,
            RG_T_MULTILANE | RG_T_CHAINBOOM | RG_T_ARMORBREAK | RG_T_CRIT | RG_T_GRAVITY | RG_T_KNOCKBACK | RG_T_LASTSTAND | RG_T_AURABUFF,
            L"超重石英弹齐射，落点二段连锁引爆；正面破甲并把整列僵尸持续向后压", 500, 116 },
/* 110 */ { L"荒芜毒沼", RGQ_HALL, PT_POISNOVINE,
            RG_T_SLOWFIELD | RG_T_CORRODE | RG_T_PLAGUE | RG_T_CURSE | RG_T_INFECTTURN | RG_T_LIFESTEAL | RG_T_AURABUFF | RG_T_MARK,
            L"脚下自动铺开腐蚀泥沼，中毒会传染并持续叠伤；伤者掉的血补给自己与队友", 500, 118 },
/* 111 */ { L"寂灭寒渊", RGQ_HALL, PT_ABSOLUTEZERO,
            RG_T_SLOWFIELD | RG_T_TIMEFREEZE | RG_T_ICERESOURCE | RG_T_WEBSLOW | RG_T_GRAVITY | RG_T_LULLABY | RG_T_CONFUSE | RG_T_AURABUFF,
            L"半径内一切冻结并陷入沉睡，被冻住的僵尸不断碎裂成阳光与金气", 500, 116 },
/* 112 */ { L"熔心裂爆", RGQ_HALL, PT_FIREIMP,
            RG_T_CHAINBOOM | RG_T_SELFDESTRUCT | RG_T_CURSE | RG_T_CRIT | RG_T_GAMBLEDMG | RG_T_LASTSTAND | RG_T_AURABUFF | RG_T_KNOCKBACK,
            L"每次攻击都是爆炸判定，炸死的会连锁再爆；血量越低引爆半径与伤害越夸张", 500, 118 },
/* 113 */ { L"噬魂血祭", RGQ_HALL, PT_SOULREAPER,
            RG_T_LIFESTEAL | RG_T_EXECUTE | RG_T_LASTSTAND | RG_T_TIMEHEAL | RG_T_CONVERT | RG_T_AURABUFF | RG_T_MARK | RG_T_CRIT,
            L"斩杀残血僵尸并把灵魂转为己用：濒死队友被拉回，全队持续回血", 500, 118 },
/* 114 */ { L"千机蜂巢", RGQ_HALL, PT_SUMMONERSHAMAN,
            RG_T_SUMMON | RG_T_MULTILANE | RG_T_PETALBARRAGE | RG_T_AURABUFF | RG_T_SHIELDALLY | RG_T_ORBITAL | RG_T_MARK | RG_T_CRIT,
            L"不断召出可独立索敌的蜂群，蜂群与本体共享光环与护盾，越打越多", 500, 108 },
/* 115 */ { L"圣辉共鸣", RGQ_HALL, PT_BELLFLOWER,
            RG_T_AURABUFF | RG_T_SHIELDALLY | RG_T_TIMEHEAL | RG_T_ENERGYGAIN | RG_T_SHARESUN | RG_T_CHI | RG_T_PURGE | RG_T_MARK,
            L"自身不还手，但全队增伤、减伤、回血、产阳光与金气；并定期净化负面状态", 500, 108 },
/* 116 */ { L"荆棘王铠", RGQ_HALL, PT_BOUNCETHORN,
            RG_T_MIRROR | RG_T_FAKEHP | RG_T_LASTSTAND | RG_T_DODGE | RG_T_SHIELDALLY | RG_T_PURGE | RG_T_TIMEHEAL | RG_T_AURABUFF,
            L"承受的伤害按比例原样奉还，荆棘外壳会自行修复；濒死时反弹效果翻倍", 500, 112 },
/* 117 */ { L"时砂回廊", RGQ_HALL, PT_CHRONODISTORTER,
            RG_T_TIMEFREEZE | RG_T_REWIND | RG_T_SLOWFIELD | RG_T_STEPMOVE | RG_T_DODGE | RG_T_CONVERT | RG_T_LULLABY | RG_T_AURABUFF,
            L"每格推进一次时间：命中的僵尸被倒回原位并冻结，己方则被加速", 500, 112 },
/* 118 */ { L"命运赌局", RGQ_HALL, PT_LUCKYROULETTE,
            RG_T_GAMBLEDMG | RG_T_GAMBLERISK | RG_T_DROPCARD | RG_T_DRAWCARD | RG_T_CRIT | RG_T_PIXELCRUSH | RG_T_ENERGYGAIN | RG_T_LASTSTAND,
            L"每发都是赌注：要么十倍暴击要么空枪；但命中就有概率直接掉出一张卡", 500, 102 },
/* 119 */ { L"逐星猎标", RGQ_HALL, PT_EAGLEEYESHOOTER,
            RG_T_MARK | RG_T_EXECUTE | RG_T_ARMORBREAK | RG_T_CRIT | RG_T_LASERCUT | RG_T_ORBITAL | RG_T_OVERLOAD | RG_T_AURABUFF,
            L"先标记最痛的目标，随后全队对它破甲、穿盾并追加处决伤害", 500, 114 },
/* 120 */ { L"不动碉堡", RGQ_HALL, PT_ENGINEERWALNUT,
            RG_T_MAZEWALL | RG_T_FAKEHP | RG_T_MIRROR | RG_T_SHIELDALLY | RG_T_DIGITRAIN | RG_T_LASTSTAND | RG_T_TIMEHEAL | RG_T_PURGE,
            L"把自己变成一堵带护盾力场的工事，替整行吸收伤害并持续自我修复", 500, 112 },
/* 121 */ { L"万象磁枢", RGQ_HALL, PT_GRAVITYCONTROLLER,
            RG_T_PULL | RG_T_KNOCKBACK | RG_T_GRAVITY | RG_T_CONFUSE | RG_T_HIJACK | RG_T_ICERESOURCE | RG_T_SLOWFIELD | RG_T_AURABUFF,
            L"把全场僵尸来回拽动、击退与致眩；重力井把它们叠成一堆再一起碾碎", 500, 114 },
/* 122 */ { L"蜕变之种", RGQ_HALL, PT_GENESISFLOWER,
            RG_T_GROWEAT | RG_T_COPYEAT | RG_T_FUSEPLANT | RG_T_CONVERT | RG_T_LIFESTEAL | RG_T_ENERGYGAIN | RG_T_AURABUFF | RG_T_TIMEHEAL,
            L"每吃掉一只僵尸就永久变大变强，还会吞掉旁边植物学走它的本事", 500, 100 },
/* 123 */ { L"镜裂分身", RGQ_HALL, PT_MIRRORFERN,
            RG_T_MIRROR | RG_T_DOUBLEBODY | RG_T_RECOMBINE | RG_T_BLINK | RG_T_DODGE | RG_T_FAKEHP | RG_T_SUMMON | RG_T_CRIT,
            L"分出多个可独立承受伤害的镜像；被打破时会在别处重新聚合再炸一次", 500, 114 },
/* 124 */ { L"虚影之息", RGQ_HALL, PT_DREAMCAP,
            RG_T_DODGE | RG_T_FAKEHP | RG_T_BLINK | RG_T_MEMESPREAD | RG_T_PURGE | RG_T_TIMEHEAL | RG_T_LASTSTAND | RG_T_CONFUSE,
            L"本体在相位之间闪烁，绝大多数攻击穿过它打空；被命中反而扩散减速孢子", 500, 106 },
/* 125 */ { L"孤注终章", RGQ_HALL, PT_PHENIXFLAME,
            RG_T_LASTSTAND | RG_T_ASSASSIN | RG_T_EXECUTE | RG_T_SELFDESTRUCT | RG_T_CRIT | RG_T_GAMBLEDMG | RG_T_LIFESTEAL | RG_T_AURABUFF,
            L"越是濒死越强，血量见底时全身燃起白焰、伤害与斩杀线同时拉满", 500, 120 },
/* 126 */ { L"深空观测", RGQ_HALL, PT_HOLOGRAMPROJECTOR,
            RG_T_ORBITAL | RG_T_LASERCUT | RG_T_MULTILANE | RG_T_MARK | RG_T_GRAVITY | RG_T_ARMORBREAK | RG_T_CRIT | RG_T_OVERLOAD,
            L"锁定僵尸最密集处，从天上连续投下贯穿光束；护甲在它面前不存在", 500, 116 },
/* 127 */ { L"千面棱彩", RGQ_HALL, PT_KALEIDOSCOPEFLOWER,
            RG_T_GAMBLEDMG | RG_T_DROPCARD | RG_T_DRAWCARD | RG_T_COPYEAT | RG_T_FUSEPLANT | RG_T_SUMMON | RG_T_BLINK | RG_T_MEMESPREAD,
            L"每回合随机复制全场任意一株的能力，样样都会一点，但什么都做不精", 500, 120 },
/* 128 */ { L"鸿蒙母树", RGQ_PRIMORDIAL, PT_ECHOBAMBOO,
            RG_ALL_TRAITS,
            L"一切生命的起点。每击杀一次就永久加粗一圈，最终长成全屏不可摧毁的母株", 10000, 150 },
/* 129 */ { L"无极星渊", RGQ_PRIMORDIAL, PT_NEBULABEET,
            RG_ALL_TRAITS,
            L"自身即一片星渊。攻击范围覆盖全屏，落入其中的僵尸同时被吸扯与灼烧", 10000, 148 },
/* 130 */ { L"归墟之喉", RGQ_PRIMORDIAL, PT_VOIDDEVOURER,
            RG_ALL_TRAITS,
            L"张开就是深渊。吞掉进入范围的一切——僵尸、子弹、护甲，转化成自身与阳光", 10000, 147 },
/* 131 */ { L"万象轮盘", RGQ_PRIMORDIAL, PT_VORTEXTURNIP,
            RG_ALL_TRAITS,
            L"每回合重掷全部能力，掷出什么就打什么；期望值高得离谱，下限也低得离谱", 10000, 145 },
/* 132 */ { L"烛照晶簇", RGQ_PRIMORDIAL, PT_CRYSTALSHOOTER,
            RG_ALL_TRAITS,
            L"创世之光凝成晶体。射出的每一发都是穿甲贯穿，无视一切护具与减伤", 10000, 144 },
/* 133 */ { L"天枢雷霆", RGQ_PRIMORDIAL, PT_THUNDEROVERLORD,
            RG_ALL_TRAITS,
            L"天枢垂落的雷柱不断砸落，命中即麻痹；雷击会在僵尸之间无限跳跃", 10000, 143 },
/* 134 */ { L"地脉根须", RGQ_PRIMORDIAL, PT_THORFLOWER,
            RG_ALL_TRAITS,
            L"根系扎穿整个地面。全屏处处是根刺，任何移动都会触发一次撕裂", 10000, 142 },
/* 135 */ { L"星枢坠幕", RGQ_PRIMORDIAL, PT_STARDIVINER,
            RG_ALL_TRAITS,
            L"把整片星空拽到地面：持续不断的星陨覆盖全屏，敌我皆伤唯独它自己无恙", 10000, 141 },
/* 136 */ { L"岁渊之钟", RGQ_PRIMORDIAL, PT_CLOCKSPROUT,
            RG_ALL_TRAITS,
            L"时间在它手里是资源。全场僵尸被反复倒回与冻结，己方攻速被拉到极限", 10000, 140 },
/* 137 */ { L"墟核裂解", RGQ_PRIMORDIAL, PT_LANTERNMELON,
            RG_ALL_TRAITS,
            L"每发都是创世级的裂解弹。落点整屏连锁引爆，被炸死的会继续炸", 10000, 139 },
/* 138 */ { L"玄黄母岩", RGQ_PRIMORDIAL, PT_CORALGUARD,
            RG_ALL_TRAITS,
            L"天地初开的原石。血量高到不可摧毁，并把承受的伤害加倍奉还给攻击者", 10000, 138 },
/* 139 */ { L"太一独尊", RGQ_PRIMORDIAL, PT_MARROW,
            RG_ALL_TRAITS,
            L"所有力量凝于一株。它的每一击都自动打出最高档伤害，从不失手也从无浮动", 10000, 137 },
/* 140 */ { L"阴阳双生", RGQ_PRIMORDIAL, PT_MIRRORSUNFLOWER,
            RG_ALL_TRAITS,
            L"同时存在两株：一株专司爆发、一株专司封锁，被打掉一株另一株立刻补上", 10000, 136 },
/* 141 */ { L"轮回祭坛", RGQ_PRIMORDIAL, PT_MOONLIGHTPRIESTESS,
            RG_ALL_TRAITS,
            L"被摧毁的植物会从祭坛上重新长出；它自己倒下时会带着半场僵尸一起走", 10000, 135 },
/* 142 */ { L"天罡战鼓", RGQ_PRIMORDIAL, PT_SPORETANK,
            RG_ALL_TRAITS,
            L"战鼓一响全队攻速与伤害同时拉满，鼓点越密加成越高，直到把整条线推平", 10000, 134 },
/* 143 */ { L"紫微帝庭", RGQ_PRIMORDIAL, PT_SEDUCTIONQUEEN,
            RG_ALL_TRAITS,
            L"在场即统御。全场僵尸被强制拉到它面前排队，任何反抗都被立刻压制", 10000, 133 },
/* 144 */ { L"沧溟潮涌", RGQ_PRIMORDIAL, PT_MONSOONREED,
            RG_ALL_TRAITS,
            L"掀起吞没一切的海啸，把整行僵尸往后推并淹死；潮水还会冲刷掉负面状态", 10000, 132 },
/* 145 */ { L"乾元鼎炉", RGQ_PRIMORDIAL, PT_BUBBLECANNON,
            RG_ALL_TRAITS,
            L"一座创世熔炉。喷出的每一团都是可以持续燃烧整屏的白焰", 10000, 131 },
/* 146 */ { L"涅槃红莲", RGQ_PRIMORDIAL, PT_FLASHBERRY,
            RG_ALL_TRAITS,
            L"一次性点燃整屏，随后从灰烬里满血重生；每次重生都比上一次更猛烈", 10000, 130 },
/* 147 */ { L"烛龙吐息", RGQ_PRIMORDIAL, PT_DRAGONBREATHVINE,
            RG_ALL_TRAITS,
            L"一口吐息烧穿整条战线，火焰会沿地面蔓延并长期停留", 10000, 129 },
/* 148 */ { L"羲和耀斑", RGQ_PRIMORDIAL, PT_DEMOLITIONEXPERT,
            RG_ALL_TRAITS,
            L"自身就是太阳。全屏被持续照射灼烧，僵尸的护具在它面前直接汽化", 10000, 128 },
/* 149 */ { L"望舒冰轮", RGQ_PRIMORDIAL, PT_FROSTQUEEN,
            RG_ALL_TRAITS,
            L"月轮悬空，全场进入永冬。被冻住的僵尸直接变成冰雕资源", 10000, 127 },
/* 150 */ { L"共工怒涛", RGQ_PRIMORDIAL, PT_BUBBLELOTUS,
            RG_ALL_TRAITS,
            L"把水压成炮。每一击都在僵尸之间水锤传导，护甲被水压从内部顶开", 10000, 126 },
/* 151 */ { L"盘古开天", RGQ_PRIMORDIAL, PT_STEAMPEPPER,
            RG_ALL_TRAITS,
            L"开局直接改写战场：背景与音乐永久转为始祖，我方全体起始强度翻倍", 10000, 125 },
/* 152 */ { L"女娲补天", RGQ_PRIMORDIAL, PT_HEALPETAL,
            RG_ALL_TRAITS,
            L"把所有损伤都补回来。全队持续满血、免疫负面状态，被摧毁的植物原地复原", 10000, 124 },
/* 153 */ { L"后土承载", RGQ_PRIMORDIAL, PT_WOODGUARDIAN,
            RG_ALL_TRAITS,
            L"大地本身替你挡伤害。整条线路获得护盾力场，任何攻击先由它承受", 10000, 123 },
/* 154 */ { L"祝融焚天", RGQ_PRIMORDIAL, PT_CANDLESPROUT,
            RG_ALL_TRAITS,
            L"火焰不再熄灭。全场持续燃烧，火焰会永久留在地面并不断扩散", 10000, 122 },
/* 155 */ { L"玄冥幽狱", RGQ_PRIMORDIAL, PT_DUSKORCHID,
            RG_ALL_TRAITS,
            L"把战线拖入永夜。全场视野被压缩，僵尸在其中持续失血并失去方向", 10000, 121 },
/* 156 */ { L"句芒春律", RGQ_PRIMORDIAL, PT_COMETCLOVER,
            RG_ALL_TRAITS,
            L"生命自己会赢。全队攻击频率与阳光产出同时被拉到极值，负面状态每秒净化", 10000, 120 },
/* 157 */ { L"蓐收金秋", RGQ_PRIMORDIAL, PT_COINSPROUT,
            RG_ALL_TRAITS,
            L"开局即是终局经济：每击杀产出阳光与金气，越打越有钱，用金币直接碾过去", 10000, 100 },
};
#pragma GCC diagnostic pop

/* ---- 肉鸽僵尸：70 种（00~49 首发，50~69 第三批「会飞会跳会召唤」） ---- */
#define RG_ZOMBIE_N 70
typedef struct {
    const wchar_t *name;
    unsigned char  tier;      /* RGQ_* */
    unsigned char  base;      /* ZT_*：行为与贴图基座 */
    unsigned long long traits; /* RZ_* 位掩码（64 位两字） */
    const wchar_t *desc;
} RgZombieDef;

/* 僵尸能力位（与植物共用 uint32，但语义独立） */
#define RZ_REVIVE3      (1u <<  0)   /* 死亡后回到 3 波前复活 */
#define RZ_TRIPLEBODY   (1u <<  1)   /* 同时存在于三条线路 */
#define RZ_INFECTSPD    (1u <<  2)   /* 触碰其他僵尸传染加速 */
#define RZ_HIJACKPLANT  (1u <<  3)   /* 劫持植物反打 */
#define RZ_GROWEAT      (1u <<  4)   /* 吃掉植物后增长身体 */
#define RZ_KNIFEBARRAGE (1u <<  5)   /* 投掷菜刀弹幕 */
#define RZ_MAZEWALL     (1u <<  6)   /* 在前方生成迷宫墙 */
#define RZ_SELFGAMBLE   (1u <<  7)   /* 50% 概率自毁 */
#define RZ_LOANHP       (1u <<  8)   /* 血量翻倍但掉落债务 */
#define RZ_WEBSLOW      (1u <<  9)   /* 吐丝减速铺路 */
#define RZ_FAKEHP       (1u << 10)   /* 假血量诱导浪费子弹 */
#define RZ_CODEWALL     (1u << 11)   /* 每步生成数字墙 */
#define RZ_NOISESLOW    (1u << 12)   /* 嘈杂使植物射速减半 */
#define RZ_GRAVITYWELL  (1u << 13)   /* 周围僵尸坠落加速 */
#define RZ_RHYTHMDASH   (1u << 14)   /* 随节奏冲刺 */
#define RZ_BLINK        (1u << 15)   /* 每秒随机位移 */
#define RZ_MIRROR3      (1u << 16)   /* 分出 3 个镜像 */
#define RZ_DRAINSUN     (1u << 17)   /* 吞噬阳光并减速植物 */
#define RZ_FORKDEATH    (1u << 18)   /* 死亡后分叉为 2 个小僵 */
#define RZ_MUTATE       (1u << 19)   /* 感染后变异刷新 */
#define RZ_LASERCUT     (1u << 20)   /* 激光切割植物 */
#define RZ_HPGAMBLE     (1u << 21)   /* 血量随机波动 */
#define RZ_STEPMOVE     (1u << 22)   /* 每回合只移动一格 */
#define RZ_PIXELIMMUNE  (1u << 23)   /* 概率像素化免疫 */
#define RZ_FUSEZOMBIE   (1u << 24)   /* 吞噬同类融合 */
#define RZ_REFLECT      (1u << 25)   /* 反弹子弹 */
#define RZ_ASSASSIN     (1u << 26)   /* 隐身三次后秒杀 */
#define RZ_GREEDSUN     (1u << 27)   /* 吸收掉落阳光 */
#define RZ_CURSEDEATH   (1u << 28)   /* 死亡后诅咒卡组 */
#define RZ_CHISHIELD    (1u << 29)   /* 内力护盾 */
#define RZ_ENTANGLE     (1u << 30)   /* 与另一僵尸同步血量 */
#define RZ_HEALBACK     (1u << 31)   /* 受伤后回血 */

/* ============================================================================
   僵尸能否"隔着距离"直接伤害植物
   ----------------------------------------------------------------------------
   用户要求：去掉僵尸远程伤害植物的机制，提升游戏体验。
   置 0（当前值）后，僵尸**只能走到植物面前啃食**才造成伤害；
   所有远程 / 范围 / 间接的植物伤害一律不生效，但它们的
   **特效、位移、偷阳光、加冷却等非伤害表现全部保留** ——
   玩家仍能看到僵尸在"攻击"，只是不再隔空掉植物的血。

   为什么做成一个开关、而不是把代码删掉：
     · 集中一处，改一个数字就能回退，不用翻 7 个地方；
     · 每处伤害点都由它统一守着，不会出现"漏改一处"的半吊子状态；
     · 升级成难度选项时可以直接接上。
   受影响的能力（7 处）：
     RZ_LASERCUT 激光 / RZ_KNIFEBARRAGE 菜刀弹幕 / RZ2_BOMBBARRAGE 轰炸 /
     RZ2_COMBOKICK 连击踢 / RZ2_BLACKHOLE 黑洞 / RZ_ASSASSIN 暗杀、
     RZ2_POISON 毒 / RZ_NOISESLOW 噪音 / RZ2_HACKSUN 黑太阳 / RZ2_WEATHER 天气、
     RZ2_REWINDPLANT 回溯、RZ_HIJACKPLANT 劫持、
     RZ3_THROW 投掷、RZ3_POISONCLOUD 毒云、RZ3_MAGNET 磁力
   保留（近战，属于核心玩法，不受本开关影响）：
     普通啃食、巨人砸击、RZ3_DEVOUR 速食、大蒜换行
   ============================================================================ */
#define ZOMBIE_REMOTE_PLANT_DMG 0

/* 关掉伤害之后，这些远程能力改成"压制植物的攻击节奏"（把 timer 往后推），
   所以它们不会变成空壳 —— 玩家仍能感觉到"后排被压制、射得慢了"，
   但植物**不会掉血**。数值单位是秒：0.6 = 该植物下一次行动推迟 0.6 秒。 */
#define ZOMBIE_SUPPRESS_TIMER   0.6f

/* ========================================================================
   第二批能力位
   ------------------------------------------------------------------------
   ⚠️ 修了一个真 Bug：这里原来写的是 `1u << 0` … `1u << 9`，
   和 RZ_ 的 bit0~bit9 **完全重叠** —— 注释说是"第二个掩码字"，
   但定义根本没写到高位去。
   后果是语义错位：RZ2_POISON（bit1）实际命中 RZ_TRIPLEBODY（bit1），
   也就是挂着"持续掉血"的僵尸真正触发的是"三线分身"。
   （`traits` 一直是 unsigned long long，所以高位本来就是空的，
     只是当初忘了把位号加上 32。）
   ======================================================================== */
#define RZ2_HACKSUN     (1ull << 32) /* 控制向日葵停产 */
#define RZ2_POISON      (1ull << 33) /* 造成持续掉血 */
#define RZ2_REWINDPLANT (1ull << 34) /* 让植物倒退 */
#define RZ2_DODGE30     (1ull << 35) /* 30% 无敌 */
#define RZ2_MAZEBACK    (1ull << 36) /* 在身后造墙 */
#define RZ2_WEATHER     (1ull << 37) /* 召唤暴风雪 */
#define RZ2_NEGCARD     (1ull << 38) /* 每步抽一张负面卡 */
#define RZ2_COMBOKICK   (1ull << 39) /* 连续踢击 */
#define RZ2_BLACKHOLE   (1ull << 40) /* 吞噬一切 */
#define RZ2_BOMBBARRAGE (1ull << 41) /* 投掷炸弹 */

/* ========================================================================
   第三批：20 个新机制（bit 42~61）
   ------------------------------------------------------------------------
   设计取向 —— 用户要求"僵尸别只会走路啃植物"：
     突破防线  FLY 飞 / JUMP 跳 / BURROW 钻地 / BLINK 闪现 / DASH 冲锋
     远程消耗  THROW 投掷 / SUMMON 召唤 / POISONCLOUD 毒云
     耐久向    SHIELD 护盾 / DEFLECT 反弹 / REGEN 自愈 / REBORN 复生 / ENRAGE 狂暴
     骚扰向    FREEZEBITE 冰噬 / STEALSUN 偷阳光 / DEVOUR 速食 / MAGNET 磁力
     团队向    TAUNT 嘲讽 / AURA 光环 / SPLIT 分裂
   64 位掩码至此用到 bit61，只剩 bit62/63 两个空位 —— 再加机制必须换结构体字段。
   ======================================================================== */
#define RZ3_FLY         (1ull << 42) /* 飞行：快速掠进但停在前排 */
#define RZ3_JUMP        (1ull << 43) /* 跳跃：快速跃进到前排 */
#define RZ3_THROW       (1ull << 44) /* 投掷：远程丢武器砸植物 */
#define RZ3_SUMMON      (1ull << 45) /* 召唤：定期召唤小僵尸 */
#define RZ3_BLINK       (1ull << 46) /* 闪现：瞬移一段距离 */
#define RZ3_DASH        (1ull << 47) /* 冲锋：短时高速冲刺 */
#define RZ3_SHIELD      (1ull << 48) /* 护盾：吸收伤害 */
#define RZ3_DEFLECT     (1ull << 49) /* 反弹：把子弹弹回去 */
#define RZ3_SPLIT       (1ull << 50) /* 分裂：死亡后裂成小僵尸 */
#define RZ3_REGEN       (1ull << 51) /* 自愈：持续回血 */
#define RZ3_ENRAGE      (1ull << 52) /* 狂暴：血量越低走得越快 */
#define RZ3_TAUNT       (1ull << 53) /* 嘲讽：强制吸引植物火力 */
#define RZ3_POISONCLOUD (1ull << 54) /* 毒云：范围持续伤害 */
#define RZ3_FREEZEBITE  (1ull << 55) /* 冰噬：啃食时冻结该植物 */
#define RZ3_STEALSUN    (1ull << 56) /* 偷阳光：啃食时抢走阳光 */
#define RZ3_DEVOUR      (1ull << 57) /* 速食：啃食速度翻倍 */
#define RZ3_REBORN      (1ull << 58) /* 复生：死亡后满血复活一次 */
#define RZ3_MAGNET      (1ull << 59) /* 磁力：把植物吸到面前 */
#define RZ3_BURROW      (1ull << 60) /* 钻地：潜行到当前前排右侧 */
#define RZ3_AURA        (1ull << 61) /* 光环：强化周围僵尸 */

/* “终极奇葩”曾直接使用全 64 位掩码，等于同时拿到复活、分裂、护盾回满、
   假血下限和反弹等互相叠加的保命机制。残局很容易出现无法稳定击杀的僵尸。
   改为一套明确的终局攻击组合：危险，但不包含任何无限续航或免死能力。 */
#define RZ2_ALL         (RZ_GROWEAT | RZ2_COMBOKICK | RZ2_BOMBBARRAGE | \
                         RZ3_THROW | RZ3_ENRAGE | RZ3_DEVOUR | RZ3_MAGNET)

static const RgZombieDef rgZombies[RG_ZOMBIE_N] = {
/* 00 */ { L"时间悖论僵尸",   RGQ_LEGEND, ZT_NORMAL,     RZ_REVIVE3 | RZ3_BLINK,
           L"死亡后回到 3 波前的位置复活" },
/* 01 */ { L"量子叠加僵尸",   RGQ_EPIC,   ZT_NORMAL,     RZ_TRIPLEBODY | RZ3_DEFLECT,
           L"同时存在于三条线路，需三线齐清" },
/* 02 */ { L"病毒扩散僵尸",   RGQ_RARE,   ZT_NORMAL,     RZ_INFECTSPD | RZ3_POISONCLOUD,
           L"触碰其他僵尸即传染，被感染者加速" },
/* 03 */ { L"黑客僵尸",       RGQ_LEGEND, ZT_NORMAL,     RZ_HIJACKPLANT | RZ3_THROW,
           L"隔空投掷干扰，拖慢你的后排节奏" },
/* 04 */ { L"贪吃蛇僵尸",     RGQ_EPIC,   ZT_NORMAL,     RZ_GROWEAT | RZ3_DEVOUR,
           L"吃掉路障与植物后身体持续增长" },
/* 05 */ { L"弹幕厨师僵尸",   RGQ_EPIC,   ZT_NEWSPAPER,  RZ_KNIFEBARRAGE | RZ3_THROW,
           L"远程投掷菜刀弹幕，压制后排" },
/* 06 */ { L"迷宫建筑僵尸",   RGQ_EPIC,   ZT_NORMAL,     RZ_MAZEWALL | RZ3_SHIELD,
           L"在前路生成迷宫墙，逼迫绕行" },
/* 07 */ { L"概率坩埚僵尸",   RGQ_MYTH,   ZT_NORMAL,     RZ_SELFGAMBLE | RZ3_REBORN,
           L"50% 概率自毁，否则获得巨额强化" },
/* 08 */ { L"贷款僵尸",       RGQ_COMMON, ZT_CONE,       RZ_LOANHP | RZ3_STEALSUN,
           L"血量翻倍，但死亡时掉落阳光债务" },
/* 09 */ { L"蜘蛛网僵尸",     RGQ_RARE,   ZT_NORMAL,     RZ_WEBSLOW | RZ3_MAGNET,
           L"吐丝减速植物并铺设蛛网路面" },
/* 10 */ { L"虚拟现实僵尸",   RGQ_RARE,   ZT_BUCKET,     RZ_FAKEHP | RZ3_SPLIT,
           L"假血量诱导你浪费大量子弹" },
/* 11 */ { L"代码编译僵尸",   RGQ_EPIC,   ZT_NORMAL,     RZ_CODEWALL | RZ3_SHIELD,
           L"每移动一步生成一道数字墙" },
/* 12 */ { L"噪音僵尸",       RGQ_RARE,   ZT_NORMAL,     RZ_NOISESLOW | RZ3_TAUNT,
           L"嘈杂声使范围内植物射速减半" },
/* 13 */ { L"重力井僵尸",     RGQ_EPIC,   ZT_NORMAL,     RZ_GRAVITYWELL | RZ3_MAGNET,
           L"周围僵尸坠落加速，越聚越快" },
/* 14 */ { L"音乐节拍僵尸",   RGQ_RARE,   ZT_NORMAL,     RZ_RHYTHMDASH | RZ3_DASH,
           L"随节拍冲刺，间歇性大幅提速" },
/* 15 */ { L"随机传送僵尸",   RGQ_COMMON, ZT_NORMAL,     RZ_BLINK | RZ3_FLY,
           L"每秒随机位移，难以稳定集火" },
/* 16 */ { L"镜像僵尸",       RGQ_EPIC,   ZT_NORMAL,     RZ_MIRROR3 | RZ3_DEFLECT,
           L"分出 3 个镜像，本体隐藏其中" },
/* 17 */ { L"吸阳光僵尸",     RGQ_RARE,   ZT_NORMAL,     RZ_DRAINSUN | RZ3_STEALSUN,
           L"吞噬阳光，并使植物产出减速" },
/* 18 */ { L"区块链僵尸",     RGQ_LEGEND, ZT_NORMAL,     RZ_FORKDEATH | RZ3_SPLIT,
           L"死亡后分叉为 2 个小型僵尸" },
/* 19 */ { L"生化实验僵尸",   RGQ_LEGEND, ZT_NORMAL,     RZ_MUTATE | RZ3_REGEN,
           L"被感染后变异刷新，属性全面提升" },
/* 20 */ { L"跨界外星僵尸",   RGQ_LEGEND, ZT_NORMAL,     RZ_LASERCUT | RZ3_THROW,
           L"蓄力发射激光扫射，压制前排节奏" },
/* 21 */ { L"赌徒僵尸",       RGQ_RARE,   ZT_CONE,       RZ_HPGAMBLE | RZ3_ENRAGE,
           L"血量随机波动，最高可达 3 倍" },
/* 22 */ { L"爬格子僵尸",     RGQ_COMMON, ZT_NORMAL,     RZ_STEPMOVE | RZ3_JUMP,
           L"每回合只移动一格，节奏极慢但极硬" },
/* 23 */ { L"像素崩坏僵尸",   RGQ_MYTH,   ZT_NORMAL,     RZ_PIXELIMMUNE | RZ3_DEFLECT,
           L"20% 概率像素化，完全免疫当次伤害" },
/* 24 */ { L"塔防合成僵尸",   RGQ_ULTRA,  ZT_GIANT,      RZ_FUSEZOMBIE | RZ3_AURA,
           L"吞噬同类融合，同时拥有全部负面能力" },
/* 25 */ { L"弹跳僵尸",       RGQ_RARE,   ZT_NORMAL,     RZ_REFLECT | RZ3_DEFLECT,
           L"反弹子弹，逼你改用爆炸与近战" },
/* 26 */ { L"暗杀僵尸",       RGQ_MYTH,   ZT_NORMAL,     RZ_ASSASSIN | RZ3_BLINK,
           L"隐身潜行突袭后排，压制其节奏" },
/* 27 */ { L"贪婪僵尸",       RGQ_COMMON, ZT_NORMAL,     RZ_GREEDSUN | RZ3_STEALSUN,
           L"吸收掉落的阳光，断你的经济" },
/* 28 */ { L"诅咒僵尸",       RGQ_LEGEND, ZT_NORMAL,     RZ_CURSEDEATH | RZ3_POISONCLOUD,
           L"一路喷毒雾，拖慢经过的植物" },
/* 29 */ { L"跨服功夫僵尸",   RGQ_RARE,   ZT_FOOTBALL,   RZ_CHISHIELD | RZ3_TAUNT,
           L"内力护盾吸收伤害，破盾后才吃伤害" },
/* 30 */ { L"量子纠缠僵尸",   RGQ_EPIC,   ZT_NORMAL,     RZ_ENTANGLE | RZ3_SPLIT,
           L"与另一只僵尸血量同步，必须同时击杀" },
/* 31 */ { L"时间回溯僵尸",   RGQ_EPIC,   ZT_NORMAL,     RZ_HEALBACK | RZ3_REGEN,
           L"受伤后持续回血，需要爆发斩杀" },
/* 32 */ { L"黑客僵尸2",      RGQ_EPIC,   ZT_NORMAL,     RZ2_HACKSUN | RZ3_SUMMON,
           L"控制向日葵停产，切断阳光来源" },
/* 33 */ { L"病原体僵尸",     RGQ_RARE,   ZT_NORMAL,     RZ2_POISON | RZ3_POISONCLOUD,
           L"使植物中毒，产出与射速一并下降" },
/* 34 */ { L"逆向时间僵尸",   RGQ_LEGEND, ZT_NORMAL,     RZ2_REWINDPLANT | RZ3_BLINK,
           L"让植物技能延后，打乱其攻击节奏" },
/* 35 */ { L"概率墙僵尸",     RGQ_RARE,   ZT_BUCKET,     RZ2_DODGE30 | RZ3_SHIELD,
           L"30% 概率进入无敌帧，免疫伤害" },
/* 36 */ { L"迷宫僵尸",       RGQ_EPIC,   ZT_NORMAL,     RZ2_MAZEBACK | RZ3_BURROW,
           L"在身后造墙，阻断后续植物的射界" },
/* 37 */ { L"随机天气僵尸",   RGQ_MYTH,   ZT_NORMAL,     RZ2_WEATHER | RZ3_FREEZEBITE,
           L"召唤暴风雪，遮蔽视野并压制前排" },
/* 38 */ { L"卡牌僵尸",       RGQ_EPIC,   ZT_NORMAL,     RZ2_NEGCARD | RZ3_SUMMON,
           L"每走一步给你塞一张负面卡" },
/* 39 */ { L"跨服联动功夫僵尸", RGQ_LEGEND, ZT_FOOTBALL, RZ2_COMBOKICK | RZ3_DASH,
           L"连续踢击，打乱前排植物的节奏" },
/* 40 */ { L"吞噬黑洞僵尸",   RGQ_MYTH,   ZT_GIANT,      RZ2_BLACKHOLE | RZ3_MAGNET,
           L"磁场压制前排植物，拖慢你的输出" },
/* 41 */ { L"弹幕厨师僵尸2",  RGQ_EPIC,   ZT_NEWSPAPER,  RZ2_BOMBBARRAGE | RZ3_THROW,
           L"投掷炸弹，压制整列植物的节奏" },
/* 42 */ { L"量子叠加僵尸2",  RGQ_EPIC,   ZT_NORMAL,     RZ_TRIPLEBODY | RZ_MIRROR3 | RZ3_ENRAGE,
           L"分身叠加，同时占据三线并分裂镜像" },
/* 43 */ { L"病毒扩散僵尸2",  RGQ_RARE,   ZT_NORMAL,     RZ_INFECTSPD | RZ2_POISON | RZ3_SHIELD,
           L"传染加速并释放毒素，拖慢植物" },
/* 44 */ { L"贪吃蛇僵尸2",    RGQ_EPIC,   ZT_NORMAL,     RZ_GROWEAT | RZ_FUSEZOMBIE | RZ3_REGEN,
           L"吞噬植物与同类，体型无限成长" },
/* 45 */ { L"虚拟现实僵尸2",  RGQ_RARE,   ZT_BUCKET,     RZ_FAKEHP | RZ_REFLECT | RZ3_THROW,
           L"假血加反弹，皮糙肉厚还扎手" },
/* 46 */ { L"代码编译僵尸2",  RGQ_EPIC,   ZT_NORMAL,     RZ_CODEWALL | RZ2_MAZEBACK | RZ3_SUMMON,
           L"前后都造墙，把植物困死在格子里" },
/* 47 */ { L"噪音僵尸2",      RGQ_RARE,   ZT_NORMAL,     RZ_NOISESLOW | RZ2_HACKSUN | RZ3_BURROW,
           L"噪音加停产，双重压制植物输出" },
/* 48 */ { L"重力井僵尸2",    RGQ_EPIC,   ZT_NORMAL,     RZ_GRAVITYWELL | RZ_RHYTHMDASH | RZ3_AURA,
           L"吸引同类并同步冲刺，成群压上" },
/* 49 */ { L"终极奇葩僵尸",   RGQ_ULTRA,  ZT_GIANT,      RZ2_ALL,
           L"同时拥有以上全部 69 种僵尸的负面能力" },

/* ========================================================================
   第三批 · 20 种「会飞会跳会召唤」的僵尸（索引 50~69）
   ------------------------------------------------------------------------
   这批解决的是同一个老问题：僵尸只会走路啃植物，防守方只要堆一堵墙就能
   无限拖时间。下面 20 种各自从不同角度拆掉这种"无脑堆墙"：
     突破防线  FLY 飞 / JUMP 跳 / BURROW 钻地 / BLINK 闪现 / DASH 冲锋
     远程消耗  THROW 投掷 / SUMMON 召唤 / POISONCLOUD 毒云
     耐久向    SHIELD 护盾 / DEFLECT 反弹 / REGEN 自愈 / REBORN 复生 / ENRAGE 狂暴
     骚扰向    FREEZEBITE 冰噬 / STEALSUN 偷阳光 / DEVOUR 速食 / MAGNET 磁力
     团队向    TAUNT 嘲讽 / AURA 光环 / SPLIT 分裂
   ======================================================================== */
/* 50 滑翔僵尸 / 普通 / 快速掠进到当前前排 */
/* 50 */ { L"滑翔僵尸",     RGQ_COMMON, ZT_NORMAL,     RZ3_FLY,
           L"悬浮掠进，但仍会被当前最前排植物挡住" },
/* 51 跳蚤僵尸 / 普通 / 快速跃进到前排 */
/* 51 */ { L"跳蚤僵尸",     RGQ_COMMON, ZT_VAULTER,    RZ3_JUMP,
           L"一路蹦跳快速接战，但不能越过前排" },
/* 52 投石僵尸 / 稀有 / 远程投掷砸植物 */
/* 52 */ { L"投石僵尸",     RGQ_RARE,   ZT_NORMAL,     RZ3_THROW,
           L"隔着防线扔石头，压制最前排" },
/* 53 亡灵法师 / 史诗 / 定期召唤小僵尸 */
/* 53 */ { L"亡灵法师",     RGQ_EPIC,   ZT_NORMAL,     RZ3_SUMMON,
           L"不停从地里拽出小僵尸，越拖越多" },
/* 54 相位僵尸 / 稀有 / 闪现前进 */
/* 54 */ { L"相位僵尸",     RGQ_RARE,   ZT_NORMAL,     RZ3_BLINK,
           L"忽明忽暗，一段一段往前闪" },
/* 55 冲锋僵尸 / 普通 / 短时高速冲刺 */
/* 55 */ { L"冲锋僵尸",     RGQ_COMMON, ZT_NORMAL,     RZ3_DASH,
           L"隔几秒就低头猛冲一段" },
/* 56 铁壁僵尸 / 稀有 / 护盾吸收伤害 */
/* 56 */ { L"铁壁僵尸",     RGQ_RARE,   ZT_BUCKET,     RZ3_SHIELD,
           L"随身带一层会自行修复的护盾" },
/* 57 镜面僵尸 / 史诗 / 反弹子弹 */
/* 57 */ { L"镜面僵尸",     RGQ_EPIC,   ZT_BUCKET,     RZ3_DEFLECT,
           L"浑身镜面，把打过来的子弹弹回去" },
/* 58 分裂僵尸 / 史诗 / 死亡后裂成小僵尸 */
/* 58 */ { L"分裂僵尸",     RGQ_EPIC,   ZT_NORMAL,     RZ3_SPLIT,
           L"打死它会原地裂成两只小僵尸" },
/* 59 再生僵尸 / 稀有 / 持续回血 */
/* 59 */ { L"再生僵尸",     RGQ_RARE,   ZT_NORMAL,     RZ3_REGEN,
           L"伤口自动愈合，不集中火力就打不死" },
/* 60 狂暴僵尸 / 传说 / 血量越低越快 */
/* 60 */ { L"狂暴僵尸",     RGQ_LEGEND, ZT_FOOTBALL,   RZ3_ENRAGE,
           L"越接近死亡跑得越疯，残血时快得离谱" },
/* 61 嘲讽僵尸 / 史诗 / 强制吸引植物火力 */
/* 61 */ { L"嘲讽僵尸",     RGQ_EPIC,   ZT_SCREENDOOR, RZ3_TAUNT,
           L"举着喇叭挑衅，逼附近植物全部打它" },
/* 62 毒云僵尸 / 史诗 / 范围持续伤害 */
/* 62 */ { L"毒云僵尸",     RGQ_EPIC,   ZT_NORMAL,     RZ3_POISONCLOUD,
           L"一路喷毒雾，腐蚀路面并拖慢植物" },
/* 63 冰噬僵尸 / 稀有 / 啃食时冻结植物 */
/* 63 */ { L"冰噬僵尸",     RGQ_RARE,   ZT_NORMAL,     RZ3_FREEZEBITE,
           L"啃谁就把谁冻住，被咬的植物还不了手" },
/* 64 盗阳僵尸 / 稀有 / 啃食时抢阳光 */
/* 64 */ { L"盗阳僵尸",     RGQ_RARE,   ZT_NORMAL,     RZ3_STEALSUN,
           L"一边啃一边把阳光塞进自己兜里" },
/* 65 饕餮僵尸 / 史诗 / 啃食速度翻倍 */
/* 65 */ { L"饕餮僵尸",     RGQ_EPIC,   ZT_NORMAL,     RZ3_DEVOUR,
           L"三倍速啃食，坚果墙在它面前像纸" },
/* 66 不死僵尸 / 传说 / 死亡后满血复活一次 */
/* 66 */ { L"不死僵尸",     RGQ_LEGEND, ZT_BUCKET,     RZ3_REBORN,
           L"第一次被打倒会站起来，而且是满血" },
/* 67 磁暴僵尸 / 传说 / 把植物吸到面前 */
/* 67 */ { L"磁暴僵尸",     RGQ_LEGEND, ZT_NORMAL,     RZ3_MAGNET,
           L"磁场压制前排植物，使其攻击变慢" },
/* 68 潜地僵尸 / 传说 / 从地下快速接近前排 */
/* 68 */ { L"潜地僵尸",     RGQ_LEGEND, ZT_DIGGER,     RZ3_BURROW,
           L"钻地快速接近当前前排，并在其右侧现身" },
/* 69 尸王 / 传奇 / 光环强化周围全部僵尸 */
/* 69 */ { L"尸王",         RGQ_ULTRA,  ZT_GIANT,      RZ3_AURA | RZ3_SUMMON | RZ3_SHIELD | RZ3_REGEN,
           L"战场的核心：给全场僵尸加攻加防，还能不停召唤" },
};

/* ---- 档位数值推导：所有肉鸽单位共用，避免手填调歪 ---- */
static float rgDmgMul (int tier) { return (tier >= 0 && tier < RGQ_COUNT) ? RGQ_DMG[tier]   : 1.0f; }
static float rgRateMul(int tier) { return (tier >= 0 && tier < RGQ_COUNT) ? RGQ_RATE[tier]  : 1.0f; }
static float rgRangeMul(int tier){ return (tier >= 0 && tier < RGQ_COUNT) ? RGQ_RANGE[tier] : 1.0f; }
/* 僵尸血量倍率：与植物同阶梯，但压一档（不然 20 倍的僵尸直接无解） */
static float rgZombieHpMul(int tier)
{
    if (tier < 0 || tier >= RGQ_COUNT) return 1.0f;
    /* ⚠️ 保险：僵尸永远不该吃到 RGQ_PRIMORDIAL 档的倍率。
       该档位的 RGQ_DMG 是为植物准备的 4000×，套到这个公式上会变成 1680 倍血量，
       直接把关卡变成无解。今天全表没有僵尸使用这一档（已核对），
       但"以后有人加一只始祖僵尸"是个很自然的动作，所以在这里钉死上界 ——
       越界时按殿堂档结算，属于**保守**的降级，不会静默失控。 */
    if (tier >= RGQ_PRIMORDIAL) tier = RGQ_HALL;
    return 1.0f + (RGQ_DMG[tier] - 2.0f) * 0.42f;    /* 普通 1.0 → 传奇 8.56 */
}
static float rgZombieSpdMul(int tier)
{
    if (tier < 0 || tier >= RGQ_COUNT) return 1.0f;
    return 1.0f + (float)tier * 0.055f;              /* 普通 1.0 → 传奇 1.275 */
}

/* ---- 阳光定价（10~200） ----
   定价原则：**性价比必须随品级递减**。
   高品级绝对强度更高，但"每点阳光换来的强度"要更低 ——
   否则玩家抽到传奇就无脑买，普通卡彻底失去存在意义。
   反例：按伤害倍率线性定价（普通 30 → 传奇 600）会把经济系统直接打死，
   所以这里压成 100→500 的温和曲线，靠"品级越高越难抽到"来控制供给。

   ⚠️ 注意：三选一拿到的是「免费」，这里的售价只在卡片作为卡槽内的
   常规植物被种植时才生效，所以高品级定价再贵也不会卡住玩家 ——
   真正限制供给的是 RGQ_DRAFT_W 的抽取概率，不是阳光。 */
static const int RGQ_COST[RGQ_COUNT] = { 100, 175, 250, 325, 400, 500, 500, 10000 };

/* 单株售价：显式定价优先，否则按品级取基准 */
static int rgPlantCost(int idx)
{
    int cost;
    if (idx < 0 || idx >= RG_PLANT_N) return 100;
    cost = (rgPlants[idx].cost > 0) ? (int)rgPlants[idx].cost
                                    : RGQ_COST[rgPlants[idx].tier];
    cost = ((cost + 12) / 25) * 25;
    /* 上界必须 ≥ 始祖的 10000：夹成 500 会让"售价 10000"变成一句空话，
       而且这种夹取是**静默**的（和当年 cost 被 uchar 截断一模一样）。 */
    return CLAMP(cost, 50, 10000);
}

/* ---- 肉鸽层运行时状态 ----
   放在数据区（而不是逻辑区）是为了让 cardBarGap()、卡槽绘制等早期函数也能读到，
   否则卡槽总数算不出来，卡片画的位置和点击判定会对不上。 */
#define RG_CARD_BASE   1000          /* 卡片编码：RG_CARD_BASE + 肉鸽植物索引 */
#define RG_MAX_SLOTS   8             /* 本局最多持有 8 株肉鸽植物 */

static int   gRgCard[RG_MAX_SLOTS];  /* 本局拿到的肉鸽植物索引 */
/* 1 = 由每波三选一送出，下一次落地免费；免费次数用掉后按品质收费。 */
static int   gRgFree[RG_MAX_SLOTS];
static int   gRgN = 0;
/* 本次三选一是否免费。当前每波奖励始终为 1。 */
static int   gDraftFree = 1;
/* 最近一次货架点击是否因为阳光不够而没成交 —— 用来让 UI 闪一下提示，
   而不是默默点不动（玩家会以为卡了）。 */
static int   gBuyFailed = 0;
static float rgCardCD[RG_MAX_SLOTS]; /* 每张肉鸽卡的再种冷却（免费但不无限刷） */
static int   gPendingRg  = -1;       /* plantIt 消费：本次要种的肉鸽植物索引 */
static int   gPendingRgZ = -1;       /* spawnZombieAt 消费：本次要刷的肉鸽僵尸索引 */
static int   gPendingGenZ = 0;       /* spawnZombieAt 消费：本次刷的是第几代分身 */
static float gWaveStallT  = 0.0f;    /* 本波已持续时间，用于卡死保护 */
static float gRgAtkMul   = 1.0f;     /* 当前开火植物的肉鸽伤害倍率（spawnPeaAt 消费） */
/* 下面三个也是"当前开火植物"决定的，由 spawnPeaAt 消费：
   神级以上植物的所有攻击都带穿透 + 溅射分裂 + 溅射半径，
   目的是让僵尸抱团时也有有效威胁。 */
static int   gRgPeaPierce = 0;       /* 子弹可穿透几个目标 */
static int   gRgPeaSplash = 0;       /* 命中后分裂成几颗小弹 */
static int   gRgPeaAoe    = 0;       /* 命中时的溅射半径（像素） */
static int   gRgPeaBounce = 0;       /* 肉鸽植物：僵尸间反弹次数 */
static float gRgBoomMul   = 1.0f;    /* 爆炸半径倍率（神级以上植物） */
/* 显式设定穿透/溅射的调用点，一律取【植物自带值】与【品质加成】的较大者。
   否则神级植物用带穿透的基座（仙人掌 / 激光豆 / 香蒲）时，
   基座自带的 pierceLeft = 2 会把品质加成整段盖掉。 */
#define RG_SET_PIERCE(pe, v) do { int _v = (int)(v); if (_v < gRgPeaPierce) _v = gRgPeaPierce; (pe).pierceLeft = _v; } while (0)
#define RG_SET_SPLASH(pe, v) do { int _v = (int)(v); if (_v < gRgPeaSplash) _v = gRgPeaSplash; (pe).splash = _v; } while (0)
static int   gRgDraftWave = -1;      /* 已经发过免费三选一的最大波数 */
static int   gRgPicked   = 0;        /* 本局拿过几次三选一 */

/* ---- 可叠加成长卡 -------------------------------------------------
   前 10 波先保证构筑成型：2 张植物 + 1 张成长；第 11 波起再转为
   1 张植物 + 2 张成长。层数仅局内有效，不改存档格式。 */
#define GROWTH_CARD_BASE 2000
enum { GROW_DMG, GROW_RATE, GROW_HP, GROW_BOOM, GROW_FROST, GROW_CD, GROW_PIERCE,
       GROW_COUNT };
typedef struct {
    const wchar_t *name;
    const wchar_t *desc;
    COLORREF col;
    int iconCat;
} GrowthDef;
static const GrowthDef growthDefs[GROW_COUNT] = {
    { L"火力萌发", L"所有植物伤害 +10%，每层独立叠加", RGB(226, 92, 62), 1 },
    { L"迅捷枝芽", L"射击间隔 -6%，最快可叠到 -40%", RGB(244, 174, 66), 1 },
    { L"深根系",   L"植物最大生命 +15%，当前植物同步补血", RGB(92, 184, 94), 2 },
    { L"爆裂花粉", L"爆炸伤害 +15%，爆炸范围 +5%", RGB(232, 126, 48), 3 },
    { L"寒霜年轮", L"减速与冻结持续时间 +12%", RGB(82, 168, 224), 3 },
    { L"循环代谢", L"所有植物卡片冷却 -8%，最多叠到 -45%", RGB(142, 196, 92), 4 },
    { L"破甲新芽", L"子弹额外获得 6% 穿透几率", RGB(174, 112, 224), 1 },
};
static int gGrowthStack[GROW_COUNT];
static int gWave = 0;                  /* 已开始的波数；难度公式在前面也要读 */
#define DRAFT_IS_GROWTH(v) ((v) >= GROWTH_CARD_BASE && (v) < GROWTH_CARD_BASE + GROW_COUNT)
#define DRAFT_GROWTH_ID(v) ((v) - GROWTH_CARD_BASE)
#define DRAFT_IS_RG(v)     ((v) >= RG_CARD_BASE && (v) < RG_CARD_BASE + RG_PLANT_N)

/* ======================================================================== */
/*                     存档 / 关卡 / 天赋 / 成就（元进程）                     */
/* ======================================================================== */
/* 这一层是「天赋 / 成就 / 抽卡」的共同地基 —— 没有存档，那三个只能是摆设。 */

#define SAVE_FILE   L"pvz_save.dat"
#define SAVE_MAGIC  0x5A565031u      /* 'PVZ1' */
/* v6: 永久普通植物 +30。植物数组长度和神级植物起点都变了，必须升版本；
   否则旧档里原本的神级槽会被当成新普通植物槽，图鉴和编组都会错位。 */
#define SAVE_VER    6

/* 历史版本的关卡数。⚠️ 这几个结构体里的数组长度**必须写死**，不能用 LV_COUNT ——
   一旦跟着宏走，以后每加一关都会让所有历史存档错位。
   （这个坑我踩过一次：把 LV_COUNT 从 6 改成 11 之后，
     SaveDataV2/V3 的布局跟着变了，连迁移分支都读不对了。） */
#define SAVE_LV_V2V4  6
/* v3~v5 时代：82 株普通植物 + 8 株神级植物。v6 在神级前插入 30 株新普通植物，
   历史结构必须固定旧长度，不能再跟随 PT_COUNT / PT_HERO_FLAME。 */
#define SAVE_PT_V3V5          90
#define SAVE_HERO_FLAME_V3V5  82

/* ---- 关卡 ---- */
#define LV_COUNT 11
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
    /* 【保留字段，未接线】zwStart/zwEnd 原本想表达"僵尸种类只在关内这个波次
       区间出现"，但这个语义和 startWave() 里那套全局解锁表（按 idx 逐步开放
       路障/铁桶/撑杆/…/巨人）是两套互斥的设计，混用会让同一只僵尸既"已解锁"
       又"不在区间内"。目前实际生效的是 startWave() 的解锁表 + zWeight 权重，
       这里保留字段只为存档/编辑器兼容，不要拿它做判断。 */
    int   zwStart, zwEnd;
} LevelDef;

static const LevelDef levelDefs[LV_COUNT] = {
    { L"前院 · 白天", L"标准草坪，适合熟悉节奏",   20, 150, 0, 0, 9, 1.00f, 1.00f, 1.00f, 0,  0, 0 },
    { L"后院 · 泳池", L"只能种在右侧的泥地上",     22, 175, 0, 0, 9, 1.05f, 1.00f, 1.00f, 3,  1, 3 },
    { L"月夜 · 墓园", L"夜间阳光稀少，僵尸更快",   20, 200, 1, 0, 9, 1.10f, 1.10f, 1.55f, 6,  2, 5 },
    { L"屋顶 · 天台", L"瓦片上的阵地战",           24, 150, 3, 0, 9, 1.25f, 1.05f, 1.00f, 10, 3, 6 },
    { L"沙漠 · 遗迹", L"烈日下阳光来得慢",         26, 225, 4, 0, 9, 1.40f, 1.10f, 1.45f, 14, 4, 6 },
    { L"无尽 · 挑战", L"撑过 20 波即通关，压力持续上升", 999, 200, 0, 0, 9, 1.20f, 1.05f, 1.00f, 20, 5, 6 },
    /* ---- 新五关：难度逐级递增，每关引入一条新的场地规则 ----
       lawnVariant 5=冰原 6=熔岩 7=虚空 8=电路 9=星界。
       数值只是托底；真正的难度来自规则变化（详见 docs/新关卡设计方案.md）。
       starReq 递增但不卡死：每关最多 3 星 × 11 关 + 20 成就 × 3 星 = 93 星可拿。 */
    { L"霜牙隘口", L"冰面让僵尸更快，也让你打得更远", 22, 200, 5, 0, 9, 1.55f, 1.08f, 1.10f, 22, 0, 0 },
    { L"熔心裂谷", L"裂隙轮换，阵地守不住就做冗余",   24, 225, 6, 0, 9, 1.75f, 1.12f, 1.25f, 28, 0, 0 },
    { L"幽影回廊", L"右四列被迷雾吞没，看不见敌人",   26, 250, 7, 0, 9, 2.00f, 1.18f, 1.35f, 34, 0, 0 },
    { L"机枢要塞", L"植物必须连成通电的链才能工作",   28, 275, 8, 0, 9, 2.25f, 1.22f, 1.45f, 40, 0, 0 },
    { L"星界王座", L"三阶段终局，规则每阶段改写",     30, 300, 9, 0, 9, 2.50f, 1.28f, 1.55f, 46, 0, 0 },
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
    L"不损失任何小推车通关", L"不用小推车且零植物损失通关", L"解锁全部普通植物",
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
    int   loadout[PT_COUNT];     /* 出战编组（1 = 该植物为开局出战植物），恒为单选 */
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
    int   lvStars[SAVE_LV_V2V4];
    int   lvUnlocked[SAVE_LV_V2V4];
    int   loadout[20];
    int   plantOwned[20];
    int   talents[TAL_COUNT];
    int   ach[ACH_COUNT];
    int   bestWave, gachaCount, totalKills, totalFreeze, totalBoom, fusionCount;
} SaveDataV2;

/* v3 存档：与 v3 时代的 SaveData 完全一致（loadout/plantOwned 已是当时的 PT_COUNT=90）。
   v4 只在尾部追加字段，前缀布局没动，所以整块 memcpy 再补默认值即可。 */
typedef struct {
    unsigned int magic, ver;
    int   stars, coins;
    int   lvStars[SAVE_LV_V2V4];
    int   lvUnlocked[SAVE_LV_V2V4];
    int   loadout[SAVE_PT_V3V5];
    int   plantOwned[SAVE_PT_V3V5];
    int   talents[TAL_COUNT];
    int   ach[ACH_COUNT];
    int   bestWave, gachaCount, totalKills, totalFreeze, totalBoom, fusionCount;
} SaveDataV3;

/* v4 存档：6 关时代的当前布局（= 972 字节）。v5 加了 5 个关卡，
   lvStars/lvUnlocked 变成 11 项，所以需要这一份留档来读出老进度。 */
typedef struct {
    unsigned int magic, ver;
    int   stars, coins;
    int   lvStars[SAVE_LV_V2V4];
    int   lvUnlocked[SAVE_LV_V2V4];
    int   loadout[SAVE_PT_V3V5];
    int   plantOwned[SAVE_PT_V3V5];
    int   talents[TAL_COUNT];
    int   ach[ACH_COUNT];
    int   bestWave, gachaCount, totalKills, totalFreeze, totalBoom, fusionCount;
    /* v4 追加的本地留存字段 */
    int   xp, level, bestKills, bestGold, bestTimeSec;
    int   lastCheckIn, checkStreak, dailyDoneDate, dailyBestWave;
} SaveDataV4;

/* v5 存档：已经是 11 关结构，但植物仍是 82 普通 + 8 神级。 */
typedef struct {
    unsigned int magic, ver;
    int   stars, coins;
    int   lvStars[LV_COUNT];
    int   lvUnlocked[LV_COUNT];
    int   loadout[SAVE_PT_V3V5];
    int   plantOwned[SAVE_PT_V3V5];
    int   talents[TAL_COUNT];
    int   ach[ACH_COUNT];
    int   bestWave, gachaCount, totalKills, totalFreeze, totalBoom, fusionCount;
    int   xp, level, bestKills, bestGold, bestTimeSec;
    int   lastCheckIn, checkStreak, dailyDoneDate, dailyBestWave;
} SaveDataV5;

static void saveCopyLegacyPlantSlots(const int *oldLoadout, const int *oldOwned,
                                     int oldCount, int oldHeroStart)
{
    int i, heroN = PT_COUNT - PT_HERO_FLAME;
    int normalN = oldHeroStart < oldCount ? oldHeroStart : oldCount;
    for (i = 0; i < normalN && i < PT_HERO_FLAME; i++) {
        gSave.loadout[i]    = oldLoadout[i];
        gSave.plantOwned[i] = oldOwned[i];
    }
    for (i = 0; i < heroN; i++) {
        int oi = oldHeroStart + i;
        int ni = PT_HERO_FLAME + i;
        if (oi >= oldCount || ni >= PT_COUNT) break;
        gSave.loadout[ni]    = oldLoadout[oi];
        gSave.plantOwned[ni] = oldOwned[oi];
    }
    /* 新增的普通植物里，普通稀有度仍遵守"初始可用"的规则。
       但**迁移路径不能因为下架而改写历史**：这里只对"存档里本来没有的新增位"
       生效，已经存在的老 plantOwned[...] 一律照原样搬过来（哪怕它已下架）——
       下架影响的是"能不能再拿到 / 看不看得见"，不是"把玩家已有的东西抹掉"。 */
    for (i = normalN; i < PT_HERO_FLAME; i++)
        if (plantDefs[i].rarity == 0 && !plantDefs[i].hero && !plantRetired(i))
            gSave.plantOwned[i] = 1;
}

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
        if (plantDefs[t].rarity == 0 && !plantDefs[t].hero && !plantRetired(t))
            gSave.plantOwned[t] = 1;
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
        memcpy(&old, raw, sizeof(old));
        memset(&gSave, 0, sizeof(gSave));
        gSave.magic = SAVE_MAGIC; gSave.ver = SAVE_VER;
        gSave.stars = old.stars; gSave.coins = old.coins;
        /* ⚠️ 长度必须用 sizeof(old.xxx)：旧的只有 6 项、新的有 11 项，
           写成 sizeof(gSave.xxx) 会从 old 里越界读 20 字节。 */
        memcpy(gSave.lvStars, old.lvStars, sizeof(old.lvStars));
        memcpy(gSave.lvUnlocked, old.lvUnlocked, sizeof(old.lvUnlocked));
        memcpy(gSave.talents, old.talents, sizeof(gSave.talents));
        memcpy(gSave.ach, old.ach, sizeof(gSave.ach));
        gSave.bestWave = old.bestWave; gSave.gachaCount = old.gachaCount;
        gSave.totalKills = old.totalKills; gSave.totalFreeze = old.totalFreeze;
        gSave.totalBoom = old.totalBoom; gSave.fusionCount = old.fusionCount;
        saveCopyLegacyPlantSlots(old.loadout, old.plantOwned, 20, 12);
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
        memcpy(gSave.lvStars, old.lvStars, sizeof(old.lvStars));       /* 长度见上 */
        memcpy(gSave.lvUnlocked, old.lvUnlocked, sizeof(old.lvUnlocked));
        saveCopyLegacyPlantSlots(old.loadout, old.plantOwned,
                                 SAVE_PT_V3V5, SAVE_HERO_FLAME_V3V5);
        memcpy(gSave.talents, old.talents, sizeof(gSave.talents));
        memcpy(gSave.ach, old.ach, sizeof(gSave.ach));
        gSave.bestWave = old.bestWave; gSave.gachaCount = old.gachaCount;
        gSave.totalKills = old.totalKills; gSave.totalFreeze = old.totalFreeze;
        gSave.totalBoom = old.totalBoom; gSave.fusionCount = old.fusionCount;
        gSave.level = 1; gSave.xp = 0;
        saveFlush();
        return;
    }
    /* ---- v4 → v5：6 关存档迁移到 11 关结构 ----
       这是玩家真实进度所在的版本（972 字节）。除了 lvStars/lvUnlocked
       要按新长度重排，其余字段的偏移全部改变，所以必须逐字段搬，
       不能用整块 memcpy（那正是 v5 之前几版的做法，长度一变就崩）。 */
    if (nread >= sizeof(SaveDataV4) && ((SaveDataV4 *)raw)->magic == SAVE_MAGIC &&
        ((SaveDataV4 *)raw)->ver == 4) {
        SaveDataV4 old;
        int i;
        memcpy(&old, raw, sizeof(old));
        memset(&gSave, 0, sizeof(gSave));
        gSave.magic = SAVE_MAGIC; gSave.ver = SAVE_VER;
        gSave.stars = old.stars; gSave.coins = old.coins;
        /* 旧档只有 6 关的数据，拷前 6 项；新增的 5 关保持 memset 的 0 */
        for (i = 0; i < SAVE_LV_V2V4; i++) {
            gSave.lvStars[i]    = old.lvStars[i];
            gSave.lvUnlocked[i] = old.lvUnlocked[i];
        }
        /* 兼容：v4 时代"星星够"也能解锁关卡。迁移时把这些补进 lvUnlocked，
           免得玩家更新后发现本来能玩的关卡反而被锁上了。
           只在迁移这一步做一次 —— 之后星星不再参与解锁。 */
        for (i = 0; i < SAVE_LV_V2V4; i++)
            if (gSave.stars >= levelDefs[i].starReq) gSave.lvUnlocked[i] = 1;
        gSave.lvUnlocked[0] = 1;        /* 第一关永远可玩 */
        saveCopyLegacyPlantSlots(old.loadout, old.plantOwned,
                                 SAVE_PT_V3V5, SAVE_HERO_FLAME_V3V5);
        memcpy(gSave.talents,    old.talents,    sizeof(old.talents));
        memcpy(gSave.ach,        old.ach,        sizeof(old.ach));
        gSave.bestWave = old.bestWave; gSave.gachaCount = old.gachaCount;
        gSave.totalKills = old.totalKills; gSave.totalFreeze = old.totalFreeze;
        gSave.totalBoom = old.totalBoom; gSave.fusionCount = old.fusionCount;
        gSave.xp = old.xp; gSave.level = old.level;
        gSave.bestKills = old.bestKills; gSave.bestGold = old.bestGold;
        gSave.bestTimeSec = old.bestTimeSec;
        gSave.lastCheckIn = old.lastCheckIn; gSave.checkStreak = old.checkStreak;
        gSave.dailyDoneDate = old.dailyDoneDate;
        gSave.dailyBestWave = old.dailyBestWave;
        saveFlush();
        return;
    }
    /* ---- v5 → v6：11 关结构不变，只在神级植物前插入 30 株普通植物 ---- */
    if (nread >= sizeof(SaveDataV5) && ((SaveDataV5 *)raw)->magic == SAVE_MAGIC &&
        ((SaveDataV5 *)raw)->ver == 5) {
        SaveDataV5 old;
        memcpy(&old, raw, sizeof(old));
        memset(&gSave, 0, sizeof(gSave));
        gSave.magic = SAVE_MAGIC; gSave.ver = SAVE_VER;
        gSave.stars = old.stars; gSave.coins = old.coins;
        memcpy(gSave.lvStars,    old.lvStars,    sizeof(old.lvStars));
        memcpy(gSave.lvUnlocked, old.lvUnlocked, sizeof(old.lvUnlocked));
        saveCopyLegacyPlantSlots(old.loadout, old.plantOwned,
                                 SAVE_PT_V3V5, SAVE_HERO_FLAME_V3V5);
        memcpy(gSave.talents, old.talents, sizeof(old.talents));
        memcpy(gSave.ach,     old.ach,     sizeof(old.ach));
        gSave.bestWave = old.bestWave; gSave.gachaCount = old.gachaCount;
        gSave.totalKills = old.totalKills; gSave.totalFreeze = old.totalFreeze;
        gSave.totalBoom = old.totalBoom; gSave.fusionCount = old.fusionCount;
        gSave.xp = old.xp; gSave.level = old.level;
        gSave.bestKills = old.bestKills; gSave.bestGold = old.bestGold;
        gSave.bestTimeSec = old.bestTimeSec;
        gSave.lastCheckIn = old.lastCheckIn; gSave.checkStreak = old.checkStreak;
        gSave.dailyDoneDate = old.dailyDoneDate;
        gSave.dailyBestWave = old.dailyBestWave;
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
    /* ⚠️ 必须排除已下架的植物：老存档里它们仍是 owned=1（普通档初见即送），
       算进去会让"分子超过分母"这类老问题重现。 */
    for (i = 0; i < PT_COLLECTIBLE; i++)
        if (!plantRetired(i) && gSave.plantOwned[i]) n++;
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
/* 通关需要的波数。
   正常关卡就是它的总波数；但「无尽 · 挑战」的 waves 是 999（永不结束），
   那样它就没法把解锁传给下一关 —— 玩家会永久卡在它后面（这是实测到的真问题：
   新加的 5 个关卡因此一个都进不去）。
   这里给它一个明确的通关里程碑，让「通关即解锁」这条链贯穿全部 11 关。
   取 20 不是随手定的：进度条上第 MAX_WAVES 波位置本来就画着终点旗。 */
static int levelClearWaves(int i)
{
    if (i < 0 || i >= LV_COUNT) return 1;
    /* ⚠️ 判据必须是 900 以上（无尽关标志），不能写成 > MAX_WAVES。
       MAX_WAVES=20 只是"里程碑间隔"，屋顶 24 波、沙漠 26 波都大于它 ——
       用 > MAX_WAVES 会把这些正常关卡的通关波数一起截断成 20，
       等于偷偷把 6 个关卡的难度砍掉一截（实测就是这么翻车的：
       选关界面里 24 波的屋顶也显示"20 波"）。 */
    if (levelDefs[i].waves > 900) return MAX_WAVES;
    return levelDefs[i].waves;
}

/* 关卡能不能进：**只看通关进度**。
   曾经是 `已通关解锁 || 星星 >= starReq` 的或关系，但那让「星星」成了
   第二条解锁路径 —— 玩家打完一关却发现下一关还锁着（因为星星不够），
   而且从界面上根本看不出"要多少星"和"要通关"哪个才是真正缺的。
   现在改成纯链式：通关第 N 关 → 解锁第 N+1 关，第一关永远可玩。
   starReq 字段保留在存档结构里（改结构会破坏二进制布局），但不再参与判定。 */
static int levelPlayable(int i)
{
    if (i < 0 || i >= LV_COUNT) return 0;
    return gSave.lvUnlocked[i];
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
    R_NIGHT_SUN,        /* 夜灯：天空阳光掉落间隔 -25%（与「绿洲」同效，待平衡） */
    R_ROOF_GRIP,        /* 防滑：屋顶关卡植物血量 +30% */
    R_DESERT_WATER,     /* 绿洲：沙漠关卡阳光掉落加速 40% */
    R_ENDLESS_HEAL,     /* 坚韧：每 3 波为受伤最重的植物回复 200 */
    R_STAR_HUNTER,      /* 猎星者：通关时额外获得 2 金币 */
    R_FUSION_MASTER,    /* 融合大师：合成后保留两只亲本的图鉴效果 */
    R_LOADOUT_PLUS,     /* 随机赠卡：获得时白送一张随机肉鸽植物卡（免费，不占开局卡位） */
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

/* ⚠️ 必须用**指定初始化** `[R_X] = {...}`，不能靠位置对齐。
   历史 bug（2026-09-20 复查发现）：这里原来是纯位置对齐的，
   而行顺序和上面的 enum 顺序不一致 —— enum 把 R_BANKER / R_NECROMANCER 放在 9/10，
   表里却把「银行家」「摄魂者」放在 45/46，于是中间约 38 个遗物
   「卡名/描述与真正生效的不是同一个」（点「磨刀石」实际拿到的是金气系统）。
   改成指定初始化后顺序不再承载语义，以后在中间插行也不会再错位。 */
static const RelicDef relicDefs[R_COUNT] = {
/*           名称          描述                                    稀有 类别 */
/* R_NONE */ [R_NONE] = { L"", L"", 0, 0 },
/* 经济 */
[R_START_SUN] = { L"储蓄罐",     L"开局阳光 +50",                              0, 0 },
[R_START_SUN_BIG] = { L"厚积薄发",   L"开局阳光 +120",                             1, 0 },
[R_SKY_SUN] = { L"晴空",       L"天空阳光掉落间隔 -15%",                     0, 0 },
[R_SUNFLOWER] = { L"光合作用",   L"向日葵每次产出 +12 阳光",                   0, 0 },
[R_SUNFLOWER_BIG] = { L"花团锦簇",   L"向日葵产出 +25，但费用 +20%",               1, 0 },
[R_KILL_SUN] = { L"拾荒者",     L"每击杀 1 只僵尸获得 +2 阳光",               0, 0 },
[R_KILL_SUN_BIG] = { L"战利品",     L"每击杀 +5 阳光，但植物费用 +8%",            1, 0 },
[R_INTEREST] = { L"利滚利",     L"每波结束时，按当前阳光的 8% 额外获得",      1, 0 },
/* 火力 */
[R_PEA_DMG] = { L"磨刀石",     L"豌豆伤害 +12%",                             0, 1 },
[R_PEA_DMG_BIG] = { L"精钢弹头",   L"豌豆伤害 +25%",                             1, 1 },
[R_RATE] = { L"疾风",       L"所有射手射击间隔 -12%",                     0, 1 },
[R_RATE_BIG] = { L"连弩",       L"射击间隔 -22%，但豌豆伤害 -10%",            1, 1 },
[R_BIG_CALIBER] = { L"大口径",     L"豌豆伤害 +45%，射击间隔 +20%",              1, 1 },
[R_GLASS_CANNON] = { L"玻璃大炮",   L"植物伤害 +80%，植物最大生命 -45%",         2, 1 },
[R_ARMOR_PIERCE] = { L"破甲",       L"对路障 / 铁桶僵尸伤害 +60%",                1, 1 },
[R_PIERCE] = { L"穿透弹",     L"豌豆命中后 25% 概率继续飞行再命中一次",     2, 1 },
[R_VOLLEY] = { L"齐射",       L"同一行 ≥4 株射手时，该行射击间隔 -30%",     1, 4 },
/* 生存 */
[R_PLANT_HP] = { L"沃土",       L"植物最大生命 +15%",                         0, 2 },
[R_PLANT_HP_BIG] = { L"巨人",       L"植物最大生命 +40%，卡片冷却 +15%",         1, 2 },
[R_THORNS] = { L"荆棘",       L"僵尸啃食植物时，每秒反受 22 伤害",          1, 2 },
[R_MEDIC] = { L"前线医疗",   L"每 12 秒治疗血量最低的植物 120",            1, 2 },
[R_SPARE_TIRE] = { L"备用轮胎",   L"小推车用掉后，30 秒自动补回",               2, 2 },
[R_NO_MOWER] = { L"背水一战",   L"移除全部小推车，全植物伤害 +65%",           2, 5 },
[R_IRON_WALL] = { L"铁壁",       L"坚果墙最大生命 +120%",                      0, 2 },
/* 控制 / 爆炸 */
[R_SLOW_LONG] = { L"霜冻",       L"减速持续时间 +50%",                         0, 3 },
[R_ICE_AGE] = { L"冰河期",     L"减速持续时间翻倍，减速幅度提升",            1, 3 },
[R_FROST_CORE] = { L"寒霜核心",   L"被减速的僵尸受到的所有伤害 +35%",           2, 4 },
[R_BOOM_R] = { L"火药桶",     L"爆炸半径 +20%",                             0, 3 },
[R_BOOM_DMG] = { L"烈性火药",   L"爆炸伤害 +55%",                             1, 3 },
[R_EMBER] = { L"余烬",       L"爆炸后原地留下 4 秒火焰，每秒 35 伤害",     2, 3 },
[R_CHAIN_BOOM] = { L"连锁引爆",   L"被爆炸炸死的僵尸会原地再爆一次",            2, 3 },
/* 构筑 / 协同 */
[R_COLUMN] = { L"同列共鸣",   L"同一列 ≥3 株植物时，该列伤害 +25%",         1, 4 },
[R_EMPTY] = { L"留白",       L"每个空单元格使相邻植物伤害 +7%",            1, 4 },
[R_FLOWER_SEA] = { L"花海",       L"每株向日葵使所有植物最大生命 +6%",          1, 4 },
[R_WALLNUT_DOGMA] = { L"坚果教条",   L"每株存活的坚果墙使射手伤害 +9%",            1, 4 },
[R_KILL_STACK] = { L"以战养战",   L"每击杀 1 只，全植物伤害本局永久 +0.6%（上限 +60%）", 2, 4 },
[R_SYMBIOSIS] = { L"温床",       L"相邻有同类植物的，伤害 +8%",                1, 4 },
[R_VANGUARD] = { L"尖兵",       L"最左列植物伤害 +40%，其余列 -10%",          1, 4 },
[R_BACKLINE] = { L"纵深防御",   L"最右列植物伤害 +60%",                       1, 4 },
/* 风险 / 取舍 */
[R_MINIMAL] = { L"极简主义",   L"植物费用 -35%，但卡片冷却 +60%",            1, 5 },
[R_NO_COOLDOWN] = { L"无冷却",     L"卡片冷却 -45%，但植物最大生命 -30%",       1, 5 },
[R_SUN_GAMBLE] = { L"阳光豪赌",   L"开局阳光 -120，但全部阳光产出翻倍",         2, 5 },
[R_BLOOD_PACT] = { L"血祭",       L"植物费用 -45%，但种下时损失 30% 生命",      1, 5 },
[R_ALL_IN] = { L"孤注一掷",   L"僵尸速度 +15%，但击杀阳光 x3",              1, 5 },
/* 模式 / 武器 / 货币 */
[R_BANKER] = { L"银行家",     L"解锁金气系统：击杀获得金气，可召唤增援",  1, 0 },
[R_NECROMANCER] = { L"摄魂者",     L"解锁幽能系统：减速获得幽能，强化夜间",     1, 0 },
[R_WEAPON_RACK] = { L"武器架",     L"为每株植物解锁副射槽，并随机给一种武器",  1, 1 },
[R_MAIN_UPGRADE] = { L"主射升级",   L"把所有植物的主射也换成随机武器",            2, 1 },
[R_WEAPON_MASTER] = { L"武器大师",   L"每株植物拥有双武器，攻击频次不变",          2, 1 },
[R_MODE_SHIFT] = { L"模式切换器", L"解锁第二种模式，游戏中可按 M 手动切换",  2, 4 },
[R_MUTANT_SEED] = { L"变异种子",   L"立即随机解锁一种新的种植模式",              1, 4 },
[R_WATER_MODE] = { L"水中模式",   L"游戏开始即解锁水中种植模式",                1, 4 },
[R_NIGHT_MODE] = { L"夜间模式",   L"游戏开始即解锁夜间种植模式",                1, 4 },
[R_NINJA] = { L"隐忍者",     L"种下植物不立即结算冷却，下一次种才扣",     1, 4 },
[R_CHARGED] = { L"充能心",     L"樱桃 / 辣椒的充能上限 +1，可放大招",       1, 4 },
[R_FURY] = { L"愤怒",       L"愤怒条涨速 +50%，满条按 E 引爆全屏雷击",    2, 4 },
/* 第二批 */
[R_SPIKE_DMG] = { L"尖刺淬毒",   L"地刺伤害翻倍",                              1, 1 },
[R_SPIKE_WIDE] = { L"尖刺蔓延",   L"地刺同时伤害相邻两行",                      1, 4 },
[R_MAGNET_FAST] = { L"强磁",       L"磁力菇吸取间隔减半",                        1, 2 },
[R_MAGNET_GOLD] = { L"磁化",       L"护具被吸走的僵尸掉落 15 金气",              1, 0 },
[R_CORN_STUN] = { L"黄油",       L"玉米投手眩晕时间翻倍",                      1, 3 },
[R_CORN_DMG] = { L"爆米花",     L"玉米投手伤害 +80%",                         1, 1 },
[R_THREE_UP] = { L"三线齐鸣",   L"三线射手上下两行伤害不再衰减",              2, 4 },
[R_THREE_MID] = { L"中路压制",   L"三线射手中间行伤害 +60%",                   1, 4 },
[R_BALLOON_POP] = { L"飞镖",       L"对气球僵尸伤害 +150%",                      1, 1 },
[R_ZAMBONI_BREAK] = { L"破冰",       L"冰车僵尸移动速度 -30%",                     1, 3 },
[R_DANCER_SILENCE] = { L"静音",       L"舞王僵尸无法召唤伴舞",                      1, 3 },
[R_NIGHT_SUN] = { L"夜灯",       L"天空阳光掉落间隔 -25%",                     1, 0 },
[R_ROOF_GRIP] = { L"防滑",       L"坚果 / 地刺最大生命 +80%",                  1, 2 },
[R_DESERT_WATER] = { L"绿洲",       L"天空阳光掉落间隔 -25%",                     2, 0 },
[R_ENDLESS_HEAL] = { L"坚韧",       L"每 3 波治疗受伤最重的植物 200",             1, 2 },
[R_STAR_HUNTER] = { L"猎星者",     L"击杀僵尸有 3% 概率掉落 1 金币",             1, 0 },
[R_FUSION_MASTER] = { L"融合大师",   L"合成的混种植物效果 +35%",                   2, 4 },
[R_LOADOUT_PLUS] = { L"随机赠卡",   L"立即白送一张随机肉鸽植物卡（免费，不占开局卡位）", 2, 4 },
[R_FIRST_STRIKE] = { L"先手",       L"开局额外获得 150 阳光",                     1, 0 },
[R_LAST_STAND] = { L"背水",       L"小推车全用完后，全植物伤害 +40%",           1, 5 },
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
/* 编组网格在 1000×650 画面里能完整显示的格数：8 列 × 4 行。
   第 5 行起点 y = 108 + 4×126 = 612，正好压到"已选候选"那行文字上，
   所以只有前 4 行可见。用它来判定"这个编制项玩家能不能看见/点到"。 */
#define LO_VISIBLE_ROWS  4
#define LO_VISIBLE_SLOTS (LO_COLS * LO_VISIBLE_ROWS)
#define LO_PAGE_PREV_X  720.0f
#define LO_PAGE_NEXT_X  842.0f
#define LO_PAGE_BTN_Y    72.0f
#define LO_PAGE_BTN_W   112.0f
#define LO_PAGE_BTN_H    30.0f
#define RW_CARD_W   200.0f
#define RW_CARD_H   268.0f
#define RW_GAP      40.0f
#define RW_Y        190.0f
static void loLayout(int i, float *x, float *y);
static int  loHit(int mx, int my);
static int  gLoadoutPage = 0;
static void rwLayout(int i, float *x, float *y);
static int  rwHit(int mx, int my);

static void metaLayoutLevel(int i, float *x, float *y);
static void doGacha(void);
static void rewardPrepare(void);
static void rewardChoose(int idx);
static void rgOpenDraft(int waveIdx);       /* 每波的肉鸽三选一：始终是免费奖励 */
static int  gDraftMode = 0;                /* 0 = 原遗物/神级池  1 = 肉鸽植物池 */
static void loadoutToggle(int t);
static void loadoutNormalize(void);     /* 定义在 loadoutBuild 之后，saveLoad 要用 */
static void metaLayoutTalent(int i, float *x, float *y);
static void drawMetaShapes(HDC dc);
static void drawMetaText(HDC dc);
static int  metaHitLevel(int mx, int my);
static int  metaHitTalent(int mx, int my);

static void applyWeaponRack(int sub);
static void applyMainUpgrade(void);

static unsigned char gRelic[R_COUNT];      /* 本局已获得的遗物（0/1） */
static int   gRelicCount = 0;

/* ---- 愤怒条 / 充能槽（relicsRecalc 会改 gFuryRate，所以放在它之前） ---- */
static float gFury = 0.0f;         /* 愤怒条：按 dt 累积到 gFuryMax */
static float gFuryMax = 45.0f;     /* 秒：满一次触发全屏雷击 */
static float gFuryRate = 1.0f;     /* 涨速系数：R_FURY 遗物 ×1.5 */
static int   gCharge = 0;          /* 充能槽 */
static float gFuryT = 0.0f;        /* 雷击表现：剩余时长（秒） */
static int   gFuryBolt = 0;        /* 本次雷击的形状种子（保证同一道雷不闪变） */
static int   gFuryReady = 0;       /* 已蓄满、等玩家按 E 引爆 */

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
/* 开局携带的硬上限：每一关开局只能选择 1 株植物。
   ------------------------------------------------------------------------
   这里是"开局选择"这一层的唯一真源。relicsRecalc 赋值走它、loadoutBuild
   取卡位走它、loadoutNormalize 收敛走它。任何入口（遗物加成、存档旧值、
   奖励自动加入）把这个上限顶大，都会被 loadoutCap() 夹回来。 */
#define LOADOUT_SLOTS_MAX 1
static int   gCardSlots = LOADOUT_SLOTS_MAX;       /* 战斗中的出战卡位（硬锁 1） */
static int   gCandidateSlots = LOADOUT_SLOTS_MAX;  /* 开战前的候选池上限（硬锁 1） */
static int loadoutCap(void)
{
    int c = (gCardSlots < gCandidateSlots) ? gCardSlots : gCandidateSlots;
    if (c < 1) c = 1;
    if (c > LOADOUT_SLOTS_MAX) c = LOADOUT_SLOTS_MAX;
    return c;
}
/* ---- 每局结算抽卡 ---- */
static int   gRewardPick[3];      /* 三个盲盒各自对应的植物（-1 = 已经是金币） */
static int   gRewardChosen;       /* 翻开了哪一张，-1 = 还没翻 */
static float gRewardT;            /* 翻开后的展示计时 */
static int   gRewardCoins;        /* 本次给了多少金币（植物全解锁时的补偿） */
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
    gFuryRate = 1.0f;

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
    /* 新规则：**每一关开局只能选择 1 株植物**，硬锁在 LOADOUT_SLOTS_MAX。
       永久编组（候选池）与实战携带共用同一个上限，两者必须相等，
       否则会出现"候选能选 2 个、开局只带 1 个"的错位。
       注意：R_LOADOUT_PLUS（扩容卡组）**不再放大这个上限** —— 它的效果改为
       "获得时白送一张随机肉鸽植物卡"，见 applyRelic() 里的 rgGrantStarterCard()。
       之所以不直接删掉这条遗物：gRelic[] 按枚举下标落盘，删/挪枚举会让旧存档
       的遗物整体错位，玩家会凭空多出/丢掉遗物。 */
    gCandidateSlots = LOADOUT_SLOTS_MAX;
    gCardSlots      = LOADOUT_SLOTS_MAX;
    /* 愤怒条涨速 +50%（叠加在常开的基础涨速上）；满条后可主动按 E 引爆 */
    if (HAS(R_FURY)) gFuryRate = 1.5f;
    if (HAS(R_FIRST_STRIKE)) gStartSunAdd += 150;
    if (HAS(R_DESERT_WATER)) gSkySunMul *= 0.75f;
    if (HAS(R_LAST_STAND))   gLastStand = 1;
    /* 这三个原来只写在 applyRelic 的 switch 里 —— 换个来源拿遗物就不生效。
       挪进 relicsRecalc，保证「持有即生效」，和其他遗物走同一条路。 */
    if (HAS(R_BANKER))       gBanker = 1;
    if (HAS(R_NECROMANCER))  gNecromancer = 1;
    gChargeMax = 1;
    if (HAS(R_CHARGED))      gChargeMax += 1;

    /* 局内成长卡：线性成长配合硬上限，避免后期攻速/冷却越界。 */
    gDmgMul      *= 1.0f + 0.10f * (float)gGrowthStack[GROW_DMG];
    {
        float m = 1.0f - 0.06f * (float)gGrowthStack[GROW_RATE];
        if (m < 0.60f) m = 0.60f;
        gRateMul *= m;
    }
    gPlantHpMul  *= 1.0f + 0.15f * (float)gGrowthStack[GROW_HP];
    gExplodeDmgMul *= 1.0f + 0.15f * (float)gGrowthStack[GROW_BOOM];
    gExplodeRadMul *= 1.0f + 0.05f * (float)gGrowthStack[GROW_BOOM];
    gSlowMul     *= 1.0f + 0.12f * (float)gGrowthStack[GROW_FROST];
    {
        float m = 1.0f - 0.08f * (float)gGrowthStack[GROW_CD];
        if (m < 0.55f) m = 0.55f;
        gCdMul *= m;
    }
    gPierceChance += 6 * gGrowthStack[GROW_PIERCE];
    if (gPierceChance > 60) gPierceChance = 60;

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
    /* 旧版即使数据表写了 500~650，运行时也会全部截成 200，
       阳光自然永远花不完。现在按 1.4 倍重定价，再以 25 为刻度取整，
       实际价格统一落在 50~500 的有意义区间。 */
    float c = plantDefs[t].cost * 1.40f * gCostMul;
    if (t == PT_SUNFLOWER) c *= gSunflowerCostMul;
    c += 12.5f;
    int v = c < 0.0f ? 0 : ((int)c / 25) * 25;
    if (v < 50) v = 50;
    if (v > 500) v = 500;
    return v;
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
static float zombieWaveHpMul(void)
{
    int w = gWave > 0 ? gWave - 1 : 0;
    return 1.0f + 0.12f * (float)w + 0.008f * (float)(w * w);
}
static float zombieWaveSpeedMul(void)
{
    int w = gWave > 0 ? gWave - 1 : 0;
    float m = 1.0f + 0.022f * (float)w;
    return m > 1.42f ? 1.42f : m;
}
static float zombieWaveBiteMul(void)
{
    int w = gWave > 0 ? gWave - 1 : 0;
    float m = 1.0f + 0.040f * (float)w;
    return m > 1.76f ? 1.76f : m;
}
static float zombieHpMax(int t){ return zHp[t] * gZombieHpMul * zombieWaveHpMul(); }


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
    /* 肉鸽植物索引 + 1（rgPlants[]），**0 = 普通植物**。
       用 +1 编码是为了兼容 memset(p, 0, sizeof) 的创建路径：
       漏设 rg 的地方会退回普通植物，而不是莫名变成 rgPlants[0]。 */
    int   rg;
    float rgt;        /* 肉鸽能力的节拍计时器（与基座的 timer 分开，互不打架） */
    int   rgStack;    /* 叠加类机制的层数（过载攻速），上限由机制自己钳 */
    float rgAux;      /* 叠加类机制的断档计时：归零就清层 */
    float rgMoveT;    /* 肉鸽移动/贷款等附加计时 */
    int   rgDebt;     /* 贷款植物的待偿阳光 */
    /* ---- 吞噬系机制用的三个新字段（2026-09-20 修复"吞噬功能无效"时加）----
       rgBaseHp：**出场血量基准**。成长类机制必须有基准才能设上限 ——
                 原来 RG_T_GROWEAT 每脉冲把 maxhp ×1.06 且无封顶，
                 40 个脉冲就涨到 10.3 倍（实测），再加每脉冲回满血 = 无敌植株。
       rgEatPend：待兑现的"确实吞掉了僵尸"次数（由 zombieDie 记账）。
                 原来根本没有"吃掉"这个前提，空场也在长。
       rgFused  ：FUSEPLANT 只能吞一次（原来每脉冲都叠乘一次）。 */
    float rgBaseHp;
    int   rgEatPend;
    int   rgFused;
    float rgBeat;     /* 音乐节拍豌豆的节拍层数：每拍 +1，上限 2（= 伤害 3 倍） */
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
    float flashPrev;  /* 上一帧的 flash：用上升沿检测「刚被命中」（冰面击退用） */
    float armSwing;
    int   eating;
    float eatBob;
    int   shield;     /* 铁门还在（正面减伤） */
    int   vaulted;    /* 撑杆僵尸已经跳过一次 */
    float atkCd;      /* 巨人砸植物的冷却 */
    float burrowT;    /* 潜地前摇剩余时间；期间显示目标并允许玩家集火 */
    float burrowTargetX;
    int   burrowPending;
    int   dead;       /* 正在播倒地动画 */
    float deadT;      /* 倒地动画剩余时间 */
    int   rg;         /* 肉鸽僵尸索引（rgZombies[]），-1 = 普通僵尸 */
    float rgt;        /* 肉鸽能力的节拍计时器 */
    int   gen;        /* 分身代数：0 = 本体，>0 = 克隆体（克隆体不再分身） */
    float basehp;     /* 出场时的血量基准，用于给"成长/自愈"设上限 */
    int   rgRevive;   /* 复生/死亡分叉/死亡诅咒只触发一次 */
    int   rgCurse;    /* 死亡诅咒已结算标记 */
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
                 int chainLeft;    /* 肉鸽连锁反弹次数；不与回旋镖/弹跳武器共用 */
                 int lastHit;      /* 上一个命中目标，避免在两只僵尸间原地重复碰撞 */
                 int aoe;          /* >0 = 命中时对周围这个半径(px)内的僵尸再补一刀 */
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

typedef struct { int type, row; float t; int rg; } Spawn;
/* rg 字段用「索引 + 1」编码，0 = 普通僵尸。
   这样 spawnQ 被零初始化时不会把所有普通僵尸都变成 rgZombies[0]。 */

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
static int   gPauseFrom = ST_PLAY;     /* 暂停菜单关闭后要回到战斗还是三选一 */
static float gTime    = 0;
/* 界面入场计时：每次 gState 变化时归零，供卡牌翻牌这类 UI 动画用。
   为什么不复用 gTime：gTime 只在 ST_PLAY 由 updateGame 累加，
   在菜单/结算里靠主循环的 else 分支补一点，语义是「游戏世界时间」。
   UI 动画需要「本界面显示了多久」，单独一个计时器才不会互相干扰。 */
static float gScreenT    = 0;
static int   gScreenPrev = ST_MENU;
static int   gSun     = 100;
static float cardCD[PT_COUNT];
/* 出战编组：本局实际带上场的植物编号（开局只带 1 株，见 LOADOUT_SLOTS_MAX） */
static int   gLoadout[PT_COUNT];
static int   gLoadoutN = 0;
/* 构建本局卡组。
   ⚠️ 上界必须是 PT_HERO_FLAME（普通植物），不能用 PT_COUNT。
   神级植物（PT_HERO_FLAME 起）的注释写得很明白："不在抽卡池里，
   只在三选一里以植物卡形式出现" —— 它们走战斗内的 bonus 卡槽，
   不该出现在编组卡组里。而编组界面也只画 [0, PT_HERO_FLAME)。
   用 PT_COUNT 的后果：旧存档迁移会把 loadout[12..19] 搬到
   loadout[PT_HERO_FLAME+i]（见 saveLoad），这些值虽然编组界面**看不见**，
   却会被这里塞进 gLoadout —— 玩家开局就多出几张"没选过、也没法取消"的卡。

   上界统一走 loadoutCap()（= LOADOUT_SLOTS_MAX = 1），
   不直接读 gCardSlots / gCandidateSlots —— 这样即使别处把两者之一改大，
   开局携带量仍然锁死在 1 株。 */
static void loadoutBuild(void)
{
    int i, n = 0, cap = loadoutCap();
    gLoadoutN = 0;
    /* 已下架的植物即使老存档里还残留 loadout=1，也一律当作没选 ——
       否则会出现"编组界面看不到它，但它却在开局卡槽里"的幽灵卡。 */
    for (i = 0; i < PT_HERO_FLAME && n < cap; i++)
        if (!plantRetired(i) && gSave.plantOwned[i] && gSave.loadout[i]) gLoadout[n++] = i;
    if (n > 0) { gLoadoutN = n; return; }
    /* 兜底：玩家一株都没选（或存档被清）时，自动挑第一株已拥有的植物顶上，
       保证开局手里一定有牌可打 —— 但同样只挑 1 株。 */
    for (i = 0; i < PT_HERO_FLAME && n < cap; i++)
        if (!plantRetired(i) && gSave.plantOwned[i]) {
            gSave.loadout[i] = 1; if (n < cap) gLoadout[n++] = i;
        }
    gLoadoutN = n;
}

/* 把"唯一出战植物"直接设成 pid（清空其余）。
   用于两条"自动把新植物加进编组"的路径：局内奖励解锁、三选一解锁。
   在上限锁 1 的规则下，语义从"带上去"变成"顶替成为唯一出战植物" ——
   否则会出现同屏 2 张卡（写第 2 个 loadout=1），破坏开局只能选 1 株的限制。 */
static void loadoutEquipOnly(int pid)
{
    int i;
    /* 这里必须挡住已下架的植物：loadoutEquipOnly 是"自动加入编组"的唯一入口，
       奖池已经过滤过，但保留这道闸门是为了防止将来有人从别处传 id 进来。 */
    if (pid < 0 || pid >= PT_HERO_FLAME || !gSave.plantOwned[pid]) return;
    if (plantRetired(pid)) return;
    for (i = 0; i < PT_HERO_FLAME; i++) gSave.loadout[i] = 0;
    gSave.loadout[pid] = 1;
    loadoutBuild();
}

/* 把候选编制收敛回当前上限。
   ------------------------------------------------------------------------
   为什么需要：候选上限曾经是 5、后来是 2，现在硬锁 1（LOADOUT_SLOTS_MAX），
   但**存档里的旧数据没有跟着收敛**。实测玩家档里 loadout 有 3 个置 1
   （索引 0 / 77 / 81），于是编组界面显示"已选候选 3 / 2" —— 数字自己就矛盾了。
   更麻烦的是多出来的那些往往落在网格可视区之外（编组网格一行 8 格、
   可见 4 行 = 32 格，而普通植物有 PT_HERO_FLAME=82 株），
   玩家想取消都点不到，只能看着一个点不掉的绿框。

   收敛规则（按索引从小到大保留）：
     · 未拥有的植物不该出现在编制里 —— 清掉；
     · 超出 loadoutCap() 的 —— 清掉（上限 1 时即"只留索引最小的那一株"）。
   收敛结果写回存档，下次启动就不会再出现。

   编组界面现在分页显示全部普通植物，因此高索引植物不再被当成“网格外”
   强制清除；它们可以在第 2/3 页正常查看和取消。 */
static void loadoutNormalize(void)
{
    int i, n = 0, changed = 0, cap = loadoutCap();
    for (i = 0; i < PT_HERO_FLAME; i++) {
        if (!gSave.loadout[i]) continue;
        if (!gSave.plantOwned[i] || plantRetired(i)) {   /* 没解锁 / 已下架的不能带 */
            gSave.loadout[i] = 0; changed = 1;
            continue;
        }
        n++;
        if (n > cap) {                       /* 超出上限的丢弃 */
            gSave.loadout[i] = 0; changed = 1;
        }
    }
    /* 上面只扫了普通植物；神级区间（>= PT_HERO_FLAME）本来就不该出现在编制里，
       旧档如果被写脏了，这里一并清掉。 */
    for (i = PT_HERO_FLAME; i < PT_COUNT; i++)
        if (gSave.loadout[i]) { gSave.loadout[i] = 0; changed = 1; }

    loadoutBuild();
    if (changed) saveFlush();                /* 收敛结果落盘，避免每次启动重算 */
}

/* 大厅里点一下：选择唯一的出战植物。
   ------------------------------------------------------------------------
   上限硬锁 1（LOADOUT_SLOTS_MAX），所以这里不是"多选 / 取消"而是"单选 / 顶替"：
     · 点已选中的那株 → 什么都不做（至少留一张，不允许出现 0 张）；
     · 点另一株       → 直接顶替，成为唯一出战植物。
   旧实现是"取消最后一个"的多选逻辑，在 cap=1 时会退化成一页点不动
   （已选中的取消不掉 + 候选池已满加不进来），因此必须换成单选取代语义。 */
static void loadoutToggle(int t)
{
    /* 范围同样是普通植物：和 loadoutBuild / 编组界面的绘制保持一致，
       否则统计出来的"已选 N"会把看不见的神级植物也算进去
       （表现是"已选 2/2"但网格里只有一个绿框）。 */
    if (t < 0 || t >= PT_HERO_FLAME || !gSave.plantOwned[t]) return;
    if (plantRetired(t)) return;             /* 已下架：点了也没用 */
    if (gSave.loadout[t]) return;            /* 已经是它了：保持至少 1 株，不取消 */
    loadoutEquipOnly(t);                     /* 清空其余编制 → 只留这一株 */
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

/* 本局通过三选一拿到的神级植物（额外卡槽，不占开局那唯一 1 个编组卡位） */
#define MAX_BONUS_PLANT 6
static int   gBonusPlant[MAX_BONUS_PLANT];
static int   gBonusN = 0;

/* 从「本局还没拿到的神级植物」里按稀有度权重抽一株。
   旧实现是「先按概率抽稀有度，再看那一档的两株是否拿过」——
   只要那一档的两株都拿过就直接返回 -1，哪怕别的档还有没拿的，
   于是三选一经常一张植物卡都不出。现在改成在整个剩余集合里抽。
   返回 -1 = MAX_BONUS_PLANT 株都拿过 / 本局植物卡槽已满。 */
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
    for (i = 0; i < PT_HERO_FLAME; i++)
        if (!gSave.plantOwned[i] && !plantRetired(i)) pool[n++] = i;
    if (n == 0) return -1;
    return pool[rand() % n];
}

/* ---- 三选一抽卡（肉鸽） ---- */
#define DRAFT_N      3
#define DRAFT_SKIP   75          /* 跳过是小补偿，不再是阳光通胀的主要来源 */
static int   gDraft[DRAFT_N];    /* 本次抽出的三个选项 id */
static int   gDraftHover = -1;
/* 本次植物卡的类型：0 = 神级（只在本局生效）  1 = 普通植物（永久解锁） */
static int   gDraftPlantPerm = 0;
static int   gWinDrafted = 0;    /* 通关时的「神级/遗物」三选一是否已发过 */
static int   gPendingWin = 0;    /* 1 = 这次三选一选完要进 ST_WIN 而不是回 ST_PLAY */
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

#if PLAT_HAS_OWN_MAIN
static int keyIsRepeat(LPARAM lp)
{
    return (lp & (1L << 30)) != 0;
}
#endif  /* PLAT_HAS_OWN_MAIN */  /* keyIsRepeat 只被 WndProc 的键盘消息用 */

/* ⚠️ toggleAllowed **不能**一起关掉 —— onClick() 会调它
   （点某些 UI 元素也触发全屏切换），而 onClick 是网页版的核心入口。
   第一版把它和 keyIsRepeat 关在同一个 #if 里，可移植侧立刻报
   'implicit declaration of toggleAllowed'。只有 keyIsRepeat 是桌面专属。 */
static int toggleAllowed(void)
{
    DWORD now = GetTickCount();
    if (now - gLastToggleTick < 350) return 0;
    gLastToggleTick = now;
    return 1;
}

/* 前向声明：onClick() 在 7095 就要用它，而它的定义在 8977（窗口那一段）。
   鼠标点右上角按钮和键盘 F11 走的是同一个函数，不另开一条实现 ——
   两条路径各写一份是这类"按钮点了没反应"Bug 的经典来源。 */
static void toggleFullscreen(void);

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

#if PLAT_HAS_OWN_MAIN
/* presentRelease 只有 WinMain 收尾调用（桌面 letterbox 缓冲）。
   ⚠️ 只排除 presentRelease，**不能**排除 presentEnsure ——
   后者被 render() 调用，可移植侧也在用。 */
static void presentRelease(void)
{
    if (gPresentBM) { DeleteObject(gPresentBM); gPresentBM = NULL; }
    if (gPresentDC) { DeleteDC(gPresentDC); gPresentDC = NULL; }
    gPresentW = gPresentH = 0;
}
#endif  /* PLAT_HAS_OWN_MAIN */

/* 窗口尺寸变了（全屏切换 / 拉伸）就把缓冲作废，下一帧重建 */
static void presentInvalidate(void)
{
    gPresentW = gPresentH = 0;
}

#if PLAT_HAS_OWN_MAIN
/* 窗口坐标 → 逻辑坐标。只有桌面端需要（消息里给的是窗口坐标）；
   网页版由外壳直接给逻辑坐标（它还负责 letterbox 与 CSS 缩放，见 web/shell.js）。
   ⚠️ #endif 必须紧跟在本函数之后 —— 第一版忘了闭合，
      把后面一大片**全局变量**（mDown / gWorldDC / gScaleXF …）全包进了条件里，
      Windows 侧因为 PLAT_HAS_OWN_MAIN=1 侥幸能编，可移植侧直接缺变量。
      这类"条件编译范围写错"的症状极难定位，写完立刻双端各编一次。 */
static void windowToGame(int wx, int wy, int *gx, int *gy)
{
    if (gViewScale <= 0.0001f) { *gx = wx; *gy = wy; return; }
    *gx = (int)((float)(wx - gViewOffX) / gViewScale);
    *gy = (int)((float)(wy - gViewOffY) / gViewScale);
}
#endif  /* PLAT_HAS_OWN_MAIN */

static int   mDown   = 0;
static float waveTimer = 0;
static float skySunTimer;
static float shakeT = 0, shakeMag = 0;
static float flashRed = 0;
static int   gKilled = 0;
static float gElapsed = 0;
static int   gMowerUsed = 0;
/* 后期特效负载控制：只缩减重复拖尾/短命粒子，不降低世界层分辨率与实体质量。 */
static int   gFxParticleLimit = MAX_PARTS;
static int   gFxParticleCursor = 0;
static int   gFxTrailSteps = 4;

static HDC     gWorldDC, gFrameDC, gScreenDC;
static HBITMAP gWorldBM, gFrameBM;
static void   *gWorldBits = NULL;       /* 32-bit 顶向 DIB，供 Direct3D 零转换上传 */
static XFORM   gScaleXF;

/* Direct3D 只负责最终呈现：GDI 继续生成高质量 2x 世界画布，GPU 负责纹理上传、
   等比缩放、黑边和垂直同步。初始化/设备恢复失败时自动回退到原 GDI 路径。 */
static IDirect3D9        *gD3D = NULL;
static IDirect3DDevice9  *gD3DDev = NULL;
static IDirect3DTexture9 *gD3DTex = NULL;
static D3DPRESENT_PARAMETERS gD3DPP;
static int gD3DW = 0, gD3DH = 0;
static int gGpuPresent = 0;

typedef struct GpuVertex {
    float x, y, z, rhw;
    float u, v;
} GpuVertex;

#define GPU_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static void gpuReleaseTexture(void)
{
    if (gD3DTex) { IDirect3DTexture9_Release(gD3DTex); gD3DTex = NULL; }
}

static int gpuCreateTexture(void)
{
    HRESULT hr;
    if (!gD3DDev) return 0;
    gpuReleaseTexture();
    hr = IDirect3DDevice9_CreateTexture(gD3DDev, VIEW_W * SS, VIEW_H * SS, 1,
             D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &gD3DTex, NULL);
    return SUCCEEDED(hr) && gD3DTex != NULL;
}

static int gpuReset(int w, int h)
{
    HRESULT hr;
    if (!gD3DDev || w <= 0 || h <= 0) return 0;
    gpuReleaseTexture();
    gD3DPP.BackBufferWidth = (UINT)w;
    gD3DPP.BackBufferHeight = (UINT)h;
    hr = IDirect3DDevice9_Reset(gD3DDev, &gD3DPP);
    if (FAILED(hr)) return 0;
    gD3DW = w; gD3DH = h;
    return gpuCreateTexture();
}

#if PLAT_HAS_OWN_MAIN
/* D3D 设备创建只在桌面端（WinMain）调用；可移植侧走 GDI 回退路径。
   把这一组排除掉，而不是用 pragma 压告警 —— 压制会让真正的问题也看不见。 */
static int gpuInit(HWND wnd, int w, int h)
{
    HRESULT hr;
    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    if (!wnd || !gWorldBits) return 0;
    gD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!gD3D) return 0;
    ZeroMemory(&gD3DPP, sizeof(gD3DPP));
    gD3DPP.Windowed = TRUE;                 /* 无边框全屏也保持 windowed，切换稳定 */
    gD3DPP.SwapEffect = D3DSWAPEFFECT_DISCARD;
    gD3DPP.hDeviceWindow = wnd;
    gD3DPP.BackBufferFormat = D3DFMT_UNKNOWN;
    gD3DPP.BackBufferWidth = (UINT)w;
    gD3DPP.BackBufferHeight = (UINT)h;
    gD3DPP.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    hr = IDirect3D9_CreateDevice(gD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                 flags, &gD3DPP, &gD3DDev);
    if (FAILED(hr)) {
        flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
        hr = IDirect3D9_CreateDevice(gD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                     flags, &gD3DPP, &gD3DDev);
    }
    if (FAILED(hr) || !gD3DDev) return 0;
    gD3DW = w; gD3DH = h;
    gGpuPresent = gpuCreateTexture();
    return gGpuPresent;
}

#endif  /* PLAT_HAS_OWN_MAIN */

static void gpuRelease(void)
{
    gpuReleaseTexture();
    if (gD3DDev) { IDirect3DDevice9_Release(gD3DDev); gD3DDev = NULL; }
    if (gD3D) { IDirect3D9_Release(gD3D); gD3D = NULL; }
    gD3DW = gD3DH = 0; gGpuPresent = 0;
}

/* 成功返回 1；设备丢失/驱动拒绝时返回 0，让调用方在这一帧走 GDI 回退。 */
static int gpuDrawFrame(int cw, int ch, int dx, int dy, int dw, int dh)
{
    D3DLOCKED_RECT lr;
    GpuVertex v[4];
    HRESULT hr;
    int y, srcPitch = VIEW_W * SS * 4;
    if (!gGpuPresent || !gD3DDev || !gWorldBits) return 0;
    if (cw != gD3DW || ch != gD3DH) {
        if (!gpuReset(cw, ch)) return 0;
    }
    hr = IDirect3DDevice9_TestCooperativeLevel(gD3DDev);
    if (hr == D3DERR_DEVICELOST) return 0;
    if (hr == D3DERR_DEVICENOTRESET && !gpuReset(cw, ch)) return 0;
    if (!gD3DTex && !gpuCreateTexture()) return 0;

    hr = IDirect3DTexture9_LockRect(gD3DTex, 0, &lr, NULL, D3DLOCK_DISCARD);
    if (FAILED(hr)) return 0;
    for (y = 0; y < VIEW_H * SS; y++)
        memcpy((unsigned char *)lr.pBits + y * lr.Pitch,
               (const unsigned char *)gWorldBits + y * srcPitch, (size_t)srcPitch);
    IDirect3DTexture9_UnlockRect(gD3DTex, 0);

    v[0] = (GpuVertex){(float)dx - 0.5f,      (float)dy - 0.5f,      0.0f, 1.0f, 0.0f, 0.0f};
    v[1] = (GpuVertex){(float)(dx + dw)-0.5f, (float)dy - 0.5f,      0.0f, 1.0f, 1.0f, 0.0f};
    v[2] = (GpuVertex){(float)dx - 0.5f,      (float)(dy + dh)-0.5f, 0.0f, 1.0f, 0.0f, 1.0f};
    v[3] = (GpuVertex){(float)(dx + dw)-0.5f, (float)(dy + dh)-0.5f, 0.0f, 1.0f, 1.0f, 1.0f};

    IDirect3DDevice9_Clear(gD3DDev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (FAILED(IDirect3DDevice9_BeginScene(gD3DDev))) return 0;
    IDirect3DDevice9_SetTexture(gD3DDev, 0, (IDirect3DBaseTexture9 *)gD3DTex);
    IDirect3DDevice9_SetRenderState(gD3DDev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(gD3DDev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(gD3DDev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetTextureStageState(gD3DDev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(gD3DDev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(gD3DDev, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetSamplerState(gD3DDev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(gD3DDev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(gD3DDev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(gD3DDev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetFVF(gD3DDev, GPU_FVF);
    hr = IDirect3DDevice9_DrawPrimitiveUP(gD3DDev, D3DPT_TRIANGLESTRIP, 2,
                                          v, sizeof(GpuVertex));
    IDirect3DDevice9_EndScene(gD3DDev);
    if (FAILED(hr)) return 0;
    hr = IDirect3DDevice9_Present(gD3DDev, NULL, NULL, NULL, NULL);
    return SUCCEEDED(hr);
}


/* 超采样倍数自适应。
   1080p 下像素密度已经足够高，再开 2x（3840×2160）只是白白多算 4 倍像素，
   肉眼几乎看不出差别，却会把降采样那一步变成掉帧主因。 */

/* ======================================================================== */
/*                                  辅助函数                                 */
/* ======================================================================== */
static inline float cellCX(int col) { return LAWN_X + col * CELL_W + CELL_W * 0.5f; }
static inline float cellBaseY(int row) { return LAWN_Y + row * CELL_H + CELL_H - 16.0f; }
static inline float rowCY(int row) { return LAWN_Y + row * CELL_H + CELL_H * 0.5f; }
#define MOWER_TRIGGER_BITE_X ((float)LAWN_X - 2.0f)
#define HOUSE_BREACH_BITE_X  ((float)LAWN_X - 44.0f)

static int plantFrontmostCol(int row);

/* 闪现、冲锋、拉拽等瞬时位移不能直接越过第一格触发小推车。
   它们最多把僵尸送到第一格中心，剩下这段必须正常走完。 */
static void zombieSpecialMove(Zombie *z, float dx)
{
    float oldX, nextX, floorX;
    int frontCol;
    if (!z) return;
    oldX = z->x;
    nextX = oldX + dx;
    floorX = cellCX(0);
    if (nextX < oldX) {
        /* 僵尸从右往左推进：同排最右侧植物就是前排。
           闪现、冲锋、跳跃和潜地都只能冲到前排面前，不能跨过去吃后排。 */
        frontCol = plantFrontmostCol(z->row);
        if (frontCol >= 0) {
            float stopX = (float)LAWN_X + (float)(frontCol + 1) * (float)CELL_W + 23.0f;
            if (nextX < stopX) nextX = (oldX > stopX) ? stopX : oldX;
        }
        if (oldX > floorX && nextX < floorX) nextX = floorX;
        else if (oldX <= floorX) nextX = oldX;
    }
    if (nextX > VIEW_W + 60.0f) nextX = VIEW_W + 60.0f;
    z->x = nextX;
}

/* 正向击退必须把僵尸推回右侧。 */
static void zombieKnockback(Zombie *z, float amount)
{
    if (!z || amount <= 0.0f) return;
    z->x += amount;
    if (z->x > VIEW_W + 60.0f) z->x = VIEW_W + 60.0f;
}

static Plant *plantAt(int row, int col)
{
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return NULL;
    if (grid[row][col].alive) return &grid[row][col];
    if (gCurMode == MODE_DOUBLE && grid2[row][col].alive) return &grid2[row][col];
    return NULL;
}

/* 僵尸从画面右侧进场，因此列号最大的存活植物是这一行的前排。 */
static int plantFrontmostCol(int row)
{
    int c;
    if (row < 0 || row >= ROWS) return -1;
    for (c = COLS - 1; c >= 0; c--)
        if (grid[row][c].alive || grid2[row][c].alive) return c;
    return -1;
}

static Plant *plantFrontmost(int row)
{
    int c = plantFrontmostCol(row);
    return c >= 0 ? plantAt(row, c) : NULL;
}

static int plantIsFrontmost(const Plant *p)
{
    return p && p->alive && p->col == plantFrontmostCol(p->row);
}

/* 修正已经绕到阵线后方的地面僵尸（旧存档/同帧跳跃也可能造成）。
   它会回到当前前排右侧，确保后排只有在前排消失后才进入交战。 */
static void zombieRespectFrontline(Zombie *z)
{
    int c;
    float stopX;
    if (!z || z->type == ZT_BALLOON) return;
    c = plantFrontmostCol(z->row);
    if (c < 0) return;
    stopX = (float)LAWN_X + (float)(c + 1) * (float)CELL_W + 23.0f;
    if (z->x < stopX) z->x = stopX;
}

/* 关卡环境可以削弱阵线，但不能让植物毫无交互地凭空消失。 */
static void plantHazardDamage(Plant *p, float damage)
{
    if (!p || !p->alive || damage <= 0.0f) return;
    p->hp -= damage;
    if (p->hp < 1.0f) p->hp = 1.0f;
    p->eaten = 0.8f;
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
/* 定义在文件后段的「新五关专属规则」里。这里先声明，因为禁种判断要用它 ——
   规则性拒绝（薄冰/供电）比范围性拒绝更硬，必须在 cellLegal 里最先判断。 */
static int newLevelCellBlocked(int r, int c);
static int newLevelRowPowered(int row);
static int newLevelZombieHidden(const Zombie *z);

static int cellLegal(int r, int c, int w, int hh)
{
    if (r < 0 || c < 0 || r >= ROWS || c >= COLS) return 0;
    if (c + w > COLS) return 0;
    if (r + hh > ROWS) return 0;
    if (!cellHasRoom(r, c, w, hh)) return 0;
    /* 关卡级种植列限制。levelDefs 里每关都填了 colFrom/colTo（后院·泳池的
       desc 明说"只能种在右侧的泥地上"），但这两个字段以前一次都没被读过，
       于是 6 个关卡在种植规则上完全一样。这里和 MODE_WATER 的"仅后 3 列"
       取交集——不是各自早退一次，取交集才能保证两个限制同时成立。 */
    /* 新五关的场地规则优先判断：薄冰不可种、供电必须连链。
       规则性拒绝排在列范围之前，玩家更能意识到「这是规则」而不是「我点偏了」。 */
    if (newLevelCellBlocked(r, c)) return 0;
    if (gCurLevel >= 0 && gCurLevel < LV_COUNT) {
        const LevelDef *ld = &levelDefs[gCurLevel];
        int lo = ld->colFrom, hi = ld->colTo;
        if (gCurMode == MODE_WATER && gCurLevel != 1 && ld->lawnVariant != 2 && lo < COLS - 3) lo = COLS - 3;
        if (hi > COLS - 1) hi = COLS - 1;
        if (c < lo) return 0;
        if (c + w - 1 > hi) return 0;
    } else if (gCurMode == MODE_WATER && c < COLS - 3) {
        return 0;
    }
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
    int n, i, limit = gFxParticleLimit;
    if (limit < 1 || limit > MAX_PARTS) limit = MAX_PARTS;
    /* 从上一次位置继续找，避免粒子接近满载时每次都从 0 扫到 500。
       只探测少量槽；确实满了就覆盖最旧游标，视觉上比整帧卡顿更自然。 */
    for (n = 0; n < 24; n++) {
        i = (gFxParticleCursor + n) % limit;
        if (!parts[i].active) break;
    }
    if (n >= 24) i = gFxParticleCursor % limit;
    gFxParticleCursor = (i + 1) % limit;
    parts[i].active = 1;
    parts[i].x = x; parts[i].y = y; parts[i].vx = vx; parts[i].vy = vy;
    parts[i].life = parts[i].maxlife = life;
    parts[i].size = size; parts[i].col = c; parts[i].grav = grav;
}

/* 屏幕震动：按需求【已关闭】。
   调用点保留（很多地方用它当"打击感"反馈），但不再产生任何位移 ——
   渲染端的 shakeT/shakeMag 逻辑留着，恒为 0 时等价于无效果。 */
static void addShake(float mag, float dur) { (void)mag; (void)dur; }

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

/* ========================================================================
   背景音乐（MCI）
   ------------------------------------------------------------------------
   为什么用 MCI 而不是 PlaySound：
     · PlaySound(SND_FILENAME | SND_LOOP) 会把**整个文件读进内存** ——
       第一关这首 34MB，一进关就顶掉 34MB 常驻；
     · MCI 的 "play … repeat" 是流式解码，内存几乎不涨，循环也无缝接。
   代价：编译命令要加 -lwinmm。
   缺文件时**静默跳过** —— 音乐是锦上添花，不该因为它让游戏起不来。
   ======================================================================== */
#define BGM_ALIAS   L"pvzbgm"
/* MCI 音量范围 0~1000。
   BGM 压到 110（≈11%）：用户反馈原来太吵，而且背景音一旦盖过音效，
   打击感就完全听不出来了 —— BGM 在这里的定位是"垫底氛围"，不是主角。 */
#define BGM_VOLUME  110
#define SFX_VOLUME  320

static int gBgmOn   = 1;    /* BGM 开关 */
static int gBgmOpen = 0;    /* 当前有没有已打开的 MCI 设备 */
static int gSfxOn   = 1;    /* 音效开关（与 BGM 分开，方便单独排查） */

static void bgmClose(void)
{
    if (!gBgmOpen) return;
    mciSendStringW(L"stop "  BGM_ALIAS, NULL, 0, NULL);
    mciSendStringW(L"close " BGM_ALIAS, NULL, 0, NULL);
    gBgmOpen = 0;
}

static void bgmPlayFile(const wchar_t *name)
{
    wchar_t path[MAX_PATH], cmd[MAX_PATH + 96];
    const wchar_t *type = L"waveaudio";
    const wchar_t *dot;
    bgmClose();
    if (!gBgmOn || !name || !*name) return;
    wsprintfW(path, L"%s%s", gAssetDir, name);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    /* MP3 必须走 mpegvideo 解码器：waveaudio 只认 WAV，
       喂 MP3 给它会直接报 -296 "无法在指定的 MCI 设备上播放指定的文件"，
       而且这条失败是**静默**的（下面只判断非 0 就 return）——
       表现成"这首歌没声音"，很容易被误当成文件没下全。
       实测：同一首 mp3 用 mpegvideo open 后 status mode = 'playing'，
       用 waveaudio open 直接失败。 */
    dot = wcsrchr(name, L'.');
    if (dot && (_wcsicmp(dot, L".mp3") == 0 || _wcsicmp(dot, L".m4a") == 0))
        type = L"mpegvideo";
    wsprintfW(cmd, L"open \"%s\" type %s alias " BGM_ALIAS, path, type);
    if (mciSendStringW(cmd, NULL, 0, NULL) != 0) return;
    gBgmOpen = 1;
    /* BGM 音量压到很低：用户反馈"背景音乐太大"，压过音效就听不见打击感了。
       MCI 的音量范围是 0~1000，这里给 110（约 11%）。 */
    wsprintfW(cmd, L"setaudio " BGM_ALIAS L" volume to %d", BGM_VOLUME);
    mciSendStringW(cmd, NULL, 0, NULL);
    /* ⚠️ 不能假定 repeat 一定被支持。
       实测（_mci_test.c）：waveaudio 播 WAV 时 `play t repeat` 返回
       **错误 259**、mode 立刻变成 stopped —— 也就是"打开成功但不出声"；
       而 mpegvideo 播 MP3 时 repeat 正常。
       所以这里失败就退回不带 repeat 的单次播放，
       循环由 bgmTick() 检测到 stopped 后自动续播。 */
    if (mciSendStringW(L"play " BGM_ALIAS L" repeat", NULL, 0, NULL) != 0)
        mciSendStringW(L"play " BGM_ALIAS, NULL, 0, NULL);
}

/* 维持 BGM 循环。
   为什么不能只靠 MCI 的 repeat：见 bgmPlayFile 的注释 ——
   waveaudio 不支持这个标志。这里每 500ms 查一次状态，
   播完了就重新 play，等效于循环，且对所有解码器都成立。
   500ms 是刻意的：MCI 状态查询会走一次内核调用，
   每帧都查没必要（循环处会有一个不到半秒的静默间隙，听感上察觉不到）。 */
static void bgmTick(void)
{
    static DWORD nextCheck = 0;
    wchar_t mode[32];
    DWORD now;
    if (!gBgmOn || !gBgmOpen) return;
    now = GetTickCount();
    if (now < nextCheck) return;
    nextCheck = now + 500;
    mode[0] = 0;
    if (mciSendStringW(L"status " BGM_ALIAS L" mode", mode, 32, NULL) != 0) return;
    if (_wcsicmp(mode, L"stopped") == 0)
        mciSendStringW(L"play " BGM_ALIAS, NULL, 0, NULL);
}

/* ========================================================================
   音效（MCI 设备池）
   ------------------------------------------------------------------------
   为什么用"池"而不是单个设备：
     MCI 的同一个 alias 同时只能放一个声音 —— 一群豌豆同时命中时，
     后到的音会把前一个**掐断**，听感上是"打击感时有时无、单薄"。
     开 8 个槽轮转，同一瞬间的几个音就能自然叠起来（打击感的关键）。
   8 是权衡：够覆盖同屏密集触发，又不至于开太多内核设备。
   ======================================================================== */
#define SFX_POOL 8

typedef struct {
    int     used;              /* 是否已 open */
    wchar_t file[64];          /* 当前打开的文件名（不含扩展名） */
    wchar_t alias[16];         /* MCI 别名 sfx0..sfx7 */
} SfxSlot;

static SfxSlot gSfx[SFX_POOL];
static int     gSfxRR   = 0;   /* 槽满时的抢占用游标 */
static int     gSfxInit = 0;

static void sfxInit(void)
{
    int i;
    if (gSfxInit) return;
    for (i = 0; i < SFX_POOL; i++)
        wsprintfW(gSfx[i].alias, L"pvzsfx%d", i);
    gSfxInit = 1;
}

/* 播放音效。name = assets/sfx/ 下的文件名（不含 .wav）。
   并发策略：优先复用"正在放同一个音"的槽 → 其次空槽 → 都没有就抢最旧的。 */
static void sfxPlay(const wchar_t *name)
{
    wchar_t path[MAX_PATH], cmd[MAX_PATH + 128];
    int i, slot = -1;
    DWORD now;
    static DWORD lastHit = 0, lastDeath = 0;

    if (!gSfxOn || !name || !*name || !gAssetDir[0]) return;
    /* MCI 是同步调用。后期密集弹幕会在一帧内产生几十次命中，逐次 restart
       同一音效既听不出更多层次，又会把主线程卡在内核调用里。 */
    now = GetTickCount();
    if (wcscmp(name, L"hit") == 0 || wcscmp(name, L"hit_hard") == 0) {
        if ((DWORD)(now - lastHit) < 32) return;
        lastHit = now;
    } else if (wcscmp(name, L"zombie_die") == 0) {
        if ((DWORD)(now - lastDeath) < 45) return;
        lastDeath = now;
    }
    sfxInit();

    for (i = 0; i < SFX_POOL; i++)
        if (gSfx[i].used && wcscmp(gSfx[i].file, name) == 0) { slot = i; break; }
    if (slot < 0)
        for (i = 0; i < SFX_POOL; i++)
            if (!gSfx[i].used) { slot = i; break; }
    if (slot < 0) {
        slot = gSfxRR;
        gSfxRR = (gSfxRR + 1) % SFX_POOL;
        wsprintfW(cmd, L"close %s", gSfx[slot].alias);
        mciSendStringW(cmd, NULL, 0, NULL);
        gSfx[slot].used = 0;
        gSfx[slot].file[0] = 0;
    }

    if (!gSfx[slot].used) {                 /* 换音才重新 open，同音复用 */
        wsprintfW(path, L"%ssfx\\%s.wav", gAssetDir, name);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
        wsprintfW(cmd, L"open \"%s\" type waveaudio alias %s", path, gSfx[slot].alias);
        if (mciSendStringW(cmd, NULL, 0, NULL) != 0) return;
        wsprintfW(cmd, L"setaudio %s volume to %d", gSfx[slot].alias, SFX_VOLUME);
        mciSendStringW(cmd, NULL, 0, NULL);
        gSfx[slot].used = 1;
        wcscpy(gSfx[slot].file, name);
    }
    /* `from 0` 必须带：不带的话第二次播放会接在上次结束的位置继续，听不见 */
    wsprintfW(cmd, L"play %s from 0", gSfx[slot].alias);
    mciSendStringW(cmd, NULL, 0, NULL);
}

/* 关卡 → BGM 文件名。下标 = gCurLevel，NULL = 该关暂无专属音乐。
   第一关由用户 2026-09-17 指定为 jazz_ai_only_Jazz AI.wav。
   ★ 2026-09-21：归档格式从 wav 换成 mp3（34.0MB → 3.1MB，同源同曲）。
     MCI 播 mp3 早已支持（见 bgmOpen 里的 mpegvideo 分支），所以只需改这一行。
     网页版尤其需要 —— 34MB 的 BGM 会让首屏加载多等十几秒。 */
static const wchar_t *BGM_BY_LEVEL[LV_COUNT] = {
    L"bgm_level1.mp3",   /* 0 前院 · 白天   用户指定的爵士乐 */
    L"bgm/lv1.mp3",      /* 1 后院 · 泳池   电子 / 平静 */
    L"bgm/lv2.mp3",      /* 2 月夜 · 墓园   节奏稍紧 */
    L"bgm/lv3.mp3",      /* 3 屋顶 · 天台   明亮 */
    L"bgm/lv4.mp3",      /* 4 沙漠 · 遗迹   沉稳 */
    L"bgm/lv5.mp3",      /* 5 无尽 · 挑战   快节奏 */
    /* 新五关：情绪跟着关卡性质走（见 docs/美术与音乐素材清单.md）。
       ice=冷冽疏离 / magma=沉重压迫 / void=诡异悬疑 / circuit=机械律动 / astral=史诗终局 */
    L"bgm/lv6_frostbite.mp3",  /*  6 霜牙隘口 */
    L"bgm/lv7_emberrift.mp3",  /*  7 熔心裂谷 */
    L"bgm/lv8_phantom.mp3",    /*  8 幽影回廊 */
    L"bgm/lv9_cogwork.mp3",    /*  9 机枢要塞 */
    L"bgm/lv10_astral.mp3",    /* 10 星界王座 */
};

/* ★ 始祖主题的三个全局量必须声明在 bgmForLevel **之前** ——
   bgmForLevel 要用它们判断"主题是否已开启"（C 是先声明后使用，
   放到函数后面会直接编译不过，这一步踩过一次）。 */
#define LV_VARIANT_PRIMORDIAL  10
static const wchar_t *BGM_PRIMORDIAL = L"bgm/primordial.mp3";
static int gPrimordialTheme = 0;

static void bgmForLevel(int lv)
{
    /* ⚠️ 始祖主题一旦开启就**不回退**：换关也不换回关卡 BGM。
       用户要求"后续除非出现下一个始祖级，不然不会再变化音乐和背景"，
       所以这里的优先级高于关卡音乐 —— 这也是这个主题"停不下来"的原因，
       要恢复常态只能重开一局（gPrimordialTheme 是局内全局，不落盘）。 */
    if (gPrimordialTheme) { bgmPlayFile(BGM_PRIMORDIAL); return; }
    if (lv >= 0 && lv < LV_COUNT && BGM_BY_LEVEL[lv] != NULL)
        bgmPlayFile(BGM_BY_LEVEL[lv]);
    else
        bgmClose();     /* 其余关卡暂无音乐：别把上一关的留在后台响 */
}

/* ==================== 始祖主题：背景 + 音乐 ====================
   用户要求：三尊始祖**出现**（出现在三选一的三个选项里）时必定
   改写背景与音乐，并且此后不再变化 —— 直到下一个始祖级出现才再变一次。

   实现要点：
     ① 背景走"草坪变体"这条既有通路（gLawnVariant），而不是另开一张全屏图：
        变体已经有 10 档（普通/夜间/水中/屋顶/沙漠/冰原/熔岩/虚空/电路/星界），
        背景绘制函数按变体选贴图，加第 11 档只需要多一个 case + 一张 lawn_primordial。
        贴图缺失时会自动退回程序化草坪（drawBackground 里已有兜底），
        所以**先上线代码、后补美术**是安全的。
     ② 音乐走 bgmPlayFile（MCI）。文件不存在时它自己 return，不会崩，
        表现是"主题切了但没声音"，不会静默损坏别的东西。
     ③ 持久化靠一个局内全局 gPrimordialTheme：
        · bgmForLevel() 开头就检查它 → 换关也不换回关卡音乐；
        · 关卡启动处设置 gLawnVariant 之后再用它覆盖一次 → 换关也不换回关卡背景。
     ④ 每次始祖出现都**重新执行**一遍（重放音乐、再闪一次屏），
        这样"下一个始祖级出现时才会再变化"是肉眼可见的。 */

static void primordialThemeApply(void)
{
    gPrimordialTheme = 1;
    gLawnVariant = LV_VARIANT_PRIMORDIAL;
    bgmPlayFile(BGM_PRIMORDIAL);
    flashRed = 0.55f;          /* 一次性全屏闪光：让玩家立刻注意到世界变了 */
}

/* 某一关"应该用哪个草坪变体"。
   抽成函数是为了两件事：
     ① 始祖主题的"不回退"只在一个地方实现 —— 关卡启动路径和测试走同一份逻辑，
        不会出现"测试过了、实机没生效"；
     ② 测试可以直接调它验证持久性，不必去模拟整个点击链路。 */
static int levelLawnVariant(int lv)
{
    int v = (lv >= 0 && lv < LV_COUNT) ? levelDefs[lv].lawnVariant : 0;
    if (gPrimordialTheme) v = LV_VARIANT_PRIMORDIAL;   /* 始祖主题一旦开启就不回退 */
    return v;
}

/* 扫描本次三选一的三个选项，只要出现始祖级就切主题。
   抽成独立函数是为了**可测** —— 测试可以直接摆 gDraft[] 再调它，
   不需要真的靠 1% 的运气抽出来。 */
static int draftScanPrimordial(void)
{
    int k, hit = 0;
    for (k = 0; k < DRAFT_N; k++) {
        int v = gDraft[k];
        if (DRAFT_IS_RG(v) && rgPlants[v - RG_CARD_BASE].tier == RGQ_PRIMORDIAL) hit = 1;
    }
    if (hit) primordialThemeApply();
    return hit;
}

/* ==================== 三选一开牌庆典（殿堂级以上）====================
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

    /* 开牌 stinger（殿堂 / 始祖各一首，程序化合成，见 art/_gen_bgm_tiers.py）。
       ⚠️ 走 sfxPlay（8 槽独立 MCI 设备）而不是 bgmPlayFile：
          后者是**单设备 BGM**，播 stinger 会把关卡 BGM 顶掉、放完还不回来；
          音效池播完自动结束，BGM 完全不受影响 —— 这是"既要音效又要背景乐"
          在本引擎里唯一可行的走法。
       文件缺失时 sfxPlay 自己 return（它先 GetFileAttributesW 判存在），
       所以**先合代码后补素材是安全的**，表现只是"演出没声音"。 */
    sfxPlay(gCelebPrim ? L"celeb_primordial" : L"celeb_hall");
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
/* 新五关的草坪变体：冰原 / 熔岩 / 虚空 / 电路 / 星界 */
static Sprite gSprLawnIce, gSprLawnMagma, gSprLawnVoid, gSprLawnCircuit, gSprLawnAstral;
/* 始祖主题专用草坪：只有在始祖级出现后才用得上，缺图会自动退回程序化草坪 */
static Sprite gSprLawnPrimordial;
static Sprite gSprWeapon[5];                  /* 5 种武器植物 */
static Sprite gSprShape[3];                   /* 多形态植物: 0=双株 1=多列 2=塔防1x2 */
static Sprite gSprGold, gSprGhost, gSprPumpkin;  /* 货币 / 增援 */
static Sprite gSprCardBack, gSprBurst;           /* 抽卡用 UI */
static Sprite gSprMenuBg, gSprMetaBg;            /* 大厅 / 元界面手绘背景 */
enum { BTN_GREEN, BTN_DARK, BTN_GOLD, BTN_PURPLE, BTN_RED, BTN_BLUE, BTN_STYLE_N };
enum { BTN_NORMAL, BTN_HOT, BTN_DOWN, BTN_STATE_N };
static Sprite gSprButton[BTN_STYLE_N][BTN_STATE_N]; /* 手绘木牌按钮：颜色 x 交互状态 */
static Sprite gSprButtonDisabled;
enum { UII_SUN, UII_COIN, UII_STAR, UII_XP,
       UII_KILL, UII_WAVE, UII_HEART, UII_DAMAGE,
       UII_COOLDOWN, UII_ICE, UII_FIRE, UII_POISON,
       UII_SHIELD, UII_PAUSE, UII_FULLSCREEN, UII_BACK, UII_COUNT };
static Sprite gSprUiIcon[UII_COUNT];              /* 统一的黄铜叶片 UI 图标 */
static Sprite gSprGrowth[GROW_COUNT];             /* 七张成长卡的独立插画 */

/* 档位徽章（显示在卡片档位标签处）。下标与 RGQ_NAME 对齐（0=普通 … 7=始祖）。
   缺图时自动退回"文字标签 + 色条"的老画法，所以是纯增强。 */
static Sprite gSprBadge[RGQ_COUNT];

/* 开牌庆典的叠加光效。缺图时庆典依然完整（光扇/冲击波/火花本来就是程序化绘制），
   贴图只是"再多一层"——整包缺这六张也不影响游戏。 */
enum { FSX_RAYS, FSX_SHOCK, FSX_SPARK, FSX_PILLAR, FSX_FLARE, FSX_RUNES, FSX_N };
static Sprite gSprFx[FSX_N];

/* ---- 子弹特效 ----
   以前豌豆是"纯色椭圆"，30 多株植物打出来的东西长得一模一样。
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
    case PT_BUBBLELOTUS:
    case PT_BUBBLECANNON:
    case PT_NEBULABEET:
    case PT_LANTERNMELON:  return &gSprBullet[BUL_SPLASH];
    case PT_ECHOBAMBOO:
    case PT_SONICBLOOM:
    case PT_MONSOONREED:   return &gSprBullet[BUL_ARC];
    case PT_RUNECACTUS:
    case PT_PAPERCRANE:    return &gSprBullet[BUL_SPINE];
    case PT_COMETCLOVER:
    case PT_GEARFLOWER:
    case PT_PAINTBRUSH:
    case PT_DUSKORCHID:    return &gSprBullet[BUL_STAR];
    case PT_PRISMWILLOW:   return &gSprBullet[BUL_BEAM];
    case PT_CANDLESPROUT:
    case PT_STEAMPEPPER:   return &gSprBullet[BUL_BURN];
    case PT_QUARTZKERNEL:  return &gSprBullet[BUL_KERNEL];
    case PT_AURORAPEA:     return &gSprBullet[BUL_ICE];
    case PT_HERO_FLAME:    return &gSprBullet[BUL_FLAME];
    case PT_HERO_CRYSTAL:  return &gSprBullet[BUL_CRYSTAL];
    case PT_HERO_FROST:    return &gSprBullet[BUL_FROST];
    case PT_HERO_DOOM:     return &gSprBullet[BUL_DOOM];
    default:               return &gSprBullet[BUL_PEA];
    }
}
static Sprite gSprSun;
static Sprite gSprLawn;

/* 地形贴图：索引与地块位一一对应
   0 焦土 / 1 冰面 / 2 蛛网 / 3 腐蚀 / 4 花蔓（见 TF_* 宏）
   这五张原来是拿 GDI 基元硬画的（棕色方块 / 蓝线 / 白圆），
   现在换成 mmx 生成的美术资源。 */
#define TILE_SPR_N 5
static Sprite gSprTile[TILE_SPR_N];
static Sprite gSprBackground;
static int    gArtLoaded = 0;   /* 成功载入的资源张数，用于 HUD 提示 */

/* ---- 肉鸽专属单位贴图（50 株 + 50 只） ----
   以前肉鸽植物/僵尸是复用基座贴图（同一个豌豆射手换个数值而已），
   50 种创意设定在画面上完全看不出来。现在每个肉鸽单位都有独立美术资源。
   命名 rgplant_00..49 / rgzombie_00..49，rg 索引 = 数组下标。 */
static Sprite gSprRgPlant[RG_PLANT_N];
static Sprite gSprRgZombie[RG_ZOMBIE_N];
static Sprite gSprRgZombieFlash[RG_ZOMBIE_N];   /* 受击白剪影 */
static Sprite gSprRgZombieBlue[RG_ZOMBIE_N];    /* 减速蓝剪影 */
static Sprite gSprRgZombieDead[RG_ZOMBIE_N][5]; /* 倒地帧（离线绕脚底派生） */

/* 取肉鸽单位贴图。rgIdx 是 1-based（0 表示普通单位，与 rg 字段一致）。
   没有专属资源时返回 NULL，调用方自行退回基座贴图。 */
static Sprite *rgPlantSprite(int rgIdx)
{
    if (rgIdx <= 0 || rgIdx > RG_PLANT_N) return NULL;
    return gSprRgPlant[rgIdx - 1].ok ? &gSprRgPlant[rgIdx - 1] : NULL;
}

static Sprite *rgZombieSprite(int rgIdx)
{
    if (rgIdx <= 0 || rgIdx > RG_ZOMBIE_N) return NULL;
    return gSprRgZombie[rgIdx - 1].ok ? &gSprRgZombie[rgIdx - 1] : NULL;
}

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
    unsigned char *px;
    int w = 0, h = 0, comp = 0, y;
    BITMAPINFO bi;
    void *bits = NULL;

    memset(s, 0, sizeof(*s));
    fp = _wfopen(path, L"rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END); size = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (size < 16) { fclose(fp); return 0; }
    buf = (unsigned char *)malloc((size_t)size);
    if (!buf) { fclose(fp); return 0; }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) { free(buf); fclose(fp); return 0; }
    fclose(fp);

    /* 4 = 强制 RGBA。JPEG 没有 alpha 通道，stb 会把 A 填 255，
       正好等于"整屏不透明"的预期，不需要额外分支。
       PNG 里存的是**已预乘**的字节（见 art/_pack_assets.py 的说明），
       stb 不会做任何 alpha 变换，所以拿到的就是原值。 */
    px = stbi_load_from_memory(buf, (int)size, &w, &h, &comp, 4);
    free(buf);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return 0; }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;            /* 负值 = 自上而下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!s->bmp || !bits) { stbi_image_free(px); return 0; }
    s->dc = CreateCompatibleDC(NULL);
    SelectObject(s->dc, s->bmp);
    /* stb 给的是 RGBA；引擎全程按 BGRA 走，这里换一次通道顺序。
       只换通道、不动数值，所以预乘关系保持不变。 */
    for (y = 0; y < h; y++) {
        const unsigned char *sp = px + (size_t)y * (size_t)w * 4;
        unsigned char *dp = (unsigned char *)bits + (size_t)y * (size_t)w * 4;
        int x;
        for (x = 0; x < w; x++, sp += 4, dp += 4) {
            dp[0] = sp[2]; dp[1] = sp[1]; dp[2] = sp[0]; dp[3] = sp[3];
        }
    }
    stbi_image_free(px);
    s->bits = bits; s->w = w; s->h = h; s->ok = 1;
    return 1;
}

/* 资源打包后有两套扩展名：带透明的存 PNG、整屏不透明的存 JPEG。
   这里按固定顺序试探，找不到就交给下一段（最后落到"缺图"的兜底）。 */
static int spriteLoadName(Sprite *s, const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    /* 顺序：.png → .jpg → .bmp。
       .bmp 放在**最后**：打包后的发行包里只会有 PNG/JPEG，
       所以正常路径一次命中，回退分支在发行包里永远不走（零开销）。 */
    wsprintfW(path, L"%s%s.png", gAssetDir, name);
    if (spriteLoad(s, path)) return 1;
    wsprintfW(path, L"%s%s.jpg", gAssetDir, name);
    if (spriteLoad(s, path)) return 1;
    /* .bmp 回退：只跑过 `_build_units.py`（还没跑 `_pack_assets.py`）的
       新素材会停在这一种格式上。没有这条回退，新素材会**静默**变成空白卡面
       —— 2026-09-21 那 50 张新卡就是这么全部加载失败的。 */
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

/* 横向三切片按钮。源图上下整体缩放、中央横向拉伸，左右雕花和木框不会
   随按钮宽度一起变胖；这让 124px 的返回键和 400px 的跳过条能共用一套美术。 */
static void spriteBlitButton(HDC dst, const Sprite *s,
                             float x, float y, float w, float h, int constAlpha)
{
    BLENDFUNCTION bf;
    int dl, dt, dw, dh, srcCap, dstCap, srcMid, dstMid;
    if (!s || !s->ok || w <= 0.0f || h <= 0.0f) return;
    dl = (int)(x * SS + 0.5f); dt = (int)(y * SS + 0.5f);
    dw = (int)(w * SS + 0.5f); dh = (int)(h * SS + 0.5f);
    if (dw <= 2 || dh <= 2) return;
    srcCap = s->w * 21 / 100;              /* 保留两端的叶片、铆钉与圆角 */
    dstCap = (int)((float)srcCap * (float)dh / (float)s->h + 0.5f);
    if (dstCap > dw / 2) dstCap = dw / 2;
    srcMid = s->w - srcCap * 2;
    dstMid = dw - dstCap * 2;
    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
    bf.SourceConstantAlpha = (BYTE)(constAlpha < 0 ? 255 : CLAMP(constAlpha, 0, 255));
    bf.AlphaFormat = AC_SRC_ALPHA;
    SetWorldTransform(dst, &gIdentXF);
    AlphaBlend(dst, dl, dt, dstCap, dh, s->dc, 0, 0, srcCap, s->h, bf);
    if (dstMid > 0 && srcMid > 0)
        AlphaBlend(dst, dl + dstCap, dt, dstMid, dh,
                   s->dc, srcCap, 0, srcMid, s->h, bf);
    AlphaBlend(dst, dl + dw - dstCap, dt, dstCap, dh,
               s->dc, s->w - srcCap, 0, srcCap, s->h, bf);
    SetWorldTransform(dst, &gCurXF);
}

static int uiButtonDown(int hot)
{
    return hot && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
}

static void drawUiButton(HDC dc, float x, float y, float w, float h,
                         int style, int hot, int enabled)
{
    const Sprite *s;
    int state = uiButtonDown(hot) ? BTN_DOWN : (hot ? BTN_HOT : BTN_NORMAL);
    if (!enabled) s = &gSprButtonDisabled;
    else if (style >= 0 && style < BTN_STYLE_N) s = &gSprButton[style][state];
    else s = NULL;
    if (s && s->ok) {
        /* 按下帧再向下挪 1px，触感比单纯换颜色更明确。 */
        spriteBlitButton(dc, s, x, y + (state == BTN_DOWN ? 1.0f : 0.0f), w, h, -1);
    } else {
        COLORREF c = enabled ? (hot ? RGB(92, 142, 76) : RGB(62, 94, 56)) : RGB(62, 62, 60);
        fillRound(dc, x, y, x + w, y + h, 10, c);
        strokeRound(dc, x, y, x + w, y + h, 10, tint(c, 0.42f), 2);
    }
}

static void drawUiIcon(HDC dc, int icon, float cx, float cy, float size, int alpha)
{
    const Sprite *s;
    float baseLogical = 80.0f;          /* 生成资产 160px，对应 SS=2 下 80 逻辑像素 */
    if (icon < 0 || icon >= UII_COUNT) return;
    s = &gSprUiIcon[icon];
    if (!s->ok) return;
    spriteBlit(dc, s, cx, cy + size * 0.5f,
               size / baseLogical, size / baseLogical, alpha);
}

/* 1x 兼容层专用的精灵绘制（保留给诊断/兼容路径）。
   资源像素是「逻辑尺寸 x SS」，在已经降采样到 1x 的帧缓冲上必须再缩回去，
   否则尺寸和坐标都会翻倍、直接画出屏幕。
   关键：这里**不能**恢复 gCurXF —— 那套 2x 变换属于世界层，
   装到帧缓冲上会让之后画的每一行文字（阳光、波次、成就…）都放大两倍。 */
static void spriteBlit1x(HDC dst, const Sprite *s, float cx, float baseY,
                         float sx, float sy, int constAlpha)
{
    BLENDFUNCTION bf;
    float w, h;
    int dl, dt, dr, db;

    if (!s->ok) return;
    w = (float)s->w / (float)SS * sx;
    h = (float)s->h / (float)SS * sy;
    dl = (int)((cx - w * 0.5f) + 0.5f);
    dt = (int)((baseY - h) + 0.5f);
    dr = (int)((cx + w * 0.5f) + 0.5f);
    db = (int)(baseY + 0.5f);
    if (dr <= dl || db <= dt) return;

    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = (BYTE)(constAlpha < 0 ? 255 : CLAMP(constAlpha, 0, 255));
    bf.AlphaFormat         = AC_SRC_ALPHA;

    SetWorldTransform(dst, &gIdentXF);
    AlphaBlend(dst, dl, dt, dr - dl, db - dt, s->dc, 0, 0, s->w, s->h, bf);
}

/* 逐帧行走动画（P4-B）：没有逐帧行走美术资源时的程序化替代。
   做法是把精灵沿高度切成 WALK_BANDS 条横带，每条带按自己的高度比例
   做横向位移：越往下（腿）摆幅越大，最上面（头/肩）只轻微反向摆。
   位移量由行走相位驱动并循环 —— 读起来就是双腿在交替迈步。

   为什么切带而不是整体平移：整体平移只是"飘"，只有上下半身错位、
   脚比头走得远，才像在迈步。带间还加了 0.9 rad 的相位差，
   让摆动像一道波从脚传到上身，而不是硬邦邦地整块剪切。

   和 spriteBlit 同一套约定：入参是设备像素（已乘 SS），
   内部先复位世界变换再 AlphaBlend，结束后恢复 gCurXF。
   源图按 band 纵向切片，不重采样、不额外占内存。 */
#define WALK_BANDS 8
static void spriteBlitWalk(HDC dst, const Sprite *s, float cx, float baseY,
                           float sx, float sy, int constAlpha,
                           float phase, float amp)
{
    BLENDFUNCTION bf;
    float w, h, scl;
    int dl, dt, dr, db, totalH, j, bands;

    if (!s->ok) return;
    w = (float)s->w / (float)SS * sx;
    h = (float)s->h / (float)SS * sy;
    dl = (int)((cx - w * 0.5f) * SS + 0.5f);
    dt = (int)((baseY - h) * SS + 0.5f);
    dr = (int)((cx + w * 0.5f) * SS + 0.5f);
    db = (int)(baseY * SS + 0.5f);
    totalH = db - dt;
    if (dr <= dl || totalH <= 0) return;

    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = (BYTE)(constAlpha < 0 ? 255 : CLAMP(constAlpha, 0, 255));
    bf.AlphaFormat         = AC_SRC_ALPHA;

    scl = (float)s->h / (float)totalH;         /* 源像素 / 目标像素 */

    /* 满屏时把 8 段步行动画收敛到 4/1 段。贴图、分辨率与受击剪影不变，
       但每只僵尸的 AlphaBlend 次数最多从 8 次降到 1 次。 */
    bands = (gFxTrailSteps <= 1) ? 1 : ((gFxTrailSteps == 2) ? 4 : WALK_BANDS);
    SetWorldTransform(dst, &gIdentXF);
    for (j = 0; j < bands; j++) {
        int y0 = dt + totalH * j / bands;
        int y1 = dt + totalH * (j + 1) / bands;
        int sy0, sy1, dx;
        float u, off;
        if (y1 <= y0) continue;
        u   = (bands > 1) ? (float)j / (float)(bands - 1) : 0.5f;
        off = amp * sinf(phase + u * 0.9f) * (0.18f + 0.82f * u * u);
        dx  = (int)(off * (float)SS);
        sy0 = (int)((float)(y0 - dt) * scl);
        sy1 = (int)((float)(y1 - dt) * scl);
        if (sy0 < 0) sy0 = 0;
        if (sy1 > s->h) sy1 = s->h;
        if (sy1 <= sy0) continue;
        AlphaBlend(dst, dl + dx, y0, dr - dl, y1 - y0,
                   s->dc, 0, sy0, s->w, sy1 - sy0, bf);
    }
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
    int total = gLoadoutN + gBonusN + gRgN;   /* 肉鸽卡也占位，否则点击判定会错位 */
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

/* 面板几何（逻辑坐标；世界层调用时会被 2x 变换放大，1x 文字层直接就是像素） */
#define TIP_W 300.0f
#define TIP_H 128.0f

/* oneX: 0 = 画在世界层（2x，随后被降采样）；1 = 画在 1x 文字层。
   两种情况的坐标语义相同，只有精灵预览的缩放换算不同 —— 以前这里固定往世界层画、
   文字固定往文字层画，于是被暂停/结算的整屏遮罩盖掉一半，出现"只有底没有字"。 */
static void drawHoverTipShapes(HDC dc, int oneX)
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
    if (gSprPlant[gTipPlant].ok) {
        if (oneX) spriteBlit1x(dc, &gSprPlant[gTipPlant],
                               tx + 48.0f, ty + TIP_H - 10.0f, 0.58f, 0.58f, -1);
        else      spriteBlit (dc, &gSprPlant[gTipPlant],
                              tx + 48.0f, ty + TIP_H - 10.0f, 0.58f, 0.58f, -1);
    } else {
        fillCircle(dc, tx + 48.0f, ty + TIP_H * 0.5f, 22.0f, RGB(150, 190, 130));
    }

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
/* 两色线性插值：k=0 取 a，k=1 取 b。
   用途：把品质色往地面色里"混"，避免光环变成一坨纯色圆饼。 */
static COLORREF mixCol(COLORREF a, COLORREF b, float k)
{
    int r = (int)((float)GetRValue(a) + ((float)GetRValue(b) - (float)GetRValue(a)) * k);
    int g = (int)((float)GetGValue(a) + ((float)GetGValue(b) - (float)GetGValue(a)) * k);
    int c = (int)((float)GetBValue(a) + ((float)GetBValue(b) - (float)GetBValue(a)) * k);
    return RGB(CLAMP(r, 0, 255), CLAMP(g, 0, 255), CLAMP(c, 0, 255));
}

/* 当前站位的"地面基色"——实现在地形系统之后（它要读 gTile / gTheme）。
   接触影必须跟着地面走：原来恒为绿色，一到冰面/焦土上就露馅，一眼假。 */
static COLORREF groundColAt(float x, float y);

static void drawContactShadow(HDC dc, float x, float y, float rx, float ry)
{
    COLORREF g = groundColAt(x, y);
    /* 三层同心椭圆由浅到深，叠出软阴影；层次越多"贴地感"越强 */
    if (gFxTrailSteps <= 1) {
        fillEllipse(dc, x, y - ry * 0.06f, rx, ry * 0.90f, tint(g, -0.32f));
        return;
    }
    fillEllipse(dc, x, y,              rx * 1.34f, ry * 1.30f, tint(g, -0.16f));
    fillEllipse(dc, x, y - ry * 0.06f, rx * 0.98f, ry * 0.94f, tint(g, -0.34f));
    if (gFxTrailSteps == 2) return;
    fillEllipse(dc, x, y - ry * 0.12f, rx * 0.56f, ry * 0.54f, tint(g, -0.52f));
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

    /* 肉鸽植物：脚下打一层【品质色柔光】。
       用三层同心椭圆由深到浅堆出光垫，比描一圈线更像"打光"，
       也更不容易看成准星。质感的第一层其实是辨识度。 */
    if (p->rg > 0) {
        COLORREF ac = RGQ_COL[rgPlants[p->rg - 1].tier];
        COLORREF g0 = groundColAt(cx, by);
        HGDIOBJ  ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        /* 只画两圈【与地面混过色】的细环：填充会变成一坨水洼，
           描边才是"精英标记"该有的克制。 */
        SelectObject(dc, getPen(mixCol(g0, ac, 0.62f), 2));
        Ellipse(dc, (int)(cx - 26.0f), (int)(by - 13.0f),
                    (int)(cx + 26.0f), (int)(by + 6.0f));
        SelectObject(dc, getPen(mixCol(g0, ac, 0.34f), 1));
        Ellipse(dc, (int)(cx - 31.0f), (int)(by - 16.0f),
                    (int)(cx + 31.0f), (int)(by + 8.0f));
        SelectObject(dc, ob);
    }

    if (p->summon && gSprPumpkin.ok)         sp = &gSprPumpkin;   /* 金气召唤的南瓜 */
    else if (p->w > 1 && gSprShape[1].ok)    sp = &gSprShape[1];
    else if (p->hh > 1 && gSprShape[2].ok)   sp = &gSprShape[2];
    else if (isShooter && wpn > WPN_NORMAL && gSprWeapon[wpn - 1].ok)
        sp = &gSprWeapon[wpn - 1];

    /* 肉鸽专属贴图优先级最高：这株植物长什么样，是它唯一的识别特征，
       被武器模组/多列形态盖掉就成了"换了数值的豌豆射手"。
       没有专属资源时才退回基座（下面 sp 仍是 &gSprPlant[p->type]）。 */
    {   Sprite *rs = rgPlantSprite(p->rg);
        if (rs) sp = rs; }

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

/* 肉鸽僵尸脚下的品质色柔光垫（双细环）。
   存活帧与倒地帧共用同一个函数，保证"倒下之后光环不消失" ——
   否则一只传说僵尸活着时脚下有金环、倒地瞬间变成光板，
   品级感断在半途。fade 用于倒地末段的淡出：
   GDI 的 Ellipse 没有 alpha 通道，所以用**把品质色往地面色回混**
   来模拟透明度衰减（k 越小越接近地面色，k=0 就是纯地面色 = 看不见）。 */
static void drawRgZombieGlow(HDC dc, Zombie *z, float fade)
{
    COLORREF ac, g0;
    HGDIOBJ  ob;
    float    k = CLAMP(fade, 0.0f, 1.0f);
    if (z->rg <= 0 || z->rg > RG_ZOMBIE_N || k <= 0.02f) return;
    ac = RGQ_COL[rgZombies[z->rg - 1].tier];
    g0 = groundColAt(z->x, z->y);
    ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SelectObject(dc, getPen(mixCol(g0, ac, 0.62f * k), 2));
    Ellipse(dc, (int)(z->x - 26.0f), (int)(z->y - 12.0f),
                (int)(z->x + 26.0f), (int)(z->y + 7.0f));
    if (gFxTrailSteps <= 1) { SelectObject(dc, ob); return; }
    SelectObject(dc, getPen(mixCol(g0, ac, 0.34f * k), 1));
    Ellipse(dc, (int)(z->x - 31.0f), (int)(z->y - 15.0f),
                (int)(z->x + 31.0f), (int)(z->y + 9.0f));
    SelectObject(dc, ob);
}

/* ========================================================================
   新五关（第 6~10 关）专属场地规则
   ------------------------------------------------------------------------
   五关各有一条不可替代的规则，难度来自「规则变化」而不是数值堆叠：
     6 霜牙隘口  RULE_ICE     冰面击退（受击被推远）+ 薄冰格不可种
     7 熔心裂谷  RULE_RIFT    岩浆裂隙周期喷发：烧植物、也伤僵尸
     8 幽影回廊  RULE_FOG     右四列迷雾，其中僵尸不可见
     9 机枢要塞  RULE_POWER   必须贴着已有植物往外种（连链才能通电）
    10 星界王座  RULE_ASTRAL  三阶段：引力加速 / 星陨 / 时空闪现

   设计取舍：规则尽量做成「种植约束」或「场地事件」，而不是去改
   植物/僵尸的行为逻辑。原因：伤害点有十几处、植物行为分支极多，
   逐处嵌入关卡规则既容易漏、又会把关卡规则和单位行为耦合死 ——
   以后加第 12 关就得再动一遍那些地方。约束类规则只在放置那一刻拦一次，
   事件类规则只在 updateGame 里推进，改动面小得多。
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
static float gRiftT;                         /* 距下次轮换（秒） */
static float gRiftWarn;                      /* 预警剩余（>0 时画裂纹） */
static float gRiftBlast;                     /* 喷发表现剩余 */
static float gHeatWave;                      /* 热浪：僵尸速度加成（随波数累积） */
/* --- 关 8 --- */
static int   gFogFromCol = 5;                /* 迷雾起始列 */
static float gFogRevealT;                    /* 揭示窗口剩余（每波开始给一次） */
/* --- 关 9 --- */
static float gPowerCutRow[ROWS];             /* 该行断电剩余秒数（短路僵尸造成） */
/* --- 关 10 --- */
static int   gPhase  = 1;                    /* 当前阶段 1~3 */
static float gPhaseT;                        /* 本阶段已进行时间 */
static float gGravAcc;                       /* P1：累积加速 */
static float gMeteorT;                       /* P2：陨石计时 */
static float gBlinkT;                        /* P3：群体闪现计时 */

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

    if (gCurLevel < 6 || gCurLevel >= LV_COUNT) return;   /* 老关卡不启用新规则 */
    switch (gCurLevel) {
    case 6:  gRuleKind = RULE_ICE;    break;
    case 7:  gRuleKind = RULE_RIFT;   break;
    case 8:  gRuleKind = RULE_FOG;    break;
    case 9:  gRuleKind = RULE_POWER;  break;
    case 10: gRuleKind = RULE_ASTRAL; break;
    default: gRuleKind = RULE_NONE;   break;
    }

    /* 关 6：随机撒 6 格薄冰。
       guard 兜底：万一放置条件写错，while 会硬挂起整个游戏，
       这类循环加一道保险远比事后排查便宜。 */
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
    /* 关 7：三处裂隙初始位置。避开最左两列 —— 那是玩家唯一的稳定后方，
       连后方都可能被烧的话，这关就只剩运气了。 */
    if (gRuleKind == RULE_RIFT) {
        for (i = 0; i < 3; i++) {
            gRiftR[i] = rndi(0, ROWS);
            gRiftC[i] = rndi(2, COLS);
        }
    }
}

/* 该格能不能种植（供 cellLegal 调用） */
static int newLevelCellBlocked(int r, int c)
{
    if (r < 0 || c < 0 || r >= ROWS || c >= COLS) return 0;

    /* 关 6：薄冰格种不了 */
    if (gRuleKind == RULE_ICE && gThinIce[r][c]) return 1;

    /* 关 9 供电：必须与同行已有植物相邻（第 0 列直接接电源）。
       做成「种植约束」而不是「断电后失效」，是因为后者要在植物行为的
       每个分支里查电，改动面大且容易漏；前者在放置那一刻就拦住，
       玩家看到的是「种不下去」这个立刻能懂的反馈。 */
    if (gRuleKind == RULE_POWER && c > 0) {
        if (!(grid[r][c - 1].alive || grid2[r][c - 1].alive)) return 1;
    }
    return 0;
}

/* 关 9：该行是否通电。
   ------------------------------------------------------------------------
   这是第 9 关的核心威胁。之前只做了「短路僵尸把 gPowerCutRow 设为 8 秒」
   和「画一条红色指示条」，但**没有任何地方真正读取它来阻止植物工作** ——
   所以断电纯粹是个视觉效果，第 9 关的核心机制是假的
   （注释里引用的 newLevelRowPowered 当时根本没写）。

   判定放在植物逐帧更新的循环里，而不是去改每个植物的行为分支：
   植物行为有几十个 case，逐处加查电既容易漏、又会把规则和单位耦合死。 */
static int newLevelRowPowered(int row)
{
    if (gRuleKind != RULE_POWER) return 1;      /* 其他关卡不受影响 */
    if (row < 0 || row >= ROWS) return 1;
    return gPowerCutRow[row] <= 0.0f;
}

/* 关 10：波次推进时刷新阶段 */
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
        addShake(9.0f, 0.55f);     /* 阶段切换给一次明显的震屏 */
        flashRed = 0.55f;
        sfxPlay(L"alarm");
    }
}

/* 每帧推进关卡规则 */
static void newLevelUpdate(float dt)
{
    int i;

    if (gRuleKind == RULE_NONE) return;

    /* ---------------- 关 7：裂隙轮换与喷发 ---------------- */
    if (gRuleKind == RULE_RIFT) {
        /* 热浪：随波数累积。设计意图是压缩「拖时间」策略 ——
           上一关刚学会用击退慢慢磨，这一关直接告诉你拖不起。 */
        gHeatWave = (float)gWave * 0.04f;

        gRiftT -= dt;
        if (gRiftT < RIFT_WARN && gRiftT + dt >= RIFT_WARN) addShake(3.0f, 0.2f);

        if (gRiftT <= 0.0f) {
            gRiftT = RIFT_CYCLE;
            gRiftBlast = 0.45f;
            addShake(7.0f, 0.35f);
            sfxPlay(L"explode");
            for (i = 0; i < 3; i++) {
                int rr = gRiftR[i], cc = gRiftC[i];
                int dr, dc, k;
                if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) continue;
                /* 裂隙改为可见的非致死灼伤，不再让任意位置的植物凭空消失。 */
                if (grid[rr][cc].alive)
                    plantHazardDamage(&grid[rr][cc], grid[rr][cc].maxhp * 0.35f);
                if (grid2[rr][cc].alive)
                    plantHazardDamage(&grid2[rr][cc], grid2[rr][cc].maxhp * 0.35f);
                /* 邻格植物轻伤，同样保底 1 点生命。 */
                for (dr = -1; dr <= 1; dr++) for (dc = -1; dc <= 1; dc++) {
                    int nr = rr + dr, nc = cc + dc;
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
                    if (grid[nr][nc].alive)  plantHazardDamage(&grid[nr][nc], 40.0f);
                    if (grid2[nr][nc].alive) plantHazardDamage(&grid2[nr][nc], 40.0f);
                }
                /* 范围内僵尸重创 —— 裂隙敌我不分，所以玩家可以赌它帮忙清怪。
                   这是本关唯一的「正面回报」，没有它裂隙就只是纯粹的惩罚。 */
                for (k = 0; k < MAX_ZOMBIES; k++) {
                    Zombie *z = &zombies[k];
                    float cx, dx;
                    if (!z->active || z->dead) continue;
                    /* 熔心僵尸免疫裂隙伤害 —— 它把这个"玩家的免费陷阱"
                       变成纯粹的单方面威胁，堵死"靠裂隙清怪"的退路。 */
                    if (z->type == ZT_MAGMAW) continue;
                    cx = (float)LAWN_X + ((float)cc + 0.5f) * (float)CELL_W;
                    dx = z->x - cx;
                    if (dx * dx < 90.0f * 90.0f) { z->hp -= 150.0f; z->flash = 0.12f; }
                }
            }
            /* 轮换到新位置 */
            for (i = 0; i < 3; i++) {
                gRiftR[i] = rndi(0, ROWS);
                gRiftC[i] = rndi(2, COLS);
            }
        }
        if (gRiftBlast > 0.0f) gRiftBlast -= dt;
        gRiftWarn = (gRiftT < RIFT_WARN) ? gRiftT : 0.0f;
    }

    /* ---------------- 关 8：迷雾揭示窗口 ---------------- */
    if (gRuleKind == RULE_FOG) {
        if (gFogRevealT > 0.0f) gFogRevealT -= dt;
        /* 前段只盖右两列，第 10 波后扩到右四列（设计里的「迷雾扩张」转折） */
        gFogFromCol = (gWave >= 10) ? 5 : 7;
    }

    /* ---------------- 关 10：三阶段规则 ---------------- */
    if (gRuleKind == RULE_ASTRAL) {
        newLevelRefreshPhase();
        gPhaseT += dt;
        if (gPhase == 1) {
            /* P1 引力崩坏：僵尸缓慢累积加速 */
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
                    int k;
                    /* 星陨只造成非致死重伤；阵线的最终击破权留给正面僵尸。 */
                    if (grid[rr][cc].alive)
                        plantHazardDamage(&grid[rr][cc], grid[rr][cc].maxhp * 0.40f);
                    if (grid2[rr][cc].alive)
                        plantHazardDamage(&grid2[rr][cc], grid2[rr][cc].maxhp * 0.40f);
                    for (k = 0; k < MAX_ZOMBIES; k++) {
                        Zombie *z = &zombies[k];
                        float cx, dx;
                        if (!z->active || z->dead || z->row != rr) continue;
                        cx = (float)LAWN_X + ((float)cc + 0.5f) * (float)CELL_W;
                        dx = z->x - cx;
                        if (dx * dx < 80.0f * 80.0f) { z->hp -= 200.0f; z->flash = 0.12f; }
                    }
                }
            }
        } else {
            /* P3 时空裂隙：每 6 秒让全场僵尸群体闪现前进 */
            gBlinkT -= dt;
            if (gBlinkT <= 0.0f) {
                gBlinkT = 6.0f;
                addShake(5.0f, 0.3f);
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    if (!z->active || z->dead) continue;
                    zombieSpecialMove(z, -(float)CELL_W * 1.6f);
                }
            }
        }
    }
}

/* 僵尸的关卡级速度修正（关 7 热浪 / 关 10 引力） */
static float newLevelSpeedMul(void)
{
    if (gRuleKind == RULE_RIFT)   return 1.0f + gHeatWave;
    if (gRuleKind == RULE_ASTRAL && gPhase == 1) return 1.0f + gGravAcc;
    return 1.0f;
}

/* 关 6 冰面击退 */
static void newLevelKnockback(Zombie *z)
{
    if (gRuleKind != RULE_ICE) return;
    if (z->dead) return;
    /* 免疫击退：巨人 + 高血量重甲单位。
       这是本关的学习目标 —— 玩家刚建立的「靠击退拖时间」会在这里失效。 */
    if (z->type == ZT_GIANT) return;
    if (z->maxhp > 2500.0f) return;
    z->x += (float)CELL_W * 0.055f;      /* 向后退（僵尸自右向左推进） */
    if (z->x > (float)VIEW_W) z->x = (float)VIEW_W;
}

/* 关 8：该僵尸此刻是否应被迷雾隐藏 */
static int newLevelZombieHidden(const Zombie *z)
{
    int col;
    if (gRuleKind != RULE_FOG) return 0;
    if (gFogRevealT > 0.0f) return 0;            /* 揭示窗口内全部可见 */
    if (z->dead) return 0;
    col = (int)((z->x - (float)LAWN_X) / (float)CELL_W);
    return (col >= gFogFromCol && col < COLS + 2);
}

/* ========================================================================
   新五关专属僵尸的行为
   ------------------------------------------------------------------------
   统一放在一个函数里按类型分发，而不是散进 updateZombies 的各个分支 ——
   后者每加一种新僵尸就要改一次主循环，越改越乱；
   这里只从主循环调一次，新僵尸的行为在本地闭环。
   ======================================================================== */
/* hitEdge：本帧是否「刚被命中」（由调用方算好传进来，见那里的说明）。
   不要在这个函数里自己比较 flash/flashPrev —— 顺序不保证，会静默失效。 */
static void newZombieBehavior(Zombie *z, float dt, int hitEdge)
{
    if (!z->active || z->dead) return;

    switch (z->type) {
    case ZT_PHANTOM:
        /* 幽影：迷雾中移速 +40%（鼓励玩家把战场往左推） */
        if (gRuleKind == RULE_FOG && gFogRevealT <= 0.0f) {
            int col = (int)((z->x - (float)LAWN_X) / (float)CELL_W);
            if (col >= gFogFromCol) z->x -= z->speed * 0.40f * dt;
        }
        break;

    case ZT_BLINKER:
        /* 闪现：每 8 秒瞬移前进 2 列。cd 借用 rgt 字段，省得加新成员。 */
        z->rgt -= dt;
        if (z->rgt <= 0.0f) {
            z->rgt = 8.0f;
            zombieSpecialMove(z, -(float)CELL_W * 2.0f);
            z->flash = 0.10f;              /* 给一帧闪烁提示玩家"它跳了" */
        }
        break;

    case ZT_SABOTEUR: {
        /* 短路：接触到植物时使该行断电 8 秒（整行植物停止工作） */
        int col = (int)((z->x - (float)LAWN_X) / (float)CELL_W);
        if (col >= 0 && col < COLS && z->rgt <= 0.0f) {
            Plant *p = &grid[z->row][col];
            if (p->alive || grid2[z->row][col].alive) {
                gPowerCutRow[z->row] = 8.0f;
                z->rgt = 10.0f;            /* 至少间隔 10 秒再切断一次 */
                sfxPlay(L"deny");
                addShake(4.0f, 0.2f);
            }
        }
        z->rgt -= dt;
        break;
    }

    case ZT_COGWORK: {
        /* 机械：受击时**20% 概率**反弹伤害给同行最靠右的植物。
           ⚠️ 概率门不能省：设计写的是 20%，但早先的实现是"每次受击都反弹"，
              结果豌豆射手连射会被每次反弹 —— 实测强度远超预期，
              本来该是"偶尔扎手"的怪物变成了"高射速植物的克星"。
           只在「刚被命中」的那一帧判定（与冰面击退同一套上升沿检测）。 */
        if (hitEdge && (rand() % 100) < 20) {
            int c, hit = -1;
            for (c = COLS - 1; c >= 0; c--) {
                if (grid[z->row][c].alive)  { hit = c; break; }
                if (grid2[z->row][c].alive) { hit = c; break; }
            }
            if (hit >= 0) {
                if (grid[z->row][hit].alive)  grid[z->row][hit].hp  -= 10.0f;
                if (grid2[z->row][hit].alive) grid2[z->row][hit].hp -= 10.0f;
                z->rgt = 0.25f;              /* 反弹后短暂冷却，避免连续扎手 */
            }
        }
        break;
    }

    case ZT_FROSTFANG: {
        /* 冰锥僵尸：受击时向**相邻行**溅射冰冻（减速 2 秒）。
           设计意图是反制"把僵尸挤在一行"的打法 —— 僵尸叠得越密，
           溅射越容易连锁，逼玩家做分层防守而不是单行堆火力。
           用 flash 上升沿检测"这一帧刚被命中"，与机械僵尸同一套判据，
           所以任何伤害来源（豌豆/爆炸/雷击）都会触发，不需要逐处挂钩。 */
        if (hitEdge) {
            int dr;
            for (dr = -1; dr <= 1; dr += 2) {
                int nr = z->row + dr;
                int k;
                if (nr < 0 || nr >= ROWS) continue;
                for (k = 0; k < MAX_ZOMBIES; k++) {
                    Zombie *o = &zombies[k];
                    if (!o->active || o->dead || o == z) continue;
                    if (o->row != nr) continue;
                    /* 只影响附近的：相邻行里前后 1.5 格以内 */
                    if (fabsf(o->x - z->x) > (float)CELL_W * 1.5f) continue;
                    /* 冰冻 3 秒：设计写的是 2 秒，但 2 秒在实战里几乎看不出来
                       （减速幅度也只有常规减速的水平），威胁太弱。
                       加长到 3 秒让"别把僵尸挤在一行"这个教训真的能感受到。 */
                    if (o->slow < 3.0f) o->slow = 3.0f;
                }
            }
        }
        break;
    }

    case ZT_MAGMAW: {
        /* 熔心僵尸：踩在裂隙格上会"点燃"它，让下次喷发**提前**。
           设计意图：裂隙本来是玩家的赌注（可能帮自己清怪），
           熔心把它翻过来变成加速器 —— 这是"破解玩家应对策略"的典型。
           rgt 当节流器，避免同一帧反复触发。 */
        if (gRuleKind == RULE_RIFT && z->rgt <= 0.0f) {
            int k;
            for (k = 0; k < 3; k++) {
                float cx;
                if (gRiftR[k] != z->row) continue;
                cx = (float)LAWN_X + ((float)gRiftC[k] + 0.5f) * (float)CELL_W;
                if (fabsf(z->x - cx) < (float)CELL_W * 0.6f) {
                    gRiftT -= 3.0f;                       /* 提前 3 秒 */
                    if (gRiftT < 0.5f) gRiftT = 0.5f;     /* 别瞬间连续爆 */
                    z->rgt = 3.0f;
                    z->flash = 0.10f;
                    break;
                }
            }
        }
        z->rgt -= dt;
        break;
    }

    case ZT_ASHWALKER:
        /* 灰烬行者：死亡时留灰烬云（用闪屏与粒子表现遮蔽感）。
           真正的"遮蔽视线"表现成本高，这里退化为一次短暂的全场烟尘，
           既传达了"视野被干扰"，又不需要新渲染层。 */
        if (z->hp <= 0.0f && !z->dead && z->deadT <= 0.0f) {
            addShake(3.0f, 0.25f);
            flashRed = 0.18f;
        }
        break;

    default:
        break;
    }
}

/* 带 alpha 的矩形填充。
   世界层里 fillRect 是不透明的，而迷雾必须半透明 ——
   玩家要能隐约看到地形，否则分不清是「没画」还是「看不见」。 */
static void fillRectA(HDC dc, float l, float t, float r, float b,
                      COLORREF col, int alpha)
{
    HDC tmp = CreateCompatibleDC(dc);
    HBITMAP bm, old;
    RECT rc;
    HBRUSH br;
    BLENDFUNCTION bf;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    rc.left = 0; rc.top = 0;
    rc.right = (LONG)(r - l); rc.bottom = (LONG)(b - t);
    if (rc.right <= 0 || rc.bottom <= 0) { DeleteDC(tmp); return; }
    bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    old = (HBITMAP)SelectObject(tmp, bm);
    br = CreateSolidBrush(col);
    FillRect(tmp, &rc, br);
    DeleteObject(br);
    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
    bf.SourceConstantAlpha = (BYTE)alpha; bf.AlphaFormat = 0;
    AlphaBlend(dc, (int)l, (int)t, rc.right, rc.bottom, tmp, 0, 0,
               rc.right, rc.bottom, bf);
    SelectObject(tmp, old);
    DeleteObject(bm);
    DeleteDC(tmp);
}

static void drawZombie(HDC dc, Zombie *z)
{
    /* 关 8：迷雾中的僵尸不渲染。
       设计上玩家仍能用范围攻击打中它们，只是「看不见」——
       这一关考的是预设火力网，而不是反应速度。 */
    if (newLevelZombieHidden(z)) return;

    {
    Sprite *sp  = &gSprZombie[z->type];
    Sprite *spF = &gSprZombieFlash[z->type];
    Sprite *spB = &gSprZombieBlue[z->type];
    /* 肉鸽僵尸优先用专属贴图。它是这个僵尸唯一的视觉身份，
       剪影（白/蓝）和倒地帧也得跟着换，否则受击的一瞬间会闪回基座模样。 */
    Sprite *rgDead = NULL;
    if (z->rg > 0 && z->rg <= RG_ZOMBIE_N) {
        Sprite *rs  = rgZombieSprite(z->rg);
        Sprite *rsF = &gSprRgZombieFlash[z->rg - 1];
        Sprite *rsB = &gSprRgZombieBlue[z->rg - 1];
        if (rs) {
            sp  = rs;
            spF = rsF->ok ? rsF : spF;
            spB = rsB->ok ? rsB : spB;
        }
        if (gSprRgZombieDead[z->rg - 1][0].ok) rgDead = &gSprRgZombieDead[z->rg - 1][0];
    }

    /* 倒地中：播放绕脚底旋转的倒地帧，末段淡出 */
    if (z->dead) {
        float t  = 1.0f - CLAMP(z->deadT / DEATH_DUR, 0, 1);
        int   fi = (int)CLAMP(t * 5.0f, 0, 4);
        float al = (t > 0.68f) ? (1.0f - (t - 0.68f) / 0.32f) : 1.0f;
        /* 专属倒地帧优先；没有就退回基座僵尸的倒地帧 */
        Sprite *sd = rgDead ? &gSprRgZombieDead[z->rg - 1][fi]
                            : &gSprZombieDead[z->type][fi];
        /* 倒地帧同样保留品质色光环（淡出阶段跟着 al 一起退场） */
        drawRgZombieGlow(dc, z, al);
        drawContactShadow(dc, z->x, z->y + 2, 22.0f, 7.5f);
        if (sd->ok) spriteBlit(dc, sd, z->x, z->y, 1.0f, 1.0f,
                               (int)(255.0f * CLAMP(al, 0, 1)));
        else        drawZombieProc(dc, z);
        return;
    }

    /* 啃食时切到第二套姿态。肉鸽僵尸没有独立啃食图，
       保持行走姿态即可（本来就是异形单位，啃食姿态意义不大）。 */
    if (z->eating && sp == &gSprZombie[z->type] && gSprZombieEat[z->type].ok) {
        sp  = &gSprZombieEat[z->type];
        spF = &gSprZombieEatFlash[z->type];
        spB = &gSprZombieEatBlue[z->type];
    }

    /* 肉鸽僵尸：脚下品质色柔光垫（和植物侧同一套视觉语言） */
    drawRgZombieGlow(dc, z, 1.0f);

    /* ---- 有美术资源：精灵渲染 ---- */
    if (sp->ok) {
        float walk = z->anim * 6.0f;
        float bob  = z->eating ? fabs(sin(z->anim * 15.0f)) * 1.6f
                               : fabs(sin(walk)) * 1.6f;
        float sway = z->eating ? sin(z->anim * 5.0f) * 0.5f : sin(z->anim * 3.0f) * 1.2f;
        float sy   = z->eating ? 0.94f : (1.0f - fabs(sin(walk)) * 0.02f);
        float cx   = z->x + sway - (z->eating ? 6.0f : 0.0f);
        /* 迈步幅度：被减速的僵尸拖脚，啃食时几乎不迈步 */
        float wPh  = z->eating ? (z->anim * 15.0f) : walk;
        float wAmp = z->eating ? 0.7f : (z->slow > 0 ? 1.8f : 2.6f);
        float cy   = z->y;
        float top  = cy - (float)sp->h / SS;

        drawContactShadow(dc, z->x, cy + 2, 22.0f, 7.5f);           /* 三层接触影 */
        /* 行走用切带版：上下半身错位才是"迈步"，整体平移只是"飘"。
           三层剪影（本体 / 减速蓝 / 受击白）必须传同一组相位与幅度，
           否则叠加的纯色剪影会和本体错开，边缘出现彩色毛边。 */
        spriteBlitWalk(dc, sp, cx, cy - bob, 1.0f, sy, -1, wPh, wAmp);
        if (z->slow > 0 && spB->ok)
            spriteBlitWalk(dc, spB, cx, cy - bob, 1.0f, sy, 96, wPh, wAmp);
        if (z->flash > 0 && spF->ok)
            spriteBlitWalk(dc, spF, cx, cy - bob, 1.0f, sy,
                           (int)(70 + 150 * CLAMP(z->flash / 0.09f, 0, 1)), wPh, wAmp);
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
}

static void drawZombieProc(HDC dc, Zombie *z)
{
    float x = z->x, y = z->y;
    float walk = z->anim * 6.0f;
    float bob  = z->eating ? sin(z->anim * 14.0f) * 1.2f : fabs(sin(walk)) * 1.6f;
    z->armSwing = z->eating ? sin(z->anim * 14.0f) * 2.0f : sin(walk) * 2.6f;
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
        LineTo(dc, (int)(x - 25), (int)(ay + 3 + z->armSwing * 0.5f));
        MoveToEx(dc, (int)(x - 4), (int)(ay + 4), NULL);
        LineTo(dc, (int)(x - 23), (int)(ay + 10));
        fillCircle(dc, x - 27, ay + 3 + z->armSwing * 0.5f, 4.2f, skinD);
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

/* 直接用一张贴图当图标。肉鸽卡要画的是它自己的样子，
   不是基座植物 —— 走 drawPlantShape 就只能按 PT_* 取图了。 */
static void drawIconSprite(HDC dc, const Sprite *sp, float cx, float cy, float sz)
{
    float s = sz / 100.0f;
    float w = (float)sp->w / (float)SS * s;
    if (w > sz) s *= sz / w;                 /* 宽图（如巨人）压回来，别溢出卡面 */
    spriteBlit(dc, sp, cx, cy + 30.0f * s, s, s, -1);
}

/* ======================================================================== */
/*                                  场景绘制                                  */
/* ======================================================================== */
static const COLORREF GRASS_A = RGB(124, 198, 76);
static const COLORREF GRASS_B = RGB(104, 172, 62);
static const COLORREF HUD_LT  = RGB(146, 104, 62);

/* ======================================================================== */
/*                     动态地形与主题化背景（事件驱动）                        */
/* ======================================================================== */
/* 需求：地形不再是一成不变的平地 ——
     ① 植物 / 僵尸的【能力】会在地块上留下持久地形（焦土 / 冰面 / 蛛网 / 腐蚀 / 花蔓）
     ② 出现【神级以上】单位时，整张地图的地面与背景切换到对应主题
   两条都不改关卡定义，所以已有的 6 个关卡照常工作。                            */

/* ---- 地块效果位 ---- */
#define TF_SCORCH   (1u << 0)   /* 焦土：站上去持续掉血 */
#define TF_ICE      (1u << 1)   /* 冰面：大幅减速 */
#define TF_WEB      (1u << 2)   /* 蛛网：黏滞减速 */
#define TF_CORRODE  (1u << 3)   /* 腐蚀：穿甲掉血 */
#define TF_BLOOM    (1u << 4)   /* 花蔓：友方地块，站在上面的植物回血 */

static void zombieDie(Zombie *z, int byMower);   /* 前向声明：地形掉血会用到 */
static int zombiesAlive(void);

static unsigned gTile[ROWS][COLS];      /* 每格的叠加地形（位或） */
static float    gTileT[ROWS][COLS];     /* 该格地形剩余时间（秒），<=0 清空 */

/* ---- 主题表 ---- */
enum { TH_NONE = 0, TH_COSMIC, TH_INFERNO, TH_GLACIER, TH_VOID,
       TH_CIRCUIT, TH_BLOOM, TH_COUNT };

typedef struct {
    const wchar_t *name;
    COLORREF sky;      /* 环境 / 天空叠加色 */
    COLORREF ground;   /* 地面叠加色 */
    COLORREF accent;   /* 强调色：粒子与描边 */
} ThemeDef;

static const ThemeDef themeDefs[TH_COUNT] = {
    { L"",       RGB(  0,  0,  0), RGB(  0,  0,  0), RGB(  0,  0,  0) }, /* 无 */
    { L"星界",   RGB( 14, 16, 46), RGB( 58, 52, 128), RGB(150, 140, 255) },
    { L"炼狱",   RGB( 58, 14,  8), RGB(140, 48, 16), RGB(255, 140, 50) },
    { L"冰川",   RGB( 28, 52, 76), RGB(118, 172, 208), RGB(180, 230, 255) },
    { L"虚空",   RGB( 10,  8, 16), RGB( 58, 40, 78), RGB(200, 60, 180) },
    { L"电路",   RGB(  8, 26, 22), RGB( 30, 84, 70), RGB( 80, 255, 190) },
    { L"毒蔓",   RGB( 26, 44, 18), RGB( 84, 138, 48), RGB(160, 255, 90) },
};

static int     gTheme       = TH_NONE;      /* 当前主题（TH_NONE = 跟随关卡） */static float   gThemeT      = 0.0f;         /* 剩余时长 */
static float   gThemePulse  = 0.0f;         /* 环境粒子节拍 */
static wchar_t gThemeWho[64] = L"";         /* 触发者名字，用于横幅 */
static float   gThemeBannerT = 0.0f;        /* 横幅剩余时间 */

/* 地面基色：优先看脚下的地块，其次看主题，最后看关卡草坪变体。
   接触影、地面装饰都从这里取色 —— 保证"影子永远和地面是同一个色系"。 */
static COLORREF groundColAt(float x, float y)
{
    int cc = (int)floor((x - (float)LAWN_X) / (float)CELL_W);
    int rr = (int)floor((y - (float)LAWN_Y) / (float)CELL_H);
    unsigned t = 0u;

    if (rr >= 0 && rr < ROWS && cc >= 0 && cc < COLS) t = gTile[rr][cc];
    if (t & TF_ICE)     return RGB(150, 178, 200);
    if (t & TF_SCORCH)  return RGB(96, 58, 36);
    if (t & TF_CORRODE) return RGB(120, 148, 60);
    if (t & TF_WEB)     return RGB(176, 180, 194);
    if (t & TF_BLOOM)   return RGB(96, 140, 70);

    if (gTheme != TH_NONE && gTheme != 0) return themeDefs[gTheme].ground;

    switch (gLawnVariant) {
    case 1:  return RGB(40, 62, 48);      /* 夜间 */
    case 3:  return RGB(122, 110, 100);   /* 屋顶 */
    case 4:  return RGB(196, 168, 96);    /* 沙漠 */
    default: return RGB(62, 96, 52);      /* 白天草坪 */
    }
}

/* 从能力位推导主题：不用给 100 个条目各加一个字段，改表即改主题 */
static int themeFromTraits(unsigned long long tr)
{
    if (tr & ((unsigned long long)RG_T_LASERCUT | RG_T_DIGITRAIN |
              RG_T_PIXELCRUSH | RZ_CODEWALL | RZ2_HACKSUN))
        return TH_CIRCUIT;
    if (tr & ((unsigned long long)RG_T_CHAINBOOM | RG_T_SELFDESTRUCT |
              RG_T_CURSE | RZ_LASERCUT | RZ2_BOMBBARRAGE))
        return TH_INFERNO;
    if (tr & ((unsigned long long)RG_T_TIMEFREEZE | RG_T_ICERESOURCE |
              RG_T_GRAVITY | RG_T_LULLABY | RZ2_WEATHER))
        return TH_GLACIER;
    if (tr & ((unsigned long long)RG_T_BLACKHOLE | RG_T_BLINK | RG_T_REWIND |
              RZ_BLINK | RZ_REVIVE3 | RZ2_BLACKHOLE))
        return TH_COSMIC;
    if (tr & ((unsigned long long)RG_T_CORRODE | RG_T_PLAGUE | RG_T_LIFESTEAL |
              RG_T_SHARESUN | RG_T_MEMESPREAD | RZ2_POISON | RZ_INFECTSPD))
        return TH_BLOOM;
    return TH_VOID;
}

/* 触发主题：dur <= 0 表示跟随触发者存活，这里统一给一段固定时长 */
static void terrainThemeSet(int th, const wchar_t *who, float dur)
{
    int i;
    if (th <= TH_NONE || th >= TH_COUNT) return;
    /* 已有的更强主题不要被弱触发顶掉（时长剩余更多者优先） */
    if (gTheme == th && gThemeT > dur) return;
    gTheme = th;
    gThemeT = dur;
    gThemePulse = 0.0f;
    if (who) { for (i = 0; i < 63 && who[i]; i++) gThemeWho[i] = who[i]; gThemeWho[i] = 0; }
    else gThemeWho[0] = 0;
    gThemeBannerT = 2.8f;
    addShake(4.0f, 0.35f);
}

/* 只改地形不改主题：神级单位落场时调用 */
static void terrainThemeByTraits(unsigned long long tr, const wchar_t *who)
{
    terrainThemeSet(themeFromTraits(tr), who, 26.0f);
}

static void terrainTileAdd(int row, int col, unsigned bits, float dur)
{
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return;
    gTile[row][col] |= bits;
    if (gTileT[row][col] < dur) gTileT[row][col] = dur;
}

static float terrainTileT(int row, int col)
{
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return 0.0f;
    return gTileT[row][col];
}

/* 地形随时间衰减；主题到点后回落。每帧调用一次。 */
static void terrainUpdate(float dt)
{
    int r, c;

    /* ---- 卡死保护（兜底）----
       若本波超过 60 秒还没清完，开始施加按最大生命计算的真伤；
       90 秒仍未结束则直接清理。它不该影响正常战斗（正常一波远短于 60 秒），
       存在的唯一目的是"保证任何能力组合都不可能让关卡永远结算不了"。

       ⚠️ 起始速率必须**高于僵尸的理论最大自愈速率**，否则两者会打平。
       僵尸自愈已由 ZOMBIE_HEAL_CAP_PER_TICK 封顶在 5%/脉冲
       （传奇档 2.5 秒 → 2%/秒），这里取 9%/秒起步，留了 4.5 倍余量。
       历史教训：曾经这里是 75 秒 / 7%/秒，而当时僵尸能回 14.4%/秒，
       于是"最后两只僵尸打不死"——兜底被自愈反超。 */
    gWaveStallT += dt;
    if (gWaveStallT > 60.0f) {
        int i;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            float extra;
            if (!z->active || z->dead) continue;
            extra = z->maxhp * (0.09f + 0.01f * CLAMP(gWaveStallT - 60.0f, 0.0f, 20.0f)) * dt;
            z->hp -= extra;
            z->flash = 0.08f;
            if (gWaveStallT > 90.0f) { z->rgRevive = 1; zombieDie(z, 1); }
            else if (z->hp <= 0.0f) zombieDie(z, 0);
        }
    }

    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
        if (gTileT[r][c] > 0.0f) {
            gTileT[r][c] -= dt;
            if (gTileT[r][c] <= 0.0f) { gTileT[r][c] = 0.0f; gTile[r][c] = 0; }
        }
    }
    if (gThemeT > 0.0f) {
        gThemeT -= dt;
        if (gThemeT <= 0.0f) { gThemeT = 0.0f; gTheme = TH_NONE; }
    }
    if (gThemeBannerT > 0.0f) gThemeBannerT -= dt;

    /* 环境粒子：主题生效期间持续飘落该主题的碎屑 */
    if (gTheme != TH_NONE) {
        gThemePulse -= dt;
        if (gThemePulse <= 0.0f) {
            const ThemeDef *td = &themeDefs[gTheme];
            int k;
            gThemePulse = 0.055f;
            for (k = 0; k < 3; k++)
                addParticle(rnd(0.0f, (float)VIEW_W), rnd(-20.0f, 40.0f),
                            rnd(-26.0f, 26.0f), rnd(30.0f, 90.0f),
                            rnd(0.9f, 1.8f), rnd(2.0f, 4.5f), td->accent, 1);
        }
    }

    /* 地块对僵尸的持续影响（焦土/腐蚀掉血、冰面/蛛网减速、花蔓回血） */
    {
        int i;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            int cc;
            if (!z->active || z->dead) continue;
            cc = (int)floor((z->x - LAWN_X) / (float)CELL_W);
            if (cc < 0 || cc >= COLS) continue;
            if (gTile[z->row][cc] == 0) continue;
            if (gTile[z->row][cc] & (TF_SCORCH | TF_CORRODE)) {
                z->hp -= 42.0f * dt * (1.0f + 0.35f * (float)gTheme);
                if (z->hp <= 0.0f) zombieDie(z, 0);
            }
            if (gTile[z->row][cc] & TF_ICE) {
                if (z->slow < 0.6f) z->slow = 0.6f;
            }
            if (gTile[z->row][cc] & TF_WEB) {
                if (z->rooted < 0.25f) z->rooted = 0.25f;
            }
        }
        for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
            Plant *p;
            if (!(gTile[r][c] & TF_BLOOM)) continue;
            p = plantAt(r, c);
            if (p && p->alive && p->hp < p->maxhp) {
                p->hp += p->maxhp * 0.05f * dt;
                if (p->hp > p->maxhp) p->hp = p->maxhp;
            }
        }
    }
}

/* 画一格地形：优先用 mmx 生成的美术贴图，只有资源缺失时才回退到矢量绘制。
   返回 1 = 已经用贴图画过了，调用方不要再画矢量版本。 */
static int terrainTileSprite(HDC dc, int ti, float x0, float y0)
{
    if (ti < 0 || ti >= TILE_SPR_N) return 0;
    if (!gSprTile[ti].ok) return 0;
    spriteBlit(dc, &gSprTile[ti], x0 + (float)CELL_W * 0.5f, y0 + (float)CELL_H,
               1.0f, 1.0f, -1);
    return 1;
}

/* 地形与主题的叠加绘制。必须在草坪画完之后调用。
   GDI 没有 alpha，所以"半透明染色"用递减密度的扫描线实现（见经验笔记）。 */
static void drawTerrainOverlay(HDC dc)
{
    int r, c;
    float lx = (float)LAWN_X, ly = (float)LAWN_Y;
    float lw = (float)(COLS * CELL_W), lh = (float)(ROWS * CELL_H);

    /* ---- 1. 主题染色：整块草坪 + 外围环境 ---- */
    if (gTheme != TH_NONE) {
        const ThemeDef *td = &themeDefs[gTheme];
        float fade = CLAMP(gThemeT / 1.6f, 0.0f, 1.0f);   /* 收尾时淡出 */
        int step = (int)(6.0f - 3.0f * fade);
        int y, x;
        if (step < 2) step = 2;
        for (y = (int)ly; y < (int)(ly + lh); y += step)
            fillRect(dc, lx, (float)y, lx + lw, (float)(y + 1), td->ground);
        /* 外围环境：四条边各来一层，把"世界变了"的感觉推到画面边缘 */
        for (y = 0; y < VIEW_H; y += step + 1) {
            fillRect(dc, 0, (float)y, lx, (float)(y + 1), td->sky);
            fillRect(dc, lx + lw, (float)y, (float)VIEW_W, (float)(y + 1), td->sky);
        }
        for (x = 0; x < VIEW_W; x += step + 1) {
            fillRect(dc, (float)x, 0, (float)(x + 1), ly, td->sky);
            fillRect(dc, (float)x, ly + lh, (float)(x + 1), (float)VIEW_H, td->sky);
        }
        /* 主题描边：一圈强调色，强度随剩余时间脉动 */
        {
            float pulse = 0.55f + 0.45f * (float)sin((double)(gTime * 3.0f));
            int wpx = (int)(2.0f + 2.0f * pulse);
            strokeRect(dc, lx - 2, ly - 2, lx + lw + 2, ly + lh + 2, td->accent, wpx);
        }
    }

    /* ---- 2. 地块地形 ---- */
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
        unsigned t = gTile[r][c];
        float x0, y0, x1, y1, k;
        if (!t) continue;
        x0 = lx + (float)c * CELL_W; y0 = ly + (float)r * CELL_H;
        x1 = x0 + CELL_W;            y1 = y0 + CELL_H;
        k = CLAMP(terrainTileT(r, c) / 3.0f, 0.0f, 1.0f);   /* 快消失时变淡 */

        if ((t & TF_SCORCH) && !terrainTileSprite(dc, 0, x0, y0)) {
            int i;
            fillRect(dc, x0 + 6, y0 + 8, x1 - 6, y1 - 6, RGB(96, 44, 24));
            for (i = 0; i < 5; i++)
                fillEllipse(dc, x0 + 18 + (float)i * 15.0f, y0 + 30 + (float)((i * 37) % 40),
                            9.0f, 4.0f, RGB(52, 22, 12));
            for (i = 0; i < 3; i++)
                line(dc, x0 + 10 + (float)i * 30.0f, y1 - 10,
                     x0 + 24 + (float)i * 30.0f, y0 + 12, RGB(255, 126, 40), 2);
        }
        if ((t & TF_CORRODE) && !terrainTileSprite(dc, 3, x0, y0)) {
            /* 腐蚀：黄绿色的腐蚀斑（和花蔓的粉色花区分开） */
            int i;
            for (i = 0; i < 6; i++)
                fillEllipse(dc, x0 + 14 + (float)((i * 53) % 74),
                            y0 + 16 + (float)((i * 41) % 74),
                            8.0f, 5.0f, RGB(178, 214, 62));
        }
        if ((t & TF_ICE) && !terrainTileSprite(dc, 1, x0, y0)) {
            int i;
            fillRect(dc, x0 + 4, y0 + 4, x1 - 4, y1 - 4, RGB(176, 218, 244));
            for (i = 0; i < 4; i++)
                line(dc, x0 + 10 + (float)i * 22.0f, y0 + 10,
                     x0 + 34 + (float)i * 22.0f, y1 - 12, RGB(236, 250, 255), 2);
            strokeRect(dc, x0 + 4, y0 + 4, x1 - 4, y1 - 4, RGB(120, 176, 220), 2);
        }
        if ((t & TF_WEB) && !terrainTileSprite(dc, 2, x0, y0)) {
            int i;
            float wcx = x0 + CELL_W * 0.5f, wcy = y0 + CELL_H * 0.5f;
            HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
            /* 放射状蛛丝 */
            for (i = 0; i < 6; i++) {
                float a = 1.0472f * (float)i;
                line(dc, wcx, wcy,
                     wcx + (float)cos((double)a) * 44.0f,
                     wcy + (float)sin((double)a) * 44.0f,
                     RGB(224, 228, 240), 1);
            }
            /* 同心丝环：必须【描边】而不是填充 —— 用 fillEllipse 会画成白圆饼 */
            for (i = 1; i <= 3; i++) {
                SelectObject(dc, getPen(RGB(238, 242, 252), 1));
                Ellipse(dc, (int)(wcx - 13.0f * (float)i), (int)(wcy - 13.0f * (float)i),
                            (int)(wcx + 13.0f * (float)i), (int)(wcy + 13.0f * (float)i));
            }
            SelectObject(dc, oldBr);
        }
        if ((t & TF_BLOOM) && !terrainTileSprite(dc, 4, x0, y0)) {
            /* 花蔓：友方地块，画成粉色小花（不要用绿点，会和腐蚀混淆） */
            int i, k2;
            for (i = 0; i < 4; i++) {
                float fx = x0 + 16.0f + (float)((i * 47) % 66);
                float fy = y0 + 18.0f + (float)((i * 61) % 66);
                for (k2 = 0; k2 < 5; k2++) {
                    float a = 1.2566f * (float)k2;
                    fillCircle(dc, fx + (float)cos((double)a) * 5.0f,
                                   fy + (float)sin((double)a) * 5.0f,
                               3.4f, RGB(240, 150, 196));
                }
                fillCircle(dc, fx, fy, 3.0f, RGB(255, 232, 120));
            }
        }
        /* 淡化：地形快消失时用扫描线压暗，模拟透明度 */
        if (k < 0.99f) {
            int y;
            int s = (int)(2.0f + 6.0f * k);
            for (y = (int)y0; y < (int)y1; y += s)
                fillRect(dc, x0, (float)y, x1, (float)(y + 1), RGB(40, 48, 34));
        }
    }
    /* ================= 新五关的场地可视元素 =================
       规则必须「可读」：玩家要能一眼看出哪格危险、哪片看不见、
       哪条供电链断了。看不见的规则等于不存在。 */
    if (gRuleKind != RULE_NONE) {
        int rr, cc;
        /* 关 6：薄冰格（不能种，必须看得见） */
        if (gRuleKind == RULE_ICE) {
            for (rr = 0; rr < ROWS; rr++) for (cc = 0; cc < COLS; cc++) {
                float x0, y0;
                if (!gThinIce[rr][cc]) continue;
                x0 = (float)LAWN_X + (float)cc * (float)CELL_W;
                y0 = (float)LAWN_Y + (float)rr * (float)CELL_H;
                fillRectA(dc, x0 + 6, y0 + 6, x0 + (float)CELL_W - 6,
                          y0 + (float)CELL_H - 6, RGB(170, 214, 240), 150);
                strokeRound(dc, x0 + 6, y0 + 6, x0 + (float)CELL_W - 6,
                            y0 + (float)CELL_H - 6, 6, RGB(228, 246, 255), 2);
            }
        }
        /* 关 7：裂隙格（预警时变红，喷发时高亮） */
        if (gRuleKind == RULE_RIFT) {
            for (rr = 0; rr < 3; rr++) {
                float x0, y0;
                COLORREF col;
                int a;
                if (gRiftR[rr] < 0 || gRiftR[rr] >= ROWS) continue;
                x0 = (float)LAWN_X + (float)gRiftC[rr] * (float)CELL_W;
                y0 = (float)LAWN_Y + (float)gRiftR[rr] * (float)CELL_H;
                if (gRiftBlast > 0.0f)      { col = RGB(255, 232, 120); a = 210; }
                else if (gRiftWarn > 0.0f)  { col = RGB(255, 128, 64);  a = 190; }
                else                        { col = RGB(150, 62, 30);   a = 80;  }
                fillRectA(dc, x0 + 8, y0 + 8, x0 + (float)CELL_W - 8,
                          y0 + (float)CELL_H - 8, col, a);
                strokeRound(dc, x0 + 8, y0 + 8, x0 + (float)CELL_W - 8,
                            y0 + (float)CELL_H - 8, 5, RGB(255, 210, 140), 2);
            }
        }
        /* 关 8：迷雾覆盖右侧若干列（揭示窗口内变淡） */
        if (gRuleKind == RULE_FOG) {
            float fx = (float)LAWN_X + (float)gFogFromCol * (float)CELL_W;
            float fw = (float)(COLS - gFogFromCol) * (float)CELL_W;
            int y;
            int alpha = (gFogRevealT > 0.0f) ? 60 : 200;
            for (y = (int)LAWN_Y; y < (int)(LAWN_Y + ROWS * CELL_H); y += 3) {
                fillRectA(dc, fx, (float)y, fx + fw, (float)(y + 2),
                          RGB(46, 34, 68), alpha);
            }
            strokeRound(dc, fx, (float)LAWN_Y, fx + fw,
                        (float)(LAWN_Y + ROWS * CELL_H), 4, RGB(150, 120, 200), 2);
        }
        /* 关 9：电源节点 + 连通范围（断链点用红块标出） */
        if (gRuleKind == RULE_POWER) {
            for (rr = 0; rr < ROWS; rr++) {
                float x0 = (float)LAWN_X;
                float y0 = (float)LAWN_Y + (float)rr * (float)CELL_H;
                int run = 0, k;
                for (k = 0; k < COLS; k++) {
                    if (grid[rr][k].alive || grid2[rr][k].alive) run++;
                    else break;
                }
                fillRect(dc, x0 - 8, y0 + 34, x0 - 3, y0 + (float)CELL_H - 34,
                         gPowerCutRow[rr] > 0 ? RGB(200, 60, 60) : RGB(90, 240, 210));
                if (run < COLS) {
                    float gx = x0 + (float)run * (float)CELL_W;
                    fillRect(dc, gx + 40, y0 + 40, gx + 56, y0 + (float)CELL_H - 40,
                             RGB(240, 90, 90));
                }
            }
        }
        /* 关 10：阶段提示条（颜色随阶段变化） */
        if (gRuleKind == RULE_ASTRAL) {
            COLORREF pc = (gPhase == 1) ? RGB(120, 150, 255)
                        : (gPhase == 2) ? RGB(255, 170, 80)
                                        : RGB(200, 90, 240);
            fillRect(dc, (float)LAWN_X, (float)LAWN_Y - 14,
                     (float)(LAWN_X + COLS * CELL_W), (float)LAWN_Y - 8, pc);
        }
    }

}

/* 诊断计数：本轮排查"菜单画面随时间变化"用，见 gameRenderCounters */
static int gBgRuns = 0, gMenuBlockRuns = 0, gMenuTextRuns = 0, gMenuBgBlits = 0;

static void drawBackground(HDC dc)
{
    int r, c;
    gBgRuns++;
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
        case 5: if (gSprLawnIce.ok)     ls = &gSprLawnIce;     break;
        case 6: if (gSprLawnMagma.ok)   ls = &gSprLawnMagma;   break;
        case 7: if (gSprLawnVoid.ok)    ls = &gSprLawnVoid;    break;
        case 8: if (gSprLawnCircuit.ok) ls = &gSprLawnCircuit; break;
        case 9: if (gSprLawnAstral.ok)  ls = &gSprLawnAstral;  break;
        case LV_VARIANT_PRIMORDIAL: if (gSprLawnPrimordial.ok) ls = &gSprLawnPrimordial; break;
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

    /* 注意：动态地形（drawTerrainOverlay）**不在这里** —— 它会随战局变化，
       它由调用方在 drawBackgroundStatic 之后单独调。 */
}


/* 顶部 HUD */
/* 把 fld 追加到 b（用全角空格分隔）；加上去会超出 maxw 就放弃这个字段。
   调用前必须先把字体选进 dc —— GetTextExtentPoint32W 量的是当前字体。 */
static int appendIfFits(HDC dc, wchar_t *b, int cap, const wchar_t *fld, float maxw)
{
    wchar_t t[256];
    SIZE sz;
    if (b[0]) { wcsncpy(t, b, cap - 1); t[cap - 1] = 0; wcscat(t, L"　"); }
    else      t[0] = 0;
    wcsncat(t, fld, (size_t)(cap - 1) - wcslen(t));
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    if ((float)sz.cx > maxw) return 0;
    wcscpy(b, t);
    return 1;
}

/* 右上角全屏按钮的矩形。
   ★ 绘制和点击判定**必须共用这一份**：以前有过"按钮画在一处、热区写在
   另一处"的写法，改了布局之后按钮就在眼前却点不动，非常难查。
   放在右上角是因为那是用户对"全屏"这个动作的直觉位置。 */
#define FULLBTN_W 36.0f
#define FULLBTN_H 32.0f
#define FULLBTN_X (VIEW_W - 8.0f - FULLBTN_W)
#define FULLBTN_Y 8.0f

/* 顶栏右侧状态栏：模式 / 武器或资源 / 愤怒条（+ 充能槽）。
   ★ 必须和文字一起在最终合成阶段调用。
   这段以前画在会被后续覆盖的 gFrameDC 上，整帧覆盖让它 100% 看不见 ——
   模式、武器、金气、幽能、充能、愤怒条这六项状态从来没有真正显示过；
   它内部的 spriteBlit(gFrameDC, ...) 还会把世界层的 2x 变换装到帧缓冲上，
   导致持有金气/幽能遗物时整层文字被放大两倍。
   位置也从「盖住草坪最右两列」的 x=812 挪到顶栏右侧空白区，
   宽度按卡片栏的真实右边界动态让位。 */
static void drawStatusPanel(HDC dc)
{
    wchar_t b[256];
    float cgap = cardBarGap();
    int   cardTotal = gLoadoutN + gBonusN;
    float cardsRight = 144.0f + (float)cardTotal * cgap;
    /* 右侧留出全屏按钮的位置，状态面板整体左移让位 */
    float pw = VIEW_W - 8.0f - cardsRight - 8.0f - (FULLBTN_W + 6.0f);
    float px, y1 = 8.0f, y2 = 35.0f, y3 = 62.0f;
    int   i;

    if (pw > 200.0f) pw = 200.0f;
    /* 面板的**右边界**要让开全屏按钮那 42px。
       只把 pw 减掉是不够的 —— pw 还有 200 的上限，一旦被截到上限，
       px 又按"右下角无按钮"算回来，模式指示灯就会画到按钮上
       （实测按钮框还在、填充却被指示灯盖住，看起来像"按钮没画"）。 */
    px = VIEW_W - 8.0f - (FULLBTN_W + 6.0f) - pw;

    /* 0) 右上角全屏按钮 —— 与 F11 / 标题栏「最大化」完全等价。
       刻意画在 early return **之前**：卡片栏顶满时状态面板可以不画，
       但这个按钮必须一直在。玩家"找不到全屏入口"的反馈就是从这里来的。 */
    {
        int   hot = (mMouseX >= FULLBTN_X && mMouseX <= FULLBTN_X + FULLBTN_W &&
                     mMouseY >= FULLBTN_Y && mMouseY <= FULLBTN_Y + FULLBTN_H);
        float bx = FULLBTN_X, by = FULLBTN_Y, bw = FULLBTN_W, bh = FULLBTN_H;
        float sz = hot ? 34.0f : 31.0f;
        if (gSprUiIcon[UII_FULLSCREEN].ok)
            drawUiIcon(dc, UII_FULLSCREEN, bx + bw * 0.5f, by + bh * 0.5f, sz, -1);
        else
            drawUiButton(dc, bx, by, bw, bh, BTN_BLUE, hot, 1);
    }

    if (pw < 118.0f) return;      /* 卡片栏顶满整条顶栏：不画，绝不叠在卡片上 */

    /* 1) 当前模式（M 切换）+ 六个模式的解锁指示灯 */
    fillRound(dc, px, y1, px + pw, y1 + 24.0f, 7, MODE_COL[gCurMode]);
    wsprintfW(b, L"%s模式  [M]", MODE_NAME[gCurMode]);
    putTextS(dc, gF15, RGB(255, 255, 255), tint(MODE_COL[gCurMode], -0.45f),
             b, px + 10, y1 + 2, 0);
    if (pw >= 192.0f)
        for (i = 0; i < MODE_COUNT; i++) {
            float lx = px + pw - 10.0f - (float)(MODE_COUNT - i) * 13.0f;
            fillRound(dc, lx, y1 + 7.0f, lx + 10.0f, y1 + 17.0f, 3,
                      gUnlockedMode[i] ? MODE_COL[i] : RGB(72, 78, 72));
        }

    /* 2) 武器 / 金气 / 幽能：按重要性依次拼，挤不下的字段直接不要 */
    SelectObject(dc, gF15);
    b[0] = 0;
    if (gHasSubWpn) {
        wsprintfW(b, L"主 %s / 副 %s", WPN_NAME[gMainWpn], WPN_NAME[gSubWpn]);
    }
    if (gBanker) {
        wchar_t fld[64];
        wsprintfW(fld, L"金币 %d", gGold);
        appendIfFits(dc, b, 256, fld, pw - 20.0f);
    }
    if (gNecromancer) {
        wchar_t fld[64];
        wsprintfW(fld, L"幽灵 %d", gGhost);
        appendIfFits(dc, b, 256, fld, pw - 20.0f);
    }
    if (b[0]) {
        fillRound(dc, px, y2, px + pw, y2 + 24.0f, 7,
                  gHasSubWpn ? WPN_COL[gSubWpn] : RGB(74, 62, 40));
        putTextS(dc, gF15, RGB(255, 255, 255),
                 gHasSubWpn ? tint(WPN_COL[gSubWpn], -0.5f) : RGB(40, 30, 18),
                 b, px + 10, y2 + 2, 0);
    }

    /* 3) 愤怒条；充能槽有的话挤在右端 */
    {
        float fw = pw, pct = gFury / gFuryMax;
        wchar_t fb[48];
        int ready = (gFuryReady && HAS(R_FURY));
        if (pct > 1.0f) pct = 1.0f;
        if (gChargeMax > 0) {
            int k;
            fw = pw - (float)gChargeMax * 16.0f - 10.0f;
            for (k = 0; k < gChargeMax; k++) {
                float dx = px + pw - 10.0f - (float)(gChargeMax - k) * 16.0f;
                fillRound(dc, dx, y3 + 6.0f, dx + 12.0f, y3 + 18.0f, 3,
                          k < gCharge ? RGB(255, 168, 56) : RGB(84, 84, 84));
            }
        }
        if (fw < 56.0f) fw = 56.0f;
        fillRound(dc, px, y3 + 4.0f, px + fw, y3 + 20.0f, 5, RGB(52, 40, 40));
        fillRound(dc, px + 2.0f, y3 + 6.0f,
                  px + 2.0f + (fw - 4.0f) * pct, y3 + 18.0f, 4,
                  ready ? RGB(255, 224, 92) : RGB(236, 108, 56));
        /* 满条时把「可以引爆」直接写在条上 —— R_FURY 的主动引爆全屏雷击
           需要玩家知道现在按 E 才有意义，否则这条永远是装饰。 */
        if (ready) wsprintfW(fb, L"可引爆 E");
        else       wsprintfW(fb, L"愤怒条 %d%%", (int)(pct * 100.0f + 0.5f));
        putTextS(dc, gF15, RGB(255, 226, 200),
                 ready ? RGB(60, 40, 0) : RGB(40, 20, 14),
                 fb, px + 8, y3 + 3, 0);
    }
}

static void drawHUDShapes(HDC dc)
{
    int i;
    /* 园艺工具台：多层木纹与黄铜压边替代一整块纯棕色。 */
    fillRect(dc, 0, 0, VIEW_W, 92, RGB(79, 48, 28));
    fillRect(dc, 0, 5, VIEW_W, 88, RGB(96, 60, 34));
    for (i = 0; i < 7; i++)
        fillRect(dc, 0, 10.0f + (float)i * 11.0f, VIEW_W,
                 11.0f + (float)i * 11.0f, i & 1 ? RGB(106, 66, 37) : RGB(83, 51, 30));
    fillRect(dc, 0, 0, VIEW_W, 5, RGB(196, 142, 72));
    fillRect(dc, 0, 87, VIEW_W, 92, RGB(48, 31, 22));
    fillRect(dc, 0, 87, VIEW_W, 89, RGB(196, 142, 72));

    /* 阳光面板 */
    cardFaceV(dc, 7, 7, 134, 81, 10, RGB(34, 82, 52), RGB(18, 45, 31), 0.0f);
    strokeRound(dc, 7, 7, 134, 81, 10, RGB(212, 162, 78), 3);
    strokeRound(dc, 11, 11, 130, 77, 8, RGB(99, 64, 36), 2);
    if (gSprUiIcon[UII_SUN].ok)
        drawUiIcon(dc, UII_SUN, 39.0f, 44.0f, 58.0f, -1);
    else {
        fillCircle(dc, 38, 44, 22, RGB(255, 220, 70));
        fillCircle(dc, 38, 44, 15, RGB(255, 236, 128));
    }

    /* 卡片：本局出战编组 + 三选一拿到的肉鸽植物 */
    {
    int   cardTotal = gLoadoutN + gBonusN + gRgN;
    float cw = (cardTotal > 8) ? 68.0f : 82.0f;      /* 卡槽多时自动收窄 */
    float cgap = cw + 4.0f;
    for (i = 0; i < cardTotal; i++) {
        int   isRg  = (i >= gLoadoutN + gBonusN);
        int   rgj   = isRg ? (i - gLoadoutN - gBonusN) : -1;
        int   ci    = isRg ? 0 : ((i < gLoadoutN) ? gLoadout[i] : gBonusPlant[i - gLoadoutN]);
        int   icon  = isRg ? (int)rgPlants[gRgCard[rgj]].base : ci;
        int   selId = isRg ? (RG_CARD_BASE + gRgCard[rgj]) : ci;
        float x = 144 + (float)i * cgap, y = 8;
        float w = cw - 4.0f, h = 76;
        int   hot = (mMouseX >= (int)x && mMouseX <= (int)(x + w) &&
                     mMouseY >= 5 && mMouseY <= 87);
        float lift = hot ? -2.0f : 0.0f;
        int   ready = isRg ? (rgCardCD[rgj] <= 0.0f &&
                              (gRgFree[rgj] || gSun >= rgPlantCost(gRgCard[rgj])))
                           : ((cardCD[ci] <= 0.0f) && (gSun >= plantCost(ci)));
        if (hot && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) lift = 1.0f;
        y += lift;
        /* 卡座 */
        fillRound(dc, x - 4, y - 1, x + w + 5, y + h + 6, 9, RGB(42, 27, 18));
        if (isRg) {
            /* 肉鸽卡：用品质色做底，一眼能看出稀有度 */
            COLORREF tc = RGQ_COL[rgPlants[gRgCard[rgj]].tier];
            cardFaceV(dc, x, y, x + w, y + h, 6,
                      tint(tc, ready ? 0.35f : 0.08f),
                      tint(tc, ready ? -0.48f : -0.62f), 0.0f);
            strokeRound(dc, x, y, x + w, y + h, 7, tc, hot ? 4 : 3);
        } else {
            /* 深绿种子袋卡面，与大厅木牌共享黄铜 + 园艺绿语言。 */
            cardFaceV(dc, x, y, x + w, y + h, 6,
                      ready ? RGB(56, 108, 70) : RGB(60, 68, 60),
                      ready ? RGB(24, 58, 38) : RGB(38, 42, 38), 0.0f);
            strokeRound(dc, x, y, x + w, y + h, 7,
                        hot ? RGB(244, 204, 112) : RGB(178, 132, 68), hot ? 4 : 3);
        }
        /* 选中高亮 */
        if (gSel == selId && !gShovel)
            strokeRound(dc, x - 5, y - 5, x + w + 5, y + h + 5, 9, RGB(255, 240, 120), 5);
        /* 图标：肉鸽卡优先用它自己的贴图 —— 卡面是玩家唯一能预判
           "这张卡长什么样"的地方，退回基座就完全看不出区别了。 */
        {
            Sprite *rs = isRg ? rgPlantSprite(gRgCard[rgj] + 1) : NULL;
            if (rs) drawIconSprite(dc, rs, x + w * 0.5f, y + h * 0.5f - 2, 56);
            else    drawIcon(dc, icon, x + w * 0.5f, y + h * 0.5f - 2, 56);
        }
        /* 冷却遮罩 */
        if (isRg) {
            if (rgCardCD[rgj] > 0.0f) {
                float frac = rgCardCD[rgj] / (float)(20 - 2 * rgPlants[gRgCard[rgj]].tier);
                float cover = h * CLAMP(frac, 0, 1);
                if (cover > 3.0f)
                    fillRect(dc, x + 3, y + 3, x + w - 3, y + cover, RGB(38, 46, 44));
                if (frac > 0.12f)
                    drawUiIcon(dc, UII_COOLDOWN, x + w * 0.5f, y + 34.0f, 23.0f, 210);
            }
        } else if (cardCD[ci] > 0.0f) {
            float frac = cardCD[ci] / plantCd(ci);
            float cover = h * CLAMP(frac, 0, 1);
            if (cover > 3.0f)
                fillRect(dc, x + 3, y + 3, x + w - 3, y + cover, RGB(38, 46, 44));
            if (frac > 0.12f)
                drawUiIcon(dc, UII_COOLDOWN, x + w * 0.5f, y + 34.0f, 23.0f, 210);
        }
        /* 阳光不足 */
        if ((!isRg && gSun < plantCost(ci)) ||
            (isRg && !gRgFree[rgj] && gSun < rgPlantCost(gRgCard[rgj]))) {
            strokeRound(dc, x + 1, y + 1, x + w - 1, y + h - 1, 6, RGB(214, 76, 62), 3);
            line(dc, x + 8, y + h - 8, x + w - 8, y + 8, RGB(186, 62, 54), 3);
        }
    }
    /* 铲子卡 */
    {
        float x = 144 + (float)cardTotal * cgap, y = 8, w = 78, h = 76;
        int hot = (mMouseX >= (int)x && mMouseX <= (int)(x + w) && mMouseY >= 5 && mMouseY <= 87);
        if (hot) y -= 2.0f;
        fillRound(dc, x - 4, y - 1, x + w + 5, y + h + 6, 9, RGB(42, 27, 18));
        cardFaceV(dc, x, y, x + w, y + h, 6, RGB(76, 98, 74), RGB(34, 52, 38), 0.0f);
        strokeRound(dc, x, y, x + w, y + h, 7,
                    hot ? RGB(244, 204, 112) : RGB(178, 132, 68), hot ? 4 : 3);
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

/* ======================================================================== */
/*                       肉鸽层运行时辅助（植物 / 僵尸）                       */
/* ======================================================================== */
/* 取植物身上的肉鸽定义：rg 用 "索引+1" 存，0 = 不是肉鸽植物。
   这么做是为了兼容 memset(p, 0, sizeof(*p)) 的创建路径 ——
   如果把 0 当成合法索引，任何漏设 rg 的地方都会变成"量子豌豆射手"。 */
static const RgPlantDef *rgOfPlant(const Plant *p)
{
    if (!p || p->rg <= 0 || p->rg > RG_PLANT_N) return NULL;
    return &rgPlants[p->rg - 1];
}
static const RgZombieDef *rgOfZombie(const Zombie *z)
{
    if (!z || z->rg <= 0 || z->rg > RG_ZOMBIE_N) return NULL;
    return &rgZombies[z->rg - 1];
}
/* 植物实际伤害倍率（普通植物 = 1.0） */
static float rgPlantAtkMul(const Plant *p)
{
    const RgPlantDef *d = rgOfPlant(p);
    float m;
    if (!d) return 1.0f;
    m = rgDmgMul(d->tier);
    /* aspect：单卡专属强度系数。**0 必须解释成"基准（×1.0）"而不是"零倍"** ——
       老条目没写这个字段，C 自动置 0，按零倍算的话全场植物攻击与血量会一起归零。
       （这是"新增字段必须有明确的缺省语义"的典型例子。） */
    if (d->aspect > 0) m *= (float)d->aspect / 100.0f;
    return m;
}
/* 僵尸实际血量 / 速度倍率（普通僵尸 = 1.0） */
static float rgZombieHpOf(const Zombie *z)
{
    const RgZombieDef *d = rgOfZombie(z);
    return d ? rgZombieHpMul(d->tier) : 1.0f;
}
static float rgZombieSpdOf(const Zombie *z)
{
    const RgZombieDef *d = rgOfZombie(z);
    return d ? rgZombieSpdMul(d->tier) : 1.0f;
}
static unsigned long long rgPlantTraitsOf(const Plant *p)
{
    const RgPlantDef *d = rgOfPlant(p);
    return d ? d->traits : 0ull;
}
static unsigned long long rgZombieTraitsOf(const Zombie *z)
{
    const RgZombieDef *d = rgOfZombie(z);
    return d ? d->traits : 0ull;
}
static int rgZombieBlocksInstantKill(const Zombie *z)
{
    const RgZombieDef *d = rgOfZombie(z);
    if (!d || !(d->traits & RZ_PIXELIMMUNE)) return 0;
    return (rand() % 100) < (28 + 4 * d->tier);
}
static int rgOwned(int idx)
{
    int i;
    for (i = 0; i < gRgN; i++) if (gRgCard[i] == idx) return 1;
    return 0;
}

/* 这株肉鸽植物够不够档位、能不能上三选一的牌。
   门槛见 RG_DRAFT_MIN_TIER —— 改那一个常量即可调整或恢复。 */
/* 这张卡实际开了几个能力位。
   卡片上原来打印的是 RGQ_TRAITN[tier]（档位代表值，殿堂=7）——
   那是"该档位典型条数"，而新增的殿堂卡都有 8 条、始祖 64 条，
   直接沿用会**少报**。改成对 traits 掩码做 popcount，卡片上写几就是几。 */
static int rgPlantTraitCount(int idx)
{
    unsigned long long t;
    int n = 0;
    if (idx < 0 || idx >= RG_PLANT_N) return 0;
    t = rgPlants[idx].traits;
    while (t) { n += (int)(t & 1ull); t >>= 1; }
    return n;
}

static int rgDraftEligible(int idx)
{
    if (idx < 0 || idx >= RG_PLANT_N) return 0;
    return (int)rgPlants[idx].tier >= (int)RG_DRAFT_MIN_TIER;
}

/* 把一株肉鸽植物放进本局卡槽。返回 1 = 成功，0 = 不可用。
   freebie = 1 是当前每波三选一的正常路径；0 仅保留给未来真正的商店。
   ⚠️ 收费点**只在"购买"这一步**，种植端不再收费：
      玩家在货架看到的价格和实际扣款必须一致，否则会出现
      "标 120、买完只剩 30，种下去又扣 120" 的双重收费。 */
static int rgGrant(int idx, int freebie)
{
    int cost;
    if (idx < 0 || idx >= RG_PLANT_N) return 0;
    if (rgOwned(idx)) return 1;                      /* 本局已有：不重复收钱 */
    cost = freebie ? 0 : rgPlantCost(idx);
    if (cost > 0 && gSun < cost) { gBuyFailed = 1; return 0; }
    if (gRgN >= RG_MAX_SLOTS) {          /* 槽满：折算成阳光补偿，不让三选一空过 */
        gSun += 75 + 25 * rgPlants[idx].tier;
        return 1;
    }
    gSun -= cost;
    gRgCard[gRgN] = idx;
    gRgFree[gRgN] = freebie;
    gRgN++;
    gRgPicked++;
    return 1;
}

/* 「随机赠卡」遗物（原 R_LOADOUT_PLUS / 扩容卡组）的获得时效果。
   ------------------------------------------------------------------------
   白送一张随机肉鸽植物卡（免费，走 gRgCard 战斗内卡槽，
   **不占**开局那唯一 1 个编组卡位）。
   ⚠️ 只能从 applyRelic() 里"本次首次获得"的分支调用一次。
      放进 relicsRecalc() 会被每次拿遗物 / 每帧重算反复触发。 */
static void rgGrantStarterCard(void)
{
    int cand[RG_PLANT_N], n = 0, i;
    /* ⚠️ 这里**故意不套** rgDraftEligible：本函数只被遗物
       R_LOADOUT_PLUS 触发一次（一局最多一张），它不参与三选一的抽卡，
       也就不会稀释其他档位的概率。而保留它能让普通档那 14 株
       不至于变成彻底无法获取的死内容。
       如果希望普通档连这里也不出，把这行改成
       `if (!rgOwned(i) && rgDraftEligible(i))` 即可。 */
    for (i = 0; i < RG_PLANT_N; i++) if (!rgOwned(i)) cand[n++] = i;
    if (n == 0) { gSun += 150; return; }   /* 全部已解锁：折算 150 阳光 */
    rgGrant(cand[rndi(0, n)], 1);
}

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
            /* 肉鸽僵尸：rg 存「索引 + 1」，0 = 普通僵尸 */
            z->rg = (gPendingRgZ >= 0 && gPendingRgZ < RG_ZOMBIE_N) ? (gPendingRgZ + 1) : 0;
            gPendingRgZ = -1;
            /* 神级以上僵尸出场 → 地形与背景切到对应主题 */
            if (z->rg > 0 && rgZombies[z->rg - 1].tier >= RGQ_MYTH)
                terrainThemeByTraits(rgZombies[z->rg - 1].traits, rgZombies[z->rg - 1].name);
            z->hp = z->maxhp = zombieHpMax(type) * levelDefs[gCurLevel].hpMul * rgZombieHpOf(z);
            z->speed = zSpeed[type] * gZombieSpdMul * gZombieSpdUp
                     * levelDefs[gCurLevel].spdMul * rgZombieSpdOf(z) * zombieWaveSpeedMul();
            z->basehp = z->maxhp;            /* 成长/自愈的上限基准 */
            z->gen    = gPendingGenZ;        /* 分身代数，见 rgZombieTraitTick */
            gPendingGenZ = 0;
            if (type == ZT_ZAMBONI) z->speed *= gZamboniSlow;      /* 破冰 */
            z->anim = rnd(0, 6.28f);
            z->shield = (type == ZT_SCREENDOOR) ? 1 : 0;
            if (type == ZT_DIGGER) {
                /* 旧版直接在草坪最右格凭空出现。现在从草坪边缘外露头并短暂停顿，
                   玩家能先看到它；真正的潜地绕后由带预警的 RZ3_BURROW 处理。 */
                z->x = LAWN_X + COLS * CELL_W + 44.0f;
                z->rooted = 0.85f;
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

/* 「吞噬成长」的记账函数：僵尸真的阵亡时，通知附近带 RG_T_GROWEAT 的植物。
   ------------------------------------------------------------------------
   为什么需要它：RG_T_GROWEAT 的原实现是"每脉冲无条件 `maxhp *= 1.06`"，
   于是卡片上写的「吞掉僵尸后成长」**从来就没有被判断过** ——
   实测空场连跑 40 个脉冲一样涨到 10.3 倍（见 _test_devour_plants.c）。
   现在改成只有"附近确实死过僵尸"才兑现一次成长，并且有上限。 */
#define RG_DEVOUR_RADIUS_PX  150.0f
static void rgNotifyDevourNear(int row, float x)
{
    int rr, cc;
    for (rr = row - 1; rr <= row + 1; rr++) {
        if (rr < 0 || rr >= ROWS) continue;
        for (cc = 0; cc < COLS; cc++) {
            Plant *q = plantAt(rr, cc);
            if (!q || !q->alive || q->rg <= 0) continue;
            if (fabsf(cellCX(cc) - x) > RG_DEVOUR_RADIUS_PX) continue;
            if (rgPlantTraitsOf(q) & RG_T_GROWEAT) {
                if (q->rgEatPend < 5) q->rgEatPend++;
            }
        }
    }
}

static void zombieDie(Zombie *z, int byMower)
{
    COLORREF c1 = RGB(158, 184, 132), c2 = RGB(94, 104, 122);
    const unsigned long long T = rgZombieTraitsOf(z);
    int i;
    if (z->dead) return;                 /* 已经在倒地了 */
    /* 僵尸侧的死亡事件能力必须在 dead=1 前结算，避免倒地动画期间重复触发。 */
    if (!byMower && (T & RZ_REVIVE3) && !z->rgRevive) {
        z->rgRevive = 1;
        z->hp = z->maxhp * 0.58f;
        z->dead = 0;
        z->deadT = 0.0f;
        z->flash = 0.45f;
        addParticle(z->x, z->y - 50.0f, 0, -90.0f, 0.7f, 20, RGB(255, 120, 120), 1);
        return;
    }
    /* 复生（RZ3_REBORN / 不死僵尸、概率坩埚僵尸）：满血站起来一次。
       与 REVIVE3 共用 rgRevive 这一个"只复活一次"的闸门。
       byMower=1（小推车 / 防卡死强杀）刻意绕过所有复活 ——
       否则兜底清场会被复活机制抵消，关卡又会结算不了。 */
    if (!byMower && (T & RZ3_REBORN) && !z->rgRevive) {
        z->rgRevive = 1;
        z->hp = z->maxhp;
        z->dead = 0;
        z->deadT = 0.0f;
        z->flash = 0.5f;
        addParticle(z->x, z->y - 50.0f, 0, -110.0f, 0.8f, 24, RGB(255, 200, 90), 1);
        addParticle(z->x, z->y - 30.0f, 0, -60.0f, 0.6f, 16, RGB(255, 255, 255), 1);
        return;
    }
    if (!byMower && (T & RZ_FORKDEATH) && z->gen == 0 && !z->rgRevive) {
        z->rgRevive = 1;
        if (zombiesAlive() < MAX_ZOMBIES - 2) {
            gPendingRgZ = z->rg - 1;
            gPendingGenZ = 1;
            spawnZombieAt(ZT_NORMAL, z->row, z->x + 28.0f);
            spawnZombieAt(ZT_NORMAL, z->row, z->x - 28.0f);
            gPendingRgZ = -1;
            gPendingGenZ = 0;
        }
    }
    if (!byMower && (T & RZ_CURSEDEATH) && !z->rgCurse) {
        z->rgCurse = 1;
        for (i = 0; i < PT_COUNT; i++)
            if (cardCD[i] > 0.0f) cardCD[i] += 2.5f;
        for (i = 0; i < RG_MAX_SLOTS; i++)
            if (rgCardCD[i] > 0.0f) rgCardCD[i] += 2.5f;
        gSun -= 25 + 10 * (z->rg > 0 ? rgZombies[z->rg - 1].tier : 0);
        if (gSun < 0) gSun = 0;
    }
    /* 被小推车碾的没时间播倒地，用更"重"的撞击音代替 */
    /* 这里才是"真的死了"（上面几段复活/分叉/诅咒都已结算完）。
       小推车碾死的不算"被吞噬"，所以排除。 */
    if (!byMower) rgNotifyDevourNear(z->row, z->x);
    sfxPlay(byMower ? L"hit_hard" : L"zombie_die");
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
    /* 半径才吃品质加成。伤害不能在 explode 里乘 —— 末尾的连锁引爆是
       递归调用且会把 dmg 再传下去，在这儿乘会逐层复利。
       爆炸伤害的品质加成放在植物 switch 的调用点（乘 gRgAtkMul）。 */
    radius *= gExplodeRadMul * gRgBoomMul;
    /* 充能槽消费点：有充能时这次爆炸伤害 +60%、范围 +30%，并消耗一格 */
    if (gCharge > 0) { gCharge--; dmg *= 1.60f; radius *= 1.30f; }
    sfxPlay(L"explode");
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
                zombieSpecialMove(z, (z->x >= cx ? 1.0f : -1.0f) * 12.0f); /* 爆炸冲击 */
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
/* 肉鸽僵尸按品质分级登场：越稀有出现得越晚、越贵 */
static const int RGZ_MINWAVE[RGQ_COUNT] = { 2, 5, 8, 12, 15, 18 };
static const int RGZ_COST[RGQ_COUNT]    = { 2, 3, 5,  7, 10, 14 };

/* 在肉鸽僵尸池里按品质权重抽一只，返回 rgZombies 索引；-1 = 本波还抽不到 */
static int rgPickZombie(int idx, int budget)
{
    int i, t, tot = 0, roll;
    for (i = 0; i < RG_ZOMBIE_N; i++) {
        t = rgZombies[i].tier;
        if (idx < RGZ_MINWAVE[t] || RGZ_COST[t] > budget) continue;
        tot += RGQ_W[t];
    }
    if (tot <= 0) return -1;
    roll = rndi(0, tot);
    for (i = 0; i < RG_ZOMBIE_N; i++) {
        t = rgZombies[i].tier;
        if (idx < RGZ_MINWAVE[t] || RGZ_COST[t] > budget) continue;
        roll -= RGQ_W[t];
        if (roll < 0) return i;
    }
    return -1;
}

static void startWave(int idx)
{
    int budget, flag, tries;
    int row, i;
    float t0;

    if (idx >= levelClearWaves(gCurLevel)) return;
    sfxPlay(L"wave");                    /* 新波次警报 */
    /* 每 5 波一次旗帜压力波，不再只在第 10 波和末波出现。 */
    flag = (((idx + 1) % 5) == 0 || idx == levelClearWaves(gCurLevel) - 1);
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

    if (gInterest > 0) {                    /* 利滚利设单波上限，避免经济指数爆炸 */
        int gain = gSun * gInterest / 100;
        if (gain > 100) gain = 100;
        gSun += gain;
    }
    t0 = gTime;
    /* 后期僵尸按关卡进度解锁。
       下面这张表就是「出怪池」的唯一来源：任何 zCost>0 的类型（旗帜除外）
       都会自动进入本波的候选池。以前池子被硬编码成普通/路障/铁桶三种，
       这里解锁了 10 种却只有 3 种真的会出场 —— 现在改成一表驱动。 */
    zCost[ZT_NORMAL] = 1;                     /* 普通僵尸：始终可用 */
    if (idx >= 2)  zCost[ZT_CONE]   = 2;      /* 路障 */
    if (idx >= 5)  zCost[ZT_BUCKET] = 4;      /* 铁桶 */
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

    /* ---- 新五关专属僵尸：只在各自关卡出场，随波次逐步加入 ----
       设计上每只都对应它那一关的规则，用来「破解玩家的应对策略」：
         霜牙隘口：冰锥（溅射冰冻）→ 冰川巨尸（免疫击退，废掉击退流）
         熔心裂谷：灰烬行者（死亡遮视线）→ 熔心（免疫裂隙，堵死靠裂隙清怪）
         幽影回廊：幽影（迷雾中加速）→ 闪现（跳过火力线）
         机枢要塞：机械（反弹伤害）→ 短路（切断整行供电）
         星界王座：全部出场（终局关）
       这样安排让每关的「新敌人」都有明确的教学意图，而不是纯粹加血。 */
    {
        int lv = gCurLevel;
        if (lv == 6 || lv == 10) {
            if (idx >= 4)  zCost[ZT_FROSTFANG] = 3;
            if (idx >= 11) zCost[ZT_GLACIER]   = 8;
        }
        if (lv == 7 || lv == 10) {
            if (idx >= 5)  zCost[ZT_ASHWALKER] = 3;
            if (idx >= 12) zCost[ZT_MAGMAW]    = 6;
        }
        if (lv == 8 || lv == 10) {
            if (idx >= 4)  zCost[ZT_PHANTOM]   = 3;
            if (idx >= 10) zCost[ZT_BLINKER]   = 4;
        }
        if (lv == 9 || lv == 10) {
            if (idx >= 6)  zCost[ZT_COGWORK]   = 4;
            if (idx >= 12) zCost[ZT_SABOTEUR]  = 5;
        }
    }
    /* 旗帜僵尸领队 */
    if (flag) {
        int r = rndi(0, ROWS);
        if (spawnQN < MAX_SPAWNS) { spawnQ[spawnQN].type = ZT_FLAG; spawnQ[spawnQN].row = r; spawnQ[spawnQN].t = t0; spawnQ[spawnQN].rg = 0; spawnQN++; }
        budget -= 1;
    }
    tries = 0;
    while (budget > 0 && tries < 600 && spawnQN < MAX_SPAWNS) {
        /* 候选池 = 本波已解锁 且 当前预算买得起 的类型（旗帜单独出，不入池）。
           按 zWeight 加权抽取：普通/路障/铁桶是主力，反制型僵尸中等频率，
           巨人 BOSS 稀有 —— 如果 13 种均匀分布，后期波次会一次涌出十几个巨人。 */
        int pool[64], pn = 0, pick, type;
        int rgz = -1, cost;
        tries++;
        for (i = 0; i < ZT_COUNT; i++) {
            int wt, k;
            if (i == ZT_FLAG || zCost[i] <= 0 || zCost[i] > budget) continue;
            wt = zWeight[i];
            for (k = 0; k < wt && pn < (int)(sizeof(pool) / sizeof(pool[0])); k++)
                pool[pn++] = i;
        }
        if (pn == 0) break;                 /* 预算买不起任何僵尸：本波到此为止 */
        /* 约 1/3 概率改出「肉鸽僵尸」：按品质分级解锁，附带 64 位负面能力位。
           它们比同级普通僵尸更硬更贵，是后期难度的主要来源。 */
        {
            int eliteChance = 32 + idx * 2;
            if (eliteChance > 68) eliteChance = 68;
            if ((rand() % 100) < eliteChance) rgz = rgPickZombie(idx, budget);
        }
        if (rgz >= 0) {
            type = rgZombies[rgz].base;
            cost = RGZ_COST[rgZombies[rgz].tier];
        } else {
            pick = pool[rndi(0, pn)];
            type = pick;
            cost = zCost[type];
        }
        row = rndi(0, ROWS);
        spawnQ[spawnQN].type = type;
        spawnQ[spawnQN].rg   = rgz + 1;     /* +1 编码：0 = 普通僵尸 */
        spawnQ[spawnQN].row = row;
        /* 同一波内的散布窗口：越到后期压得越紧。
           波次预算早就超过出怪上限（160），继续加预算没用，
           "同样数量的僵尸在更短时间内涌出"才是真正加难度的杠杆。 */
        {
            float win = 9.0f - (float)idx * 0.30f;
            if (win < 2.8f) win = 2.8f;
            spawnQ[spawnQN].t = t0 + rnd(0, win) * (flag ? 0.62f : 1.0f);
        }
        spawnQN++;
        budget -= cost;
    }
    gWave = idx + 1;
    gWaveStallT = 0.0f;                 /* 新一波：卡死计时归零 */
    if (flag) { addShake(6, 0.55f); flashRed = 0.5f; }
    waveTimer = flag ? 18.0f : 13.5f;   /* 缩短清场后的空档，保持持续压力 */
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
        peas[k].dmg = dmg * gRgAtkMul;   /* 肉鸽植物：按品质倍率放大杀伤力 */
        peas[k].row = row;
        peas[k].chainLeft = gRgPeaBounce;
        peas[k].lastHit = -1;
        peas[k].src = src; peas[k].frozen = frozen;
        peas[k].root = 0; peas[k].knockback = 0; peas[k].homing = 0;
        peas[k].bounces = 0; peas[k].bouncesBase = 0;
        /* 穿透 / 溅射由"当前开火植物"的品质决定（见植物更新循环） */
        peas[k].pierceLeft = gRgPeaPierce;
        peas[k].splash     = gRgPeaSplash;
        peas[k].aoe        = gRgPeaAoe;
        return k;
    }
    return -1;
}

static int extraPlantHasTarget(int row, float fromX, int span)
{
    int i;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        if (!z->active || z->dead) continue;
        if (span >= 0 && abs(z->row - row) > span) continue;
        if (z->x > fromX - 36.0f && z->x < VIEW_W + 80.0f) return 1;
    }
    return 0;
}

static int extraPlantNearestZombie(float x, float y, int row, int span, int forwardOnly)
{
    int i, best = -1;
    float bd = 1e30f;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        float dx, dy, d;
        if (!z->active || z->dead) continue;
        if (span >= 0 && abs(z->row - row) > span) continue;
        if (forwardOnly && z->x < x - 36.0f) continue;
        dx = z->x - x;
        dy = (z->y - 40.0f) - y;
        d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int extraPlantDensestZombie(int *outRow, float *outX)
{
    int i, j, best = -1, bestCnt = 0;
    float bx = 0.0f;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        int cnt = 0;
        if (!z->active || z->dead) continue;
        for (j = 0; j < MAX_ZOMBIES; j++) {
            Zombie *q = &zombies[j];
            if (!q->active || q->dead || q->row != z->row) continue;
            if (fabsf(q->x - z->x) <= 120.0f) cnt++;
        }
        if (cnt > bestCnt) { bestCnt = cnt; best = z->row; bx = z->x; }
    }
    if (best < 0) return 0;
    *outRow = best; *outX = bx;
    return 1;
}

/* 虚空吞噬者：普通僵尸在半血以下会被直接吞掉；巨人/BOSS 不允许被白送秒杀，
   但会吃到显著真伤、短暂定身，并为吞噬者回血。这样“吞噬”始终有可见效果。 */
static void plantVoidDevourTick(Plant *p, float dt, int r, int c, float cx, float by, float dmgMul)
{
    int i, best = -1;
    float bd = 1e30f, heal, hit;
    (void)c;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        float dx, dy, d;
        if (!z->active || z->dead) continue;
        if (abs(z->row - r) > 1) continue;
        dx = z->x - cx; dy = (z->y - 40.0f) - (by - 46.0f);
        if (dx < -46.0f || dx > 215.0f) continue;
        d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return;
    p->timer -= dt;
    if (p->timer > 0.0f) return;
    p->timer = 3.75f * gRateMul;
    {
        Zombie *z = &zombies[best];
        int swallowed = 0;
        if (z->type != ZT_GIANT && z->maxhp > 0.0f && z->hp <= z->maxhp * 0.55f) {
            zombieDie(z, 1);                 /* 绕过一次性复活，保证吞噬兑现 */
            swallowed = 1;
        } else {
            hit = z->maxhp * (z->type == ZT_GIANT ? 0.24f : 0.34f);
            if (hit < 260.0f * dmgMul) hit = 260.0f * dmgMul;
            z->hp -= hit;
            z->rooted = z->rooted < 1.25f ? 1.25f : z->rooted;
            z->flash = 0.22f;
            if (z->hp <= 0.0f) { zombieDie(z, 1); swallowed = 1; }
        }
        heal = p->maxhp * (swallowed ? 0.48f : 0.20f);
        p->hp += heal;
        if (p->hp > p->maxhp) p->hp = p->maxhp;
        for (i = 0; i < 22; i++)
            addParticle(cx + rnd(-32, 42), by - rnd(18, 92),
                        rnd(-85, 85), rnd(-120, -25), 0.48f, rnd(3, 7),
                        swallowed ? RGB(176, 110, 255) : RGB(96, 58, 180), 1);
        addShake(swallowed ? 5.0f : 2.8f, swallowed ? 0.24f : 0.14f);
    }
}

static void extraPlantHurt(Zombie *z, float dmg, float slow, float root, float knock)
{
    if (!z || !z->active || z->dead) return;
    z->hp -= dmg;
    z->flash = 0.10f;
    if (slow > z->slow) z->slow = slow;
    if (root > z->rooted) z->rooted = root;
    if (knock > 0.0f) zombieKnockback(z, knock);
    if (z->hp <= 0.0f) zombieDie(z, 0);
}

static int extraPlantShoot(int src, int row, float x, float y, float vx, float vy,
                           float dmg, int frozen, int wpn, int pierce, int splash,
                           int rootMs, int knock, int aoe, int chain, int homing)
{
    int id = spawnPeaAt(x, y, vx, vy, dmg, row, src, frozen);
    if (id >= 0) {
        peas[id].wpn = wpn;
        if (pierce > 0) RG_SET_PIERCE(peas[id], pierce);
        if (splash > 0) RG_SET_SPLASH(peas[id], splash);
        peas[id].root = rootMs;
        peas[id].knockback = knock;
        peas[id].aoe = aoe;
        peas[id].chainLeft = chain > peas[id].chainLeft ? chain : peas[id].chainLeft;
        peas[id].homing = homing;
        if (wpn == WPN_FREEZE) peas[id].freeze = rootMs > 0 ? rootMs : 800;
    }
    return id;
}

static void extraPlantHealArea(int row, int col, int rad, float heal, float shield)
{
    int rr, cc;
    for (rr = row - rad; rr <= row + rad; rr++) for (cc = col - rad; cc <= col + rad; cc++) {
        Plant *q;
        if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) continue;
        q = plantAt(rr, cc);
        if (!q || !q->alive) continue;
        if (heal > 0.0f && q->hp < q->maxhp) {
            q->hp += heal;
            if (q->hp > q->maxhp) q->hp = q->maxhp;
        }
        if (shield > 0.0f && q->shield < shield) q->shield += shield * 0.35f;
        if (q->shield > shield) q->shield = shield;
    }
}

static int extraPlantTick(Plant *p, float dt, int r, int c, float cx, float by, float py)
{
    int i, id, best, rr;
    float dmg = gDmgMul * (1.0f + fieldDmgBonusAt(r, c));
    switch (p->type) {
    case PT_BUBBLELOTUS:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 1.65f * gRateMul;
        extraPlantShoot(p->type, r, cx + 22, py, 290, 0, 28.0f * dmg, 0,
                        WPN_SPLASH, 0, 1, 650, 18, 52, 0, 0);
        p->recoil = 1.0f; return 1;

    case PT_ECHOBAMBOO:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 1.85f * gRateMul;
        extraPlantShoot(p->type, r, cx + 22, py, 420, 0, 22.0f * dmg, 0,
                        WPN_PIERCE, 5, 0, 0, 10, 0, 2, 0);
        p->recoil = 1.0f; return 1;

    case PT_DREAMCAP:
        best = extraPlantNearestZombie(cx, py, r, 0, 1);
        if (best < 0) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 3.4f;
        extraPlantHurt(&zombies[best], 30.0f * dmg, slowDur() * 1.15f, 1.15f, 0.0f);
        return 1;

    case PT_SPOREORCHESTRA:
        if (!extraPlantHasTarget(r, cx, 1)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 2.55f * gRateMul;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || abs(z->row - r) > 1 || z->x < cx - 20.0f) continue;
            extraPlantHurt(z, 16.0f * dmg, 0.70f, 0.0f, 0.0f);
        }
        for (rr = r - 1; rr <= r + 1; rr++) if (rr >= 0 && rr < ROWS)
            addParticle(cx + 22, cellBaseY(rr) - 50, rnd(80, 160), rnd(-70, 30),
                        0.55f, rnd(4, 8), RGB(170, 96, 210), 1);
        p->recoil = 1.0f; return 1;

    case PT_RUNECACTUS:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 1.35f * gRateMul;
        extraPlantShoot(p->type, r, cx + 26, py, 460, 0, 35.0f * dmg, 0,
                        WPN_PIERCE, 4, 0, 350, 0, 0, 0, 0);
        p->recoil = 1.0f; return 1;

    case PT_CLOCKSPROUT:
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 6.0f;
        for (i = 0; i < PT_COUNT; i++) if (cardCD[i] > 0.0f) { cardCD[i] -= 0.85f; if (cardCD[i] < 0.0f) cardCD[i] = 0.0f; }
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r) continue;
            extraPlantHurt(z, 10.0f * dmg, 0.85f, 0.22f, 0.0f);
        }
        return 1;

    case PT_MIRRORFERN:
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 3.8f;
        extraPlantHealArea(r, c, 1, 0.0f, 360.0f);
        best = extraPlantNearestZombie(cx, py, r, 0, 1);
        if (best >= 0 && zombies[best].x - cx < 190.0f)
            extraPlantHurt(&zombies[best], 34.0f * dmg, 0.0f, 0.0f, 16.0f);
        return 1;

    case PT_COMETCLOVER:
        best = extraPlantNearestZombie(cx, py, r, -1, 1);
        if (best < 0) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 2.25f * gRateMul;
        id = extraPlantShoot(p->type, zombies[best].row, cx + 18, py, 360, 0,
                             52.0f * dmg, 0, WPN_SPLASH, 0, 1, 0, 12, 72, 0, 1);
        if (id >= 0) {
            float tx = zombies[best].x - cx, ty = zombies[best].y - 40.0f - py;
            float len = sqrtf(tx * tx + ty * ty) + 1e-4f;
            peas[id].vx = 420.0f * tx / len; peas[id].vy = 420.0f * ty / len;
        }
        p->recoil = 1.0f; return 1;

    case PT_HONEYDRIPPER:
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = rnd(10.0f, 13.0f);
        spawnSunDrop(cx + rnd(-8, 8), by - 72, by - 6, (int)(35.0f * gTrGarden));
        extraPlantHealArea(r, c, 1, 90.0f, 0.0f);
        return 1;

    case PT_TESLAPUMPKIN:
        p->shield += 35.0f * dt; if (p->shield > 850.0f) p->shield = 850.0f;
        best = extraPlantNearestZombie(cx, py, r, 1, 0);
        if (best < 0 || fabsf(zombies[best].x - cx) > 180.0f) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 2.45f;
        extraPlantHurt(&zombies[best], 55.0f * dmg, 0.0f, 0.10f, 0.0f);
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (i == best || !z->active || z->dead) continue;
            if (abs(z->row - zombies[best].row) <= 1 && fabsf(z->x - zombies[best].x) < 125.0f)
                extraPlantHurt(z, 26.0f * dmg, 0.0f, 0.0f, 0.0f);
        }
        return 1;

    case PT_BUBBLECANNON:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 3.15f * gRateMul;
        extraPlantShoot(p->type, r, cx + 22, py, 250, -20, 78.0f * dmg, 0,
                        WPN_SPLASH, 0, 2, 950, 44, 92, 0, 0);
        p->recoil = 1.0f; return 1;

    case PT_SONICBLOOM:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 2.55f * gRateMul;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r || z->x < cx - 20.0f) continue;
            extraPlantHurt(z, 34.0f * dmg, 0.30f, 0.0f, 26.0f);
        }
        p->recoil = 1.0f; return 1;

    case PT_NEBULABEET:
        best = extraPlantNearestZombie(cx, py, r, 1, 1);
        if (best < 0) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 4.2f;
        explode(zombies[best].x, cellBaseY(zombies[best].row) - 34.0f,
                96.0f, 92.0f * dmg, 0, zombies[best].row);
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || abs(z->row - zombies[best].row) > 1) continue;
            if (fabsf(z->x - zombies[best].x) < 130.0f) extraPlantHurt(z, 0.0f, 1.0f, 0.55f, 10.0f);
        }
        addShake(3.0f, 0.15f); return 1;

    case PT_LANTERNMELON:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 2.65f * gRateMul;
        extraPlantShoot(p->type, r, cx + 14, py, 260, -55, 72.0f * dmg, 0,
                        WPN_BURN, 0, 2, 0, 0, 90, 0, 0);
        terrainTileAdd(r, c + 2, TF_SCORCH, 4.0f);
        p->recoil = 1.0f; return 1;

    case PT_CORALGUARD:
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 2.4f;
        extraPlantHealArea(r, c, 1, 32.0f, 520.0f);
        return 1;

    case PT_GEARFLOWER:
        if (!extraPlantHasTarget(r, cx, 0)) { if (p->burst > 0) p->burst--; return 1; }
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        if (p->burst < 6) p->burst++;
        p->timer = (1.55f - 0.11f * (float)p->burst) * gRateMul;
        if (p->timer < 0.72f) p->timer = 0.72f;
        id = extraPlantShoot(p->type, r, cx + 24, py, 360, 0, 25.0f * dmg, 0,
                             WPN_BOUNCE, 1, 0, 0, 8, 0, 0, 0);
        if (id >= 0) peas[id].bounces = peas[id].bouncesBase = 1;
        p->recoil = 1.0f; return 1;

    case PT_SNAILSHROOM:
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 1.8f;
        terrainTileAdd(r, c + 1, TF_WEB, 4.0f);
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r || z->x < cx - 20.0f || z->x > cx + 360.0f) continue;
            extraPlantHurt(z, 10.0f * dmg, slowDur() * 0.65f, 0.0f, 0.0f);
        }
        return 1;

    case PT_PRISMWILLOW:
        if (!extraPlantHasTarget(r, cx, 1)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 2.35f * gRateMul;
        for (rr = r - 1; rr <= r + 1; rr++) if (rr >= 0 && rr < ROWS) {
            float m = (rr == r) ? 1.0f : 0.68f;
            extraPlantShoot(p->type, rr, cx + 24, cellBaseY(rr) - 62.0f,
                            880, 0, 38.0f * dmg * m, 0, WPN_PIERCE, 10, 0, 0, 0, 0, 0, 0);
        }
        p->recoil = 1.0f; return 1;

    case PT_PAPERCRANE:
        best = extraPlantNearestZombie(cx, py, r, -1, 1);
        if (best < 0) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 1.05f * gRateMul;
        id = extraPlantShoot(p->type, zombies[best].row, cx + 18, py, 420, 0,
                             22.0f * dmg, 0, WPN_NORMAL, 0, 0, 0, 0, 0, 1, 1);
        if (id >= 0) {
            float tx = zombies[best].x - cx, ty = zombies[best].y - 40.0f - py;
            float len = sqrtf(tx * tx + ty * ty) + 1e-4f;
            peas[id].vx = 450.0f * tx / len; peas[id].vy = 450.0f * ty / len;
        }
        p->recoil = 1.0f; return 1;

    case PT_MONSOONREED:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 1.95f * gRateMul;
        extraPlantShoot(p->type, r, cx + 24, py, 400, 0, 21.0f * dmg, 1,
                        WPN_PIERCE, 7, 0, 0, 30, 0, 0, 0);
        p->recoil = 1.0f; return 1;

    case PT_CANDLESPROUT:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 1.45f * gRateMul;
        extraPlantShoot(p->type, r, cx + 20, py, 330, 0, 25.0f * dmg, 0,
                        WPN_BURN, 0, 0, 0, 0, 0, 0, 0);
        fireSpawn(cx + 88.0f, by + 6.0f, 1.8f);
        p->recoil = 1.0f; return 1;

    case PT_QUARTZKERNEL:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 2.15f * gRateMul;
        extraPlantShoot(p->type, r, cx + 20, py, 280, -30, 40.0f * dmg, 0,
                        WPN_FREEZE, 0, 0, 950, 0, 0, 0, 0);
        p->recoil = 1.0f; return 1;

    case PT_BELLFLOWER:
        if (zombiesAlive() <= 0) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 5.8f;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead) continue;
            extraPlantHurt(z, 16.0f * dmg, 0.75f, 0.22f, 0.0f);
        }
        return 1;

    case PT_VORTEXTURNIP:
        best = extraPlantNearestZombie(cx, py, r, 0, 1);
        if (best < 0) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 4.7f;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r) continue;
            if (fabsf(z->x - zombies[best].x) < 150.0f)
                extraPlantHurt(z, 45.0f * dmg, 0.60f, 0.45f, 34.0f);
        }
        addShake(3.0f, 0.14f); return 1;

    case PT_PAINTBRUSH:
        terrainTileAdd(r, c, TF_BLOOM, 4.0f);
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 1.85f * gRateMul;
        extraPlantShoot(p->type, r, cx + 20, py, 340, rnd(-24, 24), 26.0f * dmg, 0,
                        WPN_SPLASH, 0, 1, 0, 0, 42, 0, 0);
        terrainTileAdd(r, c + 1 + (rand() % 2), TF_BLOOM, 7.5f);
        p->recoil = 1.0f; return 1;

    case PT_DUSKORCHID:
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 3.2f;
        p->burst = !p->burst;
        if (p->burst) {
            int cc;
            for (cc = 0; cc < COLS; cc++) {
                Plant *q = plantAt(r, cc);
                if (q && q->alive) { q->hp += 110.0f; if (q->hp > q->maxhp) q->hp = q->maxhp; }
            }
        } else {
            best = extraPlantNearestZombie(cx, py, r, 1, 1);
            if (best >= 0) {
                id = extraPlantShoot(p->type, zombies[best].row, cx + 18, py, 370, 0,
                                     64.0f * dmg, 0, WPN_PIERCE, 2, 0, 300, 0, 0, 1, 1);
                if (id >= 0) {
                    float tx = zombies[best].x - cx, ty = zombies[best].y - 40.0f - py;
                    float len = sqrtf(tx * tx + ty * ty) + 1e-4f;
                    peas[id].vx = 420.0f * tx / len; peas[id].vy = 420.0f * ty / len;
                }
            }
        }
        return 1;

    case PT_STEAMPEPPER:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 5.1f;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r || z->x < cx - 25.0f) continue;
            extraPlantHurt(z, 95.0f * dmg, 0.55f, 0.0f, 42.0f);
            fireSpawn(z->x, z->y + 5.0f, 2.2f);
        }
        addShake(4.0f, 0.18f); p->recoil = 1.0f; return 1;

    case PT_ORBITALMOSS:
        if (!extraPlantDensestZombie(&rr, &dmg)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 5.4f;
        explode(dmg, cellBaseY(rr) - 34.0f, 132.0f, 140.0f * gDmgMul, 0, rr);
        addShake(6.0f, 0.22f); return 1;

    case PT_LULLABYONION:
        best = extraPlantNearestZombie(cx, py, r, 1, 1);
        if (best < 0) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 4.0f;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || abs(z->row - r) > 1) continue;
            if (fabsf(z->x - zombies[best].x) < 170.0f)
                extraPlantHurt(z, 22.0f * dmg, slowDur() * 1.20f, 1.05f, 0.0f);
        }
        return 1;

    case PT_AURORAPEA:
        if (!extraPlantHasTarget(r, cx, 0)) return 1;
        p->timer -= dt; if (p->timer > 0.0f) return 1;
        p->timer = 1.45f * gRateMul;
        extraPlantShoot(p->type, r, cx + 24, py, 430, 0, 44.0f * dmg, 1,
                        WPN_FREEZE, 3, 0, 950, 6, 0, 2, 0);
        p->recoil = 1.0f; return 1;

    default:
        return 0;
    }
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
    /* 肉鸽植物：rg 存「索引 + 1」，0 = 普通植物（兼容 memset 创建路径） */
    p->rg = (gPendingRg >= 0 && gPendingRg < RG_PLANT_N) ? (gPendingRg + 1) : 0;
    gPendingRg = -1;
    /* 贷款高坚果：种下时把生命翻倍，结算时再偿还一笔阳光。 */
    if (p->rg > 0 && (rgPlantTraitsOf(p) & RG_T_LOAN)) {
        p->fuseMul = 2.0f;
        p->rgDebt = 80 + 30 * rgPlants[p->rg - 1].tier;
    }
    /* 神级以上植物落场 → 地形与背景切到对应主题 */
    if (p->rg > 0 && rgPlants[p->rg - 1].tier >= RGQ_MYTH)
        terrainThemeByTraits(rgPlants[p->rg - 1].traits, rgPlants[p->rg - 1].name);
    /* 血量按品质倍率放大：稀有 4 倍 → 传奇 20 倍 */
    p->hp = p->maxhp = plantHpMax(type) * rgPlantAtkMul(p) * p->fuseMul;
    /* 镜像墙坚果：卡片写「血量 4 倍」，原来在脉冲里只做了 +5% 自愈，
       4 倍这条**从未实现**。真正该在出场时放大 —— 放在这里。 */
    if (p->rg > 0 && (rgPlantTraitsOf(p) & RG_T_MIRROR))
        p->hp = p->maxhp = p->maxhp * 4.0f;
    p->rgBaseHp = p->maxhp;      /* 成长类机制的上限基准，见 Plant 结构里的说明 */
    p->phase = rnd(0, 6.28f);
    if (gBloodPact) p->hp = p->maxhp * 0.70f;   /* 血祭 */
    sfxPlay(L"plant");                          /* 种植：泥土的"噗" */
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
    case PT_HONEYDRIPPER:
        p->timer = 5.0f; break;
    case PT_CLOCKSPROUT: case PT_MIRRORFERN: case PT_CORALGUARD:
    case PT_BELLFLOWER: case PT_DUSKORCHID: case PT_ORBITALMOSS:
        p->timer = 2.2f; break;
    case PT_DREAMCAP: case PT_SPOREORCHESTRA: case PT_TESLAPUMPKIN:
    case PT_NEBULABEET: case PT_SNAILSHROOM: case PT_VORTEXTURNIP:
    case PT_LULLABYONION:
        p->timer = 1.0f; break;
    case PT_BUBBLELOTUS: case PT_ECHOBAMBOO: case PT_RUNECACTUS:
    case PT_COMETCLOVER: case PT_BUBBLECANNON: case PT_SONICBLOOM:
    case PT_LANTERNMELON: case PT_GEARFLOWER: case PT_PRISMWILLOW:
    case PT_PAPERCRANE: case PT_MONSOONREED: case PT_CANDLESPROUT:
    case PT_QUARTZKERNEL: case PT_PAINTBRUSH: case PT_STEAMPEPPER:
    case PT_AURORAPEA:
        p->timer = 0.45f; break;
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
    gFxParticleLimit = MAX_PARTS;
    gFxParticleCursor = 0;
    gFxTrailSteps = 4;
    memset(spawnQ, 0, sizeof(spawnQ));
    spawnQN = 0;
    memcpy(zCost, zCostBase, sizeof(zCost));   /* 每局从 0 波基准还原出怪解锁表 */
    gDailyMode = 0;              /* 默认非每日；仅每日营地入口会在调用本函数前置 1 */

    memset(gRelic, 0, sizeof(gRelic));
    memset(gGrowthStack, 0, sizeof(gGrowthStack));
    gRelicCount = 0; gKillStack = 0; gKillStackDrawn = -1;
    gMedicT = 12.0f;
    relicsRecalc();
    memset(gFires, 0, sizeof(gFires));
    gSun = levelDefs[gCurLevel].startSun + gStartSunAdd;
    if (gWeekSunAdd) gSun += gWeekSunAdd;         /* 星期主题开局加成 */
    if (gDailyMode)  gSun -= 25;                  /* 每日挑战：开局比平时紧张一点 */
    gTime = 0; gWave = 0; gSel = -1; gShovel = 0;
    newLevelInit();               /* 新五关的场地规则（薄冰/裂隙/迷雾/阶段…） */
    waveTimer = 14.0f; skySunTimer = 5.0f;
    gKilled = 0; gElapsed = 0;
    shakeT = 0; shakeMag = 0; flashRed = 0; gMowerUsed = 0;
    loadoutBuild();
    /* 本局的神级植物必须每局从零开始随机 —— 上一局带到下一局会毁掉随机性 */
    memset(gBonusPlant, 0, sizeof(gBonusPlant));
    gBonusN = 0;
    /* 肉鸽层：每局清零。gRgDraftWave = -1 让第 0 波开波前也弹一次三选一 */
    gRgN = 0; gRgPicked = 0; gRgDraftWave = -1;
    gPendingRg = -1; gPendingRgZ = -1; gRgAtkMul = 1.0f;
    for (i = 0; i < RG_MAX_SLOTS; i++) { gRgCard[i] = -1; gRgFree[i] = 0; }
    gDraftFree = 1; gBuyFailed = 0;      /* 每波三选一均是免费奖励 */
    /* 动态地形与事件主题：每局从干净的平地开始 */
    {
        int rr, cc;
        for (rr = 0; rr < ROWS; rr++) for (cc = 0; cc < COLS; cc++) {
            gTile[rr][cc] = 0; gTileT[rr][cc] = 0.0f;
        }
    }
    gTheme = TH_NONE; gThemeT = 0.0f; gThemePulse = 0.0f;
    gThemeWho[0] = 0; gThemeBannerT = 0.0f;
    gWaveStallT = 0.0f; gPendingGenZ = 0;
    gWinDrafted = 0; gPendingWin = 0;
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
    gGold = 0; gGhost = 0; gFury = 0.0f; gCharge = 0;
    gFuryT = 0.0f; gFuryBolt = 0; gFuryReady = 0; gBanker = 0; gNecromancer = 0;
    /* ⚠️ 原来这里有两行：
           if (HAS(R_WATER_MODE)) { ... }   if (HAS(R_NIGHT_MODE)) { ... }
       它们是**死代码** —— 本函数在上面已经 `memset(gRelic, 0, ...)` 清空了遗物，
       而遗物只可能在本局中途通过三选一拿到，所以这两处的 HAS() 永远为假，
       「水中模式」「夜间模式」两张卡从设计上就不可能生效。
       现在改为"拿到卡即解锁并切换"（见 applyRelic 的 switch），
       与 R_MUTANT_SEED 的处理方式一致。 */
    (void)0;
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

/* 全屏雷击 —— 愤怒条的兑现方式。
   之前这段逻辑只喷了粒子 + 白闪，一颗僵尸都不伤，"45 秒满一次"于是毫无收益；
   而且 gFuryT 从来没被写成正数，渲染端的雷柱分支是纯死码。现在补齐三件事：
   ① 按行降下雷柱（地面僵尸受重伤、空中/钻地免伤）；② 置 gFuryT 让表现层画得出来；
   ③ 清空愤怒条并把 gFuryReady 灭掉。manual=1 表示玩家主动按 E 引爆（不消耗额外资源）。 */
static void furyBlast(int manual)
{
    int r, i, hit = 0;
    const float DMG = 140.0f;            /* 单次雷击对每只地面僵尸的伤害 */
    gFury  = 0.0f;
    gFuryReady = 0;
    gFuryT = 0.55f;                      /* 表现时长：够画完一轮雷柱 */
    gFuryBolt = rnd(0, 100000);
    flashRed = 0.45f;
    addShake(manual ? 9.0f : 7.0f, 0.35f);
    for (r = 0; r < ROWS; r++) {
        float cx = LAWN_X + (float)COLS * CELL_W * 0.5f;
        /* 行内雷柱特效 */
        for (i = 0; i < 7; i++)
            addParticle(cx + rnd(-260, 260), LAWN_Y + rnd(0, 40),
                        rnd(-30, 30), rnd(120, 240), 0.35f, rnd(4, 8),
                        (i & 1) ? RGB(255, 248, 214) : RGB(255, 196, 72), 1);
    }
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        if (!z->active || z->dead) continue;
        if (z->type == ZT_BALLOON || z->type == ZT_DIGGER) continue;  /* 空中/钻地：雷打不到 */
        z->hp -= DMG;                                                 /* 雷击无视护具减伤 */
        z->flash = 0.25f;
        z->slow = (z->slow < 1.2f) ? 1.2f : z->slow;                  /* 雷击附带僵直 */
        hit++;
        if (z->hp <= 0.0f) zombieDie(z, 0);
    }
    (void)hit;
}

/* ======================================================================== */
/*              肉鸽植物能力派发：与基座 switch 并行运行的"额外效果层"           */
/* ======================================================================== */
/* 设计：一株肉鸽植物 = 基座行为（贴图 / 种植 / 主攻击，复用 plantDefs）
                     + 这里按能力位补的效果（脉冲式触发）
   这样 50 株植物不必写 50 份重复 update，也不会有贴图缺失问题。
   脉冲周期由品质推导：越稀有触发越快。                                          */
static void rgPlantTraitTick(Plant *p, float dt, int r, int c,
                             float cx, float by, float py)
{
    const RgPlantDef *d;
    unsigned long long T, keep;
    int tier, i, rr;
    float period, base, R;

    if (p->rg <= 0 || p->rg > RG_PLANT_N) return;
    d = &rgPlants[p->rg - 1];
    T = d->traits;
    if (!T) return;
    tier = d->tier;

    p->rgt -= dt;
    /* 叠加类机制的断档衰减必须**每帧**跑，不能等脉冲 —— 否则"停火后掉层"
       永远不触发，过载会一路叠到天上去。 */
    if (p->rgAux > 0.0f) {
        p->rgAux -= dt;
        if (p->rgAux <= 0.0f) p->rgStack = 0;
    }
    period = 2.8f / rgRateMul(tier);          /* 品质越高，脉冲越密 */
    if ((T & RG_T_OVERLOAD) && p->rgStack > 0)
        period /= (1.0f + 0.12f * (float)p->rgStack);   /* 过载：叠层让脉冲更密 */
    if (p->rgt > 0.0f) return;
    p->rgt = period;

    keep = (unsigned long long)(gRgAtkMul * 1000.0f);   /* 保存，稍后还原 */
    gRgAtkMul = 1.0f;                         /* 本函数内的伤害已自行乘过倍率 */

    base = peaBaseDmg() * rgDmgMul(tier);
    /* 濒死狂暴 / 吸伤转能量：都是"已损失血量"驱动的增伤，
       直接由 hp/maxhp 推算，不需要额外状态字段，也不会被复制/存档影响。 */
    {
        float lost = (p->maxhp > 1.0f) ? (1.0f - p->hp / p->maxhp) : 0.0f;
        lost = CLAMP(lost, 0.0f, 1.0f);
        if (T & RG_T_LASTSTAND) base *= (1.0f + lost * 1.60f);
        if (T & RG_T_CONVERT)   base *= (1.0f + lost * 0.80f);
    }
    R    = rgRangeMul(tier) * 52.0f;          /* 影响半径（像素） */

    /* ---------- 多线齐射（量子豌豆射手） ---------- */
    if (T & RG_T_MULTILANE) {
        for (rr = r - 1; rr <= r + 1; rr++) {
            if (rr < 0 || rr >= ROWS) continue;
            spawnPeaAt(cx + 16.0f, cellBaseY(rr) - 62.0f, 340.0f, 0.0f,
                       base * 0.55f, rr, p->type, 0);
        }
    }

    /* ---------- 花瓣弹幕 / 弹幕向日葵 ---------- */
    if (T & RG_T_PETALBARRAGE) {
        int n = 5 + tier;
        for (i = 0; i < n; i++) {
            float a = 6.2832f * (float)i / (float)n;
            spawnPeaAt(cx, py, 200.0f * (float)cos(a),
                       200.0f * (float)sin(a), base * 0.30f, r, p->type, 0);
        }
    }

    /* ---------- 激光切割：整行瞬发，完全无视护甲 ---------- */
    if (T & RG_T_LASERCUT) {
        int hitAny = 0;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r) continue;
            if (z->x < cx - 10.0f) continue;
            z->hp -= base * 1.05f;            /* 真伤：不走护甲减伤 */
            z->flash = 0.12f;
            hitAny = 1;
            if (z->hp <= 0.0f) zombieDie(z, 0);
        }
        if (hitAny) addParticle(cx + 40.0f, py, 260.0f, 0.0f, 0.22f, 15,
                                RGB(120, 240, 255), 1);
    }

    /* ---------- 持续伤害：腐蚀 / 瘟疫 / 诅咒 ---------- */
    if (T & (RG_T_CORRODE | RG_T_PLAGUE | RG_T_CURSE)) {
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead) continue;
            if (z->x < cx - R || z->x > cx + R) continue;
            if ((float)(z->row - r) * (float)CELL_H > R) continue;
            z->hp -= base * 0.60f;
            z->flash = 0.09f;
            if (z->hp <= 0.0f) zombieDie(z, 0);
        }
    }

    /* ---------- 爆炸：链式 / 重组 / 自爆 ---------- */
    if (T & (RG_T_CHAINBOOM | RG_T_RECOMBINE | RG_T_SELFDESTRUCT)) {
        explode(cx, by - 30.0f, R * 0.9f, base * 1.6f, 0, r);
        addShake(3.0f, 0.12f);
    }

    /* ---------- 吸血回复（贪婪豌豆 / 逆向时间菇） ---------- */
    if (T & RG_T_LIFESTEAL) {
        float need = p->maxhp - p->hp;
        if (need > 0.0f) p->hp += need * 0.18f;      /* 每脉冲回 18% 缺口 */
    }

    /* ---------- 控制：冰冻 / 重力 / 催眠 / 蛛网 / 混乱 ---------- */
    if (T & (RG_T_TIMEFREEZE | RG_T_ICERESOURCE | RG_T_GRAVITY |
             RG_T_LULLABY | RG_T_WEBSLOW | RG_T_CONFUSE | RG_T_MEMESPREAD)) {
        float dur = 0.7f + 0.22f * (float)tier;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead) continue;
            if (z->x < cx - R || z->x > cx + R) continue;
            if ((float)(z->row - r) * (float)CELL_H > R) continue;
            if (z->slow < dur * 3.0f) z->slow = dur * 3.0f;   /* 减速 */
            if (z->rooted < dur)      z->rooted = dur;        /* 定身 */
            /* 蜘蛛丝缠绕：卡片写"在原位留下蛛网陷阱"，原来只有减速、
               没有任何陷阱 —— 这里真的给脚下那格铺一层 TF_WEB 地形
               （踩上去的僵尸会被粘住，已有的地块系统直接生效）。 */
            if (T & RG_T_WEBSLOW) {
                int wc = (int)floor((z->x - (float)LAWN_X) / (float)CELL_W);
                if (wc >= 0 && wc < COLS) terrainTileAdd(z->row, wc, TF_WEB, dur * 4.0f);
            }
            /* 噪音向日葵：卡片写"混乱走位"，原来只有减速 ——
               这里真的把它往回推（僵尸朝右 = 后退），走位真的乱掉。 */
            if (T & RG_T_CONFUSE) zombieSpecialMove(z, 26.0f + 6.0f * (float)tier);
            /* 重力玉米：卡片写「子弹坠落造成范围眩晕」，原来只有减速定身。
               真正的"眩晕"应该带冲击位移，这里补上击退。 */
            if (T & RG_T_GRAVITY) zombieSpecialMove(z, 34.0f + 8.0f * (float)tier);
        }
        /* 冰冻转化资源：暗物质冰冻 */
        if ((T & RG_T_ICERESOURCE) && (rand() % 3) == 0)
            spawnSunDrop(cx + rnd(-30, 30), by - 60.0f, by - 10.0f, 25 + 10 * tier);
        /* 模因传播葵：卡片写「传播阳光并造成减速」，原来**完全没有阳光产出**
           （它不在上面那条产阳光的 catch-all 里）。这里补上传播的那份阳光。 */
        if (T & RG_T_MEMESPREAD)
            spawnSunDrop(cx + rnd(-30, 30), by - 60.0f, by - 10.0f, 22 + 10 * tier);
    }

    /* ---------- 阳光与护盾：共享 / 吞噬 / 光环 / 内力 ---------- */
    if (T & (RG_T_SHARESUN | RG_T_EATSUN | RG_T_BLACKHOLE |
             RG_T_AURABUFF | RG_T_CHI | RG_T_DEVOURBUL)) {
        spawnSunDrop(cx + rnd(-18, 18), by - 70.0f, by - 8.0f, 20 + 12 * tier);
        /* 光环 / 内力：给范围内植物补血，等效于增伤续航 */
        if (T & (RG_T_AURABUFF | RG_T_CHI)) {
            for (rr = r - 1; rr <= r + 1; rr++) {
                int cc;
                if (rr < 0 || rr >= ROWS) continue;
                for (cc = c - 1; cc <= c + 1; cc++) {
                    Plant *q;
                    if (cc < 0 || cc >= COLS) continue;
                    q = plantAt(rr, cc);
                    if (!q || !q->alive) continue;
                    if (q->hp < q->maxhp) q->hp += q->maxhp * 0.06f;
                }
            }
        }
    }

    /* ---------- 吞噬来弹：把飞向本株的敌方子弹吃掉，每吃一发换一份阳光 ----------
       原来这一段**完全不存在**：DEVOURBUL 只被并进上面那条"产阳光"的 catch-all，
       于是"吞噬范围内所有来弹"是空的 —— 实测停在面前的豌豆毫发无损，
       产阳光量也和有没有子弹**一模一样**（_test_devour_plants.c，6 发/6 发）。
       现在补上真吞噬；原有的自产阳光保持不变（不削减任何既有收益）。 */
    if (T & RG_T_DEVOURBUL) {
        int eaten = 0;
        for (i = 0; i < MAX_PEAS; i++) {
            Pea *pe = &peas[i];
            if (!pe->active) continue;
            if (pe->vx >= 0.0f) continue;               /* 只吃"来弹"：朝我方飞的 */
            if (pe->row != r && abs(pe->row - r) > 1) continue;
            if (fabsf(pe->x - cx) > R) continue;
            pe->active = 0;                             /* 吞掉 */
            eaten++;
            addParticle(pe->x, pe->y, 0, -46.0f, 0.3f, 8, RGB(255, 224, 150), 1);
        }
        if (eaten > 0)
            spawnSunDrop(cx + rnd(-18, 18), by - 70.0f, by - 8.0f,
                         (18 + 12 * tier) * eaten);
    }

    /* ---------- 防御：镜像 / 假血 / 数字雨 / 概率墙 / 迷宫 ---------- */
    if (T & (RG_T_MIRROR | RG_T_FAKEHP | RG_T_DIGITRAIN |
             RG_T_DODGE | RG_T_MAZEWALL)) {
        if (p->hp < p->maxhp) p->hp += p->maxhp * 0.05f;   /* 自愈护层 */
        /* 数字雨：卡片写「生成屏障抵挡并反弹伤害」，原来只有自愈。
           屏障在本引擎里就是 p->shield（僵尸啃食时先扣它、且提供伤害减免），
           所以这里真的把屏障补上。 */
        if (T & RG_T_DIGITRAIN) {
            float cap = base * 2.4f;
            if (p->shield < cap) p->shield += base * 0.55f;
            if (p->shield > cap) p->shield = cap;
        }
    }

    /* ---------- 爆发：概率坩埚 / 赌徒 / 像素崩坏 / 暗杀 ---------- */
    if (T & (RG_T_GAMBLEDMG | RG_T_GAMBLERISK | RG_T_PIXELCRUSH | RG_T_ASSASSIN)) {
        float mult = 1.0f;
        if (T & RG_T_GAMBLERISK) {
            /* 赌徒豌豆：卡片写的是"伤害翻倍或归零"，而原实现和概率坩埚共用
               同一段"概率 ×10"，既不会归零、也不是翻倍 —— 两个字面承诺都没兑现。 */
            mult = ((rand() % 100) < 50) ? 2.0f : 0.0f;
        } else {
            /* 概率坩埚：卡片写的是「50% 概率打出 10 倍伤害」，
               而原实现是 (20+4*tier)%（神级只有 36%）—— 数字对不上。
               按卡片为准改成 50%：卡面写什么，玩家就该看到什么。 */
            if ((rand() % 100) < 50) mult = 10.0f;
            if (T & RG_T_ASSASSIN) mult = 18.0f;                 /* 暗杀：隐身必杀 */
        }
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r) continue;
            if (z->x < cx - 6.0f || z->x > cx + R) continue;
            if ((T & RG_T_PIXELCRUSH) && (rand() % 100) < 20 && !rgZombieBlocksInstantKill(z)) {
                z->hp = 0.0f;                                  /* 像素化即死 */
            } else {
                z->hp -= base * mult;
            }
            z->flash = 0.14f;
            if (z->hp <= 0.0f) zombieDie(z, 0);
        }
    }

    /* ---------- 抽卡：抽卡樱桃 / 卡牌抽取射手 ---------- */
    if (T & (RG_T_DROPCARD | RG_T_DRAWCARD)) {
        if ((rand() % 100) < (5 + 3 * tier)) {
            gSun += 50;
            spawnSunDrop(cx + rnd(-20, 20), by - 80.0f, by - 10.0f, 50 + 20 * tier);
        }
    }

    /* ---------- 位移：随机传送 / 爬格子 / 爬墙 ---------- */
    if ((T & RG_T_BLINK) && (rand() % 100) < 4) {
        int nr = rndi(0, ROWS), nc = rndi(0, COLS);
        Plant *dst = plantAt(nr, nc);
        if (dst && !dst->alive && cellLegal(nr, nc, 1, 1)) {
            Plant tmp = *p;
            removePlantAt(r, c);
            plantIt(nr, nc, (int)rgPlants[p->rg - 1].base);
            dst = plantAt(nr, nc);
            if (dst) { tmp.row = nr; tmp.col = nc; *dst = tmp; }
        }
    }

    /* ---------- 天气：随机天气葵 ---------- */
    if (T & RG_T_WEATHER) {
        int k;
        for (k = 0; k < 10; k++)
            addParticle(cx + rnd(-R, R), by - rnd(0, 120), rnd(-30, 30),
                        rnd(60, 180), rnd(0.4f, 0.9f), rnd(3, 7),
                        RGB(180, 220, 255), 1);
    }

    /* ---------- 节奏增伤：音乐节拍豌豆 ----------
       原来借 `p->shield` 当"节拍层数"，但 shield 的消费点（伤害减免）是
       `CLAMP(shield / 2200, 0.25, 1.0)` —— 3.0 远远够不到下限 0.25 的起点
       （要 550 才有反应），所以**那个写法等于什么都没做**。
       现在改用独立字段 rgBeat，并在下面的通用伤害里真的乘上去（上限 2 层 = 3 倍）。 */
    if (T & RG_T_RHYTHM) {
        if (p->rgBeat < 2.0f) p->rgBeat += 0.25f;
    }

    /* ---------- 通用兜底：任何肉鸽植物都至少有一次强化脉冲 ---------- */
    if (!(T & (RG_T_LASERCUT | RG_T_GAMBLEDMG | RG_T_GAMBLERISK |
               RG_T_PIXELCRUSH | RG_T_ASSASSIN | RG_T_CHAINBOOM |
               RG_T_RECOMBINE | RG_T_SELFDESTRUCT))) {
        float mult = 1.6f + 0.35f * (float)tier;
        if (T & RG_T_RHYTHM) mult *= (1.0f + p->rgBeat);   /* 节拍：最高 3 倍 */
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r) continue;
            if (z->x < cx - 6.0f || z->x > cx + R) continue;
            z->hp -= base * mult;
            z->flash = 0.10f;
            if (z->hp <= 0.0f) zombieDie(z, 0);
        }
    }

    /* ---------- 改写地块：能力在地形上留下持久痕迹 ----------
       这是"地形随能力变化"的落点 —— 打完一场，草坪就不再是原来的草坪。 */
    {
        int cc;
        float dur = 6.0f + 1.2f * (float)tier;
        int spread = (int)rgRangeMul(tier);          /* 高品质波及邻格 */
        for (cc = c - spread; cc <= c + spread; cc++) {
            if (cc < 0 || cc >= COLS) continue;
            if (T & (RG_T_CORRODE | RG_T_PLAGUE | RG_T_CURSE))
                terrainTileAdd(r, cc, TF_CORRODE, dur);
            if (T & (RG_T_WEBSLOW | RG_T_CONFUSE | RG_T_MEMESPREAD))
                terrainTileAdd(r, cc, TF_WEB, dur);
            if (T & (RG_T_TIMEFREEZE | RG_T_ICERESOURCE | RG_T_GRAVITY | RG_T_LULLABY))
                terrainTileAdd(r, cc, TF_ICE, dur);
            if (T & (RG_T_CHAINBOOM | RG_T_SELFDESTRUCT | RG_T_RECOMBINE))
                terrainTileAdd(r, cc, TF_SCORCH, dur + 2.0f);
            if (T & (RG_T_SHARESUN | RG_T_EATSUN | RG_T_AURABUFF |
                     RG_T_CHI | RG_T_LIFESTEAL | RG_T_FAKEHP | RG_T_DIGITRAIN))
                terrainTileAdd(r, cc, TF_BLOOM, dur);
        }
        /* 天气 / 黑洞这类大范围能力直接改写整行 */
        if (T & (RG_T_WEATHER | RG_T_BLACKHOLE | RG_T_PETALBARRAGE)) {
            for (cc = 0; cc < COLS; cc++)
                terrainTileAdd(r, cc, (T & RG_T_BLACKHOLE) ? TF_CORRODE : TF_ICE, dur);
        }
    }

    /* ====================================================================
       第二批机制（trait 位 48~63）
       --------------------------------------------------------------------
       统一约定：
         · 全部是**脉冲式**的，不引入新的伤害管线 —— 不碰僵尸结构、
           不改既有减伤公式，把"新增 16 个机制"的风险压到最低；
         · 需要状态的只有"叠加"类，由 Plant.rgStack / rgAux 承载；
         · 每个机制都带特效粒子，玩家能一眼看出这一株在干嘛。
       ==================================================================== */

    /* ---------- 破甲：走真伤通道，无视铁门/橄榄球的护甲减免 ---------- */
    if (T & RG_T_ARMORBREAK) {
        int hit = 0;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead) continue;
            if (z->x < cx - R || z->x > cx + R) continue;
            if ((float)(z->row - r) * (float)CELL_H > R) continue;
            z->hp -= base * 0.85f;
            z->flash = 0.10f;
            if (z->hp <= 0.0f) zombieDie(z, 0);
            hit = 1;
        }
        if (hit)
            addParticle(cx + 18.0f, py - 8.0f, 40.0f, -50.0f, 0.30f, 10,
                        RGB(180, 255, 120), 1);
    }

    /* ---------- 标记：锁定范围内最肉的一只集火 ---------- */
    if (T & RG_T_MARK) {
        Zombie *best = NULL;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead) continue;
            if (z->x < cx - R || z->x > cx + R) continue;
            if ((float)(z->row - r) * (float)CELL_H > R) continue;
            if (!best || z->hp > best->hp) best = z;
        }
        if (best) {
            best->hp -= base * 1.15f;
            best->flash = 0.14f;
            addParticle(best->x, best->y - 56.0f, 0.0f, -30.0f, 0.40f, 12,
                        RGB(255, 202, 92), 1);
            if (best->hp <= 0.0f) zombieDie(best, 0);
        }
    }

    /* ---------- 暴击：一次脉冲只掷一次骰子（攻速越高越容易吃到） ---------- */
    if (T & RG_T_CRIT) {
        float cm = ((rand() % 100) < 25) ? 3.0f : 1.0f;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead) continue;
            if (z->x < cx - R || z->x > cx + R) continue;
            if ((float)(z->row - r) * (float)CELL_H > R) continue;
            z->hp -= base * 0.30f * cm;
            z->flash = 0.08f;
            if (z->hp <= 0.0f) zombieDie(z, 0);
            if (cm > 2.0f)
                addParticle(z->x, z->y - 40.0f, 0.0f, -60.0f, 0.25f, 12,
                            RGB(255, 240, 120), 1);
        }
    }

    /* ---------- 击退 / 拉拽：直接改 x。僵尸向左进攻，
       所以"击退" = x 增大、"拉拽" = x 减小。
       ⚠️ 必须双向钳位，否则会把僵尸推进草坪深处或推出画面。 ---------- */
    if (T & (RG_T_KNOCKBACK | RG_T_PULL)) {
        float push = (T & RG_T_PULL) ? -46.0f : 62.0f;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r) continue;
            if (fabsf(z->x - cx) > R) continue;
            zombieSpecialMove(z, push);
            z->hp -= base * 0.55f;
            z->flash = 0.10f;
            if (z->x > VIEW_W + 30.0f) z->x = VIEW_W + 30.0f;
            if (z->hp <= 0.0f) zombieDie(z, 0);
        }
    }

    /* ---------- 召唤：没有独立"仆从"实体（那要动僵尸数组），
       改用**侧翼射出的追猎弹**代表仆从出击 —— 视觉上是小单位飞出去咬人，
       玩法上等价于额外火力，但零结构改动。 ---------- */
    if (T & RG_T_SUMMON) {
        int n = 2 + tier / 2;
        for (i = 0; i < n; i++)
            spawnPeaAt(cx - 8.0f, py - 24.0f + (float)i * 10.0f,
                       300.0f, -50.0f + 50.0f * (float)i,
                       base * 0.42f, r, p->type, 0);
    }

    /* ---------- 轨道轰炸：锁定本行僵尸最密的位置打一发 ---------- */
    if (T & RG_T_ORBITAL) {
        float bx = -1.0f;
        int   bcnt = 0;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            int cnt = 0, j;
            if (!z->active || z->dead || z->row != r) continue;
            for (j = 0; j < MAX_ZOMBIES; j++) {
                Zombie *q = &zombies[j];
                if (!q->active || q->dead || q->row != r) continue;
                if (fabsf(q->x - z->x) <= 110.0f) cnt++;
            }
            if (cnt > bcnt) { bcnt = cnt; bx = z->x; }
        }
        if (bx > 0.0f) {
            explode(bx, cellBaseY(r) - 8.0f, R * 0.85f, base * 1.35f, 0, r);
            addShake(3.0f, 0.10f);
        }
    }

    /* ---------- 处决：血量低于阈值直接带走。
       阈值随品级涨，但封顶 34% —— 否则高品级会把"斩杀线"抬到三分之一血
       以上，变成无脑清场，处决这个机制的取舍感就没了。 ---------- */
    if (T & RG_T_EXECUTE) {
        float thr = 0.18f + 0.03f * (float)tier;
        if (thr > 0.34f) thr = 0.34f;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead) continue;
            if (z->x < cx - R || z->x > cx + R) continue;
            if ((float)(z->row - r) * (float)CELL_H > R) continue;
            if (z->maxhp > 0.0f && z->hp / z->maxhp <= thr && !rgZombieBlocksInstantKill(z)) {
                addParticle(z->x, z->y - 48.0f, 0.0f, -44.0f, 0.45f, 16,
                            RGB(255, 92, 92), 1);
                z->hp = 0.0f;
                zombieDie(z, 0);
            }
        }
    }

    /* ---------- 护盾光环 / 时序回溯 / 净化：三个都是"照顾友军"的，
       统一按**格**遍历（plantAt），不用像素半径，免得和格边界对不齐。 ---------- */
    if (T & (RG_T_SHIELDALLY | RG_T_TIMEHEAL | RG_T_PURGE)) {
        int rad = (int)rgRangeMul(tier);
        for (rr = r - rad; rr <= r + rad; rr++) {
            int cc;
            if (rr < 0 || rr >= ROWS) continue;
            for (cc = c - rad; cc <= c + rad; cc++) {
                Plant *q;
                if (cc < 0 || cc >= COLS) continue;
                q = plantAt(rr, cc);
                if (!q || !q->alive) continue;
                if (T & RG_T_SHIELDALLY) {
                    if (q->shield < base * 2.4f) q->shield += base * 0.55f;
                }
                if (T & (RG_T_TIMEHEAL | RG_T_PURGE)) {
                    if (q->hp < q->maxhp) {
                        q->hp += q->maxhp * 0.05f;
                        if (q->hp > q->maxhp) q->hp = q->maxhp;
                    }
                }
            }
        }
        if (T & RG_T_PURGE) {                     /* 净化顺带灼烧范围内的僵尸 */
            for (i = 0; i < MAX_ZOMBIES; i++) {
                Zombie *z = &zombies[i];
                if (!z->active || z->dead) continue;
                if (z->x < cx - R || z->x > cx + R) continue;
                if ((float)(z->row - r) * (float)CELL_H > R) continue;
                z->hp -= base * 0.45f;
                if (z->hp <= 0.0f) zombieDie(z, 0);
            }
        }
    }

    /* ---------- 阳光光环：稳定额外产出（经济类辅助的立身之本） ---------- */
    if (T & RG_T_ENERGYGAIN) {
        spawnSunDrop(cx, by - 76.0f, by - 10.0f, 25 + 15 * tier);
    }

    if (T & RG_T_BOUNCE5) {
        /* 反弹次数由当前植物传给它发出的子弹，实际跳转在子弹命中后处理。 */
        gRgPeaBounce = 5;
    }
    if (T & RG_T_HIJACK) {
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead || z->row != r || fabsf(z->x - cx) > R) continue;
            z->hp -= base * 0.35f;
            z->flash = 0.08f;
            if (z->hp <= 0.0f) zombieDie(z, 0);
        }
    }
    if (T & RG_T_INFECTTURN) {
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            int j;
            if (!z->active || z->dead || z->row != r || fabsf(z->x - cx) > R) continue;
            for (j = 0; j < MAX_ZOMBIES; j++) {
                Zombie *q = &zombies[j];
                if (q == z || !q->active || q->dead || q->row != z->row) continue;
                if (fabsf(q->x - z->x) < 90.0f) { q->hp -= base * 0.22f; q->flash = 0.05f; }
            }
        }
    }
    if ((T & RG_T_FUSEPLANT) && !p->rgFused) {
        /* 真"吞噬"：邻居会**消失**（原来只是给它自己叠乘，邻居照样活着），
           而且只吞一次、有上限。
           原实现的问题（实测）：每脉冲 ×1.25 无封顶，5 个脉冲就 ×3.05，
           邻居却毫发无伤 —— 卡片上「吞噬相邻植物」是空的。 */
        Plant *nb = NULL;
        for (rr = r - 1; rr <= r + 1 && !nb; rr++) {
            int cc;
            if (rr < 0 || rr >= ROWS) continue;
            for (cc = c - 1; cc <= c + 1; cc++) {
                Plant *q;
                if (cc < 0 || cc >= COLS) continue;
                q = plantAt(rr, cc);
                if (q && q != p && q->alive) { nb = q; break; }
            }
        }
        if (nb) {
            float cap = p->rgBaseHp * 2.5f;
            p->rgFused = 1;
            p->fuseMul = 1.25f;          /* 只在出场时被读过，这里保留作为语义标记 */
            if (p->maxhp < cap) {
                p->maxhp *= 1.25f;
                if (p->maxhp > cap) p->maxhp = cap;
            }
            p->hp = p->maxhp;
            addParticle(cellCX(nb->col), cellBaseY(nb->row) - 30.0f,
                        0, -70.0f, 0.6f, 18, RGB(180, 120, 255), 1);
            removePlantAt(nb->row, nb->col);      /* 吞掉：邻居消失 */
        }
    }
    if (T & RG_T_DOUBLEBODY) {
        p->rgStack = 2;
        base *= 1.35f;
    }
    if (T & RG_T_GROWEAT) {
        /* 只在**确实吞掉过僵尸**时兑现一次成长（记账见 rgNotifyDevourNear），
           并且把上限钉在出场血量的 2.5 倍 —— 与僵尸侧 RZ_MUTATE 的成长封顶同一档。
           原实现的问题（实测）：不分是否吃过、每脉冲 ×1.06 且无上限，
           40 个脉冲即 10.3 倍，还每脉冲回满血 → 无敌植株。 */
        if (p->rgEatPend > 0) {
            float cap = p->rgBaseHp * 2.5f;
            p->rgEatPend--;
            if (p->rgStack < 5) p->rgStack++;
            if (p->maxhp < cap) {
                p->maxhp *= 1.06f;
                if (p->maxhp > cap) p->maxhp = cap;
            }
            p->hp = p->maxhp;
        }
    }

    /* ---------- 共享能力：把附近肉鸽植物的攻击特征复制到本株 ---------- */
    if (T & RG_T_COPYEAT) {
        for (i = 0; i < ROWS; i++) {
            int j;
            for (j = 0; j < COLS; j++) {
                Plant *q = plantAt(i, j);
                if (!q || q == p || !q->alive || q->rg <= 0) continue;
                if (fabsf(cellCX(j) - cx) > R || abs(i - r) > 1) continue;
                p->rgStack = q->rgStack > p->rgStack ? q->rgStack : p->rgStack;
                break;
            }
            if (p->rgStack > 0) break;
        }
    }

    /* ---------- 减速场：脉冲式持续刷新 slow 计时 ---------- */
    if (T & RG_T_SLOWFIELD) {
        float sd = 1.6f / rgRateMul(tier);
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *z = &zombies[i];
            if (!z->active || z->dead) continue;
            if (z->x < cx - R || z->x > cx + R) continue;
            if ((float)(z->row - r) * (float)CELL_H > R) continue;
            if (z->slow < sd) z->slow = sd;
            z->hp -= base * 0.25f;
            if (z->hp <= 0.0f) zombieDie(z, 0);
        }
    }

    /* ---------- 过载：叠层（"脉冲更密"的实现在函数开头的 period 计算里） ---------- */
    if (T & RG_T_OVERLOAD) {
        if (p->rgStack < 5) p->rgStack++;
        p->rgAux = 4.0f;
    }

    gRgAtkMul = (float)keep / 1000.0f;        /* 还原，交给基座 switch 用 */
}

/* ======================================================================== */
/*              肉鸽僵尸能力派发：50 种负面能力的统一出口                       */
/* ======================================================================== */
/* 僵尸自愈的总量上限（单次脉冲，按最大血的比例）
   ------------------------------------------------------------------------
   ★ 为什么必须有它 —— 这是"最后两只僵尸打不死"的根因：

   单个僵尸最多能同时挂 **4 个自愈源**，而且都是"按脉冲"结算的：
       保命组(FAKEHP/CHISHIELD/DODGE30/HEALBACK/REFLECT/ENTANGLE/FUSEZOMBIE) 10%
       RZ3_SHIELD（护盾）   12%
       RZ3_REGEN（自愈）     9%
       RZ3_TAUNT（嘲讽）     5%          合计最多 36% 基础血 / 脉冲
   而脉冲周期 = `4.6 - 0.42*tier` 秒（传奇档 2.5 秒）：
       → 最快 36% / 2.5s ≈ **14.4% 最大血 / 秒**
   这个数字高于兜底防卡死的起始 7%/s，于是两者在 75~95 秒之间打平 ——
   玩家看到的就是"这两只怎么打都打不死，关卡结算不了"。
   （实测：尸王在 200 DPS 的弱火力下要 75.3 秒才死；0 火力要 88.1 秒。）

   现在把"一次脉冲内这只僵尸能回多少血"封顶，四个源共享同一份额度。
   取 5% 的理由：它换算成速率是 2%/s（传奇档），
   明显低于兜底伤害（9%/s 起），两者不会再打平；
   同时 5% 仍然是有感的续航，不是把自愈砍没了。 */
#define ZOMBIE_HEAL_CAP_PER_TICK  0.05f

static void zombieHealBudget(Zombie *z, float *budget, float want)
{
    if (!z || want <= 0.0f || !budget || *budget <= 0.0f) return;
    if (want > *budget) want = *budget;
    *budget -= want;
    z->hp += want;
    if (z->hp > z->maxhp) z->hp = z->maxhp;
}

/* 与植物侧同构：基座（ZT_*）负责贴图与移动/啃食/砸击，
   这里按能力位补"特异功能"。脉冲周期由品质推导，越稀有越频繁。               */
static void rgZombieTraitTick(Zombie *z, float dt)
{
    const RgZombieDef *d;
    unsigned long long T;
    int tier, i, col;
    float period, dmg, healBudget;
    Plant *tg;

    if (z->rg <= 0 || z->rg > RG_ZOMBIE_N) return;
    d = &rgZombies[z->rg - 1];
    T = d->traits;
    if (!T) return;
    tier = d->tier;

    /* 潜地是两阶段动作：先在目标格显示 1.35 秒尘土预警，僵尸本体原地停住；
       预警结束才移动。蓄力期间被击杀就自然取消，玩家因此有真实反制窗口。 */
    if (z->burrowPending) {
        z->burrowT -= dt;
        if (z->rooted < 0.12f) z->rooted = 0.12f;
        if (z->burrowT <= 0.0f) {
            zombieSpecialMove(z, z->burrowTargetX - z->x);
            z->burrowPending = 0;
            z->vaulted = 1;
            addParticle(z->x, cellBaseY(z->row) - 10.0f, 0, -90.0f, 0.7f, 22,
                        RGB(176, 142, 92), 1);
            addShake(3.5f, 0.18f);
        }
        return;
    }

    z->rgt -= dt;
    period = 4.6f - 0.42f * (float)tier;      /* 普通 4.6s → 传奇 2.5s */
    if (z->rgt > 0.0f) return;
    z->rgt = period;

    /* 本回合的自愈额度：四类回血源共享它，用完为止（见 ZOMBIE_HEAL_CAP_PER_TICK） */
    healBudget = z->maxhp * ZOMBIE_HEAL_CAP_PER_TICK;

    dmg = 260.0f * rgZombieHpMul(tier);

    /* ---------- 每回合只移动一格：步进型僵尸不再依赖随机 tick */
    if (T & RZ_STEPMOVE) {
        zombieSpecialMove(z, -18.0f);
    }

    /* ---------- 前方的那株植物（多数能力的作用目标） ---------- */
    col = (int)floor((z->x - 24.0f - LAWN_X) / (float)CELL_W);
    tg  = (col >= 0 && col < COLS) ? plantAt(z->row, col) : NULL;

    /* ---------- 分身 / 叠加：量子叠加、镜像僵尸 ----------
       ⚠️ 只有【本体】（第 0 代）能分身，而且场上有数量上限。
       原来没这两个限制，分身带着同样的能力，一代传一代形成指数增长，
       zombiesAlive() 永远不为 0 → 波次清不掉 → 表现成"通关之前卡死"。 */
    if (z->gen == 0 && (T & (RZ_TRIPLEBODY | RZ_MIRROR3)) &&
        (rand() % 100) < 18 && zombiesAlive() < 36) {
        int rr = rndi(0, ROWS);
        if (rr != z->row) {
            gPendingRgZ  = z->rg - 1;
            gPendingGenZ = 1;                    /* 克隆体禁止再分身 */
            spawnZombieAt(z->type, rr, z->x + rnd(-30.0f, 30.0f));
            gPendingRgZ  = -1;
            gPendingGenZ = 0;
        }
    }

    /* ---------- 传染加速：病毒扩散 ---------- */
    if (T & RZ_INFECTSPD) {
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *q = &zombies[i];
            if (!q->active || q->dead || q == z) continue;
            if (fabsf(q->x - z->x) > 90.0f || q->row != z->row) continue;
            q->speed = q->speed * 1.06f;
            if (q->speed > 60.0f) q->speed = 60.0f;
        }
    }

    /* ---------- 重力井 / 音乐节拍：加速同行僵尸 ---------- */
    if (T & (RZ_GRAVITYWELL | RZ_RHYTHMDASH)) {
        zombieSpecialMove(z, -26.0f);         /* 自己先冲一步 */
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *q = &zombies[i];
            if (!q->active || q->dead || q == z) continue;
            if (q->row != z->row) continue;
            if (fabsf(q->x - z->x) > 130.0f) continue;
            zombieSpecialMove(q, -14.0f);
        }
    }

    /* ---------- 随机传送：随机传送僵尸 ---------- */
    if (T & RZ_BLINK) {
        zombieSpecialMove(z, rnd(-70.0f, 90.0f));
    }

    /* ---------- 自愈 / 假血 / 护盾 / 概率无敌 ----------
       ⚠️ 必须有上限。原来写的是"每脉冲回 7% 最大血"，僵尸会永远打不死；
       它还是最后一只时，波次永远清不掉 —— 这就是"通关之前卡死"的来源之一。
       现在：只在血量低于 70% 时回，且回的是【出场血量 basehp 的固定比例】，
       不随 maxhp 一起膨胀。 */
    if (T & (RZ_FAKEHP | RZ_CHISHIELD | RZ2_DODGE30 | RZ_HEALBACK |
             RZ_REFLECT | RZ_ENTANGLE | RZ_FUSEZOMBIE)) {
        if (z->hp < z->maxhp * 0.70f)
            zombieHealBudget(z, &healBudget, z->basehp * 0.10f);
    }

    /* ---------- 变异 / 赌徒血量：数值波动 ---------- */
    if (T & RZ_MUTATE) {
        if (z->maxhp < z->basehp * 2.5f) {           /* 成长封顶 2.5 倍 */
            float ratio = (z->maxhp > 0.0f) ? z->hp / z->maxhp : 1.0f;
            z->maxhp *= 1.12f;
            z->hp = z->maxhp * ratio;                /* 保持血量比例，【不回满】 */
        }
    }
    if (T & RZ_HPGAMBLE) {
        /* 原来直接 z->hp = maxhp * rnd(0.6,2.6) —— 每脉冲把已造成的伤害全抹掉。
           改成随机游走，且有下界，不会变成刷血。 */
        z->hp = CLAMP(z->hp * rnd(0.88f, 1.14f), z->basehp * 0.35f, z->maxhp);
    }

    /* ---------- 自毁：概率坩埚僵尸 ---------- */
    if (T & RZ_SELFGAMBLE) {
        if ((rand() % 100) < 50) { z->hp = 0.0f; }   /* 自毁 */
        else if (z->maxhp < z->basehp * 2.0f) {
            float ratio = (z->maxhp > 0.0f) ? z->hp / z->maxhp : 1.0f;
            z->maxhp *= 1.25f;
            z->hp = z->maxhp * ratio;
        }
        return;
    }

    /* ---------- 生长：贪吃蛇僵尸 ---------- */
    if (T & RZ_GROWEAT) {
        if (z->maxhp < z->basehp * 2.5f) {
            float ratio = (z->maxhp > 0.0f) ? z->hp / z->maxhp : 1.0f;
            z->maxhp *= 1.10f;
            z->hp = z->maxhp * ratio;                /* 只变厚，不回血 */
        }
    }

    /* ---------- 光束 / 弹幕 / 踢击：远程拆植物 ---------- */
    if (T & (RZ_LASERCUT | RZ_KNIFEBARRAGE | RZ2_BOMBBARRAGE |
             RZ2_COMBOKICK | RZ2_BLACKHOLE | RZ_ASSASSIN)) {
        Plant *q = plantFrontmost(z->row);
        if (q && q->hp > 0.0f && q->col < (int)((z->x - LAWN_X) / CELL_W)) {
            float hit;
            /* 所有远程拆植物能力统一打同行最前排，绝不越过它点名后排。
               单次最多削 45%，保证玩家至少有一次明确的受击反馈。
               ⚠️ 伤害部分受 ZOMBIE_REMOTE_PLANT_DMG 总开关控制（当前为 0 = 关闭），
                  关掉后只保留粒子表现，植物不掉血。 */
            hit = dmg;
            if (hit > q->maxhp * 0.45f) hit = q->maxhp * 0.45f;
            if (ZOMBIE_REMOTE_PLANT_DMG) {
                q->hp -= hit;
                q->eaten = 0.6f;
                if (q->hp <= 0.0f) removePlantAt(q->row, q->col);
            } else {
                q->timer += ZOMBIE_SUPPRESS_TIMER;   /* 改为压制节奏：植物不掉血 */
                q->eaten = 0.6f;
            }
            addParticle(cellCX(q->col), cellBaseY(z->row) - 40.0f,
                        rnd(-40, 40), rnd(-90, -20), 0.4f, rnd(3, 6),
                        RGB(255, 120, 90), 1);
        }
    }

    /* ---------- 中毒 / 噪音 / 停产：持续削弱前方植物 ---------- */
    if (T & (RZ2_POISON | RZ_NOISESLOW | RZ2_HACKSUN | RZ2_WEATHER)) {
        if (tg && plantIsFrontmost(tg)) {
            if (ZOMBIE_REMOTE_PLANT_DMG) {
                tg->hp -= dmg * 0.55f;
                if (tg->hp <= 0.0f) removePlantAt(z->row, col);
            } else {
                /* 中毒 / 噪音 / 停产：一律转为"压制节奏"（射速与产出变慢） */
                tg->timer += ZOMBIE_SUPPRESS_TIMER;
            }
            tg->eaten = 0.5f;
        }
    }

    /* ---------- 逆向时间：拔掉一株植物 ---------- */
    if (T & RZ2_REWINDPLANT) {
        int rr = rndi(0, ROWS), cc = rndi(0, COLS);
        Plant *q = plantAt(rr, cc);
        if (q && q->alive) {
            /* 旧版直接 removePlantAt，玩家只会看到植物凭空消失。
               现在回溯会延后技能（保留，属"干扰"不是"伤害"）；
               削 28% 最大生命那一段受 ZOMBIE_REMOTE_PLANT_DMG 控制（当前关闭）。 */
            if (ZOMBIE_REMOTE_PLANT_DMG) {
                q->hp -= q->maxhp * 0.28f;
                if (q->hp < 1.0f) q->hp = 1.0f;
            }
            q->timer += 1.1f;
            q->eaten = 1.0f;
            addParticle(cellCX(cc), cellBaseY(rr) - 38.0f, 0, -85.0f, 0.65f, 20,
                        RGB(126, 190, 255), 1);
            addShake(2.0f, 0.12f);
        }
    }

    /* ---------- 造墙：迷宫 / 代码墙 ---------- */
    if (T & (RZ_MAZEWALL | RZ_CODEWALL | RZ2_MAZEBACK)) {
        int cc = (int)floor((z->x + 20.0f - LAWN_X) / (float)CELL_W);
        if (cc >= 0 && cc < COLS && !plantAt(z->row, cc)) {
            gPendingRg = -1;
            plantIt(z->row, cc, PT_WALLNUT);
        }
    }

    /* ---------- 掠夺：吸阳光 / 贪婪 ---------- */
    if (T & (RZ_DRAINSUN | RZ_GREEDSUN)) {
        if (gSun > 0) gSun -= 15;
        for (i = 0; i < MAX_SUNS; i++) {
            SunDrop *s = &suns[i];
            if (!s->active) continue;
            if (fabsf(s->x - z->x) < 120.0f) s->active = 0;
        }
    }

    /* ---------- 负面卡：加重卡组冷却 ---------- */
    if (T & RZ2_NEGCARD) {
        int c2;
        for (c2 = 0; c2 < PT_COUNT; c2++)
            if (cardCD[c2] > 0.0f) cardCD[c2] += 1.5f;
    }

    /* ---------- 劫持植物：黑客僵尸 ---------- */
    if (T & (RZ_HIJACKPLANT | RZ2_HACKSUN)) {
        if (tg && plantIsFrontmost(tg)) {
            if (ZOMBIE_REMOTE_PLANT_DMG) tg->hp -= dmg * 0.8f;
            else                         tg->timer += ZOMBIE_SUPPRESS_TIMER * 1.4f;
        }
    }

    /* ---------- 落债：贷款僵尸死亡掉落（用掉落阳光近似） ---------- */
    if (T & RZ_LOANHP) {
        if ((rand() % 100) < 20) gSun -= 10;
        if (gSun < 0) gSun = 0;
    }

    /* ---------- 僵尸也在改写地形：走过的路会留下焦土 / 腐蚀 / 冰面 / 蛛网 ---------- */
    {
        int cc = (int)floor((z->x - LAWN_X) / (float)CELL_W);
        float dur = 5.0f + 0.9f * (float)tier;
        if (T & (RZ2_POISON | RZ_INFECTSPD))            terrainTileAdd(z->row, cc, TF_CORRODE, dur);
        if (T & RZ_WEBSLOW)                             terrainTileAdd(z->row, cc, TF_WEB, dur);
        if (T & (RZ2_WEATHER | RZ_GRAVITYWELL))          terrainTileAdd(z->row, cc, TF_ICE, dur);
        if (T & (RZ_LASERCUT | RZ2_BOMBBARRAGE | RZ2_BLACKHOLE | RZ_SELFGAMBLE))
            terrainTileAdd(z->row, cc, TF_SCORCH, dur + 1.5f);
        if (T & RZ_GRAVITYWELL)                         terrainTileAdd(z->row, cc, TF_ICE, dur);
    }

    /* ====================================================================
       第三批机制（bit 42~61）：20 个
       --------------------------------------------------------------------
       全部复用现有的 x / row / hp / gen / vaulted 字段 —— 不新增僵尸状态。
       ⚠️ 两个刻意的"降级实现"，原因写在各自注释里：
         · 嘲讽没有全局仇恨系统，改成"给自己回血"来体现"替后排挨打"；
         · 冰噬没有植物冰冻字段，用"拉长 timer + 掉血"近似攻速下降。
       宁可给一个语义稍弱但**真的生效**的效果，也不要写一个永远为假的字段。
       ==================================================================== */

    /* ---------- 飞行：快速掠进，但仍停在前排 ---------- */
    if (T & RZ3_FLY) {
        col = (int)floor((z->x - LAWN_X) / (float)CELL_W);
        for (i = col - 1; i <= col; i++) {
            Plant *q;
            if (i < 0 || i >= COLS) continue;
            q = plantAt(z->row, i);
            if (q && q->alive) { zombieSpecialMove(z, -34.0f); break; }
        }
    }

    /* ---------- 跳跃：快速跃进到第一株植物前 ---------- */
    if ((T & RZ3_JUMP) && !z->vaulted) {
        Plant *q = NULL;
        col = (int)floor((z->x - LAWN_X) / (float)CELL_W);
        if (col >= 0 && col < COLS) q = plantAt(z->row, col);
        if (q && q->alive) {
            zombieSpecialMove(z, -CELL_W * 1.15f);
            z->vaulted = 1;
            addParticle(z->x, z->y - 40.0f, 0, -80.0f, 0.4f, 12, RGB(210, 220, 240), 1);
        }
    }

    /* ---------- 投掷：隔着防线砸最前面的植物 ---------- */
    if (T & RZ3_THROW) {
        Plant *front = plantFrontmost(z->row);
        if (front && front->col < (int)((z->x - LAWN_X) / CELL_W)) {
            if (ZOMBIE_REMOTE_PLANT_DMG) {
                front->hp -= dmg * 0.55f;
                if (front->hp <= 0.0f) removePlantAt(front->row, front->col);
            } else {
                front->timer += ZOMBIE_SUPPRESS_TIMER;   /* 砸中：节奏被打乱 */
            }
            /* 投掷动作的表现保留：能看到东西砸过去，只是不再掉血 */
            addParticle(cellCX(front->col), cellBaseY(front->row) - 40.0f,
                        0, -60.0f, 0.4f, 12, RGB(180, 160, 130), 1);
        }
    }

    /* ---------- 召唤：定期从地里拽出小僵尸 ---------- */
    if (T & RZ3_SUMMON) {
        /* spawnQN 是“尚未入场的波次队列”，不是空闲实体槽；拿它限流会让
           后期排队怪多时召唤完全失效。用在场数量限流，同时把上限压到 72，
           防止多个召唤者把 160 个实体槽灌满并拖垮后期帧率。 */
        if (zombiesAlive() < 72)
            spawnZombieAt(ZT_NORMAL, z->row, VIEW_W + 30.0f);
    }

    /* ---------- 闪现：一段一段往前瞬移 ---------- */
    if (T & RZ3_BLINK) {
        zombieSpecialMove(z, -(90.0f + 20.0f * (float)tier));
        addParticle(z->x, z->y - 30.0f, 0, -50.0f, 0.35f, 14, RGB(180, 120, 255), 1);
    }

    /* ---------- 冲锋：低头猛冲一段 ---------- */
    if (T & RZ3_DASH) {
        zombieSpecialMove(z, -(52.0f + 14.0f * (float)tier));
        addParticle(z->x + 40.0f, z->y - 24.0f, -60.0f, 0, 0.25f, 10, RGB(240, 220, 180), 1);
    }

    /* ---------- 护盾：会自行修复的吸收层 ---------- */
    if ((T & RZ3_SHIELD) && z->hp < z->maxhp)
        zombieHealBudget(z, &healBudget, z->basehp * 0.12f);

    /* ---------- 反弹：把身边的子弹打回去 ---------- */
    if (T & RZ3_DEFLECT) {
        for (i = 0; i < MAX_PEAS; i++) {
            Pea *pe = &peas[i];
            if (!pe->active || pe->row != z->row) continue;
            if (fabsf(pe->x - z->x) > 60.0f) continue;
            pe->vx  = -fabsf(pe->vx);      /* 掉头飞回去 */
            pe->src = -1;                  /* 不再是植物的子弹，避免自伤判定 */
            addParticle(pe->x, pe->y, 0, -40.0f, 0.25f, 8, RGB(220, 230, 255), 1);
        }
    }

    /* ---------- 分裂：濒死时裂出两只小僵尸 ---------- */
    if ((T & RZ3_SPLIT) && z->hp < z->basehp * 0.45f && z->gen == 0) {
        z->gen = 1;                        /* 只分一次 */
        if (zombiesAlive() < MAX_ZOMBIES - 2) {
            spawnZombieAt(ZT_NORMAL, z->row, z->x + 40.0f);
            spawnZombieAt(ZT_NORMAL, z->row, z->x - 40.0f);
        }
        z->hp = z->basehp * 0.45f;
        addParticle(z->x, z->y - 40.0f, 0, -60.0f, 0.5f, 16, RGB(150, 255, 160), 1);
    }

    /* ---------- 自愈：有上限的持续回血（不设上限会永远打不死） ---------- */
    if ((T & RZ3_REGEN) && z->hp < z->maxhp * 0.85f)
        zombieHealBudget(z, &healBudget, z->basehp * 0.09f);

    /* ---------- 狂暴：越接近死亡跑得越疯 ---------- */
    if (T & RZ3_ENRAGE) {
        float lost = (z->maxhp > 1.0f) ? (1.0f - z->hp / z->maxhp) : 0.0f;
        lost = CLAMP(lost, 0.0f, 1.0f);
        zombieSpecialMove(z, -26.0f * lost * (1.0f + 0.5f * (float)tier));
    }

    /* ---------- 嘲讽：替后排挨打 ----------
       ★ 这个项目没有全局仇恨/目标系统，伪造一个代价太大且容易出 bug。
       改成"自己回血"来体现它替别人扛伤害 —— 语义弱一点，但真的生效。 */
    if ((T & RZ3_TAUNT) && z->hp < z->maxhp)
        zombieHealBudget(z, &healBudget, z->basehp * 0.05f);

    /* ---------- 毒云：走过的路留下毒雾，持续烧附近植物 ---------- */
    if (T & RZ3_POISONCLOUD) {
        col = (int)floor((z->x - LAWN_X) / (float)CELL_W);
        for (i = col - 1; i <= col + 1; i++) {
            Plant *q;
            if (i < 0 || i >= COLS) continue;
            q = plantAt(z->row, i);
            if (!q || !plantIsFrontmost(q)) continue;
            if (ZOMBIE_REMOTE_PLANT_DMG) {
                q->hp -= dmg * 0.22f;
                if (q->hp <= 0.0f) removePlantAt(q->row, q->col);
            } else {
                q->timer += ZOMBIE_SUPPRESS_TIMER * 0.7f;   /* 毒雾：持续拖慢 */
            }
        }
        terrainTileAdd(z->row, col, TF_CORRODE, 6.0f);   /* 地面毒雾保留 */
    }

    /* ---------- 冰噬：啃谁冻谁 ----------
       ★ 植物没有"被冻结"字段，用"拉长 timer（攻速下降）+ 掉血"近似，
         同时在地面留冰面 —— 比新加一个永远没人读的字段可靠。 */
    if (T & RZ3_FREEZEBITE) {
        col = (int)floor((z->x - LAWN_X) / (float)CELL_W);
        if (col >= 0 && col < COLS) {
            Plant *q = plantAt(z->row, col);
            if (q && plantIsFrontmost(q)) {
                q->timer += 0.35f;
                q->hp    -= dmg * 0.18f;
                terrainTileAdd(z->row, col, TF_ICE, 5.0f);
                if (q->hp <= 0.0f) removePlantAt(q->row, q->col);
            }
        }
    }

    /* ---------- 偷阳光：边啃边把阳光塞进自己兜里 ---------- */
    if (T & RZ3_STEALSUN) {
        col = (int)floor((z->x - LAWN_X) / (float)CELL_W);
        if (col >= 0 && col < COLS) {
            Plant *q = plantAt(z->row, col);
            if (q && plantIsFrontmost(q)) {
                gSun -= 8 + 4 * tier;
                if (gSun < 0) gSun = 0;
                addParticle(z->x, z->y - 50.0f, 0, -70.0f, 0.4f, 12, RGB(255, 220, 120), 1);
            }
        }
    }

    /* ---------- 速食：三倍速啃食，坚果墙在它面前像纸 ---------- */
    if (T & RZ3_DEVOUR) {
        col = (int)floor((z->x - LAWN_X) / (float)CELL_W);
        if (col >= 0 && col < COLS) {
            Plant *q = plantAt(z->row, col);
            if (q && plantIsFrontmost(q)) {
                q->hp -= dmg * 0.85f;
                z->eatBob += 0.3f;
                if (q->hp <= 0.0f) removePlantAt(q->row, q->col);
            }
        }
    }

    /* ---------- 复生：这里**不再需要**任何东西 ----------
       复生必须在"真的死亡"那一刻结算，所以实现在 zombieDie() 里（见该函数
       开头对 RZ3_REBORN 的处理）。这里原本留了一个空 if + 一句
       "由 zombieDie() 统一处理"的注释，但 zombieDie() 里**根本没有这段代码** ——
       结果是卡片上写的"死亡后满血复活一次"从未生效（静态审计会误判成"已接通"）。
       保留这句注释是为了说明"为什么这里故意是空的"。 */

    /* ---------- 磁力：把前排植物硬拽过来 ---------- */
    if (T & RZ3_MAGNET) {
        Plant *front = plantFrontmost(z->row);
        col = (int)floor((z->x - LAWN_X) / (float)CELL_W);
        if (front && front->col < col) {
            if (ZOMBIE_REMOTE_PLANT_DMG) {
                front->hp -= dmg * 0.35f;
                if (front->hp <= 0.0f) removePlantAt(front->row, front->col);
            } else {
                /* 植物是格子制的、没有像素坐标，拽不动；改成磁场压制节奏 */
                front->timer += ZOMBIE_SUPPRESS_TIMER;
            }
            /* 磁场特效保留 */
            addParticle(cellCX(front->col), cellBaseY(front->row) - 30.0f,
                        40.0f, 0, 0.35f, 12, RGB(150, 200, 255), 1);
        }
    }

    /* ---------- 钻地：快速接近前排，但不再从后排冒出来 ---------- */
    if ((T & RZ3_BURROW) && !z->vaulted) {
        int frontCol = plantFrontmostCol(z->row);
        if (frontCol >= 0 && frontCol < (int)((z->x - LAWN_X) / CELL_W)) {
            int curCol = (int)floor((z->x - LAWN_X) / CELL_W);
            int targetCol = frontCol;
            /* 一次最多前进三列，并且目标就是当前最前排植物的右侧。
               潜地仍能快速接战，但不会越过前排直接清后排。 */
            if (targetCol < curCol - 3) targetCol = curCol - 3;
            if (targetCol < curCol - 1) {
                z->burrowTargetX = (float)LAWN_X + (float)(targetCol + 1) * CELL_W + 23.0f;
                z->burrowT = 1.35f;
                z->burrowPending = 1;
                z->rooted = 1.35f;
                addParticle(z->burrowTargetX, cellBaseY(z->row) - 8.0f,
                            0, -45.0f, 1.1f, 24, RGB(154, 122, 78), 1);
            } else {
                z->vaulted = 1; /* 已经没有值得钻的距离，别每个脉冲重复尝试 */
            }
        }
    }

    /* ---------- 光环：强化周围僵尸（加速 + 缓回） ---------- */
    if (T & RZ3_AURA) {
        for (i = 0; i < MAX_ZOMBIES; i++) {
            Zombie *q = &zombies[i];
            if (!q->active || q->dead || q == z) continue;
            if (q->row != z->row || fabsf(q->x - z->x) > 260.0f) continue;
            zombieSpecialMove(q, -6.0f);
            if (q->hp < q->maxhp) {
                /* 光环的回血算在**被治疗者**头上，同样封顶 ——
                   否则两只光环僵尸同排就能互相刷血，又回到"打不死"。 */
                float qb = q->maxhp * ZOMBIE_HEAL_CAP_PER_TICK;
                zombieHealBudget(q, &qb, q->basehp * 0.04f);
            }
        }
    }
}

/* ==================== 始祖威压：只有始祖才有的全局被动 ====================
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

static void updateGame(float dt)
{
    int i, r, c, sl;

    /* 一帧只统计一次战斗密度。后期只收敛装饰性特效，逻辑单位与 2× 世界层
       都保持不变；高负载结束后会自动恢复完整拖尾和 500 粒子。 */
    {
        int zc = 0, pc = 0, newLimit, newSteps;
        for (i = 0; i < MAX_ZOMBIES; i++)
            if (zombies[i].active && !zombies[i].dead) zc++;
        for (i = 0; i < MAX_PEAS; i++)
            if (peas[i].active) pc++;
        if (zc > 80 || pc > 130) { newLimit = 180; newSteps = 0; }
        else if (zc > 55 || pc > 90) { newLimit = 340; newSteps = 2; }
        else { newLimit = MAX_PARTS; newSteps = 4; }
        if (newLimit < gFxParticleLimit) {
            for (i = newLimit; i < gFxParticleLimit; i++) parts[i].active = 0;
        }
        gFxParticleLimit = newLimit;
        gFxTrailSteps = newSteps;
        if (gFxParticleCursor >= gFxParticleLimit) gFxParticleCursor = 0;
    }

    gElapsed += dt;
    gTime    += dt;
    bgmTick();      /* 维持 BGM 循环：waveaudio 不支持 repeat，必须自己续播 */
    weatherUpdate(dt);
    if (shakeT > 0)   { shakeT -= dt;  if (shakeT <= 0) shakeMag = 0; }
    if (flashRed > 0)   flashRed -= dt;

    /* ------------------------- 供电断电倒计时 -------------------------
       关 9 的短路僵尸会让整行断电。「断电期间该行植物不工作」的判定
       在植物开火处（见该处的 newLevelRowPowered）。这里只推进计时器。 */
    for (i = 0; i < ROWS; i++)
        if (gPowerCutRow[i] > 0.0f) gPowerCutRow[i] -= dt;

    /* ------------------------- 卡片冷却 ------------------------- */
    for (i = 0; i < PT_COUNT; i++) {
        if (cardCD[i] > 0) { cardCD[i] -= dt; if (cardCD[i] < 0) cardCD[i] = 0; }
    }
    /* 肉鸽卡的再种冷却 */
    for (i = 0; i < RG_MAX_SLOTS; i++) {
        if (rgCardCD[i] > 0) { rgCardCD[i] -= dt; if (rgCardCD[i] < 0) rgCardCD[i] = 0; }
    }

    /* 动态地形 + 事件主题衰减（含环境粒子与地块对僵尸的持续影响） */
    terrainUpdate(dt);
    primordialAuraUpdate(dt);      /* 始祖威压：场上存在始祖时的全局被动 */

    /* ------------------------- 天空阳光 ------------------------- */
    skySunTimer -= dt;
    if (skySunTimer <= 0) {
        spawnSunDrop(rnd(LAWN_X + 50, LAWN_X + COLS * CELL_W - 50), LAWN_Y - 40,
                     rnd(LAWN_Y + 70, LAWN_Y + ROWS * CELL_H - 40), 25);
        skySunTimer = rnd(6.0f, 10.0f) * gSkySunMul * levelDefs[gCurLevel].sunMul;
    }

    newLevelUpdate(dt);      /* 新五关的场地规则推进（裂隙/迷雾/阶段/陨石） */
    traitsRecalc();          /* 羁绊已移除，保留空实现 */
    hoverUpdate();           /* 鼠标悬停提示 */

    /* 小推车存活数（背水遗物要看） */
    { int mv = 0; for (i = 0; i < ROWS; i++) if (mowers[i].active) mv++;
      gMowerAlive = mv; }

    /* ------------------------- 遗物：持续效果 ------------------------- */
    if (gState == ST_PLAY) {
        /* 按时间涨，不是按帧 —— 原来写成每帧 +1，60fps 下 1.67 秒就满，
           等于全屏雷击每 1.67 秒来一次，而且开局就有。
           注意 gFury 必须是 float：它以前是 int，gFury += dt 每次都把 0.0167
           截断成 0，愤怒条实际上永远停在 0，整套雷击从未触发过。 */
        gFury += dt * gFuryRate;
        if (gFury >= gFuryMax) {
            gFuryReady = 1;
            /* 没有 R_FURY：满了立刻自动放（基础玩法）。
               有 R_FURY：攒着，由玩家按 E 挑时机引爆；攒到 1.6 倍还没按就自动放，
               免得"忘了按"变成纯粹的收益损失。 */
            if (!HAS(R_FURY) || gFury >= gFuryMax * 1.6f) furyBlast(0);
        }
        if (gFuryT > 0.0f) {
            gFuryT -= dt;
            if (gFuryT < 0.0f) gFuryT = 0.0f;
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
        sfxPlay(L"win");
        gState = ST_WIN;
        return;
    }
    if (gWave < levelClearWaves(gCurLevel)) {
        waveTimer -= dt;
        if (waveTimer <= 0.0f) {
            /* 【肉鸽规则】每一波开波前都弹一次三选一，三张都是免费奖励。
               通关与否主要取决于这 20 波里随机到的植物质量 —— 这就是设计意图。 */
            if (gRgDraftWave != gWave) {
                gRgDraftWave = gWave;
                rgOpenDraft(gWave);
                if (gState == ST_DRAFT) return;      /* 已弹出：等玩家选完再开波 */
            }
            /* 每波结束时结算贷款。植物仍在场时扣款；已被吃掉的贷款不再重复追缴。 */
            if (zombiesAlive() == 0 && spawnQN == 0) {
                for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
                    Plant *q = plantAt(r, c);
                    if (q && q->alive && q->rgDebt > 0) {
                        gSun -= q->rgDebt;
                        if (gSun < 0) gSun = 0;
                        q->rgDebt = 0;
                    }
                }
            }
            startWave(gWave);
        } else if (gWave > 0 && zombiesAlive() == 0 && spawnQN == 0 && waveTimer > 2.0f) {
            waveTimer = 2.0f;      /* 提前清场则加速下一波 */
        }
    } else if (spawnQN == 0 && zombiesAlive() == 0) {
    /* 通关结算：贷款植物先偿还阳光，避免只拿翻倍生命不承担代价。 */
        for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
            Plant *q = plantAt(r, c);
            if (q && q->alive && q->rgDebt > 0) {
                gSun -= q->rgDebt;
                if (gSun < 0) gSun = 0;
                q->rgDebt = 0;
            }
        }
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
        /* 通关奖励：先给一次「神级植物 / 遗物」三选一（这张表只有 openDraft 能出），
           选完再进结算画面。gPendingWin 让 ST_DRAFT 的点击处理知道该跳到 ST_WIN。 */
        if (!gWinDrafted) {
            gWinDrafted = 1;
            openDraft();
            if (gState == ST_DRAFT) { gPendingWin = 1; return; }
        }
        gState = ST_WIN;
        return;
    }

    /* ------------------------- 出怪队列 ------------------------- */
    for (i = 0; i < spawnQN; ) {
        if (spawnQ[i].t <= gTime) {
            gPendingRgZ = spawnQ[i].rg - 1;      /* +1 编码还原；0 -> -1 = 普通 */
            spawnZombie(spawnQ[i].type, spawnQ[i].row);
            gPendingRgZ = -1;
            spawnQ[i] = spawnQ[--spawnQN];
        } else i++;
    }

    /* ------------------------- 植物 ------------------------- */
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) for (sl = 0; sl < 2; sl++) {
        Plant *p = (sl == 0) ? &grid[r][c] : &grid2[r][c];
        if (!p->alive) continue;
        if (p->row != r || p->col != c) continue;   /* 多格植物只跑一次 */
        if (p->rg > 0 && (rgPlantTraitsOf(p) & RG_T_STEPMOVE)) {
            p->rgMoveT += dt;
            if (p->rgMoveT >= 3.2f) {
                int nc = p->col + 1;
                if (nc < COLS && cellLegal(p->row, nc, p->w, p->hh) && !plantAt(p->row, nc)) {
                    Plant tmp = *p;
                    removePlantAt(p->row, p->col);
                    gPendingRg = tmp.rg - 1;
                    plantIt(tmp.row, nc, tmp.type);
                    { Plant *m = plantAt(tmp.row, nc); if (m) { m->rgMoveT = 0.0f; m->rgStack = tmp.rgStack; } }
                }
                p->rgMoveT = 0.0f;
            }
        }
        if (p->rg > 0 && (rgPlantTraitsOf(p) & RG_T_ROOFWALK)) {
            p->rgMoveT += dt;
            if (p->rgMoveT >= 4.5f) {
                int nc = p->col + ((p->rgMoveT > 6.0f) ? 1 : 0);
                if (nc < COLS && cellLegal(p->row, nc, p->w, p->hh) && !plantAt(p->row, nc)) {
                    Plant tmp = *p;
                    removePlantAt(p->row, p->col);
                    gPendingRg = tmp.rg - 1;
                    plantIt(tmp.row, nc, tmp.type);
                }
                p->rgMoveT = 0.0f;
            }
        }
        /* 肉鸽植物：本次开火伤害按品质倍率放大（spawnPeaAt 消费 gRgAtkMul） */
        gRgAtkMul = rgPlantAtkMul(p);
        /* 神级以上植物的【所有攻击】都带穿透 + 溅射分裂 + 溅射半径。
           用户反馈"僵尸扎堆时没有有效威胁"，所以最高两档给足：
           一颗子弹能穿透多个目标、命中后裂成多颗、并且对周围一整片再补一刀。 */
        {
            const RgPlantDef *rd = rgOfPlant(p);
            int t = rd ? (int)rd->tier : -1;
            if (t >= RGQ_MYTH) {
                int up = t - RGQ_MYTH;            /* 神级 0，传奇 1 */
                gRgPeaPierce = 4 + up * 3;        /* 神级 4 个，传奇 7 个 */
                gRgPeaSplash = 3 + up * 2;        /* 神级裂 3 颗，传奇 5 颗 */
                gRgPeaAoe    = 96 + up * 52;      /* 神级 96px，传奇 148px */
                gRgBoomMul   = 1.45f + (float)up * 0.45f;   /* 炸弹类范围同步放大 */
            } else if (t >= RGQ_EPIC) {
                /* 史诗给一档小加成：不然抱住团的僵尸对 90% 的植物都是无解的 */
                gRgPeaPierce = 1;
                gRgPeaSplash = 0;
                gRgPeaAoe    = 56;
                gRgBoomMul   = 1.15f;
            } else {
                gRgPeaPierce = 0;
                gRgPeaSplash = 0;
                gRgPeaAoe    = 0;
                gRgBoomMul   = 1.0f;
            }
            gRgPeaBounce = (rgPlantTraitsOf(p) & RG_T_BOUNCE5) ? 5 : 0;
        }
        p->phase += dt;
        if (p->recoil > 0) p->recoil -= dt * 5.0f;
        if (p->eaten  > 0) p->eaten  -= dt * 3.0f;
        /* 关 9 断电：该行断电期间植物**不工作**（不攻击、不产阳光、不触发肉鸽能力）。
           刻意放在动画衰减**之后** —— 断电不等于植物死了，
           受击抖动/被啃食的表现该继续演完，否则画面会僵住。 */
        if (!newLevelRowPowered(r)) continue;
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
            /* 肉鸽能力层：与基座行为并行触发 */
            if (p->rg > 0) rgPlantTraitTick(p, dt, r, c, cx, by, py);
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
                                memset(&peas[k], 0, sizeof(peas[k]));
                                peas[k].active = 1;
                                peas[k].x = cx + 28; peas[k].y = py;
                                peas[k].vx = 340; peas[k].vy = 0;
                                /* ⚠️ 这条路径【手搓】子弹，不经过 spawnPeaAt，
                                   所以 gRgAtkMul / 穿透 / 范围 必须在这里自己补上，
                                   否则射手类神级植物的品质加成完全不生效。 */
                                peas[k].dmg = peaBaseDmg() * (1.0f + fieldDmgBonusAt(r, c)) * gRgAtkMul;
                                peas[k].row = r; peas[k].frozen = frozen;
                                peas[k].wpn = wpn;
                                peas[k].pierceLeft = gRgPeaPierce;
                                peas[k].aoe        = gRgPeaAoe;
                                RG_SET_SPLASH(peas[k], 0); peas[k].bounces = 0;
                                peas[k].freeze = 0; peas[k].bouncesBase = 0;
                                peas[k].chainLeft = gRgPeaBounce; peas[k].lastHit = -1;
                                peas[k].sub = sub;
                                peas[k].src = p->type;
                                switch (wpn) {
                                case WPN_PIERCE: RG_SET_PIERCE(peas[k], 3); break;
                                case WPN_SPLASH: RG_SET_SPLASH(peas[k], 3); break;
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
                                        memset(&peas[k], 0, sizeof(peas[k]));
                                        peas[k].active = 1;
                                        peas[k].x = cx + 28; peas[k].y = cellBaseY(rr2) - 62;
                                        peas[k].vx = 340; peas[k].vy = 0;
                                        peas[k].dmg = peaBaseDmg() * (1.0f + fieldDmgBonusAt(rr2, c)) * tmul * gRgAtkMul;
                                        peas[k].row = rr2; peas[k].frozen = 0;
                                        peas[k].wpn = wpn; RG_SET_SPLASH(peas[k], 0);
                                        peas[k].pierceLeft = gRgPeaPierce;
                                        peas[k].aoe        = gRgPeaAoe;
                                        peas[k].bounces = 0; peas[k].freeze = 0;
                                        peas[k].bouncesBase = 0; peas[k].sub = sub;
                                        peas[k].chainLeft = gRgPeaBounce; peas[k].lastHit = -1;
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
                                    RG_SET_PIERCE(peas[k], 2);    /* 穿透 2 个 */
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
                                    RG_SET_PIERCE(peas[k], 2);
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
                        /* ⚠️ 下面这两行是补回来的：外部编辑时 `default: break;`
                           和 switch 的闭括号被一起删掉了，导致整个 updateGame
                           少一个 }、后面所有函数都被编译器当成嵌套声明
                           （报的是 'invalid storage class for function onClick'，
                           症状离病因 1000 多行）。对照 _dump.c 定位到的。 */
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
                            explode(cx, by - 18, CELL_W * 0.9f, 1800.0f * gRgAtkMul, 0, 0);
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
                    explode(cx, by - 26, CELL_W * 1.55f, 1800.0f * gRgAtkMul, 0, 0);
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
                    explode(0, 0, 0, 1800.0f * gRgAtkMul, 1, r);
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
                  if (id >= 0) RG_SET_PIERCE(peas[id], 2); }  /* 仙人掌针刺穿透 */
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
                  if (id >= 0) { RG_SET_PIERCE(peas[id], 3); peas[id].knockback = 15; } }  /* 穿刺+击退 */
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
                  if (id >= 0) RG_SET_PIERCE(peas[id], 6); }
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
                  if (id >= 0) RG_SET_PIERCE(peas[id], 12); }
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
                  if (id >= 0) { RG_SET_SPLASH(peas[id], 2); peas[id].wpn = WPN_SPLASH; if (ice) peas[id].freeze = 2200; } }
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
                      RG_SET_SPLASH(peas[id], 3); peas[id].wpn = WPN_SPLASH;
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
                int best = -1;
                float bd = 1e30f;
                for (i = 0; i < MAX_ZOMBIES; i++) {
                    Zombie *z = &zombies[i];
                    float d;
                    if (!z->active || z->dead || z->row != r) continue;
                    if (z->type == ZT_BALLOON) continue;
                    d = fabsf(z->x - cx);
                    /* 原来的 42px 判定比一格内的接触距离还小，常出现贴脸却不吞。 */
                    if (d <= CELL_W * 0.78f && d < bd) { best = i; bd = d; }
                }
                if (best >= 0) {
                    Zombie *z = &zombies[best];
                    if (z->type == ZT_GIANT) {
                        z->hp -= z->maxhp * 0.45f;  /* BOSS 不秒杀，但必定受到一次重创 */
                        z->rooted = z->rooted < 2.4f ? 2.4f : z->rooted;
                        z->flash = 0.30f;
                        if (z->hp <= 0.0f) zombieDie(z, 1);
                    } else {
                        zombieDie(z, 1);
                    }
                    for (i = 0; i < 16; i++)
                        addParticle(cx + rnd(-22, 22), by - rnd(18, 70),
                                    rnd(-80, 80), rnd(-105, -22), 0.45f, rnd(3, 7), RGB(82, 138, 210), 1);
                    addShake(4.0f, 0.18f);
                    p->alive = 0;
                }
                break;
            }
            case PT_VOIDDEVOURER:
                plantVoidDevourTick(p, dt, r, c, cx, by,
                                    gDmgMul * (1.0f + fieldDmgBonusAt(r, c)));
                break;

            default:
                if (extraPlantTick(p, dt, r, c, cx, by, py)) break;
                break;
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
                /* 打击感的核心音。按伤害分两档：重弹用更闷更重的一击、
                   普通弹用干脆的"啪" —— 一堆豌豆里能听出"哪下打疼了"。
                   并发由 sfxPlay 的 8 槽池兜底，密集命中自然叠成一片。 */
                sfxPlay(pd >= 60.0f ? L"hit_hard" : L"hit");
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
                /* 击退（knockback）：推离房屋，向右侧退 */
                if (pe->knockback > 0) {
                    zombieKnockback(z, (float)pe->knockback);
                }
                /* 溅射分裂：以 splash 为准，不再要求 wpn == WPN_SPLASH，
                   这样"穿透 + 溅射"可以同时挂在一颗子弹上。 */
                if (pe->splash > 0) {
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
                                peas[m].vy = ((float)kk - (float)(n - 1) * 0.5f) * 78.0f;  /* 扇形散开 */
                                peas[m].x = pe->x + 4.0f;
                                peas[m].y = pe->y;
                                /* 碎片继承一半穿透与半径，避免无限继承滚雪球 */
                                peas[m].pierceLeft = pe->pierceLeft / 2;
                                peas[m].aoe        = pe->aoe / 2;
                                peas[m].chainLeft  = pe->chainLeft / 2;
                                peas[m].lastHit    = hit;
                                break;
                            }
                        }
                    }
                }

                /* 溅射半径（aoe）：命中时对周围一整片僵尸再补一刀。
                   这是"僵尸扎堆"最直接的解法 —— 一颗子弹打进人堆就是一次范围伤害。
                   判定用「横向距离 + 最多波及相邻 1 行」，
                   不拿 z->y 算球面距离（僵尸 y 在脚下、子弹 y 在头部，差 60+ 像素，
                   用球面距离会把范围压得几乎打不到人）。 */
                if (pe->aoe > 0) {
                    int k3;
                    for (k3 = 0; k3 < MAX_ZOMBIES; k3++) {
                        Zombie *zz = &zombies[k3];
                        float dx;
                        int   dr2;
                        if (k3 == hit) continue;                 /* 主目标已经吃过伤害 */
                        if (!zz->active || zz->dead) continue;
                        dr2 = zz->row - pe->row;
                        if (dr2 < 0) dr2 = -dr2;
                        if (dr2 > 1) continue;
                        dx = zz->x - pe->x;
                        if (dx < -(float)pe->aoe || dx > (float)pe->aoe) continue;
                        zz->hp -= pe->dmg * 0.55f;
                        zz->flash = 0.08f;
                        if (zz->hp <= 0.0f) zombieDie(zz, 0);
                    }
                    { int k4; for (k4 = 0; k4 < 9; k4++)
                        addParticle(pe->x + rnd(-16, 16), pe->y + rnd(-16, 16),
                                    rnd(-100, 100), rnd(-100, 40), 0.26f, rnd(3, 6),
                                    RGB(255, 216, 120), 1); }
                }

                /* 穿透：以 pierceLeft 为准，不再要求 wpn == WPN_PIERCE。
                   原来那个 && 让"设了 pierceLeft 但武器不是穿透"的植物
                   （仙人掌 / 激光豆 / 香蒲 / 玉米加农）其实从来没穿透成功 ——
                   这是个一直存在的隐性 bug。 */
                if (pe->pierceLeft > 0) {
                    pe->pierceLeft--;
                    pe->dmg *= 0.85f;
                    pe->x = z->x + 26.0f;
                } else if (pe->chainLeft > 0) {
                    int k2, next = -1;
                    float nd = 1e9f;
                    /* 连锁反弹和武器弹跳分开记数。允许向前或向后找最近目标，
                       才是真正的“在僵尸之间反弹”，而不是伪装成额外穿透。 */
                    for (k2 = 0; k2 < MAX_ZOMBIES; k2++) {
                        Zombie *q = &zombies[k2];
                        float d2;
                        if (k2 == hit || k2 == pe->lastHit ||
                            !q->active || q->dead || q->row != z->row) continue;
                        d2 = fabsf(q->x - z->x);
                        if (d2 > 280.0f || d2 >= nd) continue;
                        nd = d2; next = k2;
                    }
                    pe->chainLeft--;
                    pe->lastHit = hit;
                    pe->dmg *= 0.82f;
                    if (next >= 0) {
                        float dir = (zombies[next].x >= z->x) ? 1.0f : -1.0f;
                        pe->vx = fabsf(pe->vx) * dir;
                        pe->x = z->x + dir * 28.0f;
                        addParticle(z->x, z->y - 42.0f, dir * 45.0f, -35.0f,
                                    0.24f, 5.0f, RGB(255, 226, 120), 1);
                    } else pe->active = 0;
                } else if (pe->bounces > 0) {
                    int k2, next = -1;
                    float nd = 1e9f;
                    for (k2 = 0; k2 < MAX_ZOMBIES; k2++) {
                        Zombie *q = &zombies[k2];
                        if (k2 == hit || !q->active || q->dead || q->row != z->row) continue;
                        if (q->x <= z->x + 18.0f) continue;
                        if (fabsf(q->x - z->x) < nd) { nd = fabsf(q->x - z->x); next = k2; }
                    }
                    pe->bounces--;
                    pe->x = z->x + 28.0f;
                    pe->dmg *= 0.82f;
                    if (next >= 0) pe->vx = fabsf(pe->vx);
                    else pe->active = 0;
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

        /* 在任何攻击/特殊行为之前先恢复同排前后顺序。 */
        zombieRespectFrontline(z);
        z->anim += dt;
        /* 「这一帧刚被命中」的判定。
           ⚠️ 必须在**本帧只算一次**、并显式传给所有消费者。
           踩过的坑：早先的写法是让每个消费者自己检查
           `z->flash > 0 && z->flashPrev <= 0`，但 flashPrev 的更新
           夹在这些检查中间 —— newZombieBehavior() 被调用时 flashPrev
           已经等于本帧的 flash 了，上升沿**永远检测不到**。
           后果是冰锥僵尸的溅射、机械僵尸的反弹全部静默失效
           （代码看着有 case，实际从不触发）。
           现在算一次 hitEdge，谁需要就传谁，不再依赖语句顺序。 */
        {
            int hitEdge = (z->flash > 0.0f && z->flashPrev <= 0.0f);
            if (hitEdge) newLevelKnockback(z);
            z->flashPrev = z->flash;
            if (z->flash > 0) z->flash -= dt;
            if (z->slow  > 0) z->slow  -= dt;
            /* 关卡级速度修正（关 7 热浪累积 / 关 10 引力加速）。
               不改 z->speed 而是按差值补位移：z->speed 会被减速/加速遗物
               反复改写，直接乘进去会导致效果叠加失控、无法回退。 */
            {
                float km = newLevelSpeedMul();
                if (km != 1.0f) z->x -= z->speed * (km - 1.0f) * dt;
            }
            /* 新五关专属僵尸的行为（幽影/闪现/短路/机械/冰锥…） */
            newZombieBehavior(z, dt, hitEdge);
        }


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
                if (!z->vaulted) {
                    z->vaulted = 1;                 /* 借一次性标志，保留出场时的波次加成 */
                    z->speed *= 2.2f;               /* 报纸没了 -> 暴走 */
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
                    /* 重击改为两段以上才能摧毁：满血植物不会毫无过程地消失，
                       但玩家若不处理，2.6 秒后的第二锤仍然致命。 */
                    gp->hp -= gp->maxhp * 0.55f;
                    gp->eaten = 1.0f;
                    if (gp->hp <= 0.0f) removePlantAt(gp->row, gp->col);
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
                    /* 不再碰到即删除。碾压改成持续伤害，约一秒才压碎基础植物；
                       抖动和碎屑让玩家能看懂伤害来源并有时间处理冰车。 */
                    tp->hp -= 360.0f * zombieWaveBiteMul() * dt;
                    tp->eaten = 1.0f;
                    z->eatBob += dt;
                    if (z->eatBob >= 0.18f) {
                        int q;
                        z->eatBob = 0.0f;
                        addShake(1.6f, 0.07f);
                        for (q = 0; q < 5; q++)
                            addParticle(z->x, z->y - rnd(0, 30), rnd(-70, 40), rnd(-110, -20),
                                        0.5f, rnd(3, 6), RGB(150, 200, 230), 1);
                    }
                    if (tp->hp <= 0.0f) removePlantAt(tp->row, tp->col);
                } else {
                    z->hp -= 60.0f * dt;                 /* 地刺反伤冰车 */
                }
            }
        }
        /* ---- 气球僵尸：飞在空中，不受地面植物阻挡，被攻击 3 次后落地 ---- */
        if (z->type == ZT_BALLOON) {
            if (z->maxhp - z->hp >= z->maxhp * 0.34f) {
                z->type = ZT_NORMAL;                     /* 气球破了 -> 落地变普通僵尸 */
                z->speed = zSpeed[ZT_NORMAL] * gZombieSpdMul * gZombieSpdUp
                         * levelDefs[gCurLevel].spdMul * zombieWaveSpeedMul();
                addShake(2.0f, 0.12f);
                { int q; for (q = 0; q < 16; q++)
                    addParticle(z->x + rnd(-16, 16), z->y - rnd(80, 140), rnd(-80, 80),
                                rnd(-40, 60), 0.7f, rnd(3, 7), RGB(228, 96, 110), 1); }
            }
        }

        {
            if (z->slow > 0.0f) z->slow -= dt;
            if (z->rooted > 0.0f) z->rooted -= dt;
            /* 所有地面僵尸先服从前排碰撞规则；包括旧机制已经放到后排的个体。 */
            zombieRespectFrontline(z);
            /* 肉鸽僵尸能力层 */
            if (z->rg > 0) rgZombieTraitTick(z, dt);
            zombieRespectFrontline(z);       /* 特殊能力执行后再兜底一次 */
            {
            float sp   = z->speed * (z->rooted > 0.0f ? 0.0f : (z->slow > 0 ? gSlowFactor : 1.0f));
            float bite = z->x - 24.0f;
            int   col  = (int)floor((bite - LAWN_X) / (float)CELL_W);
            Plant *tg  = (col >= 0 && col < COLS) ? plantAt(z->row, col) : NULL;

            if (tg) {
                if (z->type == ZT_BALLOON) { z->eating = 0; tg = NULL; }   /* 飞在空中，不啃植物 */
                if (z->type == ZT_GIANT) { z->eating = 0; tg = NULL; }     /* 巨人不啃，直接砸 */
                /* 撑杆僵尸：在"即将啃食"这一刻改成跳过去。
                   注意判定必须挂在啃食点上 —— 原来用更近的距离阈值，
                   僵尸会先停下来啃食，永远轮不到跳跃。 */
                if (z->type == ZT_VAULTER && !z->vaulted) {
                    z->vaulted = 1;
                    z->eating = 0;
                    zombieSpecialMove(z, -96.0f);
                    z->anim += 0.6f;
                    addShake(2.2f, 0.12f);
                    { int q; for (q = 0; q < 14; q++)
                        addParticle(z->x + rnd(-10, 50), z->y - rnd(6, 60), rnd(-90, 30),
                                    rnd(-90, 20), 0.5f, rnd(3, 6), RGB(220, 200, 170), 1); }
                    tg = NULL;
                }
            }
            if (tg) {
                /* 只在"开始啃"的那一帧响：这段每帧都在跑，
                   直接放会变成每秒 60 次的噪音墙。 */
                if (!z->eating) sfxPlay(L"zombie_bite");
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
                        tg->shield -= 80.0f * zombieWaveBiteMul() * dt;
                        if (tg->shield < 0.0f) tg->shield = 0.0f;
                    } else {
                        tg->hp   -= 80.0f * zombieWaveBiteMul() * dt;
                    }
                    if (gThorns > 0) z->hp -= (float)gThorns * dt;
                    tg->eaten = 1.0f;
                    if (tg->hp <= 0.0f) {
                        int k;
                        for (k = 0; k < 12; k++)
                            addParticle(cellCX(col) + rnd(-16, 16), cellBaseY(z->row) - rnd(4, 50),
                                        rnd(-70, 70), rnd(-120, -20), rnd(0.4f, 0.8f),
                                        rnd(3, 7), RGB(110, 180, 80), 1);
                        removePlantAt(tg->row, tg->col);
                    }
                }
            } else {
                z->eating = 0;
                z->x -= sp * dt;
            }

            /* 抵达房屋：移动后重算 bite，避免判定与画面错开一帧。 */
            bite = z->x - 24.0f;
            if (bite < MOWER_TRIGGER_BITE_X) {
                if (mowers[z->row].active && !mowers[z->row].running) {
                    mowers[z->row].running = 1;
                    sfxPlay(L"mower");       /* 小推车启动 */
                } else if (!mowers[z->row].active && bite < HOUSE_BREACH_BITE_X) {
                    /* 失败也结算：经验参与奖 + 纪录 */
                    gainXp(gKilled / 4 + 5);
                    if (gKilled > gSave.bestKills) { gSave.bestKills = gKilled; gRecNew = 1; }
                    if (gGold   > gSave.bestGold)  { gSave.bestGold  = gGold;   gRecNew = 1; }
                    saveFlush();
                    sfxPlay(L"lose");
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
                sfxPlay(L"sun_collect");     /* 收阳光：清脆"叮" */
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
/* ---- 返回导航 ------------------------------------------------------------
   曾经的问题：界面底部写着「右键 / Esc 返回」，但 Esc 的分支只做了
   `gSel = -1; gShovel = 0;` —— **根本不返回**。玩家按 Esc 没反应，
   只能以为卡住（这就是报障「没有返回按钮、要按 Esc 返回」的由来）。
   而且当时界面上也**没有任何可见的返回按钮**，只有一行小字提示。

   现在三条路都通，且共用同一个 goBack()：
     · 左上角可见按钮（不用记快捷键）
     · Esc 键
     · 鼠标右键
   共用一份逻辑是有意的 —— 之前右键能返回、Esc 不能，
   正是因为两条路径各写了一份、改一处漏另一处。 */
#define BACKBTN_X 16.0f
#define BACKBTN_Y 14.0f
#define BACKBTN_W 124.0f
#define BACKBTN_H 36.0f
#define PAUSE_BTN_X 350.0f
#define PAUSE_BTN_W 300.0f
#define PAUSE_CONT_Y 330.0f
#define PAUSE_HOME_Y 410.0f
#define PAUSE_BTN_H  58.0f

/* 哪些界面属于「元界面」：都从主菜单进来，都该有返回入口 */
static int isMetaScreen(int st)
{
    return st == ST_LEVELS  || st == ST_TALENTS || st == ST_ACH ||
           st == ST_GACHA   || st == ST_LOADOUT || st == ST_REWARD ||
           st == ST_DAILY;
}

/* 返回上一步。元界面回主菜单；局内三选一回到战斗，
   若它是通关前的最后一次选择，则回到通关结算。 */
static void goBack(void)
{
    if (gState == ST_DRAFT) {
        gDraftHover = -1;
        gBuyFailed = 0;
        if (gPendingWin) { gPendingWin = 0; gState = ST_WIN; }
        else             { gState = ST_PLAY; }
    } else if (isMetaScreen(gState)) {
        gState = ST_MENU;
    } else {
        return;
    }
    gSel = -1;
    gShovel = 0;
}

static void pauseOpen(int from)
{
    if (from != ST_PLAY && from != ST_DRAFT) from = ST_PLAY;
    gPauseFrom = from;
    gState = ST_PAUSE;
    gSel = -1;
    gShovel = 0;
}

static void pauseContinue(void)
{
    gState = (gPauseFrom == ST_DRAFT) ? ST_DRAFT : ST_PLAY;
}

static void pauseHome(void)
{
    gState = ST_MENU;
    gPauseFrom = ST_PLAY;
    gPendingWin = 0;
    gDraftHover = -1;
    gSel = -1;
    gShovel = 0;
    bgmClose();
}

/* 统一判断“这里可以点”。除了换成手形光标，这个函数也把所有界面的
   可点击范围集中到同一处，避免画面看起来像按钮、鼠标却没有任何反馈。 */
int uiPointerHot(int mx, int my)     /* 非 static：网页版用来决定是否显示手型 */
{
    int i;
    if (mx >= (int)FULLBTN_X && mx <= (int)(FULLBTN_X + FULLBTN_W) &&
        my >= (int)FULLBTN_Y && my <= (int)(FULLBTN_Y + FULLBTN_H)) return 1;
    if (isMetaScreen(gState) &&
        mx >= (int)BACKBTN_X && mx <= (int)(BACKBTN_X + BACKBTN_W) &&
        my >= (int)BACKBTN_Y && my <= (int)(BACKBTN_Y + BACKBTN_H)) return 1;

    if (gState == ST_MENU) {
        if (my >= 330 && my <= 376 &&
            ((mx >= 40 && mx <= 300) || (mx >= 320 && mx <= 450) ||
             (mx >= 470 && mx <= 600) || (mx >= 620 && mx <= 750) ||
             (mx >= 770 && mx <= 960))) return 1;
        return my >= 396 && my <= 442 && mx >= 370 && mx <= 630;
    }
    if (gState == ST_PAUSE)
        return mx >= (int)PAUSE_BTN_X && mx <= (int)(PAUSE_BTN_X + PAUSE_BTN_W) &&
               ((my >= (int)PAUSE_CONT_Y && my <= (int)(PAUSE_CONT_Y + PAUSE_BTN_H)) ||
                (my >= (int)PAUSE_HOME_Y && my <= (int)(PAUSE_HOME_Y + PAUSE_BTN_H)));
    if (gState == ST_WIN || gState == ST_LOSE)
        return mx >= 350 && mx <= 650 && my >= 370 && my <= 428;
    if (gState == ST_LEVELS) return metaHitLevel(mx, my) >= 0;
    if (gState == ST_TALENTS) return metaHitTalent(mx, my) >= 0;
    if (gState == ST_LOADOUT) {
        if (my >= (int)LO_PAGE_BTN_Y && my <= (int)(LO_PAGE_BTN_Y + LO_PAGE_BTN_H) &&
            ((mx >= (int)LO_PAGE_PREV_X && mx <= (int)(LO_PAGE_PREV_X + LO_PAGE_BTN_W)) ||
             (mx >= (int)LO_PAGE_NEXT_X && mx <= (int)(LO_PAGE_NEXT_X + LO_PAGE_BTN_W)))) return 1;
        return loHit(mx, my) >= 0;
    }
    if (gState == ST_REWARD) return gRewardChosen >= 0 || rwHit(mx, my) >= 0;
    if (gState == ST_GACHA)
        return mx >= 350 && mx <= 650 && my >= (int)GACHA_BTN_Y && my <= (int)(GACHA_BTN_Y + 64.0f);
    if (gState == ST_DAILY)
        return mx >= 370 && mx <= 630 &&
               ((my >= 270 && my <= 370) || (my >= 385 && my <= 510));
    if (gState == ST_DRAFT) {
        for (i = 0; i < DRAFT_N; i++) {
            float x, y;
            draftCardRect(i, &x, &y);
            if ((float)mx >= x && (float)mx <= x + CARD_W &&
                (float)my >= y && (float)my <= y + CARD_H) return 1;
        }
        return my >= (int)SKIP_Y && my <= (int)(SKIP_Y + SKIP_H) &&
               mx >= (int)(VIEW_W * 0.5f - 200.0f) && mx <= (int)(VIEW_W * 0.5f + 200.0f);
    }
    if (gState == ST_PLAY) {
        int total = gLoadoutN + gBonusN + gRgN + 1; /* 最后一格是铲子 */
        if (my < 92 && mx >= 140 &&
            mx < (int)(144.0f + cardBarGap() * (float)total)) return 1;
        for (i = 0; i < MAX_SUNS; i++) if (suns[i].active && !suns[i].flying) {
            float dx = (float)mx - suns[i].x, dy = (float)my - suns[i].y;
            if (dx * dx + dy * dy < 34.0f * 34.0f) return 1;
        }
    }
    return 0;
}

static void onClick(int mx, int my)
{
    int i;

    /* 右上角全屏按钮：**任何界面都可点**，所以放在所有状态分支之前。
       它是全局 UI，不该被"现在停在哪个菜单"限制住。 */
    if (mx >= (int)FULLBTN_X && mx <= (int)(FULLBTN_X + FULLBTN_W) &&
        my >= (int)FULLBTN_Y && my <= (int)(FULLBTN_Y + FULLBTN_H)) {
        if (toggleAllowed()) toggleFullscreen();
        return;
    }

    /* 左上角返回按钮：所有元界面都有，位置固定 ——
       和全屏按钮一样属于全局 UI，所以放在状态分支之前。 */
    if (isMetaScreen(gState) &&
        mx >= (int)BACKBTN_X && mx <= (int)(BACKBTN_X + BACKBTN_W) &&
        my >= (int)BACKBTN_Y && my <= (int)(BACKBTN_Y + BACKBTN_H)) {
        goBack();
        return;
    }

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
        return;
    }
    if (gState == ST_PAUSE) {
        if ((float)mx >= PAUSE_BTN_X && (float)mx <= PAUSE_BTN_X + PAUSE_BTN_W &&
            (float)my >= PAUSE_CONT_Y && (float)my <= PAUSE_CONT_Y + PAUSE_BTN_H) {
            pauseContinue();
            return;
        }
        if ((float)mx >= PAUSE_BTN_X && (float)mx <= PAUSE_BTN_X + PAUSE_BTN_W &&
            (float)my >= PAUSE_HOME_Y && (float)my <= PAUSE_HOME_Y + PAUSE_BTN_H) {
            pauseHome();
            return;
        }
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
            gLawnVariant = levelLawnVariant(lv);
            bgmForLevel(lv);        /* 关卡 BGM：第一关是本轮接入的那首爵士乐 */
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
        /* 页数上界必须用可见株数 loCount()，不能再用 PT_HERO_FLAME：
           少掉 4 株后末页可能正好被填满也被消掉，继续用旧上界会多出一页空白。 */
        int pages = (loCount() + LO_VISIBLE_SLOTS - 1) / LO_VISIBLE_SLOTS;
        if ((float)my >= LO_PAGE_BTN_Y && (float)my <= LO_PAGE_BTN_Y + LO_PAGE_BTN_H) {
            if ((float)mx >= LO_PAGE_PREV_X && (float)mx <= LO_PAGE_PREV_X + LO_PAGE_BTN_W) {
                if (gLoadoutPage > 0) gLoadoutPage--;
                return;
            }
            if ((float)mx >= LO_PAGE_NEXT_X && (float)mx <= LO_PAGE_NEXT_X + LO_PAGE_BTN_W) {
                if (gLoadoutPage + 1 < pages) gLoadoutPage++;
                return;
            }
        }
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
        if (my >= 270 && my <= 370 && mx >= 370 && mx <= 630) { doCheckIn(); return; }
        if (my >= 385 && my <= 510 && mx >= 370 && mx <= 630) { dailyStart(); return; }
        return;
    }
    if (gState == ST_DRAFT) {
        int k;
        for (k = 0; k < DRAFT_N; k++) {
            float cx0, cy0;
            draftCardRect(k, &cx0, &cy0);
            if (mx >= cx0 && mx <= cx0 + CARD_W && my >= cy0 && my <= cy0 + CARD_H) {
                gBuyFailed = 0;                   /* 每次点击先清，避免读到上一次的残留 */
                applyRelic(gDraft[k]);
                /* 货架上阳光不够：**留在货架**让玩家改选别的或跳过，
                   而不是默默关掉面板（那样玩家会以为卡死了） */
                if (gBuyFailed) return;
                if (gPendingWin) { gPendingWin = 0; gState = ST_WIN; return; }
                gState = ST_PLAY;
                return;
            }
        }
        if (my >= SKIP_Y && my <= SKIP_Y + SKIP_H &&
            mx >= VIEW_W * 0.5f - 200.0f && mx <= VIEW_W * 0.5f + 200.0f) {
            gSun += DRAFT_SKIP;
            if (gPendingWin) { gPendingWin = 0; gState = ST_WIN; return; }
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
            if (cardCD[idx] <= 0.0f && gSun >= plantCost(idx)) {
                gSel = idx; gShovel = 0; sfxPlay(L"card");
            } else {
                gSel = -1; sfxPlay(L"deny");     /* 冷却中 / 阳光不够 */
            }
            return;
        }
        /* 肉鸽植物卡：奖励后的第一次落地免费，之后按品质收费 */
        if (slot >= gLoadoutN + gBonusN && slot < gLoadoutN + gBonusN + gRgN) {
            int rgj = slot - gLoadoutN - gBonusN;
            if (rgCardCD[rgj] <= 0.0f &&
                (gRgFree[rgj] || gSun >= rgPlantCost(gRgCard[rgj]))) {
                gSel = RG_CARD_BASE + gRgCard[rgj]; gShovel = 0; sfxPlay(L"card");
            } else {
                gSel = -1; sfxPlay(L"deny");
            }
            return;
        }
        if (slot == gLoadoutN + gBonusN + gRgN) {
            gShovel = !gShovel; gSel = -1; sfxPlay(L"card");
            return;
        }
        return;
    }

    /* 3) 草坪 */
    {
        int row, col, w, hh;
        if (!mouseToCell(mx, my, &row, &col, &w, &hh)) { gSel = -1; gShovel = 0; return; }

        if (gShovel) {
            if (plantAt(row, col)) {
                removePlantAt(row, col);
                sfxPlay(L"shovel");      /* 铲除：铲子刮土 */
                gShovel = 0;
            }
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
        /* ---- 肉鸽植物：奖励后的首次落地免费，之后按品质收费 ---- */
        if (gSel >= RG_CARD_BASE) {
            int rgj, ri;
            for (rgj = 0; rgj < gRgN; rgj++)
                if (RG_CARD_BASE + gRgCard[rgj] == gSel) break;
            if (rgj < gRgN && rgCardCD[rgj] <= 0.0f &&
                (gRgFree[rgj] || gSun >= rgPlantCost(gRgCard[rgj])) &&
                cellLegal(row, col, w, hh)) {
                ri = gRgCard[rgj];
                gPendingRg = ri;
                plantIt(row, col, (int)rgPlants[ri].base);
                gPendingRg = -1;
                if (gRgFree[rgj]) gRgFree[rgj] = 0;
                else              gSun -= rgPlantCost(ri);
                rgCardCD[rgj] = (float)(20 - 2 * rgPlants[ri].tier);
                gSel = -1;
                for (i = 0; i < 12; i++)
                    addParticle(cellCX(col) + rnd(-24, 24), cellBaseY(row) - rnd(0, 20),
                                rnd(-70, 70), rnd(-110, -25), 0.45f, rnd(3, 7),
                                RGQ_COL[rgPlants[ri].tier], 1);
            }
            return;
        }
        /* 注意：这里不能用 plantAt()==NULL 判断 —— 双株模式下第二株要种进 slot1，
           而 plantAt 会返回已经存在的 slot0，导致第二株永远种不下去。
           能不能种，cellLegal() 里的 cellHasRoom() 已经判过了。 */
        if (gSel >= 0 && cellLegal(row, col, w, hh)) {
            plantIt(row, col, gSel);
            /* 三选一获得的神级植物本局免费种植，货架普通植物按 30~200 阳光计费 */
            {
                int isBonus = 0;
                for (int k = 0; k < gBonusN; k++) if (gBonusPlant[k] == gSel) { isBonus = 1; break; }
                if (!isBonus) gSun -= plantCost(gSel);
            }
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
    /* 两个边界必须挡住。编译器在可移植侧报的是
       「cand may be used uninitialized」—— 它的根因是证明不了 m > 0，
       而 **m 真的可能为 0**（n 为 0 时三个筛选循环都不进）：
         · n <= 0      → `rand() % m` 是**除零**（UB，可能直接崩）
         · n > R_COUNT → `cand[m++]` 越界写（栈缓冲区溢出）
       调用方目前不会传这两种值，但这是库函数级的输入，挡一下成本为零。 */
    if (n <= 0) return -1;
    if (n > R_COUNT) n = R_COUNT;
    roll = rand() % 100;
    if (forceRare)      target = (roll < 30) ? 2 : 1;
    else if (roll < 13) target = 2;
    else if (roll < 45) target = 1;
    else                target = 0;
    for (i = 0; i < n; i++) if (relicDefs[pool[i]].rarity == target) cand[m++] = pool[i];
    if (m == 0) for (i = 0; i < n; i++) if (relicDefs[pool[i]].rarity == 0) cand[m++] = pool[i];
    if (m == 0) for (i = 0; i < n; i++) cand[m++] = pool[i];
    if (m <= 0) return -1;              /* 兜底：上游给了空池 */
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
         ② 本局神级卡槽满 / MAX_BONUS_PLANT 株都拿过 -> 永久解锁一株还没解锁的普通植物
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
    gDraftMode = 0;                       /* 走遗物/神级池 */
    gState = ST_DRAFT;
}

/* ========================================================================
   肉鸽三选一（rgOpenDraft）
   ------------------------------------------------------------------------
   规则（用户指定）：
     · 每一波都出现一次
     · 第 1~10 波：至少 2 张植物卡；第 11 波起：1 植物 + 2 成长
     · 拿到即【免费】—— 种下去不花阳光，也不占永久编组
     · 品质越高越难抽到，所以"通关靠随机到好的三选一"
   编码：gDraft[i] = RG_CARD_BASE + 肉鸽植物索引（>= RG_CARD_BASE 即为肉鸽卡）
   ======================================================================== */
static void rgOpenDraft(int waveIdx)
{
    int cand[RG_PLANT_N], n = 0, i, k, j;
    int growthPool[GROW_COUNT], gn = GROW_COUNT;
    int plantNum;
    /* 这是波次奖励，不是商店。以前第二次起改成了收费货架，
       导致高价植物卡点击后被拒绝，看起来就像「第一个框点不动」。 */
    gDraftFree = 1;

    /* 候选池：本局还没拿到的、且**档位够**的肉鸽植物。
       低于 RG_DRAFT_MIN_TIER 的（当前是普通档 14 株）不上牌 ——
       这是提高其他档位概率的杠杆：池子总权重从 2004 降到 1584，
       稀有/史诗/传说/神级/传奇/殿堂的占比会整体抬升。 */
    for (i = 0; i < RG_PLANT_N; i++)
        if (!rgOwned(i) && rgDraftEligible(i)) cand[n++] = i;
    for (i = 0; i < GROW_COUNT; i++) growthPool[i] = i;

    for (k = 0; k < DRAFT_N; k++) gDraft[k] = DRAFT_ESSENCE;

    /* 前十波是阵容成型期，三张里硬保底两张不同植物；第十一波起，
       阵容已经具备基本骨架，改为一张植物 + 两张成长卡。
       注意这里不能再用 gRgN < RG_MAX_SLOTS 限制“是否出植物”：卡槽满时
       选植物已有阳光补偿逻辑，而奖励界面仍必须兑现用户看到的卡池规则。 */
    plantNum = (waveIdx < 10) ? 2 : 1;
    if (plantNum > n) plantNum = n;        /* 理论上仅全图鉴耗尽时触发 */

    {
        int order[DRAFT_N];
        for (k = 0; k < DRAFT_N; k++) order[k] = k;
        for (k = DRAFT_N - 1; k > 0; k--) {          /* 位置随机打乱，不固定 */
            int s = rndi(0, k + 1), t = order[k];
            order[k] = order[s]; order[s] = t;
        }
        /* ① 先放植物（按品质权重抽，且互不重复）
             ⚠️ 这里用的是 RGQ_DRAFT_W，不是 RGQ_W —— 后者是肉鸽僵尸的权重。
                两个数组索引都是 RGQ_*，极易改错一个；所以约定：
                "植物只看 DRAFT 版，僵尸只看 RGQ_W"，改概率只动对应那一个数组。 */
        for (j = 0; j < plantNum && n > 0; j++) {
            int tot = 0, roll, t2;
            for (t2 = 0; t2 < n; t2++) tot += RGQ_DRAFT_W[rgPlants[cand[t2]].tier];
            if (tot <= 0) tot = 1;
            roll = rndi(0, tot);
            for (t2 = 0; t2 < n; t2++) {
                roll -= RGQ_DRAFT_W[rgPlants[cand[t2]].tier];
                if (roll < 0) break;
            }
            if (t2 >= n) t2 = n - 1;
            gDraft[order[j]] = RG_CARD_BASE + cand[t2];
            cand[t2] = cand[--n];                    /* 抽掉，保证不重复 */
        }
        /* ② 剩下的位置给不重复的成长卡 */
        for (j = plantNum; j < DRAFT_N; j++) {
            int s = rndi(0, gn);
            gDraft[order[j]] = GROWTH_CARD_BASE + growthPool[s];
            growthPool[s] = growthPool[--gn];
        }
    }

    /* 始祖级出现即改写背景与音乐（用户要求）。放在这里而不是选卡时：
       "出现必定伴随变化"说的是**它出现在选项里**那一刻。 */
    draftScanPrimordial();
    draftCelebScan();          /* 殿堂级以上：开牌庆典演出 */

    gDraftMode  = 1;
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

static void applyGrowth(int id)
{
    if (id < 0 || id >= GROW_COUNT) return;
    if (gGrowthStack[id] < 12) gGrowthStack[id]++;
    relicsRecalc();
    plantsRefreshHp();
}

static void applyRelic(int id)
{
    if (DRAFT_IS_GROWTH(id)) {
        applyGrowth(DRAFT_GROWTH_ID(id));
        return;
    }
    if (DRAFT_IS_RG(id)) {                   /* 肉鸽植物：每波三选一免费领取 */
        int ri = id - RG_CARD_BASE;
        /* 始祖不免费：freebie=0 时 rgGrant 会先比较 gSun 与售价，
           不够就置 gBuyFailed 并**不扣款也不发卡**，所以不会出现
           "标价 10000、实际白拿"或"钱扣了卡没到"这两种情况。 */
        int freeb = (rgPlants[ri].tier == RGQ_PRIMORDIAL) ? 0 : gDraftFree;
        rgGrant(ri, freeb);
        return;
    }
    if (DRAFT_IS_PLANT(id)) {
        int pid = DRAFT_PLANT_ID(id);
        if (pid >= PT_COUNT) {                   /* 全图鉴兜底：植物精华 */
            gSun += DRAFT_ESSENCE_SUN;
            gGold += DRAFT_ESSENCE_GOLD;
            return;
        }
        if (gDraftPlantPerm) {                   /* 普通植物卡：永久解锁 */
            if (pid >= 0 && pid < PT_HERO_FLAME && !gSave.plantOwned[pid]) {
                gSave.plantOwned[pid] = 1;
                /* 上限锁 1：不是"有空位才带上"，而是直接顶替成为唯一出战植物 */
                loadoutEquipOnly(pid);
                saveFlush();
            }
            return;
        }
        applyHeroPlant(pid);
        return;
    }
    if (id <= 0 || id >= R_COUNT) return;
    {
        /* 本局第一次拿到才触发一次性效果（遗物在 resetGame 里清零，属局内获取；
           若挂在 relicsRecalc 上会被反复重算反复触发）。 */
        int isNew = !gRelic[id];
        if (isNew) { gRelic[id] = 1; gRelicCount++; }
        relicsRecalc();
        plantsRefreshHp();
        /* 「随机赠卡」：拿到的一瞬间白送一张随机肉鸽植物卡（免费）。
           上限锁 1 之后不能再靠加卡位，改成这种"立即可用的额外卡"。 */
        if (isNew && id == R_LOADOUT_PLUS) rgGrantStarterCard();
    }
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
    /* 水中 / 夜间模式：拿到即解锁并切过去。
       这两条原来写在 resetGame() 里（见那里的注释），因为 HAS() 恒为假而从未生效。 */
    case R_WATER_MODE:       gUnlockedMode[MODE_WATER] = 1; gCurMode = MODE_WATER; break;
    case R_NIGHT_MODE:       gUnlockedMode[MODE_NIGHT] = 1; gCurMode = MODE_NIGHT; break;
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

/* ---- 卡牌翻牌动画（P4-B） ----
   每张卡按索引错开 0.09s 入场，单张翻牌 0.46s：
     t < 0.5   合上：卡背从满宽压到最窄
     t >= 0.5  展开：正面从最窄张到满宽
   压缩靠「以卡中心为轴的水平缩放」实现，用世界变换交给 GDI：
   fillRound / Ellipse / line 这些矢量绘制天然吃变换，
   卡面、色条、稀有度光环会一起被压扁，看起来才像真的在翻。
   唯一的例外是精灵 —— spriteBlit 内部会临时把变换复位成设备坐标，
   好在精灵的居中点正好落在缩放轴（卡中心）上，位置不用改，
   只要把横向缩放 sx 乘上 k 就等价于被压扁了。 */
#define FLIP_LEAD   0.09f      /* 相邻两张卡的入场间隔 */
#define FLIP_DUR    0.46f      /* 单张翻牌时长（合上 0.23 + 展开 0.23） */
#define FLIP_MIN    0.045f     /* 最窄比例：留一点厚度，别真画成 0 宽 */

/* 返回水平压缩比例 k（1 = 满宽）；tOut 给出该卡的归一化进度 */
static float flipK(int i, float *tOut)
{
    float t = (gScreenT - (float)i * FLIP_LEAD) / FLIP_DUR;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (tOut) *tOut = t;
    return fabsf(2.0f * t - 1.0f);
}

/* 世界层：卡片底板、色条、图标（跟随 2x 超采样，边缘平滑） */
/* ==================== 庆典演出：绘制部分 ==================== */

/* ① 卡片后面：旋转光扇 + 外扩光环 + 顶部光柱 */

/* ==================== 档位徽章 + 光效贴图（2026-09-21 新增素材的接入） ====================
   两条原则：
     ① **缺图为纯回退**：徽章没加载出来就只画文字（原来的样子）；
        光效没加载出来，庆典依然完整 —— 光扇/冲击波/火花本来就是程序化画的。
        所以整包缺这 14 张也跑得起来，不会出现"少一张图界面就空掉"。
     ② 位置/尺寸只有一处定义：徽章边长用 BADGE_ICON，档位名文字的 x 由它推出来，
        避免两处写死导致徽章和文字错位。 */
/* 档位徽章的显示边长（逻辑像素）。
   ⚠️ 位置不能放在卡片左上角的档位标签栏里，这是实测出来的结论：
      卡片左上角是 **圆角**，而档位标签栏本身只有约 14 逻辑像素高；
      在圆角的切角范围内（卡内约 28 逻辑像素）标签栏被切得只剩约 9 像素，
      徽章放进去必然"上半截戳出卡片"（抓帧能清楚看到徽章顶着一块卡外底色）。
      试过缩到 20px 与 16px：要么仍被切，要么小到认不出。
   所以改成游戏卡牌里最常见的做法 —— **档位标记放在卡片右上角内侧**：
   那里已经越过了圆角、且不与档位名/价格/卡名/描述任何一项抢位置。
   26 是在卡宽 256 下"一眼可辨"的尺寸。 */
#define BADGE_ICON 26.0f

static void drawTierBadge(HDC dc, int tier, float x, float y, float size)
{
    Sprite *s;
    if (tier < 0 || tier >= RGQ_COUNT) return;
    s = &gSprBadge[tier];
    if (!s->ok) return;
    /* spriteBlit 把贴图**底边**对齐到 baseY、水平居中到 cx，
       所以这里要把 (x, y) 这个"上左角"换算成 (中心, 底边)。 */
    spriteBlit(dc, s, x + size * 0.5f, y + size,
               size / ((float)s->w / (float)SS),
               size / ((float)s->h / (float)SS), -1);
}

/* 光效贴图叠加：以 (cx, cy) 为中心、边长 size、整体透明度 alpha。
   素材是"黑底 luma key"抠出来的（见 art/_gen_badges.py 的说明），
   边缘自带渐隐，直接 AlphaBlend 就能融进背景，不需要加色混合。 */
/* 测试钩子：把光效叠加整体关掉，用来做"加了贴图 vs 没加贴图"的逐像素 A/B。
   为什么不做成环境变量：环境变量是运行时的隐藏开关，容易在发布版里被误触发；
   普通全局变量只有测试代码会写，语义清楚。 */
static int gFxOverlayOff = 0;

static void fxOverlay(HDC dc, int idx, float cx, float cy, float size, int alpha)
{
    Sprite *s;
    if (gFxOverlayOff) return;
    if (idx < 0 || idx >= FSX_N || alpha <= 0) return;
    s = &gSprFx[idx];
    if (!s->ok) return;
    spriteBlit(dc, s, cx, cy + size * 0.5f,
               size / ((float)s->w / (float)SS),
               size / ((float)s->h / (float)SS), alpha);
}

static void drawCelebBack(HDC dc)
{
    float p, x, y, cx, cy, rot, fade;
    /* ⚠️ 必须**显式**设置世界变换，不能依赖"上一步留下的 DC 状态"。
       踩过：庆典的东西位置与尺度全错（画面出现 1x 与 2x 两套重影）——
       前面某个绘制阶段把 DC 变换复位成单位变换却没还原，
       而本函数用裸 GDI 图元（fillRect / fillCircle / poly），
       它们吃 DC 当下的变换，不是 gCurXF。
       显式设一次，开销可忽略，也不受调用顺序影响。 */
    SetWorldTransform(dc, &gCurXF);


    COLORREF tc;
    int k;

    if (!celebActive()) return;
    p  = celebP();
    tc = RGQ_COL[gCelebTier];
    draftCardRect(gCelebSlot, &x, &y);
    cx = x + CARD_W * 0.5f;
    cy = y + CARD_H * 0.5f;

    /* 顶部光柱：从画面上沿倾泻到卡心。
       ⚠️ 宽度要克制（早先 150+60 太粗，白色光柱直接把卡片糊掉），
          并且 p>0.40 就基本收掉 —— 演出必须让位给"能看清卡片"。 */
    {
        float f = 1.0f - CLAMP((p - 0.30f) / 0.30f, 0.0f, 1.0f);
        float w = (gCelebPrim ? 96.0f : 76.0f) + 30.0f * f;
        if (f > 0.01f) {
            fillRectA(dc, cx - w, 0.0f, cx + w, cy, tint(tc, 0.40f), (int)(52.0f * f));
            fillRectA(dc, cx - w * 0.30f, 0.0f, cx + w * 0.30f, cy,
                      RGB(255, 252, 240), (int)(72.0f * f));
        }
    }

    /* 旋转光扇：18 道宽窄相间的楔形，整体缓慢自转。
       ⚠️ 半径必须**限制在卡片附近**（早先冲到 2000+，横向光带横扫全屏、
          彻底盖住卡片，肉眼就是"屏幕被白条纹刷满"）。
          现在最外沿只比卡片大一圈，读起来是"卡片在发光"而不是"屏幕在闪"。
       楔形用四点 poly —— 只给三角形做不出有宽度的光柱。 */
    {
        float grow = celebEaseOut(p / 0.40f);
        float r0 = 100.0f;
        float r1 = r0 + (gCelebPrim ? 240.0f : 180.0f) * grow;
        fade = 1.0f - CLAMP((p - 0.26f) / 0.34f, 0.0f, 1.0f);   /* p≈0.60 完全消失 */
        rot  = p * (gCelebPrim ? 0.75f : 0.50f) * 6.28318f
             + (float)(gCelebSeed & 255u) * 0.0246f;
        if (fade > 0.02f) {
            for (k = 0; k < CELEB_RAY_N; k++) {
                float a  = rot + 6.28318f * (float)k / (float)CELEB_RAY_N;
                float aw = (k & 1) ? 0.048f : 0.024f;      /* 宽窄相间 */
                float br = (k & 1) ? 0.40f : 0.20f;        /* 亮暗相间：整体偏暗，别刺眼 */
                float ca = cosf(a),   sa = sinf(a);
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

    /* 外扩光环：一圈离散光点向外推开（GDI 画不了粗圆弧，点阵更省事也更"能量"）。
       半径同样收在卡片附近；始祖多套一圈、推得稍远。 */
    {
        int ring, rings = gCelebPrim ? 2 : 1;
        for (ring = 0; ring < rings; ring++) {
            float ph = CLAMP((p - (float)ring * 0.10f) / 0.48f, 0.0f, 1.0f);
            float rr, f2;
            int n = 34, i2;
            if (ph <= 0.0f || ph >= 1.0f) continue;
            rr = 80.0f + (gCelebPrim ? 300.0f : 230.0f) * celebEaseOut(ph);
            f2 = 1.0f - ph;
            for (i2 = 0; i2 < n; i2++) {
                float a = 6.28318f * (float)i2 / (float)n + rot * 0.7f;
                fillCircle(dc, cx + cosf(a) * rr, cy + sinf(a) * rr * 0.80f,
                           3.0f + 5.0f * f2, tint(tc, 0.15f + 0.40f * f2));
            }
        }
    }

    /* ---- 贴图光效：在程序化光扇/光环之上再叠一层 ------------
       每层都在自己的作用域块里，fade 互不干扰；
       alpha 归零时 fxOverlay 自己 return，省掉一堆"要不要画"的判断。
       半径仍然收在卡片尺度附近（早先光扇铺满全屏、把卡片糊掉的教训）。 */
    {
        float f = 1.0f - CLAMP((p - 0.22f) / 0.40f, 0.0f, 1.0f);
        if (f > 0.02f)
            fxOverlay(dc, FSX_PILLAR, cx, cy, CARD_W * 1.08f, (int)(120.0f * f));
    }
    {
        float grow = celebEaseOut(p / 0.45f);
        float f = 1.0f - CLAMP((p - 0.30f) / 0.36f, 0.0f, 1.0f);
        if (f > 0.02f)
            fxOverlay(dc, FSX_RAYS, cx, cy,
                      150.0f + (gCelebPrim ? 260.0f : 190.0f) * grow,
                      (int)((gCelebPrim ? 150.0f : 110.0f) * f));
    }
    if (gCelebPrim) {                      /* 始祖多一层符文环 */
        float f = 1.0f - CLAMP((p - 0.34f) / 0.42f, 0.0f, 1.0f);
        if (f > 0.02f)
            fxOverlay(dc, FSX_RUNES, cx, cy,
                      150.0f + 220.0f * celebEaseOut(p / 0.50f), (int)(140.0f * f));
    }
}

/* ③ 卡片上面：全屏彩幕 + 冲击波 + 迸射火花 + 卡缘脉冲 */
static void drawCelebFront(HDC dc)
{
    float p, x, y, cx, cy;
    /* ⚠️ 必须**显式**设置世界变换，不能依赖"上一步留下的 DC 状态"。
       踩过：庆典的东西位置与尺度全错（画面出现 1x 与 2x 两套重影）——
       前面某个绘制阶段把 DC 变换复位成单位变换却没还原，
       而本函数用裸 GDI 图元（fillRect / fillCircle / poly），
       它们吃 DC 当下的变换，不是 gCurXF。
       显式设一次，开销可忽略，也不受调用顺序影响。 */
    SetWorldTransform(dc, &gCurXF);

    COLORREF tc;
    int k;

    if (!celebActive()) return;
    p  = celebP();
    tc = RGQ_COL[gCelebTier];
    draftCardRect(gCelebSlot, &x, &y);
    cx = x + CARD_W * 0.5f;
    cy = y + CARD_H * 0.5f;

    /* 全屏彩幕：开头 26% 的强闪，两层（档位色 + 白核）叠出"过曝"感。
       只在开场出现 —— 它盖住一切，所以必须短。 */
    {
        float f = 1.0f - CLAMP(p / 0.26f, 0.0f, 1.0f);
        if (f > 0.01f) {
            fillRectA(dc, 0, 0, VIEW_W, VIEW_H, tint(tc, 0.70f),
                      (int)((gCelebPrim ? 170.0f : 130.0f) * f));
            fillRectA(dc, 0, 0, VIEW_W, VIEW_H, RGB(255, 255, 255),
                      (int)((gCelebPrim ? 110.0f : 72.0f) * f * f));
        }
    }

    /* 卡缘脉冲：卡片外面套几圈逐渐变淡的描边 —— 它绕卡一圈、不挡卡面内容，
       是整段演出里唯一可以从头留到尾的效果（收尾时淡出）。 */
    {
        float pulse = 0.55f + 0.45f * sinf(p * 12.56f);
        float fade  = 1.0f - CLAMP((p - 0.74f) / 0.26f, 0.0f, 1.0f);
        for (k = 0; k < 4; k++) {
            float o = 4.0f + (float)k * 5.0f + 3.0f * pulse;
            strokeRound(dc, x - o, y - o, x + CARD_W + o, y + CARD_H + o,
                        18.0f + o, tint(tc, (0.55f - 0.12f * (float)k) * fade),
                        (k == 0) ? 4 : 3);
        }
    }

    /* 冲击波：从卡心推出去的一圈点。
       ⚠️ 半径压到卡片尺度附近（早先 780 直接推出屏幕外，看不出是"从卡上炸开"）。 */
    {
        float ph = CLAMP(p / 0.46f, 0.0f, 1.0f);
        if (ph > 0.0f && ph < 1.0f) {
            float rr = 56.0f + (gCelebPrim ? 380.0f : 290.0f) * celebEaseOut(ph);
            float f  = 1.0f - ph;
            int n = 42, i2;
            for (i2 = 0; i2 < n; i2++) {
                float a = 6.28318f * (float)i2 / (float)n;
                fillCircle(dc, cx + cosf(a) * rr, cy + sinf(a) * rr * 0.80f,
                           5.5f * f + 1.5f, tint(tc, 0.45f + 0.35f * f));
            }
        }
    }

    /* 迸射火花：26/40 条带拖尾的亮点，角度/速度/延迟全部由 gCelebSeed 决定。
       固定种子 → 抓帧可复现；也保证同一次演出形状稳定，不会每帧乱跳。
       距离同样收在卡片附近（早先 380+255 会飞到屏幕外）。 */
    {
        int sparks = gCelebPrim ? 36 : 24;
        for (k = 0; k < sparks; k++) {
            unsigned h = gCelebSeed + (unsigned)k * 2246822519u;
            float a   = (float)((h >> 8) & 1023u) / 1023.0f * 6.28318f;
            float sp  = (gCelebPrim ? 230.0f : 170.0f) + (float)((h >> 18) & 127u);
            float dly = (float)((h >> 3) & 31u) / 31.0f * 0.20f;
            float lp  = (p - dly) / 0.50f;
            float d, f;
            if (lp <= 0.0f || lp >= 1.0f) continue;
            f = 1.0f - lp;
            d = sp * celebEaseOut(lp);
            fillCircle(dc, cx + cosf(a) * d, cy + sinf(a) * d * 0.72f,
                       5.0f * f + 1.5f, tint(tc, 0.62f));
            fillCircle(dc, cx + cosf(a) * d * 0.78f, cy + sinf(a) * d * 0.78f * 0.72f,
                       3.2f * f + 1.0f, tint(tc, 0.30f));
        }
    }

    /* ---- 贴图光效（压在卡片与彩幕之上）----
       顺序：闪斑（开场一击）→ 冲击波（外扩）→ 火花（迸射）。
       闪斑刻意做得大（180→400）：它和全屏彩幕一起构成"过曝"那一瞬，
       是整个演出里最有冲击力的一帧。 */
    {
        float f = 1.0f - CLAMP(p / 0.20f, 0.0f, 1.0f);
        if (f > 0.02f)
            fxOverlay(dc, FSX_FLARE, cx, cy, 180.0f + 220.0f * (1.0f - f),
                      (int)((gCelebPrim ? 235.0f : 190.0f) * f));
    }
    {
        float ph = CLAMP(p / 0.46f, 0.0f, 1.0f);
        if (ph > 0.0f && ph < 1.0f)
            fxOverlay(dc, FSX_SHOCK, cx, cy,
                      60.0f + (gCelebPrim ? 420.0f : 320.0f) * celebEaseOut(ph),
                      (int)(190.0f * (1.0f - ph)));
    }
    {
        float lp = CLAMP((p - 0.06f) / 0.50f, 0.0f, 1.0f);
        if (lp > 0.0f && lp < 1.0f)
            fxOverlay(dc, FSX_SPARK, cx, cy,
                      80.0f + (gCelebPrim ? 380.0f : 300.0f) * celebEaseOut(lp),
                      (int)(175.0f * (1.0f - lp)));
    }
}

/* ④ 文字层：档位横幅 + 卡名 + 副标题。字体档位切换伪造"弹出"（GDI 不能缩放字体） */
static void drawCelebText(HDC dc)
{
    float p, x, y, cx, bar;
    /* ⚠️ 必须**显式**设置世界变换，不能依赖"上一步留下的 DC 状态"。
       踩过：庆典的东西位置与尺度全错（画面出现 1x 与 2x 两套重影）——
       前面某个绘制阶段把 DC 变换复位成单位变换却没还原，
       而本函数用裸 GDI 图元（fillRect / fillCircle / poly），
       它们吃 DC 当下的变换，不是 gCurXF。
       显式设一次，开销可忽略，也不受调用顺序影响。 */
    SetWorldTransform(dc, &gCurXF);

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
    /* ⚠️ 必须**显式**设置世界变换，不能依赖"上一步留下的 DC 状态"。
       踩过：庆典的东西位置与尺度全错（画面出现 1x 与 2x 两套重影）——
       前面某个绘制阶段把 DC 变换复位成单位变换却没还原，
       而本函数用裸 GDI 图元（fillRect / fillCircle / poly），
       它们吃 DC 当下的变换，不是 gCurXF。
       显式设一次，开销可忽略，也不受调用顺序影响。 */
    SetWorldTransform(dc, &gCurXF);

    float pulse;
    if (gPrimordialAlive <= 0) return;
    pulse = 0.55f + 0.45f * sinf(gTime * 3.4f);
    wsprintfW(buf, L"始祖威压 ×%d", gPrimordialAlive);
    fillRectA(dc, VIEW_W * 0.5f - 104.0f, 6.0f, VIEW_W * 0.5f + 104.0f, 30.0f,
              RGB(24, 16, 8), (int)(150.0f * pulse + 40.0f));
    putTextCS(dc, gF15, RGB(255, 226, 150), RGB(60, 40, 10), buf,
              VIEW_W * 0.5f, 18.0f);
}

static void drawDraftShapes(HDC dc)
{
    int i;
    for (i = 0; i < DRAFT_N; i++) {
        float x, y, cx, ft, k;
        XFORM saved, fx;
        int rid = gDraft[i];
        int hov = (gDraftHover == i);
        int isPlant;
        int isGrowth = DRAFT_IS_GROWTH(rid);
        int isRg  = DRAFT_IS_RG(rid);                /* 肉鸽植物三选一卡 */
        int rgIdx = isRg ? (rid - RG_CARD_BASE) : -1;
        int face;                                  /* 1 = 已翻到正面 */
        COLORREF rc, cc;

        k = flipK(i, &ft);
        if (ft <= 0.001f) continue;                 /* 还没轮到这张卡 */
        face = (ft >= 0.5f);
        isPlant = DRAFT_IS_PLANT(rid) || isRg;
        if (k < FLIP_MIN) k = FLIP_MIN;

        draftCardRect(i, &x, &y);
        cx = x + CARD_W * 0.5f;

        /* 以卡中心为轴水平压缩 k 倍。saved 里已经含了 2x 缩放和震屏偏移，
           所以直接改 eM11 并按 (1-k) 补 eDx 即可，不会把震屏吃掉。 */
        saved = gCurXF;
        fx = saved;
        fx.eM11 = saved.eM11 * k;
        fx.eDx  = saved.eDx + cx * saved.eM11 * (1.0f - k);
        gCurXF = fx;
        SetWorldTransform(dc, &fx);

        /* 投影：两张卡都画，翻转时跟着一起变窄 */
        fillRound(dc, x + 5, y + 8, x + CARD_W + 5, y + CARD_H + 8, 18, RGB(22, 36, 26));

        if (!face) {
            /* ---------- 卡背：木纹底板 + 徽记 ---------- */
            cardFaceV(dc, x, y, x + CARD_W, y + CARD_H, 18,
                      RGB(104, 70, 40), RGB(50, 32, 18), 1.0f);
            strokeRound(dc, x, y, x + CARD_W, y + CARD_H, 18, RGB(38, 24, 14), 3);
            strokeRound(dc, x + 9, y + 9, x + CARD_W - 9, y + CARD_H - 9, 12,
                        RGB(186, 146, 88), 2);
            /* 四角铆钉 */
            fillCircle(dc, x + 22, y + 22, 5.0f, RGB(206, 170, 106));
            fillCircle(dc, x + CARD_W - 22, y + 22, 5.0f, RGB(206, 170, 106));
            fillCircle(dc, x + 22, y + CARD_H - 22, 5.0f, RGB(206, 170, 106));
            fillCircle(dc, x + CARD_W - 22, y + CARD_H - 22, 5.0f, RGB(206, 170, 106));
            /* 中央徽记：有卡背资源就用资源，没有就用矢量拼一个菱形 */
            if (gSprCardBack.ok) {
                float es = (CARD_W * 0.60f) / ((float)gSprCardBack.w / (float)SS);
                spriteBlit(dc, &gSprCardBack, cx, y + CARD_H * 0.5f +
                           (float)gSprCardBack.h / (float)SS * es * 0.5f,
                           es * k, es, -1);
            } else {
                float my = y + CARD_H * 0.5f, r = 62.0f;
                POINT p[4];
                p[0].x = (int)cx;      p[0].y = (int)(my - r);
                p[1].x = (int)(cx + r);p[1].y = (int)my;
                p[2].x = (int)cx;      p[2].y = (int)(my + r);
                p[3].x = (int)(cx - r);p[3].y = (int)my;
                poly(dc, p, 4, RGB(150, 110, 62), 1, RGB(214, 176, 108), 3);
            }
            if (hov) strokeRound(dc, x + 8, y + 8, x + CARD_W - 8, y + CARD_H - 8, 12,
                                 RGB(214, 176, 108), 2);
            gCurXF = saved;
            SetWorldTransform(dc, &saved);
            continue;
        }

        /* ---------- 正面 ---------- */
        if (isGrowth) {
            int gi = DRAFT_GROWTH_ID(rid);
            rc = growthDefs[gi].col;
            cc = tint(rc, -0.18f);
        } else if (isPlant) {
            int pid = isRg ? (int)rgPlants[rgIdx].base : DRAFT_PLANT_ID(rid);
            if (isRg) {                            /* 肉鸽植物：用品质色做条 */
                rc = cc = RGQ_COL[rgPlants[rgIdx].tier];
            } else if (pid >= PT_COUNT) {          /* 植物精华兜底卡 */
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
        /* 卡面：纵向渐变；遗物卡是暖羊皮纸（叠木纹/纸纹），
           植物卡是深紫（不叠木纹，免得和金色光环打架） */
        if (isGrowth) {
            /* 成长卡用深色植物图鉴底，既能托住高饱和插画，也避开
               GradientFill 在翻牌横向变换下产生的浅色矩形残片。 */
            fillRound(dc, x, y, x + CARD_W, y + CARD_H, 18,
                      hov ? RGB(27, 52, 36) : RGB(18, 36, 26));
            fillRound(dc, x + 7, y + 70, x + CARD_W - 7, y + 252, 14,
                      hov ? tint(rc, -0.72f) : RGB(20, 43, 30));
            strokeRound(dc, x + 7, y + 70, x + CARD_W - 7, y + 252, 14,
                        tint(rc, -0.25f), 2);
            fillEllipse(dc, x + CARD_W * 0.5f, y + 169.0f, 88.0f, 69.0f,
                        tint(rc, -0.76f));
            fillEllipse(dc, x + CARD_W * 0.5f, y + 169.0f, 72.0f, 56.0f,
                        tint(rc, -0.67f));
        } else {
            cardFaceV(dc, x, y, x + CARD_W, y + CARD_H, 18,
                      isPlant ? (hov ? RGB(62, 52, 82) : RGB(44, 38, 62))
                              : (hov ? RGB(255, 253, 245) : RGB(252, 248, 234)),
                      isPlant ? (hov ? RGB(30, 24, 42) : RGB(21, 17, 32))
                              : (hov ? RGB(236, 226, 196) : RGB(224, 214, 188)),
                      isPlant ? 0.0f : 0.55f);
        }
        fillRound(dc, x, y, x + CARD_W, y + 18, 18, rc);
        fillRect(dc, x, y + 9, x + CARD_W, y + 18, rc);
        strokeRound(dc, x, y, x + CARD_W, y + CARD_H, 18,
                    hov ? rc : tint(rc, -0.28f), hov ? 5 : 3);
        if (isGrowth) {
            int gi = DRAFT_GROWTH_ID(rid);
            if (gSprGrowth[gi].ok)
                spriteBlit(dc, &gSprGrowth[gi], x + CARD_W * 0.5f, y + 222.0f,
                           k, 1.0f, -1);
            else
                drawRelicIcon(dc, growthDefs[gi].iconCat, cc,
                              x + CARD_W * 0.5f, y + 196.0f, 52.0f);
        } else if (isPlant) {
            /* 神级植物：直接画植物本体 + 一圈稀有度光环 */
            int pid = isRg ? (int)rgPlants[rgIdx].base : DRAFT_PLANT_ID(rid);
            Sprite *rs = isRg ? rgPlantSprite(rgIdx + 1) : NULL;
            int g2;
            for (g2 = 0; g2 < 3; g2++)
                fillEllipse(dc, x + CARD_W * 0.5f, y + 210.0f - (float)g2 * 5.0f,
                            62.0f - (float)g2 * 12.0f, 17.0f - (float)g2 * 3.5f,
                            (g2 & 1) ? tint(rc, 0.45f) : tint(rc, -0.15f));
            /* 精灵不吃世界变换，横向缩放要自己乘 k */
            if (rs) {                              /* 肉鸽专属贴图 */
                spriteBlit(dc, rs, x + CARD_W * 0.5f, y + 246.0f,
                           0.82f * k, 0.82f, -1);
            } else if (pid >= PT_COUNT) {          /* 植物精华：画一颗太阳 */
                if (gSprSun.ok)
                    spriteBlit(dc, &gSprSun, x + CARD_W * 0.5f, y + 246.0f,
                               0.82f * k, 0.82f, -1);
                else
                    drawRelicIcon(dc, 4, cc, x + CARD_W * 0.5f, y + 196.0f, 52.0f);
            } else if (gSprPlant[pid].ok) {
                spriteBlit(dc, &gSprPlant[pid], x + CARD_W * 0.5f, y + 246.0f,
                           0.82f * k, 0.82f, -1);
            } else {
                drawRelicIcon(dc, 4, cc, x + CARD_W * 0.5f, y + 196.0f, 52.0f);
            }
        } else {
            drawRelicIcon(dc, relicDefs[rid].cat, cc, x + CARD_W * 0.5f, y + 196.0f, 52.0f);
        }
        if (hov) strokeRound(dc, x + 8, y + 8, x + CARD_W - 8, y + CARD_H - 8, 12,
                             tint(rc, 0.45f), 2);

        gCurXF = saved;
        SetWorldTransform(dc, &saved);
    }
    /* 跳过条 */
    {
        float sx = VIEW_W * 0.5f - 200.0f, hover = (mMouseY >= SKIP_Y && mMouseY <= SKIP_Y + SKIP_H &&
                                                    mMouseX >= sx && mMouseX <= sx + 400.0f);
        drawUiButton(dc, sx, SKIP_Y, 400.0f, SKIP_H, BTN_DARK, (int)hover, 1);
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
    for (i = 0; i < GROW_COUNT; i++) n += gGrowthStack[i];
    /* 每波三选一是奖励，不再伪装成会因阳光不足而点不动的货架。 */
    wsprintfW(buf, L"第 %d 波 · 免费奖励，任选一项", gWave);
    putTextCS(dc, gF30, RGB(255, 246, 200), RGB(18, 32, 20), buf, VIEW_W * 0.5f, 100.0f);

    for (i = 0; i < DRAFT_N; i++) {
        float x, y, ft;
        int rid = gDraft[i];
        COLORREF rc;
        /* 翻牌没结束就先不出字：文字层没有 alpha，半边宽度上写满字
           比「晚 0.4 秒出现」难看得多。等 k 回到 1（正面满宽）再画。 */
        if (flipK(i, &ft) < 1.0f) continue;
        draftCardRect(i, &x, &y);
        if (DRAFT_IS_GROWTH(rid)) {
            int gi = DRAFT_GROWTH_ID(rid);
            rc = growthDefs[gi].col;
            putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), L"成长",
                     x + 12.0f, y + 2.0f, 0);
            wsprintfW(buf, L"第 %d 层", gGrowthStack[gi] + 1);
            putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), buf,
                     x + CARD_W - 14.0f, y + 2.0f, 2);
            putTextCS(dc, gF22, RGB(248, 246, 224), RGB(12, 25, 17), growthDefs[gi].name,
                      x + CARD_W * 0.5f, y + 54.0f);
            putTextWrap(dc, gF15, RGB(194, 218, 184), growthDefs[gi].desc,
                        x + 24.0f, y + 268.0f, CARD_W - 48.0f, 22.0f, 3);
            wsprintfW(buf, L"当前 %d 层  →  %d 层", gGrowthStack[gi], gGrowthStack[gi] + 1);
            putTextCS(dc, gF15, tint(rc, 0.42f), RGB(12, 25, 17), buf,
                      x + CARD_W * 0.5f, y + 238.0f);
        } else if (DRAFT_IS_RG(rid)) {
            /* ---- 肉鸽植物卡：品质 + 名称 + 效果 ----
               当前每波三选一都是免费奖励。价格分支仅作为未来商店模式的兼容通道。 */
            int ri     = rid - RG_CARD_BASE;
            int tier   = rgPlants[ri].tier;
            int cost   = rgPlantCost(ri);
            /* ⚠️ 始祖是**唯一**不享受"每波免费"的档位：
               它的售价 10000 是真要付的，否则那个数字没有任何意义。
               其余档位维持原设计（每波奖励免费领）。 */
            int isPrim = (tier == RGQ_PRIMORDIAL);
            int canBuy = (gDraftFree && !isPrim) || rgOwned(ri) || (gSun >= cost);
            rc = RGQ_COL[tier];
            /* 档位名保持原位（不右移：右移会撞到右侧的价格文字）。 */
            wsprintfW(buf, L"%s", RGQ_NAME[tier]);
            putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), buf,
                     x + 12.0f, y + 2.0f, 0);
            /* 档位徽章画在**卡片右上角内侧**。
               横向从右边缘回退 15（避开右边框与圆角），
               纵向 y+38 已经越过右上圆角的切角范围，且落在卡名上方。 */
            drawTierBadge(dc, tier,
                          x + CARD_W - 15.0f - BADGE_ICON, y + 38.0f, BADGE_ICON);
            if (isPrim && !rgOwned(ri)) {
                /* 始祖的价格用**金色**常亮显示，不跟随免费/付费分支 ——
                   它永远是要花钱的，而且价格高到一眼就能认出来。 */
                putTextS(dc, gF15, canBuy ? RGB(255, 236, 160) : RGB(255, 120, 120),
                         tint(rc, -0.5f), L"始祖价 10000", x + CARD_W - 14.0f, y + 2.0f, 2);
            } else if (gDraftFree) {
                putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), L"免费",
                         x + CARD_W - 14.0f, y + 2.0f, 2);
            } else if (rgOwned(ri)) {
                putTextS(dc, gF15, RGB(210, 255, 190), tint(rc, -0.5f), L"已有",
                         x + CARD_W - 14.0f, y + 2.0f, 2);
            } else {
                wsprintfW(buf, L"阳光 %d", cost);
                putTextS(dc, gF15, canBuy ? RGB(255, 240, 150) : RGB(255, 138, 138),
                         tint(rc, -0.5f), buf, x + CARD_W - 14.0f, y + 2.0f, 2);
            }
            putTextCS(dc, gF22, RGB(255, 244, 214), RGB(40, 26, 60), rgPlants[ri].name,
                      x + CARD_W * 0.5f, y + 54.0f);
            putTextWrap(dc, gF15, RGB(212, 204, 232), rgPlants[ri].desc,
                        x + 22.0f, y + 268.0f, CARD_W - 44.0f, 22.0f, 4);
            /* 注意：wsprintfW 不支持 %f（会输出乱码并让后面的 %d 串位），
               所以倍率取整后再用 %d 打印。 */
            wsprintfW(buf, L"杀伤 %d×　能力 %d 条",
                      (int)rgDmgMul(tier), rgPlantTraitCount(ri));
            putTextCS(dc, gF15, tint(rc, 0.35f), RGB(40, 26, 60), buf,
                      x + CARD_W * 0.5f, y + 264.0f);
            if (!canBuy)
                putTextCS(dc, gF15, RGB(255, 118, 118), RGB(40, 26, 60), L"阳光不足",
                          x + CARD_W * 0.5f, y + 232.0f);
        } else if (DRAFT_IS_PLANT(rid)) {
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
    /* 元界面先完整覆盖游戏世界。新背景是 2000x1300 的 2x 母版，
       用同一张安静的温室墙承接选关 / 图鉴 / 成就，缺图时仍走旧底色。 */
    if (gSprMetaBg.ok)
        spriteBlit(dc, &gSprMetaBg, VIEW_W * 0.5f, VIEW_H, 1.0f, 1.0f, -1);
    else
        fillRect(dc, 0, 0, VIEW_W, VIEW_H, RGB(16, 26, 20));

    /* 左上角返回按钮。画在所有分支之前 —— 它是元界面的公共 UI，
       不能因为某个界面（例如抽卡）自己 early return 就消失。 */
    {
        int hot = (mMouseX >= (int)BACKBTN_X && mMouseX <= (int)(BACKBTN_X + BACKBTN_W) &&
                   mMouseY >= (int)BACKBTN_Y && mMouseY <= (int)(BACKBTN_Y + BACKBTN_H));
        drawUiButton(dc, BACKBTN_X, BACKBTN_Y, BACKBTN_W, BACKBTN_H,
                     BTN_DARK, hot, 1);
        if (gSprUiIcon[UII_BACK].ok)
            drawUiIcon(dc, UII_BACK, BACKBTN_X + 23.0f,
                       BACKBTN_Y + BACKBTN_H * 0.5f, hot ? 30.0f : 27.0f, -1);
        /* 18pt 文字实际高约 25px，按钮高 36 → 上边距 5 才是视觉居中 */
        putTextS(dc, gF18, RGB(242, 255, 238), RGB(20, 34, 24), L"返回",
                 BACKBTN_X + 78.0f, BACKBTN_Y + 5.0f, 1);
    }

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
            /* 星级使用统一徽章，不再用三个纯色圆点占位。 */
            { int s; for (s = 0; s < 3; s++)
                drawUiIcon(dc, UII_STAR,
                           x + LV_CARD_W - 26.0f - (float)s * 22.0f, y + 22.0f,
                           18.0f, s < done ? 255 : 70); }
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
            drawUiIcon(dc, UII_STAR, x + 22.0f, y + 21.0f, 25.0f, got ? 255 : 65);
        }
    } else if (gState == ST_GACHA) {
        float bx = VIEW_W * 0.5f - 150.0f;
        int can = (gSave.coins >= 1);
        {
            int hot = can && (mMouseX >= (int)bx && mMouseX <= (int)(bx + 300.0f) &&
                              mMouseY >= (int)GACHA_BTN_Y && mMouseY <= (int)(GACHA_BTN_Y + 64.0f));
            drawUiButton(dc, bx, GACHA_BTN_Y, 300.0f, 64.0f,
                         BTN_PURPLE, hot, can);
        }
        /* 三档概率不再用三个纯色圆占位：共用种子档案袋母版，
           稀有度由背后的光晕区分，保持包装统一且不额外占三张大图。 */
        {
            int k;
            const COLORREF rarityCol[3] = {
                RGB(170, 174, 166), RGB(78, 154, 238), RGB(244, 188, 58)
            };
            for (k = 0; k < 3; k++) {
                float cx = VIEW_W * 0.5f - 120.0f + (float)k * 120.0f;
                float bob = sinf(gTime * 1.7f + (float)k * 1.8f) * 3.0f;
                fillCircle(dc, cx, 187.0f + bob, 46.0f, tint(rarityCol[k], -0.42f));
                fillCircle(dc, cx, 187.0f + bob, 40.0f, tint(rarityCol[k], -0.12f));
                if (gSprCardBack.ok)
                    spriteBlit(dc, &gSprCardBack, cx, 239.0f + bob,
                               0.38f, 0.38f, -1);
                else
                    fillCircle(dc, cx, 187.0f + bob, 31.0f, rarityCol[k]);
            }
        }
    } else if (gState == ST_LOADOUT) {
        int first = gLoadoutPage * LO_VISIBLE_SLOTS;
        int last = first + LO_VISIBLE_SLOTS;
        int s;                                   /* 网格格位循环变量 */
        int prevHot = mMouseX >= (int)LO_PAGE_PREV_X && mMouseX <= (int)(LO_PAGE_PREV_X + LO_PAGE_BTN_W) &&
                      mMouseY >= (int)LO_PAGE_BTN_Y && mMouseY <= (int)(LO_PAGE_BTN_Y + LO_PAGE_BTN_H);
        int nextHot = mMouseX >= (int)LO_PAGE_NEXT_X && mMouseX <= (int)(LO_PAGE_NEXT_X + LO_PAGE_BTN_W) &&
                      mMouseY >= (int)LO_PAGE_BTN_Y && mMouseY <= (int)(LO_PAGE_BTN_Y + LO_PAGE_BTN_H);
        if (last > loCount()) last = loCount();
        drawUiButton(dc, LO_PAGE_PREV_X, LO_PAGE_BTN_Y, LO_PAGE_BTN_W, LO_PAGE_BTN_H,
                     BTN_DARK, prevHot, gLoadoutPage > 0);
        drawUiButton(dc, LO_PAGE_NEXT_X, LO_PAGE_BTN_Y, LO_PAGE_BTN_W, LO_PAGE_BTN_H,
                     BTN_DARK, nextHot,
                     last < loCount());
        /* ⚠️ 循环变量是「格位 s」而不是植物 id：已下架的植物被压实表过滤掉了，
           直接用 id 遍历会在网格里留下 4 个空洞。 */
        for (s = first; s < last; s++) {
            float x, y;
            int pid = loId(s);
            int own, sel, hov;
            if (pid < 0) continue;
            own = gSave.plantOwned[pid];
            sel = gSave.loadout[pid];
            hov = (loHit(mMouseX, mMouseY) == pid);
            loLayout(s, &x, &y);
            {
                COLORREF base = !own ? RGB(44, 44, 42)
                               : (sel ? (hov ? RGB(92, 132, 72) : RGB(70, 104, 58))
                                      : (hov ? RGB(62, 68, 60) : RGB(48, 54, 48)));
                cardFaceV(dc, x, y, x + LO_CARD_W, y + LO_CARD_H, 12,
                          tint(base, 0.18f), tint(base, -0.28f), 0.0f);
            }
            strokeRound(dc, x, y, x + LO_CARD_W, y + LO_CARD_H, 12,
                        !own ? RGB(72, 72, 70)
                             : (sel ? RGB(190, 244, 150) : RGB(96, 104, 96)),
                        sel ? 5 : 2);
            /* 资源原本已经存在，只是旧编组页没有绘制它们，导致整页像空框。
               缩略图留出底部 28px 给植物名，选中勾仍压在右上角。 */
            if (own)
                drawIcon(dc, pid, x + LO_CARD_W * 0.5f, y + 49.0f,
                         hov ? 64.0f : 60.0f);
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
                else {
                    /* 卡背木纹回退，保持与抽卡卡一致 */
                    cardFaceV(dc, x, y, x + RW_CARD_W, y + RW_CARD_H, 14,
                              RGB(104, 70, 40), RGB(50, 32, 18), 1.0f);
                    strokeRound(dc, x, y, x + RW_CARD_W, y + RW_CARD_H, 14, RGB(38, 24, 14), 3);
                }
            } else {
                if (gSprBurst.ok)
                    spriteBlit(dc, &gSprBurst, x + RW_CARD_W * 0.5f,
                               y + RW_CARD_H * 0.5f + 46.0f, 0.80f, 0.80f, -1);
                cardFaceV(dc, x, y, x + RW_CARD_W, y + RW_CARD_H, 14,
                          RGB(255, 253, 245), RGB(224, 214, 188), 0.55f);
                strokeRound(dc, x, y, x + RW_CARD_W, y + RW_CARD_H, 14, RGB(240, 196, 90), 5);
            }
        }
    } else if (gState == ST_DAILY) {
        /* 每日营地：签到面板 / 每日挑战面板 / 纪录墙 */
        int signedIn = (gSave.lastCheckIn == todayYMD());
        int dailyDone = (gSave.dailyDoneDate == todayYMD());
        /* 签到与每日挑战本质上也是大按钮：统一使用木牌材质与交互状态。 */
        {
            int hotSign = !signedIn && mMouseX >= 370 && mMouseX <= 630 &&
                          mMouseY >= 270 && mMouseY <= 370;
            int hotDaily = !dailyDone && mMouseX >= 370 && mMouseX <= 630 &&
                           mMouseY >= 385 && mMouseY <= 510;
            drawUiButton(dc, 370.0f, 270.0f, 260.0f, 100.0f,
                         signedIn ? BTN_GREEN : BTN_GOLD, hotSign, 1);
            drawUiButton(dc, 370.0f, 385.0f, 260.0f, 125.0f,
                         dailyDone ? BTN_GREEN : BTN_BLUE, hotDaily, 1);
        }
        /* 纪录墙（深色面板） */
        fillRound(dc, 120.0f, 530.0f, 880.0f, 640.0f, 14, RGB(40, 52, 44));
        strokeRound(dc, 120.0f, 530.0f, 880.0f, 640.0f, 14, RGB(120, 160, 130), 2);
    }
}

static void drawMetaText(HDC dc)
{
    wchar_t buf[256];
    int i, n;
    /* 通用标题链里没有 ST_REWARD —— 它原先落到默认值「植物抽卡」，
       然后 ST_REWARD 分支又在同一个 (x=500, y=62) 位置画了一遍自己的标题，
       两条 30pt 大字 100% 叠死。奖励界面的标题交给它自己的分支画。 */
    if (gState != ST_REWARD)
        putTextCS(dc, gF30, RGB(255, 246, 200), RGB(18, 32, 20),
                  gState == ST_LEVELS ? L"选择关卡" :
                  gState == ST_TALENTS ? L"天赋树"   :
                  gState == ST_ACH     ? L"成就"     :
                  gState == ST_DAILY   ? L"每日营地" :
                  gState == ST_LOADOUT ? L"植物编组" : L"植物抽卡",
                  VIEW_W * 0.5f, META_TITLE_Y);
    wsprintfW(buf, L"星星 %d　金币 %d", gSave.stars, gSave.coins);
    if (gState == ST_LOADOUT) {
        /* 编组界面这一行要和右侧的操作提示共用一行，所以改成左对齐；
           两行都居中时会在 x=500 完全叠死（实测重叠比 100%）。 */
        putTextS(dc, gF18, RGB(255, 226, 150), RGB(18, 32, 20), buf, LO_X0, 85.0f, 0);
    } else {
        putTextCS(dc, gF18, RGB(255, 226, 150), RGB(18, 32, 20), buf, VIEW_W * 0.5f, 90.0f);
    }

    if (gState == ST_LEVELS) {
        for (i = 0; i < LV_COUNT; i++) {
            float x, y;
            int ok = levelPlayable(i);
            metaLayoutLevel(i, &x, &y);
            putTextS(dc, gF18, ok ? RGB(240, 255, 230) : RGB(140, 140, 138),
                     RGB(18, 32, 20), levelDefs[i].name, x + 16.0f, y + 26.0f, 0);
            putTextS(dc, gF15, ok ? RGB(190, 220, 180) : RGB(120, 120, 118),
                     RGB(18, 32, 20), levelDefs[i].desc, x + 16.0f, y + 50.0f, 0);
            /* 无尽关显示它的通关里程碑而不是 999 —— 玩家要能看出"打到哪算赢" */
            wsprintfW(buf, L"%d 波", levelClearWaves(i));
            putTextS(dc, gF15, RGB(220, 210, 160), RGB(18, 32, 20), buf, x + 16.0f, y + 74.0f, 0);
            if (!ok) {
                /* 解锁条件已改成纯通关制，所以提示也要跟着改 ——
                   还写"需要 N 星解锁"会让玩家去刷星星，而星星现在根本不解锁关卡。 */
                putTextS(dc, gF15, RGB(255, 180, 140), RGB(18, 32, 20),
                         L"通关上一关后解锁", x + LV_CARD_W - 16.0f, y + 74.0f, 2);
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
                     RGB(18, 32, 20), achName[i], x + 42.0f, y + 5.0f, 0);
            putTextS(dc, gF15, gSave.ach[i] ? RGB(170, 220, 150) : RGB(140, 140, 134),
                     RGB(18, 32, 20), achDesc[i], x + 42.0f, y + 23.0f, 0);
        }
        wsprintfW(buf, L"已完成 %d / %d", n, ACH_COUNT);
        /* y 从 636 抬到 618：15pt 文字在 636 时下沿到 656，越出 650 的画面；
           成就列表最后一行卡片底边在 600，618 不会撞上。 */
        putTextS(dc, gF15, RGB(255, 226, 150), RGB(18, 32, 20), buf, 52.0f, 618.0f, 0);
    } else if (gState == ST_LOADOUT) {
        /* 提示行改成右对齐，和左边的货币行拼成一行。
           每关开局只能带 1 株，所以这里不再写"候选 N / 实战 M"那套双数字，
           直接读 LOADOUT_SLOTS_MAX 报出"已选 1 / 1"。 */
        {
            int pages = (loCount() + LO_VISIBLE_SLOTS - 1) / LO_VISIBLE_SLOTS;
            wchar_t hb[64];
            wsprintfW(hb, L"第 %d / %d 页", gLoadoutPage + 1, pages);
            putTextS(dc, gF15, RGB(220, 240, 210), RGB(18, 32, 20), hb,
                     706.0f, 79.0f, 2);
            putTextCS(dc, gF15, RGB(232, 244, 226), RGB(18, 32, 20),
                      L"上一页", LO_PAGE_PREV_X + LO_PAGE_BTN_W * 0.5f,
                      LO_PAGE_BTN_Y + LO_PAGE_BTN_H * 0.5f);
            putTextCS(dc, gF15, RGB(232, 244, 226), RGB(18, 32, 20),
                      L"下一页", LO_PAGE_NEXT_X + LO_PAGE_BTN_W * 0.5f,
                      LO_PAGE_BTN_Y + LO_PAGE_BTN_H * 0.5f);
        }
        {
            int sel = 0;
            /* 只统计「已拥有 且 在编制里」的普通植物：
               ① 上界用 PT_HERO_FLAME —— 神级植物不在编组界面显示，
                  算进来会让"已选 1/1"配 1 个绿框；
               ② 必须带上 plantOwned —— 旧档里存在 loadout=1 但没解锁的条目
                  （实测索引 77/81 就是这种），只数 loadout 会把它们算进去。
               这两条加起来才能保证"数字 == 画面上的绿框数"。 */
            for (i = 0; i < PT_HERO_FLAME; i++)
                if (!plantRetired(i) && gSave.loadout[i] && gSave.plantOwned[i]) sel++;
            wsprintfW(buf, L"开局出战植物 %d / %d 株　点击卡片切换", sel, LOADOUT_SLOTS_MAX);
            /* y 从 640 抬到 612：22pt 文字画在 640 时下沿到 666，
               已经跑出 650 高的画面（实测越界 4px）。卡组网格底边在 604。 */
            putTextCS(dc, gF22, sel >= LOADOUT_SLOTS_MAX ? RGB(180, 255, 150) : RGB(255, 226, 150),
                      RGB(18, 32, 20), buf, VIEW_W * 0.5f, 612.0f);
        }
        { int first = gLoadoutPage * LO_VISIBLE_SLOTS;
          int last = first + LO_VISIBLE_SLOTS;
          int p;
          if (last > loCount()) last = loCount();
          /* 这里同样按格位遍历再映射回 pid —— 必须和 drawMetaShapes 那边的
             网格完全一致，否则文字会整体错位到相邻的卡片上。 */
          for (p = first; p < last; p++) {
            float x, y;
            int pid = loId(p);
            int own;
            if (pid < 0) continue;
            own = gSave.plantOwned[pid];
            loLayout(p, &x, &y);
            putTextS(dc, gF15, own ? RGB(240, 255, 230) : RGB(150, 150, 148),
                     RGB(18, 32, 20), plantDefs[pid].name, x + LO_CARD_W * 0.5f, y + 92.0f, 1);
            if (!own)
                putTextS(dc, gF15, RGB(180, 180, 176), RGB(18, 32, 20),
                         L"未解锁", x + LO_CARD_W * 0.5f, y + 108.0f, 1);
          }
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
            int total = plantPoolTotal();
            wsprintfW(buf, L"已解锁植物 %d / %d", plantOwnedCount(), total);
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
                      L"每日签到", 500.0f, 294.0f);
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
                      L"每日挑战 · 撑过 N 波", 500.0f, 407.0f);
            wsprintfW(buf, L"今日目标：无尽关撑过 %d 波", target);
            putTextCS(dc, gF18, RGB(230, 240, 255), RGB(18, 30, 42), buf, 500.0f, 438.0f);
            if ((dailySeed / 13) % 2)
                wsprintfW(buf, L"僵尸生命 x%.2f · 速度 x%.2f · 阳光 稀少",
                          1.0f + 0.15f * (float)((dailySeed / 7)  % 3),
                          1.0f + 0.05f * (float)((dailySeed / 11) % 3));
            else
                wsprintfW(buf, L"僵尸生命 x%.2f · 速度 x%.2f · 阳光 正常",
                          1.0f + 0.15f * (float)((dailySeed / 7)  % 3),
                          1.0f + 0.05f * (float)((dailySeed / 11) % 3));
            putTextCS(dc, gF15, RGB(190, 210, 235), RGB(18, 30, 42), buf, 500.0f, 466.0f);
            putTextCS(dc, gF15,
                      dailyDone ? RGB(180, 240, 180) : RGB(255, 226, 150),
                      RGB(18, 30, 42),
                      dailyDone ? L"今日已完成 ✓（奖励已领）" : L"完成奖励：星星 +3 金币 +5",
                      500.0f, 493.0f);
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
    /* 提示要和实际行为一致：现在三条路都能返回 */
    if (gState != ST_DAILY)
        putTextCS(dc, gF15, RGB(180, 200, 180), RGB(18, 32, 20),
                  L"左上角「← 返回」　或　右键 / Esc", VIEW_W * 0.5f,
                  gState == ST_LOADOUT ? 640.0f : 620.0f);
}

/* ---- 大厅：卡组编组（从全部普通植物里选**唯一 1 株**作为每关开局的出战植物） ---- */
static void loLayout(int i, float *x, float *y)
{
    int v = i - gLoadoutPage * LO_VISIBLE_SLOTS;
    *x = LO_X0 + (float)(v % LO_COLS) * (LO_CARD_W + LO_GAP_X);
    *y = LO_Y0 + (float)(v / LO_COLS) * (LO_CARD_H + LO_GAP_Y);
}
/* 编组界面只列普通植物：神级植物是每局随机产物，不属于可编组的池子 */
static int loHit(int mx, int my)
{
    int s, first = gLoadoutPage * LO_VISIBLE_SLOTS;
    int last = first + LO_VISIBLE_SLOTS;
    if (last > loCount()) last = loCount();
    for (s = first; s < last; s++) {
        float x, y;
        int pid = loId(s);
        if (pid < 0) continue;
        loLayout(s, &x, &y);
        if ((float)mx >= x && (float)mx <= x + LO_CARD_W &&
            (float)my >= y && (float)my <= y + LO_CARD_H) return pid;
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
    for (i = 0; i < PT_HERO_FLAME; i++)
        if (!gSave.plantOwned[i] && !plantRetired(i)) pool[n++] = i;
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
        /* 新植物默认加入编组：上限锁 1，所以是"顶替"而不是"追加" */
        loadoutEquipOnly(gRewardPick[idx]);
        { int i, owned = 0; for (i = 0; i < PT_COLLECTIBLE; i++)
              if (!plantRetired(i) && gSave.plantOwned[i]) owned++;
          if (owned >= plantPoolTotal()) achUnlock(5); }
    }
    saveFlush();
}

/* 抽卡：抽一株还没解锁的植物 */
static void doGacha(void)
{
    int pool[PT_COUNT], n = 0, i, target, roll, picked = -1, guard;
    if (gSave.coins < 1) return;
    for (i = 0; i < PT_HERO_FLAME; i++)
        if (!gSave.plantOwned[i] && !plantRetired(i)) pool[n++] = i;
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
    { int owned = 0; for (i = 0; i < PT_COLLECTIBLE; i++)
          if (!plantRetired(i) && gSave.plantOwned[i]) owned++;
      if (owned >= plantPoolTotal()) achUnlock(5); }
    saveFlush();
}

/* ======================================================================== */
/*                                   渲染                                    */
/* ======================================================================== */
#define BAR_X 178.0f
#define BAR_Y 620.0f
#define BAR_W 644.0f
#define BAR_H 20.0f

/* 全屏雷击的可见形态：5 道从顶部落到地面的锯齿雷柱。
   alpha 1→0 递减，越接近结束越细越淡；形状由 seed 决定的局部随机数生成，
   所以同一次雷击的每一帧都是同一道雷（不会闪成噪声）。 */
static void drawFuryBolts(HDC dc, float alpha, int seed)
{
    unsigned s = (unsigned)seed * 2654435761u + 12345u;
    int b, i;
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    for (b = 0; b < 5; b++) {
        float x0 = LAWN_X + 40.0f + (float)(COLS * CELL_W - 80) * (float)b / 4.0f;
        float y  = (float)LAWN_Y - 40.0f;
        int wide = 2 + (int)(6.0f * alpha);
        for (i = 0; i < 9; i++) {
            float nx, ny;
            s = s * 1103515245u + 12345u;
            nx = x0 + (float)((int)((s >> 16) % 41u) - 20);
            ny = y + 50.0f + (float)((int)((s >> 20) % 24u));
            line(dc, x0, y, nx, ny, RGB(255, 250, 226), wide);
            line(dc, x0 + (float)wide, y + 2.0f, nx + (float)wide, ny,
                 RGB(255, 190, 68), (wide > 3) ? wide - 2 : 2);
            x0 = nx; y = ny;
        }
        fillCircle(dc, x0, y, 6.0f + 12.0f * alpha, RGB(255, 226, 150));
        fillCircle(dc, x0, y, 3.0f + 6.0f * alpha, RGB(255, 255, 245));
    }
}


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
    int   mw = levelClearWaves(gCurLevel);      /* 无尽关取通关里程碑，进度条才有终点 */
    float pr = (float)gWave / (float)mw;
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
    drawTerrainOverlay(gWorldDC);        /* 动态地形：随战局变化，每帧照画 */
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

    /* 潜地落点预警：尘土圆环会在僵尸钻出前持续收缩，暂停时也保持可见。 */
    for (i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &zombies[i];
        if (z->active && !z->dead && z->burrowPending) {
            float f = CLAMP(z->burrowT / 1.35f, 0.0f, 1.0f);
            float rr = 22.0f + 15.0f * f;
            float yy = cellBaseY(z->row) - 4.0f;
            fillEllipse(gWorldDC, z->burrowTargetX, yy, rr, 10.0f + 5.0f * f,
                        RGB(82, 58, 34));
            strokeRound(gWorldDC, z->burrowTargetX - rr, yy - 10.0f,
                        z->burrowTargetX + rr, yy + 10.0f, 10.0f,
                        RGB(224, 176, 92), 3);
            line(gWorldDC, z->burrowTargetX, yy - 18.0f,
                 z->burrowTargetX, yy - 34.0f, RGB(255, 218, 116), 3);
        }
    }

    /* 僵尸（按行从远到近）。极端拥挤时同一行几十只会大面积互相遮住，
       只均匀抽样绘制最多 20 只；战斗逻辑、碰撞和血量仍对全部单位运行。 */
    for (r = 0; r < ROWS; r++) {
        int rowCount = 0, acc = 0;
        int drawLimit = (gFxTrailSteps <= 0) ? 20 : MAX_ZOMBIES;
        for (i = 0; i < MAX_ZOMBIES; i++)
            if (zombies[i].active && zombies[i].row == r) rowCount++;
        for (i = 0; i < MAX_ZOMBIES; i++) {
            if (!zombies[i].active || zombies[i].row != r) continue;
            if (rowCount > drawLimit) {
                acc += drawLimit;
                if (acc < rowCount) continue;
                acc -= rowCount;
            }
            drawZombie(gWorldDC, &zombies[i]);
        }
    }

    /* 豌豆 */
    {
    int peaCount = 0, peaAcc = 0;
    int peaLimit = (gFxTrailSteps <= 0) ? 120 : MAX_PEAS;
    for (i = 0; i < MAX_PEAS; i++) if (peas[i].active) peaCount++;
    for (i = 0; i < MAX_PEAS; i++) {
        Pea *pe = &peas[i];
        COLORREF c, d;
        if (!pe->active) continue;
        if (peaCount > peaLimit) {
            peaAcc += peaLimit;
            if (peaAcc < peaCount) continue;
            peaAcc -= peaCount;
        }
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
            /* 速度拖尾：方向跟着速度向量走。
               以前固定往左拖（x-7 / x-3.5），抛射类（玉米/西瓜/芦苇）和
               斜飞的弹看着像贴了张横向贴纸；改成按速度方向分段递减，
               每段半径和亮度都往下走，才有"锥形尾迹"的读感。
               越快的弹拖得越长，慢弹几乎只剩一个头。 */
            {
                float sp = sqrtf(pe->vx * pe->vx + pe->vy * pe->vy);
                float nx = (sp > 1.0f) ? (pe->vx / sp) : 1.0f;
                float ny = (sp > 1.0f) ? (pe->vy / sp) : 0.0f;
                float L  = 10.0f + CLAMP(sp / 520.0f, 0.0f, 1.0f) * 34.0f;
                float rb = (pe->dmg > 200.0f) ? 9.0f : 6.6f;
                int s2, trailN = gFxTrailSteps;
                for (s2 = trailN; s2 >= 1; s2--) {
                    float u   = (float)s2 / (float)trailN;     /* 1 = 尾迹最末端 */
                    float off = L * u;
                    float rr  = rb * (1.0f - 0.62f * u);
                    fillEllipse(gWorldDC, pe->x - nx * off, pe->y - ny * off,
                                rr * 1.55f, rr, tint(gc, -0.34f + 0.17f * (1.0f - u)));
                }
                /* 弹头根部的亮芯，把拖尾和精灵接起来 */
                fillEllipse(gWorldDC, pe->x - nx * L * 0.16f, pe->y - ny * L * 0.16f,
                            rb * 1.50f, rb * 1.05f, tint(gc, 0.22f));
            }
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
    }

    /* 粒子 */
    for (i = 0; i < gFxParticleLimit; i++) {
        Particle *pt = &parts[i];
        float k;
        if (!pt->active) continue;
        k = pt->life / pt->maxlife;
        fillCircle(gWorldDC, pt->x, pt->y, pt->size * (0.3f + 0.7f * k), pt->col);
    }

    /* 雷击：具体形状由 drawFuryBolts 画（原先把 gFuryT 直接清零、只补一次粒子，
       所以"全屏雷击"在画面上从来没有出现过）。 */
    if (gFuryT > 0.0f)
        drawFuryBolts(gWorldDC, gFuryT / 0.55f, gFuryBolt);

    /* 余烬火焰 */
    drawFires(gWorldDC);

    /* 天气层：飘落的花瓣（画在僵尸之后 = 从近处飘过，但在阳光之前避免挡住可点目标） */
    drawWeather(gWorldDC);

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

    /* 悬停提示在高分辨率文字阶段统一绘制：画在这里会被暂停/结算的
       整屏遮罩盖掉，只剩文字没有底板。 */
    drawHUDShapes(gWorldDC);
    drawProgressBar(gWorldDC);

    /* 三选一：暗幕 + 卡片。GDI 没有 alpha，用隔行扫描线把画面压暗，
       既能把背景压下去，又保留了「游戏还在后面」的临场感。 */
    if (gState == ST_DRAFT) {
        int yy;
        for (yy = 0; yy < VIEW_H; yy += 2)
            fillRect(gWorldDC, 0, yy, VIEW_W, yy + 1, RGB(14, 24, 16));
        drawCelebBack(gWorldDC);       /* 光扇/光环：必须在卡片之下 */
        drawDraftShapes(gWorldDC);
        drawCelebFront(gWorldDC);      /* 闪光/冲击波/火花：压在卡片之上 */
    }
    /* 元界面（选关 / 天赋 / 成就 / 抽卡） */
    if (gState == ST_LEVELS || gState == ST_TALENTS ||
        gState == ST_ACH || gState == ST_GACHA ||
        gState == ST_LOADOUT || gState == ST_REWARD ||
        gState == ST_DAILY) {
        drawMetaShapes(gWorldDC);
    }

    /* 菜单的底色与植物图标画在世界层，与其余精灵共用 2x 坐标约定。 */
    if (gState == ST_MENU) {
        float by = 330.0f;
        gMenuBlockRuns++;
        if (gSprMenuBg.ok) {
            gMenuBgBlits++;
            spriteBlit(gWorldDC, &gSprMenuBg, VIEW_W * 0.5f, VIEW_H,
                       1.0f, 1.0f, -1);
        } else
            fillRect(gWorldDC, 0, 0, VIEW_W, VIEW_H, RGB(20, 44, 26));
        fillRect(gWorldDC, 0, 0, VIEW_W, 5, RGB(150, 104, 62));
        for (i = 0; i < gLoadoutN; i++)
            drawIcon(gWorldDC, gLoadout[i], 130 + i * 82.0f, 480, 58);
        /* 四个入口按钮 */
        { int b;
          const float bx[5] = { 40.0f, 320.0f, 470.0f, 620.0f, 770.0f };
          const float bw[5] = { 260.0f, 130.0f, 130.0f, 130.0f, 190.0f };
          for (b = 0; b < 5; b++) {
              int hov = (mMouseX >= (int)bx[b] && mMouseX <= (int)(bx[b] + bw[b]) &&
                         mMouseY >= (int)by && mMouseY <= (int)(by + 46));
              int style = (b == 0) ? BTN_GREEN : ((b == 4) ? BTN_PURPLE : BTN_DARK);
              drawUiButton(gWorldDC, bx[b], by, bw[b], 46.0f, style, hov, 1);
          }
          {
              static const int mi[5] = { UII_DAMAGE, UII_SHIELD, UII_XP, UII_STAR, UII_COIN };
              for (b = 0; b < 5; b++)
                  drawUiIcon(gWorldDC, mi[b], bx[b] + 24.0f, by + 23.0f,
                             b == 0 ? 31.0f : 27.0f, -1);
          }
        }
        /* 第二排：每日营地（签到 + 每日挑战） */
        { int by2 = 396;
          int hov = (mMouseX >= 370 && mMouseX <= 630 && mMouseY >= by2 && mMouseY <= by2 + 46);
          drawUiButton(gWorldDC, 370.0f, (float)by2, 260.0f, 46.0f,
                       BTN_GOLD, hov, 1);
          drawUiIcon(gWorldDC, UII_WAVE, 405.0f, (float)by2 + 23.0f, 29.0f, -1);
        }
    }

    /* ================= 高分辨率后处理与文字层 =================
       旧链路先把世界层降到 1000x650，再把完整帧放大全屏；这会丢掉
       2x 超采样已经拥有的细节。现在后处理和文字也直接画进 2x 世界层，
       最终根据窗口大小只重采样一次。 */
    gCurXF = gScaleXF;
    SetWorldTransform(gWorldDC, &gScaleXF);
    if (gState == ST_PLAY || gState == ST_PAUSE ||
        gState == ST_WIN  || gState == ST_LOSE)
        drawVignette(gWorldDC, SS);

    /* ================= 文字层（2x，随最终输出保持清晰） ================= */
    if (gState == ST_MENU) {
        gMenuTextRuns++;
        float cx = VIEW_W * 0.5f;
        putTextCS(gWorldDC, gF72, RGB(255, 236, 120), RGB(20, 40, 20), L"植物大战僵尸", cx, 200);
        putTextCS(gWorldDC, gF22, RGB(190, 235, 190), RGB(16, 34, 16), L"C 语言 · Win32 GDI 复刻版", cx, 268);
        /* 菜单四个入口按钮的文字 */
        {
            const wchar_t *nm[5] = { L"开始游戏", L"编组", L"天赋", L"成就", L"抽卡" };
            const float bx[5] = { 40.0f, 320.0f, 470.0f, 620.0f, 770.0f };
            const float bw[5] = { 260.0f, 130.0f, 130.0f, 130.0f, 190.0f };
            int b;
            for (b = 0; b < 5; b++)
                putTextCS(gWorldDC, gF22, b == 0 ? RGB(255, 255, 235) : RGB(226, 240, 226),
                          RGB(20, 40, 20), nm[b], bx[b] + bw[b] * 0.5f, 353.0f);
            putTextCS(gWorldDC, gF22, RGB(255, 244, 210), RGB(20, 40, 20),
                      L"每日营地", 500.0f, 419.0f);
            /* 等级 / 经验 / 收集度 / 货币（一行，下面挂经验条） */
            wsprintfW(buf, L"Lv.%d", gSave.level);
            putTextCS(gWorldDC, gF15, RGB(255, 226, 150), RGB(20, 40, 20), buf, 315.0f, 296.0f);
            drawUiIcon(gWorldDC, UII_XP, 372.0f, 298.0f, 23.0f, -1);
            wsprintfW(buf, L"%d/%d", gSave.xp, xpNeed(gSave.level));
            putTextS(gWorldDC, gF15, RGB(232, 240, 220), RGB(20, 40, 20), buf, 388.0f, 286.0f, 0);
            drawUiIcon(gWorldDC, UII_STAR, 493.0f, 298.0f, 23.0f, -1);
            wsprintfW(buf, L"%d", gSave.stars);
            putTextS(gWorldDC, gF15, RGB(255, 226, 150), RGB(20, 40, 20), buf, 509.0f, 286.0f, 0);
            drawUiIcon(gWorldDC, UII_COIN, 556.0f, 298.0f, 23.0f, -1);
            wsprintfW(buf, L"%d", gSave.coins);
            putTextS(gWorldDC, gF15, RGB(255, 226, 150), RGB(20, 40, 20), buf, 572.0f, 286.0f, 0);
            wsprintfW(buf, L"植物 %d/%d", plantOwnedCount(), plantPoolTotal());
            putTextCS(gWorldDC, gF15, RGB(220, 240, 210), RGB(20, 40, 20), buf, 665.0f, 296.0f);
            { float x0 = cx - 130.0f, w = 260.0f, f = (float)gSave.xp / (float)xpNeed(gSave.level);
              if (f > 1.0f) f = 1.0f;
              fillRound(gWorldDC, x0, 306.0f, x0 + w, 320.0f, 7, RGB(30, 48, 30));
              strokeRound(gWorldDC, x0, 306.0f, x0 + w, 320.0f, 7, RGB(150, 200, 130), 2);
              fillRound(gWorldDC, x0 + 3.0f, 309.0f, x0 + 3.0f + (w - 6.0f) * f, 317.0f, 4,
                        RGB(120, 220, 90)); }
        }
        putTextCS(gWorldDC, gF15, RGB(160, 200, 160), RGB(16, 34, 16),
                  L"左键选卡片 → 左键点草坪种植  |  右键取消  |  Esc 暂停菜单  |  R 重开", cx, 596);
        putTextCS(gWorldDC, gF15, RGB(160, 200, 160), RGB(16, 34, 16),
                  L"阳光自动拾取（A 键开关） · 飘落特效（W 键开关） · 别让僵尸走到最左边", cx, 622);
    } else if (gState == ST_PLAY || gState == ST_PAUSE ||
               gState == ST_WIN || gState == ST_LOSE || gState == ST_DRAFT) {
        /* ---- HUD 文字 ---- */
        /* 阳光数字：四周描一圈深色再压主色，任何底色上都读得清 */
        wsprintfW(buf, L"%d", gSun);
        putTextS(gWorldDC, gF22, RGB(38, 24, 10), RGB(38, 24, 10), buf, 123, 29, 2);
        putTextS(gWorldDC, gF22, RGB(38, 24, 10), RGB(38, 24, 10), buf, 125, 31, 2);
        putTextS(gWorldDC, gF22, RGB(255, 252, 236), RGB(56, 38, 20), buf, 124, 30, 2);

        /* 所有可种卡都显示真实价格；奖励植物的首次免费显示“赠”。 */
        {
            int   normalTotal = gLoadoutN + gBonusN;
            int   cardTotal = normalTotal + gRgN;
            float cgap = cardBarGap();
            for (i = 0; i < cardTotal; i++) {
                float bx = 144 + (float)i * cgap;
                int hot = (mMouseX >= (int)bx && mMouseX <= (int)(bx + cgap - 8.0f) &&
                           mMouseY >= 5 && mMouseY <= 87);
                float lift = hot ? -2.0f : 0.0f;
                int afford;
                float cd = 0.0f;
                if (hot && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) lift = 1.0f;
                if (i < normalTotal) {
                    int ci = (i < gLoadoutN) ? gLoadout[i] : gBonusPlant[i - gLoadoutN];
                    afford = (gSun >= plantCost(ci));
                    wsprintfW(buf, L"%d", plantCost(ci));
                    cd = cardCD[ci];
                } else {
                    int rgj = i - normalTotal;
                    afford = gRgFree[rgj] || gSun >= rgPlantCost(gRgCard[rgj]);
                    if (gRgFree[rgj]) lstrcpyW(buf, L"赠");
                    else wsprintfW(buf, L"%d", rgPlantCost(gRgCard[rgj]));
                    cd = rgCardCD[rgj];
                }
                drawUiIcon(gWorldDC, UII_SUN, bx + 9.0f, 70.0f + lift, 14.0f,
                           afford ? 235 : 130);
                putTextCS(gWorldDC, gF15, afford ? RGB(255, 255, 255) : RGB(255, 140, 140),
                          RGB(28, 18, 10), buf, bx + (cgap - 8.0f) * 0.5f + 5.0f,
                          8 + 76 - 12 + lift);
                if (cd > 0.15f) {
                    wsprintfW(buf, L"%.0f", ceilf(cd));
                    putTextCS(gWorldDC, gF15, RGB(244, 240, 220), RGB(20, 28, 22), buf,
                              bx + (cgap - 8.0f) * 0.5f, 30.0f + lift);
                }
            }
        }

        /* 卡片悬停提示：槽位 → 编组植物（之前把槽位当植物类型索引，
           悬停空卡位会报出根本没上场的植物名和价格） */
        if (mMouseY < 92 && mMouseX >= 144) {
            float cgap = cardBarGap();
            int   normalTotal = gLoadoutN + gBonusN;
            int   total = normalTotal + gRgN;
            int   slot  = (int)floor((mMouseX - 144.0f) / cgap);
            if (slot >= 0 && slot < normalTotal) {
                int ci = (slot < gLoadoutN) ? gLoadout[slot] : gBonusPlant[slot - gLoadoutN];
                wsprintfW(buf, L"%s   %d 阳光", plantDefs[ci].name, plantCost(ci));
                putTextCS(gWorldDC, gF18, RGB(255, 248, 210), RGB(20, 30, 20), buf, (float)mMouseX, 100);
            } else if (slot >= normalTotal && slot < total) {
                int rgj = slot - normalTotal;
                if (gRgFree[rgj]) wsprintfW(buf, L"%s   首次免费", rgPlants[gRgCard[rgj]].name);
                else wsprintfW(buf, L"%s   %d 阳光", rgPlants[gRgCard[rgj]].name,
                               rgPlantCost(gRgCard[rgj]));
                putTextCS(gWorldDC, gF18, RGB(255, 248, 210), RGB(20, 30, 20), buf, (float)mMouseX, 100);
            } else if (slot == total) {
                putTextCS(gWorldDC, gF18, RGB(255, 248, 210), RGB(20, 30, 20),
                          L"铲子 · 点植物铲除", (float)mMouseX, 100);
            }
        }

        /* 波次 */
        wsprintfW(buf, L"第 %d / %d 波", gWave, levelClearWaves(gCurLevel));
        drawUiIcon(gWorldDC, UII_WAVE, BAR_X + 18.0f,
                   BAR_Y + BAR_H * 0.5f, 24.0f, -1);
        putTextCS(gWorldDC, gF15, RGB(255, 250, 220), RGB(28, 22, 14), buf,
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
        drawUiIcon(gWorldDC, UII_KILL, 17.0f, 628.0f, 29.0f, -1);
        putTextS(gWorldDC, gF18, RGB(232, 228, 202), RGB(20, 30, 20), buf, 34, 614, 0);

        wsprintfW(buf, L"自动拾取 [A] %s", gAutoSun ? L"开" : L"关");
        putTextS(gWorldDC, gF15, gAutoSun ? RGB(178, 240, 150) : RGB(206, 206, 196),
                 RGB(20, 30, 20), buf, 982, 607, 2);
        wsprintfW(buf, L"飘落特效 [W] %s", gWeatherOn ? L"开" : L"关");
        putTextS(gWorldDC, gF15, gWeatherOn ? RGB(178, 240, 150) : RGB(206, 206, 196),
                 RGB(20, 30, 20), buf, 982, 627, 2);

        /* 顶栏右侧状态栏（模式 / 武器 / 资源 / 愤怒条） */
        drawStatusPanel(gWorldDC);

        /* 三选一界面文字 */
        if (gState == ST_DRAFT) drawDraftText(gWorldDC);
        if (gState == ST_DRAFT) drawCelebText(gWorldDC);
        if (gState == ST_PLAY)  drawPrimordialHud(gWorldDC);   /* 始祖在场指示 */

        /* 元界面文字 */
        if (gState == ST_LEVELS || gState == ST_TALENTS ||
            gState == ST_ACH || gState == ST_GACHA ||
            gState == ST_LOADOUT || gState == ST_REWARD ||
            gState == ST_DAILY) drawMetaText(gWorldDC);

        if (gState == ST_PLAY && gWave == 0)
            putTextCS(gWorldDC, gF30, RGB(255, 244, 180), RGB(24, 46, 24),
                      L"准备迎战僵尸…", LAWN_X + COLS * CELL_W * 0.5f, LAWN_Y + 46);

        /* 事件主题横幅：出现神级以上单位时，提示地形与背景已经异变 */
        if (gTheme != TH_NONE && gThemeBannerT > 0.0f &&
            (gState == ST_PLAY || gState == ST_PAUSE)) {
            const ThemeDef *td = &themeDefs[gTheme];
            float bf = CLAMP(gThemeBannerT / 0.6f, 0.0f, 1.0f);
            float by2 = 100.0f;      /* 压在顶栏下方，避开"准备迎战僵尸…" */
            int   bw  = (int)(300.0f * bf);
            fillRound(gWorldDC, VIEW_W * 0.5f - (float)bw, by2 - 4,
                      VIEW_W * 0.5f + (float)bw, by2 + 36, 10, RGB(14, 12, 24));
            strokeRound(gWorldDC, VIEW_W * 0.5f - (float)bw, by2 - 4,
                        VIEW_W * 0.5f + (float)bw, by2 + 36, 10, td->accent, 3);
            wsprintfW(buf, L"地形异变 · %s", td->name);
            putTextCS(gWorldDC, gF22, td->accent, RGB(10, 8, 16), buf,
                      VIEW_W * 0.5f, by2 + 16);
            if (gThemeWho[0])
                putTextCS(gWorldDC, gF15, RGB(232, 232, 240), RGB(10, 8, 16),
                          gThemeWho, VIEW_W * 0.5f, by2 + 50);
        }

        if (gState == ST_PAUSE) {
            int hc = (mMouseX >= (int)PAUSE_BTN_X && mMouseX <= (int)(PAUSE_BTN_X + PAUSE_BTN_W) &&
                      mMouseY >= (int)PAUSE_CONT_Y && mMouseY <= (int)(PAUSE_CONT_Y + PAUSE_BTN_H));
            int hh = (mMouseX >= (int)PAUSE_BTN_X && mMouseX <= (int)(PAUSE_BTN_X + PAUSE_BTN_W) &&
                      mMouseY >= (int)PAUSE_HOME_Y && mMouseY <= (int)(PAUSE_HOME_Y + PAUSE_BTN_H));
            fillRect(gWorldDC, 0, 92, VIEW_W, VIEW_H, RGB(13, 22, 16));
            fillRound(gWorldDC, 300, 154, 700, 520, 18, RGB(30, 50, 34));
            strokeRound(gWorldDC, 300, 154, 700, 520, 18, RGB(132, 190, 112), 4);
            drawUiIcon(gWorldDC, UII_PAUSE, 346.0f, 225.0f, 50.0f, -1);
            putTextCS(gWorldDC, gF54, RGB(255, 236, 120), RGB(20, 40, 20), L"暂停菜单", 500, 220);
            putTextCS(gWorldDC, gF18, RGB(196, 224, 192), RGB(16, 34, 16),
                      L"战斗进度已保留", 500, 278);
            drawUiButton(gWorldDC, PAUSE_BTN_X, PAUSE_CONT_Y,
                         PAUSE_BTN_W, PAUSE_BTN_H, BTN_GREEN, hc, 1);
            putTextCS(gWorldDC, gF22, RGB(246, 255, 238), RGB(20, 38, 20),
                      L"继续游戏", 500, PAUSE_CONT_Y + PAUSE_BTN_H * 0.5f);
            drawUiButton(gWorldDC, PAUSE_BTN_X, PAUSE_HOME_Y,
                         PAUSE_BTN_W, PAUSE_BTN_H, BTN_RED, hh, 1);
            putTextCS(gWorldDC, gF22, RGB(255, 238, 226), RGB(42, 22, 18),
                      L"回到主页", 500, PAUSE_HOME_Y + PAUSE_BTN_H * 0.5f);
            putTextCS(gWorldDC, gF15, RGB(164, 192, 164), RGB(16, 30, 18),
                      L"再按 Esc 可继续", 500, 494);
        }
        if (gState == ST_WIN || gState == ST_LOSE) {
            int win = (gState == ST_WIN);
            fillRect(gWorldDC, 0, 92, VIEW_W, VIEW_H, RGB(14, 24, 16));
            strokeRound(gWorldDC, 240, 190, 760, 470, 14, win ? RGB(150, 210, 100) : RGB(200, 80, 70), 5);
            fillRound(gWorldDC, 242, 192, 758, 468, 13, win ? RGB(34, 66, 38) : RGB(60, 30, 28));
            putTextCS(gWorldDC, gF54, win ? RGB(190, 255, 150) : RGB(255, 150, 140),
                      RGB(12, 20, 12), win ? L"守住了草坪！" : L"僵尸吃掉了你的脑子…", 500, 250);
            wsprintfW(buf, L"击杀 %d 只僵尸   用时 %d 秒", gKilled, (int)gElapsed);
            putTextCS(gWorldDC, gF22, RGB(255, 244, 200), RGB(12, 20, 12), buf, 500, 330);
            {
                int hot = mMouseX >= 350 && mMouseX <= 650 && mMouseY >= 370 && mMouseY <= 428;
                drawUiButton(gWorldDC, 350.0f, 370.0f, 300.0f, 58.0f,
                             win ? BTN_GREEN : BTN_RED, hot, 1);
            }
            putTextCS(gWorldDC, gF18, RGB(210, 240, 210), RGB(12, 20, 12),
                      L"再玩一局　[R]", 500, 399);
        }
    }

    /* 元界面文字独立绘制，避免把选关/每日营地误当成游戏 HUD */
    if (gState == ST_LEVELS || gState == ST_TALENTS ||
        gState == ST_ACH || gState == ST_GACHA ||
        gState == ST_LOADOUT || gState == ST_REWARD ||
        gState == ST_DAILY) drawMetaText(gWorldDC);

    /* 升级横幅（菜单 / 结算都显示） */
    if (gLvUpT > 0.0f) {
        wsprintfW(buf, L"升级！Lv.%d", gLvUpNew);
        putTextCS(gWorldDC, gF30, RGB(255, 236, 120), RGB(40, 24, 8), buf, VIEW_W * 0.5f, 96.0f);
        putTextCS(gWorldDC, gF15, RGB(255, 240, 190), RGB(40, 24, 8),
                  L"星星 +2 · 金币 +3", VIEW_W * 0.5f, 128.0f);
    }

    /* 悬停提示：底板和文字同一层、同一次绘制，且压在所有界面之上。
       （以前底板画在世界层、文字画在文字层，被暂停/结算遮罩切开成两半） */
    drawHoverTipShapes(gWorldDC, 0);
    drawHoverTipText(gWorldDC);

    /* 恢复单位变换，最终呈现阶段直接读取完整的 2x 世界画布。 */
    SetWorldTransform(gWorldDC, &id);

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

        /* GPU 路径一次完成 2x 世界画布 -> 实际窗口的缩放与黑边。
           成功后不再执行 CPU 降采样，也不创建/复制全屏 GDI 位图。 */
        if (gpuDrawFrame(cw, ch, dx, dy, dw, dh)) return;

        /* Direct3D 不可用或设备暂时丢失：生成 1x 帧并走原有 GDI 回退。
           这条路径保证远程桌面、旧驱动和设备恢复期间仍然可玩。 */
        SetStretchBltMode(gFrameDC, HALFTONE);
        SetBrushOrgEx(gFrameDC, 0, 0, NULL);
        StretchBlt(gFrameDC, 0, 0, VIEW_W, VIEW_H,
                   gWorldDC, 0, 0, VIEW_W * SS, VIEW_H * SS, SRCCOPY);

        if (dw == VIEW_W && dh == VIEW_H && dx == 0 && dy == 0) {
            /* 窗口正好 1:1：直接贴，最快，也没有中间态 */
            BitBlt(out, 0, 0, VIEW_W, VIEW_H, gFrameDC, 0, 0, SRCCOPY);
        } else {
            /* 先在离屏把「黑边 + 缩放后的画面」合成完，再一次性贴到窗口。
               这样窗口每帧只被写一次，不会出现"先全黑再出画面"的闪烁。 */
            presentEnsure(cw, ch);
            if (gPresentDC) {
                presentFillBlack(gPresentDC, cw, ch);
                if (dw > VIEW_W || dh > VIEW_H) {
                    /* 大窗口直接从 2x 世界层呈现：只采样一次，保住精灵描边、
                       粒子和高分辨率文字。1920x1080 下通常仍是缩小而非放大。 */
                    SetStretchBltMode(gPresentDC, HALFTONE);
                    SetBrushOrgEx(gPresentDC, 0, 0, NULL);
                    StretchBlt(gPresentDC, dx, dy, dw, dh, gWorldDC,
                               0, 0, VIEW_W * SS, VIEW_H * SS, SRCCOPY);
                } else {
                    SetStretchBltMode(gPresentDC, HALFTONE);
                    SetBrushOrgEx(gPresentDC, 0, 0, NULL);
                    StretchBlt(gPresentDC, dx, dy, dw, dh, gFrameDC,
                               0, 0, VIEW_W, VIEW_H, SRCCOPY);
                }
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
/* ==================== 平台无关的输入入口 ====================
   坐标是**逻辑游戏坐标**（0..VIEW_W / 0..VIEW_H）。
   桌面端由 WndProc 经 windowToGame() 换算后调用；
   网页版由外壳把"触摸点 → 逻辑坐标"换算后调用（外壳那一步还要处理
   letterbox 黑边与 CSS 缩放，见 web/shell.js 的 mapPointer）。
   两条路走同一份命中判定，不会出现"桌面点得中、平板点不中"。 */

void gamePointerMove(float gx, float gy)
{
    mMouseX = (int)gx;
    mMouseY = (int)gy;
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
}

void gamePointerDown(float gx, float gy)
{
    mDown = 1;
    mMouseX = (int)gx;
    mMouseY = (int)gy;
    onClick(mMouseX, mMouseY);
}

void gamePointerUp(void) { mDown = 0; }

/* ==================== 平台无关的生命周期入口 ====================
   createLayers / artLoadAll 都是 static，平台层（plat_web.c / plat_ios.m）
   在另一个编译单元里，所以这里包一层暴露出去。 */

/* 前置声明：这几个的定义都在本块**之后**
   （createLayers 在窗口那一段、spriteRelease 在精灵那一段）。
   漏了它们会报 implicit declaration + "conflicting types"，很难定位。 */
static void createLayers(HWND hWnd);
static void spriteRelease(Sprite *s);
static void gpuRelease(void);
void gameBoot(void);
void gameStep(float dt, HDC out);

/* 设置资源目录（宽字符，末尾记得带分隔符）。
   网页版由外壳注入（它知道资源是从哪个 URL 前缀取的）；
   桌面端用 plat.h 里的 platAssetDir() 内联实现，不走这里。 */
/* 固定本局的随机性（跨平台渲染比对用）。
   ⚠️ 必须在 resetGame **之前**调用 —— resetGame 内部会 srand()，
      普通模式用 time(NULL)，所以不切成每日模式的话每次开局都不一样。
   gDailyMode/gDailySeed 是 static，外面拿不到，所以在这里开个口子。 */
/* 已成功载入的资源张数（诊断用：跨平台比对时先确认两边载图数量一致） */
int gameArtCount(void) { return gArtLoaded; }
/* 诊断：当前界面状态（比对两侧是否处在同一屏） */
int gameStateId(void) { return (int)gState; }
int gameMenuBgOk(void) { return gSprMenuBg.ok; }
void gameRenderCounters(int *bg, int *blk, int *txt, int *blit)
{
    if (bg)   *bg   = gBgRuns;
    if (blk)  *blk  = gMenuBlockRuns;
    if (txt)  *txt  = gMenuTextRuns;
    if (blit) *blit = gMenuBgBlits;
}

/* 诊断：直接暴露几个精灵的像素缓冲，用于判断"画面变了"是
   精灵数据被改写，还是绘制路径出了问题。
   which: 0=gSprMenuBg 1=gSprBackground 2=gSprButton[BTN_DARK][NORMAL]
   返回 bits 指针；w/h 回填尺寸。 */
const unsigned char *gameSpriteProbe(int which, int *w, int *h)
{
    const Sprite *s = NULL;
    if (which == 0) s = &gSprMenuBg;
    else if (which == 1) s = &gSprBackground;
    else if (which == 2) s = &gSprButton[BTN_DARK][BTN_NORMAL];
    if (!s) return NULL;
    if (w) *w = s->w;
    if (h) *h = s->h;
    return (const unsigned char *)s->bits;
}
int gameLawnOk(void) { return gSprLawn.ok; }

void gameSetDeterministic(unsigned seed)
{
    gDailyMode = 1;
    gDailySeed = (int)seed;
}

void gameSetAssetDir(const char *dir)
{
    wchar_t wdir[MAX_PATH];
    int n = 0;
    if (!dir) return;
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, MAX_PATH);
    while (wdir[n]) n++;
    if (n > 0 && wdir[n - 1] != L'\\' && wdir[n - 1] != L'/') { wdir[n++] = L'\\'; wdir[n] = 0; }
    wsprintfW(gAssetDir, L"%s", wdir);
}

void gameInitAll(void)
{
    createLayers(NULL);
    gameBoot();          /* 字体 + 载图 + 开局，见其定义处 */
}

/* 释放全部贴图与 GPU 资源。桌面端在 WinMain 收尾调用；
   网页版不需要（页面卸载时整块内存随 wasm 实例回收），
   但 iOS 需要 —— 那边切后台可能被回收内存。 */
void gameShutdownAll(void)
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
    spriteRelease(&gSprBackground);
    spriteRelease(&gSprLawn);
    spriteRelease(&gSprLawnNight);
    spriteRelease(&gSprLawnWater);
    spriteRelease(&gSprLawnRoof);
    spriteRelease(&gSprLawnDesert);
    spriteRelease(&gSprLawnIce);
    spriteRelease(&gSprLawnMagma);
    spriteRelease(&gSprLawnVoid);
    spriteRelease(&gSprLawnCircuit);
    spriteRelease(&gSprLawnAstral);
    spriteRelease(&gSprLawnPrimordial);
    { int rp; for (rp = 0; rp < RG_PLANT_N; rp++) spriteRelease(&gSprRgPlant[rp]); }
    { int rz; for (rz = 0; rz < RG_ZOMBIE_N; rz++) spriteRelease(&gSprRgZombie[rz]); }
    { int b; for (b = 0; b < RGQ_COUNT; b++) spriteRelease(&gSprBadge[b]); }
    { int f; for (f = 0; f < FSX_N; f++) spriteRelease(&gSprFx[f]); }
    gpuRelease();
}

/* ---- 键盘处理：抽成独立函数，让 iOS 的屏幕按钮复用同一条逻辑 ----
   原来这段直接写在 WndProc 的 case WM_KEYDOWN 里，与消息循环耦合。
   抽出来后：桌面端由消息泵调用、iOS 由 platInjectKey() 调用，
   两条路走同一份分支判断，不会出现"桌面能按、平板按了没反应"。 */
/* 非 static：网页版的屏幕快捷键按钮直接调它（plat_web.c 的 platWebKey）。
   抽出来的初衷就是这个 —— 桌面按 F11、平板点屏幕按钮，走同一条分支判断。 */
void handleKey(int wp, int isRepeat)
{
        if (wp == VK_F11) {
            if (!isRepeat && toggleAllowed()) toggleFullscreen();
            return;
        }
        /* Esc：战斗/三选一时弹出带「回到主页」的暂停菜单；
           暂停时再按则继续。大厅子界面仍按原有返回按钮逻辑。 */
        else if (wp == VK_ESCAPE) {
            if (isMetaScreen(gState))             goBack();
            else if (gState == ST_PAUSE)          pauseContinue();
            else if (gState == ST_PLAY || gState == ST_DRAFT) pauseOpen(gState);
            else                                  { gSel = -1; gShovel = 0; }
        }
        else if (wp == 'A') { gAutoSun = !gAutoSun; }
        else if (wp == 'W') { gWeatherOn = !gWeatherOn; }
        /* B：背景音乐开关。关掉时直接 close 掉 MCI 设备（而不是暂停），
           这样再打开会从头播 —— 比「暂停后从中间续上」更符合直觉，
           也避免长时间挂着一个不用的流式设备。 */
        else if (wp == 'B') { gBgmOn = !gBgmOn;
                              if (!gBgmOn) bgmClose();
                              else if (gState == ST_PLAY) bgmForLevel(gCurLevel); }
        /* N：音效开关。刻意和 B（BGM）分开 ——
           排查"某个音是不是根本没触发"时需要能单独关掉另一半声音。 */
        else if (wp == 'N') { gSfxOn = !gSfxOn; }
        else if (wp == 'S' && gState == ST_DRAFT) { gSun += DRAFT_SKIP; goBack(); }
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
        else if (wp == 'E' && gState == ST_PLAY && gFuryReady && HAS(R_FURY)) {
            /* R_FURY 的兑现口：主动引爆攒满的全屏雷击，自己挑时机（不用等自动放）。
               没有这个遗物时愤怒条满了会立刻自动放，所以这里不接普通玩家的按键。 */
            furyBlast(1);
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
            if (gState == ST_PLAY || gState == ST_DRAFT) pauseOpen(gState);
            else if (gState == ST_PAUSE) pauseContinue();
        } else if (wp == 'R') {
            resetGame();
            gState = ST_PLAY;
        /* 数字键 = 卡槽位（和鼠标点击同一映射），不是植物类型。
           之前用 wp < '1'+PT_COUNT，PT_COUNT=33 时把 '1'..'Q' 的键全吞了，
           而且选的是植物类型而非编组里的卡。

           ⚠️ 必须限定 `gState == ST_PLAY`。
           这条分支原来**没有**状态判断，而同一段里其它分支（G/H/E/F/S/M/P）
           全都检查了 —— 唯独这里漏了。后果实测如下：
             在菜单/编组界面按数字键 2，gSel 被从 -1 静默改成 77
             （编组里第 2 株的植物 id），然后进战斗就是
             "没人点过、第二张卡却亮着"，也就是用户报的「没选却出现选取」。
           更糟的是带着这个脏值点草坪会真的把植物种下去。
           注意 gSun/cardCD 在非战斗界面是上一局的残留值，条件很容易成立。 */
        } else if ((wp >= '1' && wp <= '9') && gState == ST_PLAY) {
            int slot = (int)(wp - '1');
            if (slot < gLoadoutN + gBonusN) {
                int idx = (slot < gLoadoutN) ? gLoadout[slot] : gBonusPlant[slot - gLoadoutN];
                if (cardCD[idx] <= 0.0f && gSun >= plantCost(idx)) { gSel = idx; gShovel = 0; }
            }
        }
        return;
}

/* ---- 建立分层离屏缓冲区（世界层 / 帧层 / 屏幕层）----
   抽成独立函数的两个理由：
     ① iOS 侧完全没有 HWND —— 那边的 GetDC/CreateCompatibleDC/CreateDIBSection
        由 plat_ios.m 实现成"位图上下文"，这段代码本身**一字不改**就能用；
     ② headless 的渲染基线测试（_test_render_baseline.c）需要一个窗口才能搭出
        gWorldDC/gWorldBits，抽出来之后传 NULL 即可建起来。
   两个平台的差异全部落在 plat.h 的实现里，这里只做"建三层 + 选入位图"。 */
static void createLayers(HWND hWnd)
{
    /* ---------- 建立分层缓冲区 ---------- */
    gScreenDC = GetDC(hWnd);
    gFrameDC  = CreateCompatibleDC(gScreenDC);
    gFrameBM  = CreateCompatibleBitmap(gScreenDC, VIEW_W, VIEW_H);
    SelectObject(gFrameDC, gFrameBM);
    gWorldDC  = CreateCompatibleDC(gScreenDC);
    {
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = VIEW_W * SS;
        bi.bmiHeader.biHeight = -(VIEW_H * SS); /* 顶向：GPU 上传无需再逐行翻转 */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        gWorldBM = CreateDIBSection(gScreenDC, &bi, DIB_RGB_COLORS,
                  &gWorldBits, NULL, 0);
    }
    SelectObject(gWorldDC, gWorldBM);
    ReleaseDC(hWnd, gScreenDC);

    /* ⚠️ 世界变换的初始化**必须**属于"建层"这一步。
       上一轮把这段留在 WinMain 里，抽出来的 createLayers() 就成了半成品：
       任何不走 WinMain 的调用方（headless 渲染基线测试、以及 iOS 后端 ——
       那边根本没有 WinMain）拿到的画布是 1:1 的，所有绘制会缩到左上角四分之一。
       实测症状：抓帧里卡片只占画面左上角一小块。
       GM_ADVANCED 也必须在这里设：默认的 GM_COMPATIBLE 下
       SetWorldTransform 不生效（它只支持平移），2x 超采样直接失效。 */
    SetGraphicsMode(gWorldDC, GM_ADVANCED);
    gScaleXF.eM11 = (FLOAT)SS; gScaleXF.eM12 = 0;
    gScaleXF.eM21 = 0;         gScaleXF.eM22 = (FLOAT)SS;
    gScaleXF.eDx  = 0;         gScaleXF.eDy  = 0;
    /* 单位变换（spriteBlit 会临时切到它做"设备坐标"的 blit，再切回来）
       与当前变换的初值也一并在这里定好 —— 它们都是"画布状态"，不是窗口状态。 */
    gIdentXF.eM11 = 1; gIdentXF.eM12 = 0; gIdentXF.eM21 = 0;
    gIdentXF.eM22 = 1; gIdentXF.eDx = 0;  gIdentXF.eDy = 0;
    gCurXF = gScaleXF;
}
/* ---- 载入全部美术资源，返回成功张数 ----
   ⚠️ 必须独立成函数，不能留在 WinMain 里（createLayers 就栽在这上面）：
      headless 的渲染/卡牌测试需要一个"真的把贴图读进来"的入口，
      而它没法执行 WinMain。以前测试是靠**手抄一份加载循环**绕过，
      那份副本漏掉了新素材就变成了"测试全绿但游戏里一片空白"。
      iOS 侧同样需要它（那边没有 WinMain）。
   缺图不致命：每个槽位失败会退回程序化绘制或基座贴图，返回值只用于 HUD 提示。 */
static int artLoadAll(void)
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
        L"plant_01", L"plant_02", L"plant_03", L"plant_04", L"plant_05",
        L"plant_06", L"plant_07", L"plant_08", L"plant_09", L"plant_10",
        L"plant_11", L"plant_12", L"plant_13", L"plant_14", L"plant_15",
        L"plant_16", L"plant_17", L"plant_18", L"plant_19", L"plant_20",
        L"plant_21", L"plant_22", L"plant_23", L"plant_24", L"plant_25",
        L"plant_26", L"plant_27", L"plant_28", L"plant_29", L"plant_30",
        L"plant_31", L"plant_32", L"plant_33", L"plant_34", L"plant_35",
        L"plant_36", L"plant_37", L"plant_38", L"plant_39", L"plant_40",
        L"plant_41", L"plant_42", L"plant_43", L"plant_44", L"plant_45",
        L"plant_46", L"plant_47", L"plant_48", L"plant_49", L"plant_50",
        L"plant_51", L"plant_52", L"plant_53", L"plant_54", L"plant_55",
        L"plant_56", L"plant_57", L"plant_58", L"plant_59", L"plant_60",
        L"plant_61", L"plant_62", L"plant_63", L"plant_64", L"plant_65",
        L"plant_66", L"plant_67", L"plant_68", L"plant_69", L"plant_70",
        L"plant_71", L"plant_72", L"plant_73", L"plant_74", L"plant_75",
        L"plant_76", L"plant_77", L"plant_78", L"plant_79", L"plant_80",
        L"hero_flame", L"hero_ironnut", L"hero_crystal", L"hero_thornvine",
        L"hero_sungod", L"hero_frost", L"hero_worldtree", L"hero_doom"
    };
    static const wchar_t *zombieFile[ZT_COUNT] = {
        L"zombie_normal", L"zombie_cone", L"zombie_bucket", L"zombie_flag",
        L"zombie_dancer", L"zombie_zamboni", L"zombie_balloon",
        L"zombie_vaulter", L"zombie_screendoor", L"zombie_football",
        L"zombie_newspaper", L"zombie_digger", L"zombie_giant",
        L"zombie_frostfang", L"zombie_glacier", L"zombie_magmaw",
        L"zombie_ashwalker", L"zombie_phantom", L"zombie_blinker",
        L"zombie_cogwork", L"zombie_saboteur"
    };
    static const wchar_t *zombieEatFile[ZT_COUNT] = {
        L"zombie_normal_eat", L"zombie_cone_eat",
        L"zombie_bucket_eat", L"zombie_flag_eat",
        L"zombie_dancer", L"zombie_zamboni", L"zombie_balloon",
        L"zombie_vaulter", L"zombie_screendoor", L"zombie_football",
        L"zombie_newspaper", L"zombie_digger", L"zombie_giant",
        /* 新僵尸没有独立啃食图：下面会自动退化成行走图 */
        L"zombie_frostfang", L"zombie_glacier", L"zombie_magmaw",
        L"zombie_ashwalker", L"zombie_phantom", L"zombie_blinker",
        L"zombie_cogwork", L"zombie_saboteur"
    };
    int i, nArt = 0;
    /* 资源目录：桌面端是 exe 同级的 assets\，iOS 是 App Bundle 内的 assets。
       差异收在 platAssetDir 里（iOS 版走 NSBundle，见 plat_ios.m）。 */
    platAssetDir(gAssetDir, MAX_PATH);   /* 已含结尾分隔符，无需再补 */

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
    nArt += spriteLoadName(&gSprLawnIce,     L"lawn_ice");
    nArt += spriteLoadName(&gSprLawnMagma,   L"lawn_magma");
    nArt += spriteLoadName(&gSprLawnVoid,    L"lawn_void");
    nArt += spriteLoadName(&gSprLawnCircuit, L"lawn_circuit");
    nArt += spriteLoadName(&gSprLawnAstral,  L"lawn_astral");
    nArt += spriteLoadName(&gSprLawnPrimordial, L"lawn_primordial");
    { int k;
      static const wchar_t *tf[TILE_SPR_N] = {
          L"tile_scorch", L"tile_ice", L"tile_web", L"tile_corrode", L"tile_bloom"};
      for (k = 0; k < TILE_SPR_N; k++) nArt += spriteLoadName(&gSprTile[k], tf[k]);
    }
    /* 肉鸽专属单位贴图：rgplant_NN / rgzombie_NN
       一张都缺也不影响跑（会退回基座贴图），所以不计入 nArt 的门槛。 */
    { int k;
      wchar_t nm[64];
      for (k = 0; k < RG_PLANT_N; k++) {
          wsprintfW(nm, L"rgplant_%02d", k);
          nArt += spriteLoadName(&gSprRgPlant[k], nm);
      }
      for (k = 0; k < RG_ZOMBIE_N; k++) {
          wsprintfW(nm, L"rgzombie_%02d", k);
          if (spriteLoadName(&gSprRgZombie[k], nm)) {
              spriteTint(&gSprRgZombieFlash[k], &gSprRgZombie[k], 255, 255, 255);
              spriteTint(&gSprRgZombieBlue[k],  &gSprRgZombie[k], 110, 200, 255);
              nArt++;
          }
          {   /* 倒地帧：离线绕脚底旋转派生，缺了就退回基座僵尸的倒地帧 */
              int d;
              for (d = 0; d < 5; d++) {
                  wsprintfW(nm, L"rgzombie_%02d_dead%d", k, d);
                  nArt += spriteLoadName(&gSprRgZombieDead[k][d], nm);
              }
          }
      }
    }
    { int k;
      static const wchar_t *wn[WPN_COUNT-1] = {L"plant_pierce", L"plant_splash",
          L"plant_bounce", L"plant_freeze", L"plant_burn"};
      static const wchar_t *sn[3] = {L"plant_doublestack", L"plant_multilane", L"plant_tall"};
      for (k = 0; k < WPN_COUNT - 1; k++) nArt += spriteLoadName(&gSprWeapon[k], wn[k]);
      for (k = 0; k < 3; k++) nArt += spriteLoadName(&gSprShape[k], sn[k]);
      { int b; for (b = 0; b < BUL_COUNT; b++) nArt += spriteLoadName(&gSprBullet[b], bulletFile[b]); }
    nArt += spriteLoadName(&gSprCardBack, L"ui_cardback_v2");


    /* 档位徽章。名字用"档位英文名"而不是数字：badge_hall.png 这种名字人类可读，
       也不会因为改了 RGQ_COUNT 的顺序而错位。
       注意**不计入 nArt 门槛** —— 缺一枚徽章不该让 HUD 报"资源缺失"。 */
    {
        static const wchar_t *badgeFile[RGQ_COUNT] = {
            L"badge_common", L"badge_rare", L"badge_epic", L"badge_legend",
            L"badge_myth", L"badge_ultra", L"badge_hall", L"badge_primordial"
        };
        int bi;
        for (bi = 0; bi < RGQ_COUNT; bi++)
            spriteLoadName(&gSprBadge[bi], badgeFile[bi]);
    }
    /* 开牌庆典的叠加光效（同徽章：缺图为纯回退，不影响任何逻辑） */
    {
        static const wchar_t *fxFile[FSX_N] = {
            L"fx_rays", L"fx_shock", L"fx_spark",
            L"fx_pillar", L"fx_flare", L"fx_runes"
        };
        int fi;
        for (fi = 0; fi < FSX_N; fi++)
            spriteLoadName(&gSprFx[fi], fxFile[fi]);
    }
    nArt += spriteLoadName(&gSprBurst,    L"ui_burst");
    nArt += spriteLoadName(&gSprMenuBg,   L"ui_menu_bg_v1");
    nArt += spriteLoadName(&gSprMetaBg,   L"ui_meta_bg_v1");
    {
        static const wchar_t *sty[BTN_STYLE_N] = {
            L"green", L"dark", L"gold", L"purple", L"red", L"blue"
        };
        static const wchar_t *sta[BTN_STATE_N] = { L"normal", L"hot", L"down" };
        int bs, bt;
        wchar_t nm[64];
        for (bs = 0; bs < BTN_STYLE_N; bs++) for (bt = 0; bt < BTN_STATE_N; bt++) {
            wsprintfW(nm, L"ui_btn_%s_%s", sty[bs], sta[bt]);
            nArt += spriteLoadName(&gSprButton[bs][bt], nm);
        }
        nArt += spriteLoadName(&gSprButtonDisabled, L"ui_btn_disabled");
    }
    {
        static const wchar_t *uiName[UII_COUNT] = {
            L"sun", L"coin", L"star", L"xp", L"kill", L"wave", L"heart", L"damage",
            L"cooldown", L"ice", L"fire", L"poison", L"shield", L"pause", L"fullscreen", L"back"
        };
        int ui;
        wchar_t nm[64];
        for (ui = 0; ui < UII_COUNT; ui++) {
            wsprintfW(nm, L"ui_icon_%s_v1", uiName[ui]);
            nArt += spriteLoadName(&gSprUiIcon[ui], nm);
        }
    }
    {
        static const wchar_t *growthName[GROW_COUNT] = {
            L"damage", L"rate", L"hp", L"boom", L"frost", L"cooldown", L"pierce"
        };
        int gi;
        wchar_t nm[64];
        for (gi = 0; gi < GROW_COUNT; gi++) {
            wsprintfW(nm, L"growth_%s_v1", growthName[gi]);
            nArt += spriteLoadName(&gSprGrowth[gi], nm);
        }
    }
    nArt += spriteLoadName(&gSprGold,    L"relic_gold");
      nArt += spriteLoadName(&gSprGhost,   L"relic_ghost");
      nArt += spriteLoadName(&gSprPumpkin, L"relic_pumpkin");
    }
    gArtLoaded = nArt;
    saveLoad();                                   /* 读存档（首次运行会建初始档） */
    /* 存档读完再收敛编制。放在这里而不是 saveLoad 内部：
       saveLoad 有多条 return 分支（各版本迁移各一条），
       在每条末尾都补调用很容易漏掉一条。 */
    loadoutNormalize();
    return nArt;
}


#if PLAT_HAS_OWN_MAIN
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
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lp;
        SetWindowPos(hWnd, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        presentInvalidate();
        return 0;
    }
    case WM_MOUSEMOVE: {
        /* 换算成逻辑坐标后交给平台无关入口 —— 网页版走的是同一个函数
           （见 gamePointerMove 的说明）。 */
        int gx, gy;
        windowToGame((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &gx, &gy);
        gamePointerMove((float)gx, (float)gy);
        SetCursor(LoadCursorW(NULL, uiPointerHot(mMouseX, mMouseY) ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int gx, gy;
        windowToGame((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &gx, &gy);
        gamePointerDown((float)gx, (float)gy);
        return 0;
    }
    case WM_LBUTTONUP: gamePointerUp(); return 0;
    case WM_SYSCOMMAND:
        /* 点标题栏「最大化」→ 切【真全屏】，而不是 Windows 原生最大化。
           原生最大化会保留标题栏、并把 1000x650 的画面上下留出黑边 ——
           玩家点完会觉得"根本没满屏"，观感比不加这个按钮还差。
           低 4 位是系统保留位，比较前要先掩掉（这是 Win32 的标准写法）。 */
        if ((wp & 0xFFF0) == SC_MAXIMIZE) {
            if (toggleAllowed()) toggleFullscreen();
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:                      /* Alt+Enter 也切全屏 */
        if (wp == VK_RETURN) {
            if (!keyIsRepeat(lp) && toggleAllowed()) toggleFullscreen();
            return 0;                        /* 吞掉，免得触发系统菜单 */
        }
        break;
    case WM_RBUTTONDOWN:
        /* 用同一个 goBack()：右键 / Esc / 按钮 三条路一份逻辑 */
        if (isMetaScreen(gState) || gState == ST_DRAFT) { goBack(); return 0; }
        gSel = -1; gShovel = 0; return 0;
    case WM_KEYDOWN:
        handleKey((int)wp, keyIsRepeat(lp));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

/* ==================== 平台无关的驱动接口 ====================
   见 _extract_driver.py 的说明：桌面 / 网页 / iOS 三端共用这三个函数，
   各自的循环外壳（消息泵 / requestAnimationFrame / CADisplayLink）在外层。

   ⚠️ 这三个必须是**非 static**：网页版把 pvz.c 与 plat_web.c 编成两个
      编译单元，plat_web.c 要能链接到它们。（桌面端用不到跨单元调用，
      但接口一致比省一个符号重要。） */

/* 开局：字体 + 美术资源 + 重置到主菜单。
   不含窗口创建 —— 窗口是平台的私事（网页版根本没有窗口）。 */
#endif  /* PLAT_HAS_OWN_MAIN */
void gameBoot(void)
{
    /* ---------- 字体 ---------- */
    gF15 = mkFont(15, 0, L"微软雅黑");
    gF18 = mkFont(18, 1, L"微软雅黑");
    gF22 = mkFont(22, 1, L"微软雅黑");
    gF30 = mkFont(30, 1, L"微软雅黑");
    gF54 = mkFont(54, 1, L"微软雅黑");
    gF72 = mkFont(72, 1, L"微软雅黑");

    gArtLoaded = artLoadAll();
    resetGame();
    gState = ST_MENU;
}

/* 平台层用来给"上一帧渲染花了多久"做节流（桌面端据此 Sleep 到 60fps）。 */
float gLastFrameUsed = 0.0f;

/* 一帧：界面计时 → 更新 → 渲染。
   `out` 是呈现目标 DC：桌面端传窗口 DC，网页端传一块假 DC
   （这样 render 的呈现步骤不会覆盖 gWorldDC，调用方随后自己取像素上传）。 */
void gameStep(float dt, HDC out)
{
    float used;
    DWORD t0;

    /* 界面计时：切状态就归零，入场动画（翻牌等）依赖它 */
    if (gState != gScreenPrev) {
        gScreenPrev = gState;
        gScreenT = 0.0f;
        /* 卡片选中态（gSel / gShovel）**只属于战斗**。
           一旦切到战斗以外的界面就作废，免得它跨界面存活 ——
           表现就是"没人点过、卡片却亮着"（实测：菜单按数字键把 gSel
           设成编组里的植物 id，进战斗后那张卡就自己亮了）。
           ST_PLAY ↔ ST_PAUSE 之间不清：暂停时卡片栏仍然画着，
           选中态该保留，恢复后接着种。
           这一道是兜底 —— 真正的根因修复在 WndProc 的数字键分支。 */
        if (gState != ST_PLAY && gState != ST_PAUSE) { gSel = -1; gShovel = 0; }
    }
    else                          gScreenT += dt;

    if (gState == ST_PLAY) updateGame(dt);
    else {
        gTime += dt; weatherUpdate(dt); if (gLvUpT > 0.0f) gLvUpT -= dt;
        /* 庆典演出发生在 ST_DRAFT（属于"非战斗"分支），
           所以必须在这里推进计时 —— 放 updateGame 里的话演出根本不会走。 */
        celebUpdate(dt);
    }

    t0 = GetTickCount();
    render(out);
    used = (float)((DWORD)(GetTickCount() - t0)) / 1000.0f;

    /* 记录渲染耗时给诊断用（见 _mk_framedump.py 的 perf 行）。 */
    gDbgRenderMs = used * 1000.0f;
    if (gDbgFrameCnt == 0) gDbgPerfT0 = GetTickCount();
    gDbgFrameCnt++;
    if (gDbgFrameCnt > 5) { gDbgSumRender += (double)gDbgRenderMs; gDbgPerfN++; }
    gLastFrameUsed = used;
}

/* 让平台层取回"世界层"的像素与尺寸 —— 网页版每帧把它上传到 canvas。
   返回的是 gWorldBits（BGRA、自上而下、行距 w*4）；w/h 是**设备像素**
   （= 逻辑尺寸 × SS），与 spriteBlit 的内部约定一致。 */
const void *gameWorldBits(int *w, int *h, int *stride)
{
    if (w)      *w = VIEW_W * SS;
    if (h)      *h = VIEW_H * SS;
    if (stride) *stride = VIEW_W * SS * 4;
    return gWorldBits;
}

#if PLAT_HAS_OWN_MAIN
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

    /* 禁止 Windows 把整个客户区当成低分辨率位图做 DPI 虚拟化缩放。
       必须在创建任何窗口之前设置；跨显示器时由 WM_DPICHANGED 接管尺寸。 */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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

    /* 保留 WS_MAXIMIZEBOX：标题栏那个「最大化」按钮要能点。
       点它不会走 Windows 的原生最大化，而是被 WndProc 里的 WM_SYSCOMMAND
       拦下来切【真全屏】（无边框铺满显示器）—— 那才是玩家想要的效果。
       仍去掉 WS_THICKFRAME：游戏内部固定 1000x650 等比缩放，
       拖边框拉伸只会让黑边变化、鼠标坐标换算更容易出错，没有收益。 */
    style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME;
    r.left = 0; r.top = 0; r.right = VIEW_W; r.bottom = VIEW_H;
    AdjustWindowRect(&r, style, FALSE);
    hWnd = CreateWindowExW(0, L"PvZClassC", L"植物大战僵尸 — C 语言复刻版",
                           style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, hInst, NULL);
    if (!hWnd) return 1;
    hWndMain = hWnd;

    createLayers(hWnd);        /* 窗口相关的层建立留在桌面端 */

    /* 可用 PVZ_GDI=1 强制关闭 GPU，便于极老显卡或远程桌面环境排障。 */
    {
        wchar_t forceGdi[8];
        if (GetEnvironmentVariableW(L"PVZ_GDI", forceGdi, 8) == 0 &&
            gpuInit(hWnd, VIEW_W, VIEW_H))
            SetWindowTextW(hWnd, L"植物大战僵尸 — C 语言复刻版 · GPU");
        else
            SetWindowTextW(hWnd, L"植物大战僵尸 — C 语言复刻版 · GDI 兼容模式");
    }

    /* 世界变换与 GM_ADVANCED 已由 createLayers() 设好（见那里的注释：
       那属于"建层"的一部分，留在 WinMain 会让 headless / iOS 拿到错画布）。 */

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    gameBoot();          /* 字体 + 载图 + 开局（与网页版共用，见其定义处） */

    prev = GetTickCount();
    while (running) {
        float dt;

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

        /* 一帧的全部工作都在 gameStep 里 —— 网页版 / iOS 调的是同一个函数，
           区别只是它们的循环外壳（rAF / CADisplayLink）与呈现方式。 */
        {
            HDC hdc = GetDC(hWnd);
            gameStep(dt, hdc);
            ReleaseDC(hWnd, hdc);
        }
        {
            float rest = 1.0f / 60.0f - gLastFrameUsed;
            if (rest > 0.001f) Sleep((DWORD)(rest * 1000.0f));
            else if (rest > 0.0f) Sleep(1);
        }
    }

    /* ---------- 清理 ---------- */
    gameShutdownAll();
    DeleteObject(gWorldBM); DeleteDC(gWorldDC);
    presentRelease();
    if (gBlackBrush) { DeleteObject(gBlackBrush); gBlackBrush = NULL; }
    DeleteObject(gFrameBM); DeleteDC(gFrameDC);
    DeleteObject(gF15); DeleteObject(gF18); DeleteObject(gF22);
    DeleteObject(gF30); DeleteObject(gF54); DeleteObject(gF72);
    return 0;
}

#endif  /* PLAT_HAS_OWN_MAIN */
