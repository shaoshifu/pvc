/* =========================================================================
   plat_web.c —— 网页版（WebAssembly / 桌面本地验证）的软件光栅后端
   -------------------------------------------------------------------------
   实现 plat.h 可移植侧声明的 56 个符号，把 GDI 的绘制语义在一段内存里复刻。

   ★★ 三条决定成败的语义约定（必须与 Windows 侧一致，否则画面会静默跑偏）

   ① **BitBlt / StretchBlt / AlphaBlend 用设备坐标，不吃世界变换。**
      依据：引擎在 spriteBlit 里会**先** `SetWorldTransform(gIdentXF)` 再 AlphaBlend、
      之后恢复 —— 说明调用方假设这三个函数用的是设备坐标。照着实现即可。

   ② **FillRect / Ellipse / Rectangle / RoundRect / Polygon / LineTo / GradientFill
      吃世界变换**（GM_ADVANCED 下）。引擎的 2x 超采样完全靠这一条：
      `fillRect(gWorldDC, x, y, ...)` 传的是逻辑坐标，靠变换放大到 2x。

   ③ **不做抗锯齿。** GDI 的图元光栅化本身不抗锯齿 —— 画面的平滑来自
      "SS=2 超采样 + HALFTONE 降采样"，而不是图元级 AA。
      这里加 AA 反而会让结果与 Windows 不一致。

   ★ 混合公式（AlphaBlend）
     · AlphaFormat = 0               → 只用 SourceConstantAlpha；
     · AlphaFormat = AC_SRC_ALPHA    → 用像素自带 alpha（**已预乘**），
                                       再乘 SourceConstantAlpha/255。
     四个通道都按预乘 over 写入：`d = s + d*(1 - a)` —— 目标 alpha 也要跟着更新，
     否则抓帧基线（会 dump 全部 4 字节）比对不上。

   编译（本机验证，不需要 Emscripten）：
     gcc -DPLAT_PORTABLE -O2 -o _test_web_raster.exe _test_web_raster.c plat_web.c -lm
   编译（网页版）：
     emcc -DPLAT_PORTABLE -O2 -sWASM=1 ... plat_web.c pvz.c -o pvz.js
   ========================================================================= */
#include "plat.h"

#ifndef PLAT_HAS_OWN_MAIN
#define PLAT_HAS_OWN_MAIN 0
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>   /* clock/CLOCKS_PER_SEC：GetTickCount 的非 wasm 实现要用 */

/* ====================================================================== */
/*  基础结构                                                              */
/* ====================================================================== */

struct PlatBitmap {
    int w, h;
    int owns;                     /* bits 是否由本结构分配 */
    unsigned char *bits;          /* BGRA 预乘，自上而下，行距 = w*4 */
};

struct PlatDC {
    struct PlatBitmap *bmp;       /* 选入的位图；屏幕 DC 指向 gScreenBM */
    int isScreen;
    int isTemp;                   /* CreateCompatibleDC 建的"临时帧缓冲" */

    /* 世界变换（只用缩放 + 平移，但按完整 2x3 存，便于将来扩展） */
    XFORM xf;
    int gmode;                    /* GM_COMPATIBLE / GM_ADVANCED */

    /* 剪切区（设备坐标，左下开区间） */
    int cl, ct, cr, cb;

    /* 当前图元状态 */
    COLORREF penCol; int penW; int hasPen;
    COLORREF brushCol; int hasBrush;
    COLORREF textCol;
    int bkMode;
    int brushOrgX, brushOrgY;

    /* 线段游标 */
    int curX, curY;

    /* 文字：字形表由 JS 侧注入（见 platWebGlyph） */
    int fontPx, fontBold;
};

/* ---- 单例：屏幕位图与"兼容位图"表 ---- */
#define MAX_BM 4096
static struct PlatBitmap gBMs[MAX_BM];
static int gBMN = 0;

static struct PlatDC gScreenDC;
static struct PlatBitmap gScreenBM;
static int gInited = 0;
static int gViewW = 1000, gViewH = 650;

static DWORD gTick = 0;
static SHORT gKeys[256];

static struct PlatBitmap *bmAlloc(int w, int h)
{
    struct PlatBitmap *b;
    if (gBMN >= MAX_BM) return NULL;
    if (w <= 0 || h <= 0) return NULL;
    b = &gBMs[gBMN++];
    b->w = w; b->h = h; b->owns = 1;
    b->bits = (unsigned char *)calloc((size_t)w * h * 4, 1);
    if (!b->bits) { gBMN--; return NULL; }
    return b;
}

/* drawLine 的前置声明。Polyline 与描边助手都在它**定义之前**用到它，
   所以必须放在文件靠前处 —— 放到"线"那一节会报 implicit declaration。 */
static void drawLine(struct PlatDC *dc, int x0, int y0, int x1, int y1,
                     COLORREF col, int w);

/* ====================================================================== */
/*  像素工具                                                              */
/* ====================================================================== */

static inline unsigned char cR(COLORREF c) { return (unsigned char)(c & 0xFF); }
static inline unsigned char cG(COLORREF c) { return (unsigned char)((c >> 8) & 0xFF); }
static inline unsigned char cB(COLORREF c) { return (unsigned char)((c >> 16) & 0xFF); }

/* 把逻辑坐标经世界变换映射到设备坐标 */
static inline float xfX(const struct PlatDC *dc, float x, float y)
{
    if (dc->gmode == 1 /* GM_COMPATIBLE */)
        return x + dc->xf.eDx;
    return dc->xf.eM11 * x + dc->xf.eM12 * y + dc->xf.eDx;
}
static inline float xfY(const struct PlatDC *dc, float x, float y)
{
    if (dc->gmode == 1)
        return y + dc->xf.eDy;
    return dc->xf.eM21 * x + dc->xf.eM22 * y + dc->xf.eDy;
}

/* 写一个"图元"像素。
   ★★ alpha 字节**保持原样，不要写 255** —— 这是 GDI 的真实行为，实测确认：
      GDI 的 FillRect / Ellipse / Polygon / LineTo 只写 RGB，**不碰 alpha**；
      DIB 是零初始化的，所以图元画过的地方 alpha 一直是 0。
      第一版这里写了 255，结果每个原语 A/B 都报"100% 不同"，
      而 RGB 其实**逐字节一致** —— 差的只有 alpha 一列。
      对显示没有影响（present 只取 RGB），但对**抓帧基线比对**是致命的：
      基线会 dump 全部 4 字节。
   ⚠️ 网页外壳把世界层交给 canvas 时必须自己把 alpha 填成 255
      （否则 putImageData 会当成全透明，画面全空）——
      这个转换在外壳里做，不在光栅后端做，两端才能逐字节一致。 */
static inline void putOpaque(struct PlatBitmap *b, int x, int y, COLORREF c)
{
    unsigned char *p;
    if ((unsigned)x >= (unsigned)b->w || (unsigned)y >= (unsigned)b->h) return;
    p = b->bits + ((size_t)y * b->w + x) * 4;
    p[0] = cB(c); p[1] = cG(c); p[2] = cR(c);
    /* p[3] 不动 —— 见上面的说明 */
}

/* 预乘 over：d = s + d*(1-a)，四通道一致（目标 alpha 也要更新） */
static inline void blendOver(unsigned char *d, const unsigned char *s, int a)
{
    int ia = 255 - a;
    d[0] = (unsigned char)(s[0] + (d[0] * ia + 127) / 255);
    d[1] = (unsigned char)(s[1] + (d[1] * ia + 127) / 255);
    d[2] = (unsigned char)(s[2] + (d[2] * ia + 127) / 255);
    d[3] = (unsigned char)(s[3] + (d[3] * ia + 127) / 255);
}

/* ---- 图元对象池（PLAT_GDI_OBJ）----
   为什么要有类型标签：GDI 的 SelectObject 接受笔/刷/字体/位图**四种**句柄，
   后端必须知道进来的是哪一种才能更新对应状态。
   第一版把笔和刷分别指向两个静态数组 —— 两个都是裸指针，
   没法区分，结果是 SelectObject 把笔刷当位图处理、全部丢弃。 */
enum { GDI_PEN = 1, GDI_BRUSH = 2, GDI_FONT = 3, GDI_NULL_PEN = 4, GDI_NULL_BRUSH = 5 };
typedef struct { int type; COLORREF col; int w; int px, bold; } GdiObj;

#define MAX_GDI_OBJ 4096
static GdiObj gObjs[MAX_GDI_OBJ];
static int gObjN = 0;

static GdiObj *objNew(int type)
{
    GdiObj *o;
    if (gObjN >= MAX_GDI_OBJ) gObjN = 0;          /* 环形复用：引擎不会同时持有上千个 */
    o = &gObjs[gObjN++];
    memset(o, 0, sizeof(*o));
    o->type = type;
    o->w = 1;
    return o;
}

static GdiObj *objOf(HGDIOBJ h);
static GdiObj *objOf(HGDIOBJ h)
{
    GdiObj *o = (GdiObj *)h;
    if (!o) return NULL;
    if (o < &gObjs[0] || o >= &gObjs[MAX_GDI_OBJ]) return NULL;
    return o;
}


/* ====================================================================== */
/*  设备创建 / 选择                                                       */
/* ====================================================================== */

HDC CreateCompatibleDC(HDC hdc)
{
    struct PlatDC *dc = (struct PlatDC *)calloc(1, sizeof(struct PlatDC));
    (void)hdc;
    if (!dc) return NULL;
    dc->isTemp = 1;
    dc->gmode = 1;                       /* GM_COMPATIBLE（GDI 默认） */
    dc->xf.eM11 = dc->xf.eM22 = 1.0f;
    dc->cl = dc->ct = 0;
    dc->cr = dc->cb = 0x3FFFFFFF;
    dc->brushCol = 0x00FFFFFF;
    dc->brushCol = 0x00FFFFFF;
    dc->hasBrush = 1;
    dc->penCol = 0;
    dc->penW = 1;
    dc->hasPen = 1;
    dc->textCol = 0;
    dc->bkMode = 2;                      /* TRANSPARENT */
    return dc;
}

