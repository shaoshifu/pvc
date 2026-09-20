/* =========================================================================
   _ab_prim.c —— **原语级**跨平台比对
   -------------------------------------------------------------------------
   整屏比对出一个 99.8% 的差异时，根本没法定位是哪一层出了问题
   （菜单背景？文字？变换？混合？）。所以先做**单个原语**的比对：
   同一段绘制序列、同一块画布，一次跑真 GDI、一次跑软件光栅，逐像素比。

   这个测试只依赖 plat.h 的接口，**不经过 pvz.c** ——
   于是它能分别链接真实的 gdi32（Windows）与 plat_web.c（软件光栅），
   形成一个干净的对照。

   用法：
     gcc -O2 -o _ab_prim_win.exe _ab_prim.c -lgdi32 -luser32 -lm
     gcc -DPLAT_PORTABLE -O2 -o _ab_prim_web.exe _ab_prim.c plat_web.c -lm \
         -Wl,--allow-multiple-definition
     ./_ab_prim_win.exe _prim_win.raw
     ./_ab_prim_web.exe _prim_web.raw
     然后用 _ab_prim_cmp.py 比对
   ========================================================================= */
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CW 64
#define CH 64
#define SS 2                      /* 与引擎一致：2x 超采样 */
#define DW (CW * SS)
#define DH (CH * SS)

/* plat_web.c 的 platWebFrame() 引用了 gameWorldBits()（它的实现在 pvz.c 里）。
   本测试**故意不链接 pvz.c**（要保持"纯原语"的干净对照），所以这里给个桩。
   链接 Windows 侧时这个桩没人用，无副作用。 */
const void *gameWorldBits(int *w, int *h, int *stride)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (stride) *stride = 0;
    return NULL;
}

static HDC     gDC;
static HBITMAP gBM;
static void   *gBits;

static void setup(void)
{
    BITMAPINFO bi;
    HDC screen;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = DW;
    bi.bmiHeader.biHeight      = -DH;              /* 负高度 = 自上而下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    gDC = CreateCompatibleDC(screen);
    gBM = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &gBits, NULL, 0);
    SelectObject(gDC, gBM);
    SetGraphicsMode(gDC, GM_ADVANCED);

    /* 世界变换：2x 缩放（引擎的做法，见 createLayers） */
    {
        XFORM xf;
        xf.eM11 = (float)SS; xf.eM12 = 0;
        xf.eM21 = 0;         xf.eM22 = (float)SS;
        xf.eDx  = 0;         xf.eDy  = 0;
        SetWorldTransform(gDC, &xf);
    }
}

/* 每张测试图先清成同一个底色（半透明感更容易暴露混合差异） */
static void clear(void)
{
    XFORM id;
    id.eM11 = 1; id.eM12 = 0; id.eM21 = 0; id.eM22 = 1; id.eDx = 0; id.eDy = 0;
    SetWorldTransform(gDC, &id);
    {
        RECT rc; HBRUSH br;
        rc.left = 0; rc.top = 0; rc.right = DW; rc.bottom = DH;
        br = CreateSolidBrush(RGB(20, 30, 40));
        FillRect(gDC, &rc, br);
        DeleteObject(br);
    }
    {
        XFORM xf;
        xf.eM11 = (float)SS; xf.eM12 = 0;
        xf.eM21 = 0;         xf.eM22 = (float)SS;
        xf.eDx  = 0;         xf.eDy  = 0;
        SetWorldTransform(gDC, &xf);
    }
}

static void plot(int n, const char *label)
{
    (void)label;
    /* ★ 必须先 flush：GDI 的绘制是批处理的，不 flush 就读到陈旧/空内存。
       缺这一句的症状是"所有原语 100% 不一致"，包括最简单的底色填充 ——
       会把人往"混合公式写错了"的方向带偏。 */
    GdiFlush();
    {
        FILE *f;
        char path[256];
        sprintf(path, "_prim_%02d.raw", n);
        f = fopen(path, "wb");
        if (f) {
            fwrite("PRM1", 1, 4, f);
            /* ⚠️ DW/DH 是宏，不能取地址 —— 必须先落到变量 */
            { int wv = DW, hv = DH;
              fwrite(&wv, 4, 1, f);
              fwrite(&hv, 4, 1, f); }
            fwrite(gBits, 1, (size_t)DW * DH * 4, f);
            fclose(f);
        }
    }
}

