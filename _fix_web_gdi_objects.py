# -*- coding: utf-8 -*-
"""修 plat_web.c 的图元对象模型 + 补描边。

【两个必须修的语义问题】

① **笔/刷/字体必须可区分。**
   第一版把 CreatePen 的返回值指向一个 pens[] 数组、CreateSolidBrush 指向
   brushes[] 数组 —— 两个都是"指向静态数组的指针"，后端**没法判断**
   进来的到底是笔还是刷。于是 SelectObject 只能当位图处理，笔刷全丢。
   改成**单一对象池 + 类型标签**：CreatePen→type=1，CreateSolidBrush→type=2，
   CreateFontW→type=3，GetStockObject(NULL_PEN=8)→type=4，NULL_BRUSH=5→type=5。
   SelectObject 按 type 分派。

② **RoundRect / Ellipse / Rectangle / Polygon 是"既填又描"。**
   GDI 的这四个函数都会**同时**用当前刷子填充、用当前笔描边；
   要只填就选 NULL_PEN，要只描就选 NULL_BRUSH。
   第一版只实现了填充 → 所有描边（按钮边框、卡片描边、圆形标尺）
   都会消失，而且不报错。
   （这也是上一步修 plat.h 的 NULL_PEN/NULL_BRUSH 的原因：
    原来两者都是 NULL，连"要填还是要描"都判断不了。）

【幂等】靠文件里是否已有 `PLAT_GDI_OBJ` 标记判断。
用法：python _fix_web_gdi_objects.py
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "plat_web.c")

MARK = "/* ---- 图元对象池（PLAT_GDI_OBJ）"

# ---------------------------------------------------------------- 新对象模型
NEW_OBJ = '''/* ---- 图元对象池（PLAT_GDI_OBJ）----
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

'''

# ---------------------------------------------------------------- 新 SelectObject
NEW_SEL = '''static GdiObj *objOf(HGDIOBJ h)
{
    GdiObj *o = (GdiObj *)h;
    if (!o) return NULL;
    if (o < &gObjs[0] || o >= &gObjs[MAX_GDI_OBJ]) return NULL;
    return o;
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

'''

# ---------------------------------------------------------------- 描边助手
STROKE = '''/* ---- 描边：GDI 的 Ellipse/Rectangle/RoundRect/Polygon 都是"既填又描"，
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
    o.rx = ((float)x1 - (float)x0) * 0.5f;
    o.ry = ((float)y1 - (float)y0) * 0.5f;
    i2 = o;
    i2.rx = o.rx - (float)w;
    i2.ry = o.ry - (float)w;
    for (y = y0 - w; y <= y1 + w; y++)
        for (x = x0 - w; x <= x1 + w; x++) {
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

'''


def main():
    s = io.open(SRC, encoding="utf-8").read()
    if MARK in s:
        print("  已修过，跳过（幂等）")
        return 0

    # ---- ① 替换对象模型 -------------------------------------------------
    a = s.index("HPEN CreatePen(int style, int w, COLORREF c)")
    b = s.index("HGDIOBJ GetStockObject(int idx) { (void)idx; return NULL; }")
    b = s.index("\n", b) + 1
    s = s[:a] + NEW_OBJ + s[b:]
    print("  ✔ 图元对象模型改为带类型标签的对象池")

    # ---- ② 替换 SelectObject -------------------------------------------
    a = s.index("HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj)")
    b = s.index("\nBOOL DeleteObject(HGDIOBJ obj)")
    s = s[:a] + NEW_SEL.rstrip("\n") + "\n" + s[b:]
    print("  ✔ SelectObject 按类型分派（笔/刷/字体/位图）")

    # ---- ③ 插入描边助手（放在三个"直接绘制"函数之前）------------------
    anchor = "/* ====================================================================== */\n/*  三个\"直接绘制\"函数（会被 pvz.c 直接调用）                             */"
    assert s.count(anchor) == 1, "描边助手锚点不唯一"
    s = s.replace(anchor, STROKE + anchor)
    print("  ✔ 插入描边助手（椭圆/矩形/多边形）")

    # ---- ④ DrawTextW 之前插入 drawLine 的前置声明 ----------------------
    # （strokePolyShape 用了 drawLine，而 drawLine 定义在后面）
    anchor2 = "/* ---- 描边：GDI 的"
    assert s.count(anchor2) == 1
    s = s.replace(anchor2,
                  "static void drawLine(struct PlatDC *dc, int x0, int y0, int x1, int y1,\n"
                  "                     COLORREF col, int w);\n\n" + anchor2)
    print("  ✔ 补 drawLine 前置声明")

    # ---- ⑤ 四个形状函数：填充之后补描边 --------------------------------
    s = s.replace("""    fillScan(dc, x0, y0, x1, y1, dc->brushCol, inEllipse, &e, 1);
    return TRUE;""",
"""    if (dc->hasBrush) fillScan(dc, x0, y0, x1, y1, dc->brushCol, inEllipse, &e, 1);
    if (dc->hasPen)   strokeEllipseShape(dc, x0, y0, x1, y1, dc->penCol, dc->penW);
    return TRUE;""")
    s = s.replace("""    box[0] = x0; box[1] = y0; box[2] = x1; box[3] = y1;
    fillScan(dc, x0, y0, x1, y1, dc->brushCol, inRect, box, 1);
    return TRUE;""",
"""    box[0] = x0; box[1] = y0; box[2] = x1; box[3] = y1;
    if (dc->hasBrush) fillScan(dc, x0, y0, x1, y1, dc->brushCol, inRect, box, 1);
    if (dc->hasPen)   strokeRectShape(dc, x0, y0, x1, y1, dc->penCol, dc->penW);
    return TRUE;""")
    s = s.replace("""    q.rad = (float)(ew < eh ? ew : eh) * 0.5f;
    fillScan(dc, x0, y0, x1, y1, dc->brushCol, inRound, &q, 1);
    return TRUE;""",
"""    q.rad = (float)(ew < eh ? ew : eh) * 0.5f;
    if (dc->hasBrush) fillScan(dc, x0, y0, x1, y1, dc->brushCol, inRound, &q, 1);
    if (dc->hasPen)   strokeRoundShape(dc, x0, y0, x1, y1, q.rad, dc->penCol, dc->penW);
    return TRUE;""")
    s = s.replace("""    q.p = pts; q.n = n; q.ox = 0; q.oy = 0;
    fillScan(dc, minx, miny, maxx + 1, maxy + 1, dc->brushCol, inPoly, &q, 1);
    return TRUE;""",
"""    q.p = pts; q.n = n; q.ox = 0; q.oy = 0;
    if (dc->hasBrush) fillScan(dc, minx, miny, maxx + 1, maxy + 1, dc->brushCol, inPoly, &q, 1);
    if (dc->hasPen)   strokePolyShape(dc, pts, n, dc->penCol, dc->penW);
    return TRUE;""")
    print("  ✔ 四个形状函数补上描边（之前只填充）")

    # ---- ⑥ PlatDC 加 hasPen / hasBrush 字段 ----------------------------
    s = s.replace("""    COLORREF penCol; int penW;
    COLORREF brushCol;""",
"""    COLORREF penCol; int penW; int hasPen;
    COLORREF brushCol; int hasBrush;""")
    # 默认状态：GDI 新建 DC 时是"黑色笔 + 白色刷"，两个都有效
    s = s.replace("""    dc->brushCol = 0x00FFFFFF;
    dc->penCol = 0;
    dc->penW = 1;""",
"""    dc->brushCol = 0x00FFFFFF;
    dc->brushCol = 0x00FFFFFF;
    dc->hasBrush = 1;
    dc->penCol = 0;
    dc->penW = 1;
    dc->hasPen = 1;""")
    s = s.replace("""    gScreenDC.brushCol = 0x00FFFFFF;
    gScreenDC.bkMode = 2;""",
"""    gScreenDC.brushCol = 0x00FFFFFF;
    gScreenDC.hasBrush = 1;
    gScreenDC.hasPen = 1;
    gScreenDC.bkMode = 2;""")
    print("  ✔ PlatDC 加 hasPen/hasBrush 状态")
    # GM_COMPATIBLE 时矩形不需要变换（FillRect 在兼容模式下不吃变换）
    s = s.replace("#define PLAT_HAS_OWN_MAIN 0\n#endif",
                  "#define PLAT_HAS_OWN_MAIN 0\n#endif")

    # ---- ⑦ CreateFontW 写进对象池 --------------------------------------
    s = s.replace("""    static int fonts[64];
    static int n = 0;
    int px = h < 0 ? -h : h;                 /* GDI：负高度 = 字符高度 */
    (void)w; (void)esc; (void)orient; (void)italic; (void)under; (void)strike;
    (void)charset; (void)prec; (void)clip; (void)quality; (void)pitch; (void)face;
    if (n >= 64) n = 0;
    fonts[n] = (px << 1) | (weight >= 700 ? 1 : 0);
    return (HFONT)&fonts[n++];""",
"""    int px = h < 0 ? -h : h;                 /* GDI：负高度 = 字符高度 */
    GdiObj *o;
    (void)w; (void)esc; (void)orient; (void)italic; (void)under; (void)strike;
    (void)charset; (void)prec; (void)clip; (void)quality; (void)pitch; (void)face;
    o = objNew(GDI_FONT);
    o->px = px;
    o->bold = (weight >= 700) ? 1 : 0;
    return (HFONT)o;""")
    print("  ✔ CreateFontW 改用对象池")

    io.open(SRC, "w", encoding="utf-8", newline="\n").write(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