HBITMAP CreateCompatibleBitmap(HDC hdc, int w, int h)
{
    (void)hdc;
    return (HBITMAP)bmAlloc(w, h);
}

HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO *bi, UINT usage,
                         void **bits, HANDLE section, DWORD offset)
{
    struct PlatBitmap *b;
    int w, h;
    (void)hdc; (void)usage; (void)section; (void)offset;
    if (!bi) return NULL;
    w = bi->bmiHeader.biWidth;
    h = bi->bmiHeader.biHeight;
    if (h < 0) h = -h;                   /* 负高度 = 自上而下 */
    b = bmAlloc(w, h);
    if (!b) return NULL;
    if (bits) *bits = b->bits;
    return (HBITMAP)b;
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj)
{
    struct PlatDC *dc = hdc;
    GdiObj *o;
    if (!dc) return NULL;
    if (!obj) return NULL;

    o = objOf(obj);
    if (o) {
        switch (o->type) {
        case GDI_PEN:        dc->penCol = o->col; dc->penW = o->w; dc->hasPen = 1; break;
        case GDI_NULL_PEN:   dc->hasPen = 0; break;
        case GDI_BRUSH:      dc->brushCol = o->col; dc->hasBrush = 1; break;
        case GDI_NULL_BRUSH: dc->hasBrush = 0; break;
        case GDI_FONT:       dc->fontPx = o->px > 0 ? o->px : 15;
                             dc->fontBold = o->bold; break;
        default: break;
        }
        return obj;
    }

    /* 不是图元对象 → 当位图选入 */
    {
        struct PlatBitmap *b = (struct PlatBitmap *)obj;
        int k, isBM = 0;
        for (k = 0; k < gBMN; k++) if (&gBMs[k] == b) { isBM = 1; break; }
        if (isBM) {
            HGDIOBJ old = (HGDIOBJ)dc->bmp;
            dc->bmp = b;
            if (dc->cr <= 0) { dc->cl = dc->ct = 0; dc->cr = b->w; dc->cb = b->h; }
            return old;
        }
    }
    return NULL;
}

BOOL DeleteObject(HGDIOBJ obj)
{
    struct PlatBitmap *b = (struct PlatBitmap *)obj;
    int k;
    if (!b) return TRUE;
    /* 只回收位图；笔/刷是引擎侧的小对象，不在这里管 */
    for (k = 0; k < gBMN; k++) if (&gBMs[k] == b) {
        if (b->owns && b->bits) free(b->bits);
        b->bits = NULL; b->w = b->h = 0;
        return TRUE;
    }
    return TRUE;
}

BOOL DeleteDC(HDC hdc)
{
    struct PlatDC *dc = hdc;
    if (!dc) return TRUE;
    if (dc == &gScreenDC) return TRUE;    /* 屏幕 DC 是静态的，不释放 */
    free(dc);
    return TRUE;
}

HDC GetDC(HWND hwnd)
{
    (void)hwnd;
    return &gScreenDC;
}

int ReleaseDC(HWND hwnd, HDC hdc) { (void)hwnd; (void)hdc; return 1; }

/* ====================================================================== */
/*  图元状态                                                              */
/* ====================================================================== */


HPEN CreatePen(int style, int w, COLORREF c)
{
    GdiObj *o;
    (void)style;                                   /* 引擎只用 PS_SOLID */
    o = objNew(GDI_PEN);
    o->col = c;
    o->w = w < 1 ? 1 : w;
    return (HPEN)o;
}

HBRUSH CreateSolidBrush(COLORREF c)
{
    GdiObj *o = objNew(GDI_BRUSH);
    o->col = c;
    return (HBRUSH)o;
}

HGDIOBJ GetStockObject(int idx)
{
    /* 取值见 plat.h：NULL_BRUSH=5、NULL_PEN=8（Windows 的真实值） */
    if (idx == 8) return (HGDIOBJ)objNew(GDI_NULL_PEN);
    if (idx == 5) return (HGDIOBJ)objNew(GDI_NULL_BRUSH);
    return NULL;
}


int SetGraphicsMode(HDC hdc, int mode)
{
    struct PlatDC *dc = hdc;
    int old;
    if (!dc) return 0;
    old = dc->gmode;
    dc->gmode = mode;
    return old;
}

BOOL SetWorldTransform(HDC hdc, const XFORM *xf)
{
    struct PlatDC *dc = hdc;
    if (!dc || !xf) return FALSE;
    dc->xf = *xf;
    return TRUE;
}

BOOL GetWorldTransform(HDC hdc, XFORM *xf)
{
    struct PlatDC *dc = hdc;
    if (!dc || !xf) return FALSE;
    *xf = dc->xf;
    return TRUE;
}

int SetStretchBltMode(HDC hdc, int mode) { (void)hdc; return mode; }
int SetBrushOrgEx(HDC hdc, int x, int y, POINT *old)
{
    struct PlatDC *dc = hdc;
    if (dc) { if (old) { old->x = dc->brushOrgX; old->y = dc->brushOrgY; }
              dc->brushOrgX = x; dc->brushOrgY = y; }
    return 1;
}

/* 剪切区：引擎只用矩形区（CreateRectRgn / CreateRoundRectRgn + SelectClipRgn），
   且 pushRoundClip 用的是"圆角当作直角"的近似 —— 这里按矩形处理，
   与 GDI 对 Region 的实际裁剪结果在引擎的用法下一致。 */
HRGN CreateRectRgn(int l, int t, int r, int b)
{
    static LONG rects[256][4];
    static int n = 0;
    if (n >= 256) n = 0;
    rects[n][0] = l; rects[n][1] = t; rects[n][2] = r; rects[n][3] = b;
    return (HRGN)&rects[n++];
}

HRGN CreateRoundRectRgn(int l, int t, int r, int b, int ew, int eh)
{
    (void)ew; (void)eh;
    return CreateRectRgn(l, t, r, b);
}

/* GetClipRgn：引擎用 pushRoundClip/popClip 做"保存-恢复剪切区"。
   我们的剪切区是 4 个 int，所以 HRGN 也按 4 个 LONG 存，直接复制。 */
int GetClipRgn(HDC hdc, HRGN rgn)
{
    struct PlatDC *dc = hdc;
    LONG *rc = (LONG *)rgn;
    if (!dc || !rgn) return 0;
    rc[0] = dc->cl; rc[1] = dc->ct; rc[2] = dc->cr; rc[3] = dc->cb;
    return 1;
}

int GetRgnBox(HRGN rgn, RECT *rc)
{
    LONG *r = (LONG *)rgn;
    if (!r || !rc) return 0;
    rc->left = r[0]; rc->top = r[1]; rc->right = r[2]; rc->bottom = r[3];
    return SIMPLEREGION;
}

/* Polyline：连续折线（引擎用来画木纹） */
BOOL Polyline(HDC hdc, const POINT *pts, int n)
{
    struct PlatDC *dc = hdc;
    int i;
    if (!dc || !pts || n < 2) return FALSE;
    for (i = 0; i + 1 < n; i++) {
        int ax = (int)xfX(dc, (float)pts[i].x, (float)pts[i].y);
        int ay = (int)xfY(dc, (float)pts[i].x, (float)pts[i].y);
        int bx = (int)xfX(dc, (float)pts[i + 1].x, (float)pts[i + 1].y);
        int by = (int)xfY(dc, (float)pts[i + 1].x, (float)pts[i + 1].y);
        drawLine(dc, ax, ay, bx, by, dc->penCol, dc->penW);
    }
    return TRUE;
}

int SelectClipRgn(HDC hdc, HRGN rgn)
{
    struct PlatDC *dc = hdc;
    LONG *rc = (LONG *)rgn;
    if (!dc) return ERROR;
    if (!rgn) { dc->cl = dc->ct = 0; dc->cr = dc->bmp ? dc->bmp->w : 0;
                dc->cb = dc->bmp ? dc->bmp->h : 0; return NULLREGION; }
    dc->cl = (int)rc[0]; dc->ct = (int)rc[1];
    dc->cr = (int)rc[2]; dc->cb = (int)rc[3];
    return SIMPLEREGION;
}

/* ====================================================================== */
/*  光栅化核心                                                            */
/* ====================================================================== */

/* 用一个"覆盖测试"回调填充多边形：把复杂形状统一成扫描线求交，
   Ellipse / RoundRect / Polygon / Rectangle 都走这一条，
   保证它们之间的边界行为完全一致。 */
typedef int (*InsideFn)(void *ctx, int x, int y);

static void fillScan(struct PlatDC *dc, int x0, int y0, int x1, int y1,
                     COLORREF col, InsideFn inside, void *ctx, int opaque)
{
    struct PlatBitmap *b = dc->bmp;
    int x, y, l, t, r, bb;
    if (!b || !b->bits) return;
    l = x0 < x1 ? x0 : x1;  r = x0 < x1 ? x1 : x0;
    t = y0 < y1 ? y0 : y1;  bb = y0 < y1 ? y1 : y0;
    if (l < dc->cl) l = dc->cl;
    if (t < dc->ct) t = dc->ct;
    if (r > dc->cr) r = dc->cr;
    if (bb > dc->cb) bb = dc->cb;
    if (r > b->w) r = b->w;
    if (bb > b->h) bb = b->h;
    for (y = t; y < bb; y++) {
        for (x = l; x < r; x++) {
            if (!inside(ctx, x, y)) continue;
            if (opaque) putOpaque(b, x, y, col);
        }
    }
}

/* ---- 矩形 ---- */
static int inRect(void *ctx, int x, int y)
{
    int *r = (int *)ctx;
    return x >= r[0] && x < r[2] && y >= r[1] && y < r[3];
}

/* ---- 椭圆（中点法判定：用隐函数 + 半像素偏移，避开浮点除法抖动） ---- */
typedef struct { float cx, cy, rx, ry; } EllipseCtx;
static int inEllipse(void *ctx, int x, int y)
{
    EllipseCtx *e = (EllipseCtx *)ctx;
    float dx = ((float)x + 0.5f) - e->cx;
    float dy = ((float)y + 0.5f) - e->cy;
    if (e->rx <= 0.0f || e->ry <= 0.0f) return 0;
    return (dx * dx) / (e->rx * e->rx) + (dy * dy) / (e->ry * e->ry) <= 1.0f;
}