int main(void)
{
    int n = 0;
    setup();

    /* ① fillRect（走世界变换 → 应覆盖 2x 区域） */
    clear();
    {
        RECT rc; HBRUSH br;
        rc.left = 4; rc.top = 4; rc.right = 20; rc.bottom = 16;
        br = CreateSolidBrush(RGB(220, 60, 60));
        FillRect(gDC, &rc, br);
        DeleteObject(br);
    }
    plot(n++, "fillRect + 2x world transform");

    /* ② RoundRect 填充（NULL_PEN → 只填） */
    clear();
    {
        SelectObject(gDC, CreateSolidBrush(RGB(60, 200, 90)));
        SelectObject(gDC, GetStockObject(NULL_PEN));
        RoundRect(gDC, 8, 8, 40, 32, 12, 12);
    }
    plot(n++, "RoundRect fill (NULL_PEN)");

    /* ③ RoundRect 描边（NULL_BRUSH → 只描） */
    clear();
    {
        SelectObject(gDC, GetStockObject(NULL_BRUSH));
        SelectObject(gDC, CreatePen(0, 3, RGB(250, 220, 80)));
        RoundRect(gDC, 8, 8, 40, 32, 12, 12);
    }
    plot(n++, "RoundRect stroke (NULL_BRUSH)");

    /* ④ 椭圆填充 + 描边 */
    clear();
    {
        SelectObject(gDC, CreateSolidBrush(RGB(80, 140, 240)));
        SelectObject(gDC, CreatePen(0, 2, RGB(255, 255, 255)));
        Ellipse(gDC, 6, 6, 44, 34);
    }
    plot(n++, "Ellipse fill+stroke");

    /* ⑤ Polygon */
    clear();
    {
        POINT p[4];
        p[0].x = 6;  p[0].y = 6;
        p[1].x = 50; p[1].y = 14;
        p[2].x = 34; p[2].y = 52;
        p[3].x = 10; p[3].y = 40;
        SelectObject(gDC, CreateSolidBrush(RGB(230, 120, 200)));
        SelectObject(gDC, GetStockObject(NULL_PEN));
        Polygon(gDC, p, 4);
    }
    plot(n++, "Polygon");

    /* ⑥ 线（宽 3） */
    clear();
    {
        SelectObject(gDC, CreatePen(0, 3, RGB(255, 200, 60)));
        MoveToEx(gDC, 6, 8, NULL);
        LineTo(gDC, 52, 48);
    }
    plot(n++, "Line w=3");

    /* ⑦ 渐变（GradientFill 竖直） */
    clear();
    {
        TRIVERTEX v[2];
        GRADIENT_RECT gr;
        v[0].x = 6; v[0].y = 6;
        v[0].Red = (unsigned short)(240 << 8); v[0].Green = (unsigned short)(40 << 8);
        v[0].Blue = (unsigned short)(40 << 8); v[0].Alpha = 0xFF00;
        v[1].x = 54; v[1].y = 50;
        v[1].Red = (unsigned short)(40 << 8); v[1].Green = (unsigned short)(40 << 8);
        v[1].Blue = (unsigned short)(240 << 8); v[1].Alpha = 0xFF00;
        gr.UpperLeft = 0; gr.LowerRight = 1;
        GradientFill(gDC, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    }
    plot(n++, "GradientFill V");

    /* ⑧ AlphaBlend：造一张预乘的源图（无预乘 → 与 GDI 的 AC_SRC_ALPHA 对比） */
    clear();
    {
        BITMAPINFO bi;
        HDC src;
        HBITMAP sbm;
        void *sbits;
        BLENDFUNCTION bf;
        int y, x;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 16; bi.bmiHeader.biHeight = -16;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        src = CreateCompatibleDC(NULL);
        sbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &sbits, NULL, 0);
        SelectObject(src, sbm);
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                unsigned char *p = (unsigned char *)sbits + ((size_t)y * 16 + x) * 4;
                int a = (x + y) * 8; if (a > 255) a = 255;
                /* 预乘：BGRA = (B*a/255, G*a/255, R*a/255, a) */
                p[0] = (unsigned char)(240 * a / 255);
                p[1] = (unsigned char)(160 * a / 255);
                p[2] = (unsigned char)( 60 * a / 255);
                p[3] = (unsigned char)a;
            }
        bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
        /* 源 16x16 → 目标 32x32，同时验证缩放 */
        AlphaBlend(gDC, 12, 12, 32, 32, src, 0, 0, 16, 16, bf);
        DeleteObject(sbm); DeleteDC(src);
    }
    plot(n++, "AlphaBlend scaled 16->32 (AC_SRC_ALPHA)");

    /* ⑨ StretchBlt 不透明缩放 */
    clear();
    {
        BITMAPINFO bi;
        HDC src; HBITMAP sbm; void *sbits;
        int y, x;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = -8;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        src = CreateCompatibleDC(NULL);
        sbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &sbits, NULL, 0);
        SelectObject(src, sbm);
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                unsigned char *p = (unsigned char *)sbits + ((size_t)y * 8 + x) * 4;
                p[0] = (unsigned char)((x & 1) ? 200 : 40);
                p[1] = (unsigned char)((y & 1) ? 200 : 40);
                p[2] = 120; p[3] = 255;
            }
        StretchBlt(gDC, 4, 4, 48, 48, src, 0, 0, 8, 8, SRCCOPY);
        DeleteObject(sbm); DeleteDC(src);
    }
    plot(n++, "StretchBlt 8->48 opaque");

    /* ⑩ 剪切区（SelectClipRgn 之后 fillRect 不应越界） */
    clear();
    {
        RECT rc; HBRUSH br; HRGN rg;
        rg = CreateRectRgn(20, 20, 60, 60);
        SelectClipRgn(gDC, rg);
        rc.left = 0; rc.top = 0; rc.right = 64; rc.bottom = 64;
        br = CreateSolidBrush(RGB(120, 255, 120));
        FillRect(gDC, &rc, br);
        DeleteObject(br);
        SelectClipRgn(gDC, NULL);
    }
    plot(n++, "Clip region");

    printf("已导出 %d 张原语图（每张 %dx%d）\n", n, DW, DH);
    return 0;
}