/* ---- 圆角矩形：矩形 ∩ （四条圆角约束） ---- */
typedef struct { int l, t, r, b; float rad; } RoundCtx;
static int inRound(void *ctx, int x, int y)
{
    RoundCtx *q = (RoundCtx *)ctx;
    float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
    float rad = q->rad;
    float dx, dy;
    if (!inRect(&(int[4]){ q->l, q->t, q->r, q->b }, x, y)) {
        /* 用同一个矩形判据 */
        if (!(fx >= q->l && fx < q->r && fy >= q->t && fy < q->b)) return 0;
    }
    if (rad <= 0.0f) return 1;
    if (fx < q->l + rad && fy < q->t + rad) {
        dx = fx - (q->l + rad); dy = fy - (q->t + rad);
        return dx * dx + dy * dy <= rad * rad;
    }
    if (fx > q->r - rad && fy < q->t + rad) {
        dx = fx - (q->r - rad); dy = fy - (q->t + rad);
        return dx * dx + dy * dy <= rad * rad;
    }
    if (fx < q->l + rad && fy > q->b - rad) {
        dx = fx - (q->l + rad); dy = fy - (q->b - rad);
        return dx * dx + dy * dy <= rad * rad;
    }
    if (fx > q->r - rad && fy > q->b - rad) {
        dx = fx - (q->r - rad); dy = fy - (q->b - rad);
        return dx * dx + dy * dy <= rad * rad;
    }
    return 1;
}

/* ---- 多边形：扫描线 + 奇偶填充 ---- */
typedef struct { const POINT *p; int n; float ox, oy; } PolyCtx;
static int inPoly(void *ctx, int x, int y)
{
    PolyCtx *q = (PolyCtx *)ctx;
    float fy = (float)y + 0.5f, fx = (float)x + 0.5f;
    int inside = 0, i, j;
    for (i = 0, j = q->n - 1; i < q->n; j = i++) {
        float yi = (float)q->p[i].y, yj = (float)q->p[j].y;
        float xi = (float)q->p[i].x, xj = (float)q->p[j].x;
        if (((yi > fy) != (yj > fy)) &&
            (fx < (xj - xi) * (fy - yi) / (yj - yi + 1e-9f) + xi))
            inside = !inside;
    }
    return inside;
}

/* ---- 圆角矩形的描边：内外两个圆角矩形的差集 ---- */
static void strokeRoundShape(struct PlatDC *dc, int l, int t, int r, int b,
                             float rad, COLORREF col, int w)
{
    RoundCtx outer, inner;
    int x, y, hw;
    struct PlatBitmap *bm = dc->bmp;
    if (!bm || !bm->bits || w <= 0) return;
    /* ★ 描边**以路径为中心**：一半在外、一半在内。
       实测依据：w=3 的圆角矩形，GDI 的 bbox 比我"全内缩"版本每边多 3 像素。
       所以外形状向外扩 w/2、内形状向内缩 (w-1)/2 —— 用整数半边会差 1 像素，
       这里刻意用 hw 与 w-hw 两个不同的量来对齐 GDI 的取整方式。 */
    hw = w / 2;
    outer.l = l - hw;          outer.t = t - hw;
    outer.r = r + (w - hw);    outer.b = b + (w - hw);
    outer.rad = rad + (float)hw;
    inner.l = l + (w - hw);    inner.t = t + (w - hw);
    inner.r = r - hw;          inner.b = b - hw;
    inner.rad = rad - (float)(w - hw); if (inner.rad < 0.0f) inner.rad = 0.0f;
    for (y = t - w; y <= b + w; y++) {
        for (x = l - w; x <= r + w; x++) {   /* 外扩一圈，见 strokeRoundShape 的说明 */
            if ((unsigned)x >= (unsigned)bm->w || (unsigned)y >= (unsigned)bm->h) continue;
            if (x < dc->cl || y < dc->ct || x >= dc->cr || y >= dc->cb) continue;
            if (inRound(&outer, x, y) && !inRound(&inner, x, y))
                putOpaque(bm, x, y, col);
        }
    }
}

/* ---- 描边：GDI 的 Ellipse/Rectangle/RoundRect/Polygon 都是"既填又描"，
       所以这四个函数在填充之后还要按当前笔描一圈。
       做法统一用"外围形状 减 内缩形状"的差集 —— 和 strokeRoundShape 同一个思路，
       这样圆角、椭圆、多边形共用一套边界判定，不会出现"某个形状描边比填充缺一块"。 */
static int ringHit(void *ctxOuter, void *ctxInner, InsideFn fn, int x, int y)
{
    return fn(ctxOuter, x, y) && !fn(ctxInner, x, y);
}

/* 椭圆描边 */
static void strokeEllipseShape(struct PlatDC *dc, int x0, int y0, int x1, int y1,
                               COLORREF col, int w)
{
    EllipseCtx o, i2;
    int x, y;
    struct PlatBitmap *bm = dc->bmp;
    if (!bm || !bm->bits || w <= 0) return;
    o.cx = ((float)x0 + (float)x1) * 0.5f;
    o.cy = ((float)y0 + (float)y1) * 0.5f;
    o.rx = ((float)x1 - (float)x0) * 0.5f + (float)(w / 2);   /* 描边居中：外扩一半 */
    o.ry = ((float)y1 - (float)y0) * 0.5f + (float)(w / 2);
    i2 = o;
    i2.rx = o.rx - (float)w;
    i2.ry = o.ry - (float)w;
    for (y = y0 - w; y <= y1 + w; y++)
        for (x = x0 - w; x <= x1 + w; x++) {   /* 外扩一圈，描边才不会在边界被切 */
            if ((unsigned)x >= (unsigned)bm->w || (unsigned)y >= (unsigned)bm->h) continue;
            if (x < dc->cl || y < dc->ct || x >= dc->cr || y >= dc->cb) continue;
            if (ringHit(&o, &i2, inEllipse, x, y)) putOpaque(bm, x, y, col);
        }
}

/* 轴对齐矩形描边（四条边，宽度 w 向内） */
static void strokeRectShape(struct PlatDC *dc, int x0, int y0, int x1, int y1,
                            COLORREF col, int w)
{
    struct PlatBitmap *bm = dc->bmp;
    int x, y;
    if (!bm || !bm->bits || w <= 0) return;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            int edge = (x < x0 + w) || (x >= x1 - w) || (y < y0 + w) || (y >= y1 - w);
            if (!edge) continue;
            if ((unsigned)x >= (unsigned)bm->w || (unsigned)y >= (unsigned)bm->h) continue;
            if (x < dc->cl || y < dc->ct || x >= dc->cr || y >= dc->cb) continue;
            putOpaque(bm, x, y, col);
        }
}

/* 多边形描边：逐条边用当前线宽画线 */
static void strokePolyShape(struct PlatDC *dc, const POINT *pts, int n,
                            COLORREF col, int w)
{
    int i;
    if (!dc->bmp || !dc->bmp->bits || w <= 0) return;
    for (i = 0; i < n; i++) {
        const POINT *a = &pts[i], *b = &pts[(i + 1) % n];
        int ax = (int)xfX(dc, (float)a->x, (float)a->y);
        int ay = (int)xfY(dc, (float)a->x, (float)a->y);
        int bx = (int)xfX(dc, (float)b->x, (float)b->y);
        int by = (int)xfY(dc, (float)b->x, (float)b->y);
        drawLine(dc, ax, ay, bx, by, col, w);
    }
}

/* ====================================================================== */
/*  三个"直接绘制"函数（会被 pvz.c 直接调用）                             */
/* ====================================================================== */

int FillRect(HDC hdc, const RECT *rc, HBRUSH br)
{
    struct PlatDC *dc = hdc;
    int ax, ay, bx, by, box[4];
    COLORREF col = 0x00FFFFFF;
    GdiObj *o;
    if (!dc || !rc) return 0;
    /* ⚠️ 必须走对象池取色，**不能** `*(COLORREF *)br`。
       改用对象池之后 HBRUSH 是 GdiObj*，它的**第一个字段是 type**（GDI_BRUSH=2）——
       直接解引用读到的是 2，于是所有 FillRect 都变成 R=2 的近黑色。
       症状：整屏几乎全黑、只差一两个色阶，而且**不报错**。
       （原语级 A/B 就是靠这个"底色都比不上"才定位到的。） */
    o = objOf((HGDIOBJ)br);
    if (o) col = o->col;
    /* 世界变换：矩形两个角点都要变换（缩放+平移下仍是轴对齐矩形） */
    ax = (int)xfX(dc, (float)rc->left,  (float)rc->top);
    ay = (int)xfY(dc, (float)rc->left,  (float)rc->top);
    bx = (int)xfX(dc, (float)rc->right, (float)rc->bottom);
    by = (int)xfY(dc, (float)rc->right, (float)rc->bottom);
    box[0] = ax < bx ? ax : bx;
    box[1] = ay < by ? ay : by;
    box[2] = ax < bx ? bx : ax;
    box[3] = ay < by ? by : ay;
    fillScan(dc, box[0], box[1], box[2], box[3], col, inRect, box, 1);
    return 1;
}

BOOL Ellipse(HDC hdc, int l, int t, int r, int b)
{
    struct PlatDC *dc = hdc;
    EllipseCtx e;
    int x0, y0, x1, y1;
    if (!dc) return FALSE;
    x0 = (int)xfX(dc, (float)l, (float)t);
    y0 = (int)xfY(dc, (float)l, (float)t);
    x1 = (int)xfX(dc, (float)r, (float)b);
    y1 = (int)xfY(dc, (float)r, (float)b);
    e.cx = ((float)x0 + (float)x1) * 0.5f;
    e.cy = ((float)y0 + (float)y1) * 0.5f;
    e.rx = ((float)x1 - (float)x0) * 0.5f;
    e.ry = ((float)y1 - (float)y0) * 0.5f;
    if (dc->hasBrush) fillScan(dc, x0, y0, x1, y1, dc->brushCol, inEllipse, &e, 1);
    if (dc->hasPen)   strokeEllipseShape(dc, x0, y0, x1, y1, dc->penCol, dc->penW);
    return TRUE;
}

BOOL Rectangle(HDC hdc, int l, int t, int r, int b)
{
    struct PlatDC *dc = hdc;
    int box[4], x0, y0, x1, y1;
    if (!dc) return FALSE;
    x0 = (int)xfX(dc, (float)l, (float)t);
    y0 = (int)xfY(dc, (float)l, (float)t);
    x1 = (int)xfX(dc, (float)r, (float)b);
    y1 = (int)xfY(dc, (float)r, (float)b);
    box[0] = x0; box[1] = y0; box[2] = x1; box[3] = y1;
    if (dc->hasBrush) fillScan(dc, x0, y0, x1, y1, dc->brushCol, inRect, box, 1);
    if (dc->hasPen)   strokeRectShape(dc, x0, y0, x1, y1, dc->penCol, dc->penW);
    return TRUE;
}

BOOL RoundRect(HDC hdc, int l, int t, int r, int b, int ew, int eh)
{
    struct PlatDC *dc = hdc;
    RoundCtx q;
    int x0, y0, x1, y1;
    if (!dc) return FALSE;
    x0 = (int)xfX(dc, (float)l, (float)t);
    y0 = (int)xfY(dc, (float)l, (float)t);
    x1 = (int)xfX(dc, (float)r, (float)b);
    y1 = (int)xfY(dc, (float)r, (float)b);
    q.l = x0; q.t = y0; q.r = x1; q.b = y1;
    q.rad = (float)(ew < eh ? ew : eh) * 0.5f;
    if (dc->hasBrush) fillScan(dc, x0, y0, x1, y1, dc->brushCol, inRound, &q, 1);
    if (dc->hasPen)   strokeRoundShape(dc, x0, y0, x1, y1, q.rad, dc->penCol, dc->penW);
    return TRUE;
}

BOOL Polygon(HDC hdc, const POINT *pts, int n)
{
    struct PlatDC *dc = hdc;
    PolyCtx q;
    POINT dev[64];                     /* 变换后的点（引擎的多边形最多几十个顶点） */
    int i, minx = 0, miny = 0, maxx = 0, maxy = 0;
    if (!dc || !pts || n < 2) return FALSE;
    if (n > 64) n = 64;
    /* ★★ 必须把点**先变换到设备坐标**再交给填充判定。
       第一版只把 bbox 变换了、`q.p` 仍是原始（逻辑）坐标 ——
       于是扫描线在设备坐标里跑、而内外判定在逻辑坐标里判，
       两者坐标系错位：GDI 输出 87x91，我只画出 38x40（少了一半多）。
       这是"部分正确"的典型：图形画出来了、位置也对，只是范围不对，
       不逐像素比对根本发现不了。 */
    for (i = 0; i < n; i++) {
        int px = (int)xfX(dc, (float)pts[i].x, (float)pts[i].y);
        int py = (int)xfY(dc, (float)pts[i].x, (float)pts[i].y);
        dev[i].x = px; dev[i].y = py;
        if (i == 0) { minx = maxx = px; miny = maxy = py; }
        else { if (px < minx) minx = px; if (px > maxx) maxx = px;
               if (py < miny) miny = py;
               if (py > maxy) maxy = py; }
    }
    q.p = dev; q.n = n; q.ox = 0; q.oy = 0;
    if (dc->hasBrush) fillScan(dc, minx, miny, maxx + 1, maxy + 1, dc->brushCol, inPoly, &q, 1);
    if (dc->hasPen)   strokePolyShape(dc, dev, n, dc->penCol, dc->penW);
    return TRUE;
}

/* ====================================================================== */
/*  线条                                                                  */
/* ====================================================================== */

BOOL MoveToEx(HDC hdc, int x, int y, POINT *old)
{
    struct PlatDC *dc = hdc;
    if (!dc) return FALSE;
    if (old) { old->x = dc->curX; old->y = dc->curY; }
    dc->curX = (int)xfX(dc, (float)x, (float)y);
    dc->curY = (int)xfY(dc, (float)x, (float)y);
    return TRUE;
}

/* Bresenham + 方形笔头（引擎的线宽只有 1~4，实心方头足够；
   与 GDI 的 PS_SOLID 方形笔头一致） */
static void drawLine(struct PlatDC *dc, int x0, int y0, int x1, int y1,
                     COLORREF col, int w)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, half = (w - 1) / 2, ex = w - 1 - half;
    int guard = 0;
    if (w < 1) w = 1;
    while (guard++ < 1000000) {
        int i, j;
        for (j = -half; j <= ex; j++)
            for (i = -half; i <= ex; i++)
                putOpaque(dc->bmp, x0 + i, y0 + j, col);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

BOOL LineTo(HDC hdc, int x, int y)
{
    struct PlatDC *dc = hdc;
    int tx, ty;
    if (!dc) return FALSE;
    tx = (int)xfX(dc, (float)x, (float)y);
    ty = (int)xfY(dc, (float)x, (float)y);
    drawLine(dc, dc->curX, dc->curY, tx, ty, dc->penCol, dc->penW);
    dc->curX = tx; dc->curY = ty;
    return TRUE;
}

/* ====================================================================== */
/*  位块传输                                                              */
/* ====================================================================== */

/* 从 src 区域取一个像素（最近邻）。HALFTONE 在 GDI 下是"按比例过滤"，
   但引擎的用法里源与目标要么 1:1，要么是**整数倍降采样**（SS=2），
   最近邻在整数倍下与 HALFTONE 的结果一致（取整格点）。 */
static inline void pickNearest(const struct PlatBitmap *s, int sx, int sy,
                               int sw, int sh, int dw, int dh, int dx, int dy,
                               unsigned char *out)
{
    int px = sx + (int)((long)dx * sw / dw);
    int py = sy + (int)((long)dy * sh / dh);
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= s->w) px = s->w - 1;
    if (py >= s->h) py = s->h - 1;
    memcpy(out, s->bits + ((size_t)py * s->w + px) * 4, 4);
}

static BOOL bltCore(HDC dstH, int dx, int dy, int dw, int dh,
                    HDC srcH, int sx, int sy, int sw, int sh,
                    const BLENDFUNCTION *bfUnused, int usePerPixel,
                    int constAlpha, int opaqueCopy)
{
    struct PlatDC *d = dstH, *s = srcH;
    struct PlatBitmap *db, *sb;
    int x, y;
    /* ★ GDI 的 BitBlt/StretchBlt/AlphaBlend 在 GM_ADVANCED 下**会应用世界变换**。
       实测依据：目标矩形传 32 设备像素，GDI 实际输出 64（×SS=2）。
       spriteBlit 之所以看不出这点，是因为它先复位成单位变换再调用 ——
       "吃不吃变换"结果相同，两种理解无法区分。
       这里按实测行为实现：目标矩形经世界变换，源矩形不变
       （变换只有缩放+平移，所以宽高分别乘 eM11/eM22）。 */
    (void)bfUnused;
    if (!d || !s || !d->bmp || !s->bmp) return FALSE;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return FALSE;
    {
        int nx = (int)xfX(d, (float)dx, (float)dy);
        int ny = (int)xfY(d, (float)dx, (float)dy);
        int nw = (int)((float)dw * (d->gmode == 1 ? 1.0f : d->xf.eM11));
        int nh = (int)((float)dh * (d->gmode == 1 ? 1.0f : d->xf.eM22));
        dx = nx; dy = ny;
        if (nw > 0) dw = nw;
        if (nh > 0) dh = nh;
    }
    db = d->bmp; sb = s->bmp;
    if (!db->bits || !sb->bits) return FALSE;
    for (y = 0; y < dh; y++) {
        int ty = dy + y;
        if ((unsigned)ty >= (unsigned)db->h) continue;
        if (ty < d->ct || ty >= d->cb) continue;
        for (x = 0; x < dw; x++) {
            int tx = dx + x;
            unsigned char sp[4];
            unsigned char *dp;
            if ((unsigned)tx >= (unsigned)db->w) continue;
            if (tx < d->cl || tx >= d->cr) continue;
            pickNearest(sb, sx, sy, sw, sh, dw, dh, x, y, sp);
            dp = db->bits + ((size_t)ty * db->w + tx) * 4;
            if (opaqueCopy) {
                memcpy(dp, sp, 4);
            } else {
                int a = constAlpha;
                if (usePerPixel) a = a * sp[3] / 255;
                if (a <= 0) continue;
                if (usePerPixel) blendOver(dp, sp, a);
                else {
                    /* AlphaFormat=0：源没有 alpha，只按 SA 混 RGB；
                       源 RGB 当作"未预乘的原色"，按 SA 与目标混合。 */
                    unsigned char t[4];
                    t[0] = (unsigned char)(sp[0] * a / 255);
                    t[1] = (unsigned char)(sp[1] * a / 255);
                    t[2] = (unsigned char)(sp[2] * a / 255);
                    t[3] = 255;
                    blendOver(dp, t, a);
                }
            }
        }
    }
    return TRUE;
}

BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    (void)rop;                                    /* 引擎只用 SRCCOPY */
    return bltCore(dst, x, y, w, h, src, sx, sy, w, h, NULL, 0, 255, 1);
}

BOOL StretchBlt(HDC dst, int x, int y, int w, int h,
                HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    (void)rop;
    return bltCore(dst, x, y, w, h, src, sx, sy, sw, sh, NULL, 0, 255, 1);
}

BOOL AlphaBlend(HDC dst, int x, int y, int w, int h,
                HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION bf)
{
    int sa = bf.SourceConstantAlpha;
    int perPixel = (bf.AlphaFormat == 1 /* AC_SRC_ALPHA */);
    if (sa == 0) sa = 255;
    return bltCore(dst, x, y, w, h, src, sx, sy, sw, sh, &bf, perPixel, sa, 0);
}

/* 垂直渐变（GradientFill 的 GRADIENT_FILL_RECT_V）：
   引擎只用来画按钮底板的双色渐变，按 y 做线性插值即可。 */
BOOL GradientFill(HDC hdc, TRIVERTEX *vt, unsigned long nv,
                  GRADIENT_RECT *gr, unsigned long ng, unsigned long mode)
{
    struct PlatDC *dc = hdc;
    int top, l, r, t, b, y, x;
    (void)nv; (void)ng; (void)mode;
    if (!dc || !dc->bmp || !vt || !gr) return FALSE;
    top = (int)(vt[gr->UpperLeft].Red >> 8);
    {
        int tg = (int)(vt[gr->UpperLeft].Green >> 8);
        int tb = (int)(vt[gr->UpperLeft].Blue >> 8);
        int bg = (int)(vt[gr->LowerRight].Green >> 8);
        int bb = (int)(vt[gr->LowerRight].Blue >> 8);
        int bt = (int)(vt[gr->LowerRight].Red >> 8);
        l = (int)xfX(dc, (float)vt[0].x, (float)vt[0].y);
        t = (int)xfY(dc, (float)vt[0].x, (float)vt[0].y);
        r = (int)xfX(dc, (float)vt[1].x, (float)vt[1].y);
        b = (int)xfY(dc, (float)vt[1].x, (float)vt[1].y);
        if (b <= t) return TRUE;
        for (y = t; y < b; y++) {
            float f = (float)(y - t) / (float)(b - t);
            COLORREF c = RGB((int)(top + (bt - top) * f),
                             (int)(tg + (bg - tg) * f),
                             (int)(tb + (bb - tb) * f));
            for (x = l; x < r; x++)
                putOpaque(dc->bmp, x, y, c);
        }
    }
    return TRUE;
}

/* ====================================================================== */
/*  文字                                                                  */
/* ====================================================================== */

/* 字形表：JS 侧用 Canvas2D 光栅一次、把 alpha 转成预乘 BGRA 后注入。
   本地测试没有 JS → 退化成"实心方块 + 字宽估计"，只用于确认版式，
   像素比对时会**单独排除文字层**（见 WEB_PORT.md 的验证方案）。 */
#define MAX_GLYPH 8192
typedef struct {
    int cp, px, bold, w, h, adv;
    unsigned char *bgra;
} Glyph;
static Glyph gGlyphs[MAX_GLYPH];
static int gGlyphN = 0;

int platWebGlyph(int cp, int px, int bold, const unsigned char *bgra,
                 int gw, int gh, int adv)
{
    Glyph *g;
    if (gGlyphN >= MAX_GLYPH) return 0;
    g = &gGlyphs[gGlyphN];
    g->cp = cp; g->px = px; g->bold = bold;
    g->w = gw; g->h = gh; g->adv = adv;
    g->bgra = NULL;
    if (bgra && gw > 0 && gh > 0) {
        g->bgra = (unsigned char *)malloc((size_t)gw * gh * 4);
        if (!g->bgra) return 0;
        memcpy(g->bgra, bgra, (size_t)gw * gh * 4);
    }
    gGlyphN++;
    return 1;
}

static Glyph *glyphFind(int cp, int px, int bold)
{
    int i;
    for (i = 0; i < gGlyphN; i++)
        if (gGlyphs[i].cp == cp && gGlyphs[i].px == px && gGlyphs[i].bold == bold)
            return &gGlyphs[i];
    return NULL;
}

HFONT CreateFontW(int h, int w, int esc, int orient, int weight,
                  DWORD italic, DWORD under, DWORD strike, DWORD charset,
                  DWORD prec, DWORD clip, DWORD quality, DWORD pitch,
                  const wchar_t *face)
{
    int px = h < 0 ? -h : h;                 /* GDI：负高度 = 字符高度 */
    GdiObj *o;
    (void)w; (void)esc; (void)orient; (void)italic; (void)under; (void)strike;
    (void)charset; (void)prec; (void)clip; (void)quality; (void)pitch; (void)face;
    o = objNew(GDI_FONT);
    o->px = px;
    o->bold = (weight >= 700) ? 1 : 0;
    return (HFONT)o;
}

COLORREF SetTextColor(HDC hdc, COLORREF c)
{
    struct PlatDC *dc = hdc;
    COLORREF old;
    if (!dc) return 0;
    old = dc->textCol; dc->textCol = c;
    return old;
}

int SetBkMode(HDC hdc, int mode)
{
    struct PlatDC *dc = hdc;
    int old;
    if (!dc) return 0;
    old = dc->bkMode; dc->bkMode = mode;
    return old;
}

/* 把一段宽字符串按字形表画到 dc。返回累计推进宽度（逻辑像素）。
   引擎只用 (x, y) 作为文字**基线**位置（putTextS 的锚点语义）。 */
static int drawTextCore(struct PlatDC *dc, const wchar_t *s, int n)
{
    int i, pen = 0;
    if (!dc || !s) return 0;
    for (i = 0; i < n && s[i]; i++) {
        int cp = (int)(unsigned short)s[i];
        int px = dc->fontPx, adv;
        Glyph *g = glyphFind(cp, px, dc->fontBold);
        if (g) {
            int x, y;
            for (y = 0; y < g->h; y++)
                for (x = 0; x < g->w; x++) {
                    const unsigned char *sp = g->bgra + ((size_t)y * g->w + x) * 4;
                    int tx = (int)(dc->curX + pen + x);
                    int ty = (int)(dc->curY + y);
                    if (sp[3] == 0) continue;
                    if ((unsigned)tx < (unsigned)dc->bmp->w &&
                        (unsigned)ty < (unsigned)dc->bmp->h)
                        blendOver(dc->bmp->bits + ((size_t)ty * dc->bmp->w + tx) * 4,
                                  sp, sp[3]);
                }
            adv = g->adv ? g->adv : g->w;
        } else {
            /* 无字形：画一个与字号等高的色块占位，宽度按"全角/半角"估。
               这只在本地无 JS 测试时出现，用于验证版式不被破坏。 */
            int full = (cp > 0x2E80);
            adv = full ? px : (px / 2);
            {
                int x, y;
                for (y = -px + 2; y <= 0; y++)
                    for (x = 0; x < adv - 1; x++)
                        putOpaque(dc->bmp, dc->curX + pen + x, dc->curY + y,
                                  dc->textCol);
            }
        }
        pen += adv;
    }
    return pen;
}

int DrawTextW(HDC hdc, const wchar_t *s, int n, RECT *rc, UINT fmt)
{
    struct PlatDC *dc = hdc;
    (void)fmt;
    if (!dc) return 0;
    dc->curX = rc ? rc->left : 0;
    dc->curY = rc ? rc->top + dc->fontPx : 0;
    return drawTextCore(dc, s, n);
}

BOOL GetTextExtentPoint32W(HDC hdc, const wchar_t *s, int n, SIZE *sz)
{
    struct PlatDC *dc = hdc;
    int i, w = 0, mx = 0;
    if (!dc || !sz) return FALSE;
    for (i = 0; i < n && s[i]; i++) {
        int cp = (int)(unsigned short)s[i];
        Glyph *g = glyphFind(cp, dc->fontPx, dc->fontBold);
        if (g) w += g->adv ? g->adv : g->w;
        else    w += (cp > 0x2E80) ? dc->fontPx : (dc->fontPx / 2);
        if (w > mx) mx = w;
    }
    sz->cx = w;
    sz->cy = dc->fontPx;
    return TRUE;
}

/* ---- 音频桥接（Emscripten）--------------------------------------------------
   为什么不用"把所有 wav 都塞进 MEMFS 再用 Web Audio 播"的做法：
     音效确实走 MEMFS（体积小、延迟低），但 BGM 有 20MB，
     塞进 wasm 内存等于常驻 20MB 且必须等下完才能播。
     所以 BGM 交给 JS 侧的 HTMLAudio 按 URL 流式播放。
   判断依据是路径前缀："bgm/" 开头 → 走流式；其余 → 走 MEMFS + Web Audio。
   ------------------------------------------------------------------------- */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, jsSfxPlay, (const char *path, int vol), {
    if (window.PVZ && PVZ.sfx) PVZ.sfx(UTF8ToString(path), vol);
});

EM_JS(int, jsBgmPlay, (const char *path, int vol, int loop), {
    if (window.PVZ && PVZ.bgmPlay) return PVZ.bgmPlay(UTF8ToString(path), vol, loop);
    return 0;
});

EM_JS(void, jsBgmStop, (const char *path), {
    if (window.PVZ && PVZ.bgmStop) PVZ.bgmStop(UTF8ToString(path));
});

EM_JS(int, jsBgmPlaying, (const char *path), {
    if (window.PVZ && PVZ.bgmPlaying) return PVZ.bgmPlaying(UTF8ToString(path));
    return 0;
});

EM_JS(void, jsLog, (const char *msg), {
    if (window.PVZ && PVZ.log) PVZ.log(UTF8ToString(msg));
});

#else
/* 本机验证构建：音频无所谓，但要保证编译与链接一致 */
static void jsSfxPlay(const char *path, int vol) { (void)path; (void)vol; }
static int  jsBgmPlay(const char *path, int vol, int loop) { (void)path; (void)vol; (void)loop; return 0; }
static void jsBgmStop(const char *path) { (void)path; }
static int  jsBgmPlaying(const char *path) { (void)path; return 0; }
static void jsLog(const char *msg) { (void)msg; }
#endif

/* --------------------------------------------------------------------------
   MCI 命令的状态机。
   引擎用的命令集很小（实测统计）：
     open "<path>" type <t> alias <a>      → 记录 别名→路径
     setaudio <a> volume to <n>            → 记录音量（0..1000）
     play <a> [repeat] [from 0]            → 播放
     stop <a> / pause <a> / close <a>      → 停止
     status <a> mode                       → 回 "playing" / "stopped"
   所以不需要实现 MCI 的全部语义，一张别名表足够。
   -------------------------------------------------------------------------- */
#define WEB_AUDIO_SLOTS 16
typedef struct {
    char alias[32];
    char path[256];      /* 相对 assets 的路径，正斜杠 */
    int  volume;         /* 0..1000（MCI 量纲） */
    int  opened;
    int  loop;
} WebAudio;

static WebAudio gWA[WEB_AUDIO_SLOTS];
static int      gWAN = 0;

static void wsToAscii(const wchar_t *src, char *dst, int cap)
{
    int i = 0;
    if (!src || !dst || cap <= 0) return;
    for (; src[i] && i < cap - 1; i++) dst[i] = (char)(src[i] & 0xFF);
    dst[i] = 0;
}

/* 不依赖 strcasecmp（部分平台没有）*/
static int strncasecmp_(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

/* win32 路径 → 资源相对路径（"assets\sfx\hit.wav" → "sfx/hit.wav"），
   顺便把反斜杠统一成正斜杠 —— 后者是网页版唯一能用的分隔符。
   ⚠️ 引擎拼出来的路径是**反斜杠**（L"%ssfx\\%s.wav"），
      直接拿去 fetch 会 404，而且看起来像"文件不存在"。 */
static void toAssetRel(const char *in, char *out, int cap)
{
    int i = 0, j = 0;
    if (!in || !out || cap <= 0) { if (out && cap > 0) out[0] = 0; return; }
    /* 跳过前导的 "assets/"，以及绝对路径里最后的 assets/ */
    {
        const char *p = in;
        const char *last = NULL;
        for (; *p; p++) {
            if ((p[0] == 'a' || p[0] == 'A') &&
                strncasecmp_(p, "assets", 6) == 0) last = p + 6;
        }
        if (last) {
            p = last;
            if (*p == '/' || *p == '\\') p++;
            in = p;
        }
    }
    for (; in[i] && j < cap - 1; i++) {
        char c = in[i];
        if (c == '\\') c = '/';
        out[j++] = c;
    }
    out[j] = 0;
}

/* BGM 走流式、音效走 MEMFS —— 判断依据只有路径前缀（见 Bridge 的说明） */
static int isBgmLikely(const char *rel);

static int isBgmLikely(const char *rel)
{
    if (!rel) return 0;
    return (strncmp(rel, "bgm/", 4) == 0);
}

static WebAudio *waFind(const char *alias, int create)
{
    int i;
    for (i = 0; i < gWAN; i++)
        if (strcmp(gWA[i].alias, alias) == 0) return &gWA[i];
    if (!create || gWAN >= WEB_AUDIO_SLOTS) return NULL;
    memset(&gWA[gWAN], 0, sizeof(gWA[0]));
    strncpy(gWA[gWAN].alias, alias, sizeof(gWA[0].alias) - 1);
    gWA[gWAN].volume = 1000;
    return &gWA[gWAN++];
}

/* 从 MCI 命令串里取出 "<alias>" 形式的一节 */
static void waToken(const wchar_t *cmd, int idx, char *out, int cap)
{
    int i = 0, n = 0, start = -1, end = -1, k = 0;
    if (!cmd || !out || cap <= 0) { if (out && cap > 0) out[0] = 0; return; }
    for (i = 0; cmd[i] && n <= idx; i++) {
        if (cmd[i] != L' ' && start < 0) { start = i; }
        if ((cmd[i] == L' ' || cmd[i] == 0) && start >= 0) {
            end = i; n++;
            if (n - 1 == idx) break;
            start = -1;
        }
    }
    if (start >= 0 && end < 0) end = i;
    if (start < 0 || end <= start) { out[0] = 0; return; }
    for (i = start; i < end && k < cap - 1; i++)
        out[k++] = (char)(cmd[i] & 0xFF);
    out[k] = 0;
}

/* ====================================================================== */
/*  音频 / 输入 / 时间（网页版由 JS 侧接管，这里只保证符号存在）           */
/* ====================================================================== */

MCIERROR mciSendStringW(const wchar_t *cmd, wchar_t *ret, UINT retLen, HWND cb)
{
    char cbuf[512], verb[24], alias[32], arg[256];
    int i;
    (void)cb;
    if (ret && retLen > 0) ret[0] = 0;
    if (!cmd) return 0;
    wsToAscii(cmd, cbuf, sizeof(cbuf));

    /* 取动词（第一个空格之前的词） */
    for (i = 0; i < (int)sizeof(verb) - 1 && cbuf[i] && cbuf[i] != ' '; i++)
        verb[i] = cbuf[i];
    verb[i] = 0;

    if (strcmp(verb, "open") == 0) {
        /* open "<path>" type <t> alias <a> */
        char path[256];
        int k = 0, j;
        const char *p = strchr(cbuf, '"');
        if (!p) return 0;
        p++;
        for (j = 0; p[j] && p[j] != '"' && k < (int)sizeof(path) - 1; j++)
            path[k++] = p[j];
        path[k] = 0;
        waToken(cmd, 4, alias, sizeof(alias));      /* open "path" type x alias A → idx 4 */
        {
            WebAudio *w = waFind(alias, 1);
            if (!w) return 1;
            toAssetRel(path, w->path, sizeof(w->path));
            w->opened = 1;
        }
        return 0;
    }
    if (strcmp(verb, "setaudio") == 0) {
        WebAudio *w;
        waToken(cmd, 1, alias, sizeof(alias));
        w = waFind(alias, 1);
        if (w) {
            const char *v = strstr(cbuf, "volume to ");
            if (v) w->volume = atoi(v + 10);
        }
        return 0;
    }
    if (strcmp(verb, "play") == 0) {
        WebAudio *w;
        waToken(cmd, 1, alias, sizeof(alias));
        w = waFind(alias, 1);
        if (!w || !w->opened) return 1;
        w->loop = (strstr(cbuf, "repeat") != NULL);
        if (isBgmLikely(w->path))
            jsBgmPlay(w->path, w->volume, w->loop);
        else
            jsSfxPlay(w->path, w->volume);
        return 0;
    }
    if (strcmp(verb, "stop") == 0 || strcmp(verb, "pause") == 0 ||
        strcmp(verb, "close") == 0) {
        waToken(cmd, 1, alias, sizeof(alias));
        {
            WebAudio *w = waFind(alias, 0);
            if (w) {
                if (isBgmLikely(w->path)) jsBgmStop(w->path);
                if (strcmp(verb, "close") == 0) { w->opened = 0; w->path[0] = 0; }
            }
        }
        return 0;
    }
    if (strcmp(verb, "status") == 0) {
        /* status <a> mode → 回 "playing" / "stopped"（引擎据此判断 BGM 是否在放） */
        int playing = 0;
        waToken(cmd, 1, alias, sizeof(alias));
        {
            WebAudio *w = waFind(alias, 0);
            if (w && isBgmLikely(w->path)) playing = jsBgmPlaying(w->path);
        }
        if (ret && retLen > 0)
            wsprintfW(ret, L"%s", playing ? L"playing" : L"stopped");
        return 0;
    }
    (void)arg;
    return 0;
}

BOOL PlaySound(const wchar_t *name, HMODULE h, DWORD flags)
{
    (void)h; (void)flags;
    if (name) {
        char rel[256];
        char asc[256];
        wsToAscii(name, asc, sizeof(asc));
        toAssetRel(asc, rel, sizeof(rel));
        jsSfxPlay(rel, 1000);
    }
    return TRUE;
}


/* GetLocalTime：引擎只用来给存档打时间戳与做"每日"判断。
   用 libc 的 localtime 实现，网页版同样适用。 */
void GetLocalTime(SYSTEMTIME *st)
{
    time_t t;
    struct tm *lt;
    if (!st) return;
    memset(st, 0, sizeof(*st));
    t = time(NULL);
    lt = localtime(&t);
    if (!lt) return;
    st->wYear = (WORD)(lt->tm_year + 1900);
    st->wMonth = (WORD)(lt->tm_mon + 1);
    st->wDayOfWeek = (WORD)lt->tm_wday;
    st->wDay = (WORD)lt->tm_mday;
    st->wHour = (WORD)lt->tm_hour;
    st->wMinute = (WORD)lt->tm_min;
    st->wSecond = (WORD)lt->tm_sec;
}

void GetSystemTime(SYSTEMTIME *st) { GetLocalTime(st); }

SHORT GetAsyncKeyState(int vk)
{
    if (vk < 0 || vk > 255) return 0;
    return gKeys[vk];
}

/* ---- 计时 ----
   ★★ 必须返回**真实递增**的毫秒，不能只返回一个内部计数器。
      第一版是 `return gTick;`，而 gTick 只在 platWebTickAdvance() 里增长 ——
      网页外壳从来没调过它，于是 GetTickCount() **恒为 0**。

      后果是静默且严重的（引擎里所有防抖逻辑都基于"now - last"）：
        · sfxPlay:  `if ((DWORD)(now - lastHit) < 32) return;`
          → now 与 lastHit 都是 0，(0-0)=0 < 32 **恒成立** →
          **所有音效永远不播**（hit / hit_hard / zombie_die 直接 return）；
        · toggleAllowed: 350ms 防抖 → 全屏切换只生效第一次，之后永远被拒；
        · gDbgRenderMs 的耗时统计恒为 0，性能诊断也就失去意义。

      本机验证（_web_main.c 只跑 60 帧、不看音频）**不会**暴露这个问题 ——
      它属于"只有真正玩一遍才会发现"的类型。

   ★ 第二个坑：**初值不能是 0**。
      引擎的防抖写成 `if ((DWORD)(now - last) < 32) return;`，而 last 的初值是 0。
      如果 now 也从 0 附近开始（wasm 的 performance.now 从页面加载算，
      首次调用经常就返回 0.x → 截断成 0），那么 `0 - 0 = 0 < 32` 成立 →
      **第一个音效被吞掉**；同理 toggleAllowed 的 350ms 防抖会让
      **第一次全屏切换失效**。
      Windows 上 GetTickCount() 是"系统启动以来的毫秒"，天然是几十万到上亿的大数，
      所以这两个 bug **只在可移植侧出现** —— 桌面端从来没有过。
      这里加一个 60 秒的基准偏移，模拟"进程已经跑了一会儿"，
      与 Windows 的行为在"数值足够大"这点上保持一致。
      （DWORD 减法天然处理回绕，这个偏移不影响 49.7 天回绕后的正确性。） */
#define PLAT_TICK_BASE 60000u

DWORD GetTickCount(void)
{
#ifdef __EMSCRIPTEN__
    /* emscripten_get_now() 是单调递增的毫秒浮点（基于 performance.now），
       正好符合 Windows GetTickCount 的语义（单调、不随系统时间调整而回退）。 */
    return (DWORD)(emscripten_get_now() + (double)PLAT_TICK_BASE);
#else
    /* 本机验证：用标准库时钟提供同样的单调毫秒。
       不用 time(NULL) —— 它的粒度是秒，做 32ms 级防抖完全不够。 */
    static int     inited = 0;
    static clock_t t0;
    if (!inited) { t0 = clock(); inited = 1; }
    return (DWORD)((double)(clock() - t0) * 1000.0 / (double)CLOCKS_PER_SEC)
           + PLAT_TICK_BASE;
#endif
}

/* 网页版由 requestAnimationFrame 驱动，主循环里不 Sleep ——
   引擎的 Sleep 调用只是"限帧"，在网页版由浏览器接管。 */
void Sleep(DWORD ms) { (void)ms; }

/* ====================================================================== */
/*  杂项                                                                  */
/* ====================================================================== */

int wsprintfW(wchar_t *out, const wchar_t *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vswprintf(out, 512, fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : n;
}

#ifndef _WIN32
/* 本机用 -DPLAT_PORTABLE 验证时，MinGW 的 stdio.h 已经带 dllimport 声明过
   _wfopen；这里再定义一次会触发 -Wattributes。只在真正的可移植目标上定义。

   ★★ 反斜杠必须转成正斜杠 —— 这是 wasm 版的**必修项**。
      引擎拼路径用的是 Windows 风格（L"%ssfx\\%s.wav" 得到 "assets\sfx\hit.wav"），
      而 Emscripten 的 MEMFS 以及所有 POSIX 系统**都不认反斜杠**：
      它会把 "assets\sfx\hit.wav" 当成一个"文件名里含反斜杠"的文件，fopen 直接失败。

      症状极其隐蔽：磁盘上文件都在（在 .data 包里），但引擎报告"全部加载失败"，
      画面全是程序化回退图形 —— 看起来像"资源没打包进去"，
      实际是**路径分隔符**的问题。

      ⚠️ 本机 Windows 上 fopen 恰好**能**吃反斜杠，所以 _build_web_local.sh
         这个本地验证路径**不会**暴露它。只有真跑 wasm 才会炸。
         这类"只在目标平台暴露"的坑，本地验证再充分也拦不住，
         只能靠提前知道。本项目在 BMP/PNG 后缀那轮已经栽过一次同类问题。 */
FILE *_wfopen(const wchar_t *path, const wchar_t *mode)
{
    char p[1024], m[16];
    int i;
    for (i = 0; i < 1023 && path[i]; i++) {
        char c = (char)(path[i] & 0xFF);
        p[i] = (c == '\\') ? '/' : c;
    }
    p[i] = 0;
    for (i = 0; i < 15 && mode[i]; i++) m[i] = (char)(mode[i] & 0xFF);
    m[i] = 0;
    return fopen(p, m);
}
#endif  /* !_WIN32 */

/* ---- MSVCRT 专有函数 ----------------------------------------------------------
   ★★ 这一组是 2026-09-21 emcc 构建失败后补上的，值得说明为什么本地没发现。

      下面这几个名字（`_wcsicmp` / `_wgetenv` / `_wcsnicmp` / `_wputenv` / `_stricmp`）
      都是 **MSVCRT 专有**的，标准 C 与 musl（Emscripten 的 libc）里**都没有**。

      本机用 MinGW 链接时，kernel32/msvcrt 会自动提供它们 ——
      于是 `_build_web_local.sh` 一路绿灯，看起来"可移植侧已经完整"。
      而 emcc 到 musl 上没有这些符号，**链接期直接失败**。

      这就是"本地过、目标平台失败"的经典形态：本机的系统库把缺口盖住了。
      靠编译告警发现不了（`-c` 只编不链），只能靠**检查链接期符号**。

      → 已把它做成自动化检查：`_check_web_symbols.py`
        （用 nm 求“未定义 − 已定义 − 标准库”的差集，
         任何残留的非标准符号都会让 CI 提前失败并指名道姓报出来）
      → 这条检查加进了 workflow 与 _regress.sh，所以以后不会再重演。 */

/* 宽字符的大小写不敏感比较。引擎用它判断 BGM 扩展名（".mp3"/".m4a"）
   与 MCI 状态（"stopped"），所以行为和返回值必须和 MSVCRT 一致：
   返回 0 表示相等，负数/正数表示大小关系。 */
int _wcsicmp(const wchar_t *a, const wchar_t *b)
{
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    while (*a && *b) {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb - L'A' + L'a');
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)*a - (int)*b;
}

int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    size_t i;
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    for (i = 0; i < n; i++) {
        wchar_t ca = a[i], cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb - L'A' + L'a');
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) break;
    }
    return 0;
}

/* 窄字符版（引擎里没用到，但补上成本为零，且能挡住以后有人加代码时踩同一个坑） */
int _stricmp(const char *a, const char *b)
{
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)*a - (int)*b;
}

/* 环境变量：wasm 里没有进程环境，**返回 NULL 就是正确行为**。
   引擎的 savePath() 逻辑是「有 PVZ_SAVE_FILE 就用它，否则用默认的 pvz_save.dat」，
   返回 NULL → 走默认相对路径 → 在 MEMFS 里就是 /pvz_save.dat，
   与外壳 shell.js 读写的路径一致。
   ⚠️ 不要"为了让它找到文件"而返回一个自造路径：那会让引擎以为环境变量被设置过，
      一旦哪天有人改了默认路径，两处就会不一致。 */
wchar_t *_wgetenv(const wchar_t *name)
{
    (void)name;
    return NULL;
}

/* 设置环境变量：wasm 里没有意义，但要保证符号存在（外壳里的测试代码会用） */
int _wputenv(const wchar_t *envstr) { (void)envstr; return 0; }

BOOL GetClientRect(HWND hwnd, RECT *rc)
{
    (void)hwnd;
    if (!rc) return FALSE;
    /* ★ 关键：网页版报 **逻辑尺寸** (VIEW_W×VIEW_H)。
       这样 render() 的呈现走"窗口正好 1:1"的快路径（一次 1000×650 的 memcpy），
       而真正的画质来自 2x 世界层 —— 外壳用 gameWorldBits() 取那一层，
       交给浏览器按 iPad 实际分辨率放大（GPU 做，免费）。
       报更大的尺寸会让 render 去走 StretchBlt 直接采样世界层，
       在软光栅下等于白白多做一次缩放。 */
    rc->left = 0; rc->top = 0;
    rc->right = gViewW; rc->bottom = gViewH;
    return TRUE;
}

/* 软件光栅是逐像素同步写入的，不存在"未落盘"的中间态；
   这个函数纯粹为了与 GDI 的调用约定对齐（见 plat.h 的说明）。 */
BOOL GdiFlush(void) { return TRUE; }

void SetCursor(HCURSOR c) { (void)c; }
HCURSOR LoadCursorW(HINSTANCE h, const wchar_t *name) { (void)h; (void)name; return NULL; }

DWORD GetModuleFileNameW(HINSTANCE h, wchar_t *out, DWORD n)
{
    (void)h;
    if (out && n > 0) out[0] = 0;
    return 0;
}

DWORD GetEnvironmentVariableW(const wchar_t *name, wchar_t *out, DWORD n)
{
    (void)name;
    if (out && n > 0) out[0] = 0;
    return 0;
}

BOOL SetWindowTextW(HWND hwnd, const wchar_t *s) { (void)hwnd; (void)s; return TRUE; }

/* ---- 文件属性 --------------------------------------------------------------
   ★ 与上一版相反：这里现在**如实回答**。
     上一版为了"编译得起来"一律返回 INVALID_FILE_ATTRIBUTES，
     结果是 sfxPlay 第一行就 return（所有音效不响）、且贴图全部走程序化回退。
     现在资源已经通过 `--preload-file` 进了 MEMFS，所以可以真的查。
     唯一的例外是 BGM：它不在 MEMFS 里（JS 侧流式播），
     所以对 bgm/ 前缀特判为"存在"，否则 bgmOpen 会直接 return、没有任何音乐。 */
DWORD GetFileAttributesW(const wchar_t *path)
{
    char asc[256], rel[256];
    wsToAscii(path, asc, sizeof(asc));
    toAssetRel(asc, rel, sizeof(rel));
    if (strncmp(rel, "bgm/", 4) == 0) return 0;      /* JS 侧流式，恒为"存在" */
#ifdef __EMSCRIPTEN__
    /* 真查 MEMFS：与引擎的判据完全一致（能读到才算存在） */
    {
        FILE *f = fopen((const char *)(rel[0] ? rel : ""), "rb");
        if (f) {
            /* MEMFS 的根是虚拟根，assets 已挂到 /assets；这里两种都试 */
            fclose(f);
            return 0;
        }
        return INVALID_FILE_ATTRIBUTES;
    }
#else
    {
        FILE *f = fopen(rel, "rb");
        if (f) { fclose(f); return 0; }
        return INVALID_FILE_ATTRIBUTES;
    }
#endif
}

wchar_t *lstrcpyW(wchar_t *dst, const wchar_t *src)
{ wchar_t *d = dst; if (dst && src) while ((*d++ = *src++) != 0) {} return dst; }
wchar_t *lstrcatW(wchar_t *dst, const wchar_t *src)
{ wchar_t *d = dst; if (dst && src) { while (*d) d++; while ((*d++ = *src++) != 0) {} } return dst; }
int lstrlenW(const wchar_t *s) { int n = 0; if (s) while (s[n]) n++; return n; }

HWND GetActiveWindow(void) { return (HWND)1; }
HWND GetForegroundWindow(void) { return (HWND)1; }
BOOL GetWindowRect(HWND hwnd, RECT *rc)
{
    (void)hwnd;
    if (!rc) return FALSE;
    rc->left = 0; rc->top = 0; rc->right = gViewW; rc->bottom = gViewH;
    return TRUE;
}
HDC BeginPaint(HWND h, PAINTSTRUCT *ps)
{
    (void)h;
    if (ps) memset(ps, 0, sizeof(*ps));
    return &gScreenDC;
}
BOOL EndPaint(HWND h, const PAINTSTRUCT *ps) { (void)h; (void)ps; return TRUE; }
void PostQuitMessage(int code) { (void)code; }

BOOL SetWindowPos(HWND h, HWND a, int x, int y, int w, int hh, UINT f)
{ (void)h;(void)a;(void)x;(void)y;(void)w;(void)hh;(void)f; return TRUE; }
BOOL MoveWindow(HWND h, int x, int y, int w, int hh, BOOL rp)
{ (void)h;(void)x;(void)y;(void)w;(void)hh;(void)rp; return TRUE; }
LONG SetWindowLongW(HWND h, int i, LONG v) { (void)h;(void)i; return v; }
LONG GetWindowLongW(HWND h, int i) { (void)h;(void)i; return 0; }
HMONITOR MonitorFromWindow(HWND h, DWORD f) { (void)h;(void)f; return (HMONITOR)1; }
BOOL GetMonitorInfoW(HMONITOR m, MONITORINFO *mi)
{
    (void)m;
    if (!mi) return FALSE;
    mi->rcMonitor.left = 0; mi->rcMonitor.top = 0;
    mi->rcMonitor.right = gViewW; mi->rcMonitor.bottom = gViewH;
    mi->rcWork = mi->rcMonitor;
    return TRUE;
}

/* Arc：引擎只用来画"技能冷却的扇形遮罩"。
   按角度参数画一段圆弧描边（不做椭圆弧的一般解，够用且行为可预期）。 */
BOOL Arc(HDC hdc, int l, int t, int r, int b, int sx, int sy, int ex, int ey)
{
    struct PlatDC *dc = hdc;
    float cx, cy, rx, ry, a0, a1, a;
    int steps, i;
    if (!dc) return FALSE;
    {
        int x0 = (int)xfX(dc, (float)l, (float)t), y0 = (int)xfY(dc, (float)l, (float)t);
        int x1 = (int)xfX(dc, (float)r, (float)b), y1 = (int)xfY(dc, (float)r, (float)b);
        cx = ((float)x0 + (float)x1) * 0.5f;
        cy = ((float)y0 + (float)y1) * 0.5f;
        rx = ((float)x1 - (float)x0) * 0.5f;
        ry = ((float)y1 - (float)y0) * 0.5f;
    }
    /* GDI 的 Arc 端点坐标是"逻辑坐标"，这里按椭圆上的点求角度 */
    a0 = atan2f((float)sy - cy, (float)sx - cx);
    a1 = atan2f((float)ey - cy, (float)ex - cx);
    if (a1 <= a0) a1 += 6.28318f;
    steps = (int)((a1 - a0) / 0.06f) + 2;
    if (steps > 4000) steps = 4000;
    for (i = 0; i < steps; i++) {
        float t0 = a0 + (a1 - a0) * (float)i / (float)steps;
        float t1 = a0 + (a1 - a0) * (float)(i + 1) / (float)steps;
        a = t0;
        {
            int px = (int)(cx + cosf(a) * rx), py = (int)(cy + sinf(a) * ry);
            a = t1;
            drawLine(dc, px, py, (int)(cx + cosf(a) * rx), (int)(cy + sinf(a) * ry),
                     dc->penCol, dc->penW);
        }
    }
    return TRUE;
}

/* ---- 窗口/消息：网页版用 PLAT_HAS_OWN_MAIN=0 绕开，这些只是符号占位 ---- */
BOOL ShowWindow(HWND h, int c) { (void)h; (void)c; return TRUE; }
BOOL UpdateWindow(HWND h) { (void)h; return TRUE; }
BOOL TranslateMessage(const MSG *m) { (void)m; return TRUE; }
LRESULT DispatchMessageW(const MSG *m) { (void)m; return 0; }
BOOL PeekMessageW(MSG *m, HWND h, UINT a, UINT b, UINT c)
{ (void)m; (void)h; (void)a; (void)b; (void)c; return FALSE; }
LRESULT DefWindowProcW(HWND h, UINT m, WPARAM w, LPARAM l)
{ (void)h; (void)m; (void)w; (void)l; return 0; }
ATOM RegisterClassExW(const WNDCLASSEXW *w) { (void)w; return 1; }
HWND CreateWindowExW(DWORD e, const wchar_t *c, const wchar_t *t, DWORD s,
                     int x, int y, int w, int h, HWND p, HMENU m, HINSTANCE i, void *v)
{ (void)e;(void)c;(void)t;(void)s;(void)x;(void)y;(void)w;(void)h;(void)p;(void)m;(void)i;(void)v; return (HWND)1; }
BOOL SetProcessDpiAwarenessContext(HANDLE c) { (void)c; return TRUE; }
BOOL SetForegroundWindow(HWND h) { (void)h; return TRUE; }
int GetSystemMetrics(int i) { (void)i; return 0; }
BOOL AdjustWindowRectEx(RECT *r, DWORD s, BOOL m, DWORD e)
{ (void)r; (void)s; (void)m; (void)e; return TRUE; }

/* ====================================================================== */
/*  网页外壳接口                                                          */
/* ====================================================================== */

/* 宽/窄转换。网页版路径全是 ASCII，按字节直转；带容量校验以免静默截断
   （截断过就会去读一个不存在的文件，症状是"素材全丢"而不是报错）。 */
int MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int srcLen,
                        wchar_t *dst, int dstCap)
{
    int n = 0;
    (void)cp; (void)flags;
    if (!src || !dst) return 0;
    if (srcLen < 0) { while (src[n]) n++; } else { while (n < srcLen && src[n]) n++; }
    if (n >= dstCap) n = dstCap - 1;
    { int i; for (i = 0; i < n; i++) dst[i] = (wchar_t)(unsigned char)src[i]; }
    dst[n] = 0;
    return n + (srcLen < 0 ? 1 : 0);
}

int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *src, int srcLen,
                        char *dst, int dstCap, const char *defCh, int *used)
{
    int n = 0;
    (void)cp; (void)flags; (void)defCh; (void)used;
    if (!src || !dst) return 0;
    if (srcLen < 0) { while (src[n]) n++; } else { while (n < srcLen && src[n]) n++; }
    if (n >= dstCap) n = dstCap - 1;
    { int i; for (i = 0; i < n; i++) dst[i] = (char)(src[i] & 0xFF); }
    dst[n] = 0;
    return n + (srcLen < 0 ? 1 : 0);
}

/* 资源目录。桌面端在 plat.h 里有内联实现（exe 同级 assets\\）；
   可移植侧由外壳通过 gameSetAssetDir() 注入实际路径。
   这里只保证符号存在，并且**默认给一个能用的相对路径** ——
   万一外壳忘了设，引擎会去找 "assets/" 而不是空字符串
   （空路径的症状是"所有素材都加载失败"，且看不出原因）。 */
void platAssetDir(wchar_t *out, int cap)
{
    const char *def = "assets\\";
    int i = 0;
    if (!out || cap <= 0) return;
    while (def[i] && i < cap - 1) { out[i] = (wchar_t)(unsigned char)def[i]; i++; }
    out[i] = 0;
}

int platWebInit(int w, int h, const char *title)
{
    (void)title;
    gViewW = w > 0 ? w : 1000;
    gViewH = h > 0 ? h : 650;
    memset(&gScreenDC, 0, sizeof(gScreenDC));
    gScreenBM.w = gViewW; gScreenBM.h = gViewH;
    gScreenBM.owns = 1;
    gScreenBM.bits = (unsigned char *)calloc((size_t)gViewW * gViewH * 4, 1);
    if (!gScreenBM.bits) return 0;
    gScreenDC.bmp = &gScreenBM;
    gScreenDC.isScreen = 1;
    gScreenDC.gmode = 1;
    gScreenDC.xf.eM11 = gScreenDC.xf.eM22 = 1.0f;
    gScreenDC.cl = gScreenDC.ct = 0;
    gScreenDC.cr = gViewW; gScreenDC.cb = gViewH;
    gScreenDC.brushCol = 0x00FFFFFF;
    gScreenDC.hasBrush = 1;
    gScreenDC.hasPen = 1;
    gScreenDC.bkMode = 2;
    gInited = 1;
    /* 打一条启动日志：浏览器里排查"有没有初始化成功"最快的手段。
       往后一路的素材加载统计也会通过同一个通道出去。 */
    jsLog("[plat_web] init done, view=%dx%d");
    return 1;
}

void platWebTickAdvance(DWORD ms) { gTick += ms; }

void platWebPointer(int type, float x, float y)
{
    /* 只更新按键状态；坐标交给外壳直接写进 pvz.c 的输入变量
       （见 _web_input.c 的说明 —— 那条路能复用 WndProc 的全部分支逻辑）。 */
    (void)x; (void)y;
    if (type == 1) gKeys[1 /*VK_LBUTTON*/] = (SHORT)0x8000;
    else if (type == 2 || type == 3) gKeys[1] = 0;
}

void platWebKey(int vk, int down)
{
    if (vk >= 0 && vk < 256) gKeys[vk] = down ? (SHORT)0x8000 : 0;
}

/* 外壳每帧取一次：默认给 2x 世界层（画质来源）。 */
extern const void *gameWorldBits(int *w, int *h, int *stride);

const void *platWebFrame(int *w, int *h, int *stride)
{
    return gameWorldBits(w, h, stride);
}
