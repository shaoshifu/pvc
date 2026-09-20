# -*- coding: utf-8 -*-
"""
从 pvz.c 生成「UI 可见性证明」诊断副本 _uiproof.c / _uiproof.exe。

解决的问题
----------
上一版审计只测「文字被放在哪」，测不出「这句话到底有没有出现在屏幕上」。
实测就撞上了：右下角状态栏（模式/武器/货币/充能/愤怒条）整块画进 gFrameDC，
而 gFrameDC 随后被 StretchBlt 全幅覆盖 —— 那一整块是死代码，
审计却把它的坐标当成了真实缺陷。

本版的三件核心设施
------------------
1. 探针标记：每次 putText* 调用点（按源码行号哈希）在文字原位压一块**实心纯色**小矩形，
   颜色全局唯一（乘数与模数互质，lcm=15200 > 总行数）。于是「可见性」变成一个
   可判定的像素问题：该文字墨迹框内出现该独有色 -> 真的画到屏幕上了；
   0 个像素 -> 被后画的东西盖掉了（死代码 / 被遮挡）。
   不用「把文字改成探针色」是因为 CLEARTYPE 抗锯齿 + HALFTONE 降采样会把颜色混掉。

2. 帧导出：render() 结尾把 gFrameDC（1x 合成后的最终帧）按状态写成 BMP + 帧号 txt。
   这是唯一可信的真值来源 —— Windows 侧 PrintWindow 拿到的是「整个窗口」（含标题栏
   偏移），用来判坐标会系统性错位，实测连阳性对照的纯色块都找不见。

3. 行号透传：宏把 __LINE__ 带进钩子，报告里的每条都能直接指回 pvz.c 的行号。

用法：
    PVZ_UITRACE=t.tsv PVZ_DUMPDIR=frames PVZ_PROBE=1  _uiproof.exe
    驱动器发 F1..F12 / Home / End 切界面，每切一次自动连拍。
"""

import json
import os
import subprocess
import sys

SRC = "pvz.c"
DST = "_uiproof.c"
LINEMAP = "_uiproof_lines.json"

BOUNDS = []
CUM = 0

HOOK = r'''
/* ================== UI 可见性证明钩子（仅诊断版） ================== */

/* ---- 量字形墨迹：把字符串真画一次，扫出非白像素的范围 ----
   为什么不能用 GetTextExtentPoint32W：它返回的是字体「行高」，
   微软雅黑 15px 时 cy=20、18px 时 cy=25，比字号大不少。拿行高当占位框
   去判重叠，20px 行距的两行字会被判成压字（实测误报过）。只有真渲染取墨迹，
   才是眼睛看到的那块。 */
typedef struct { HFONT f; wchar_t s[96]; int x0, y0, x1, y1; } InkEnt;
static InkEnt gInk[2048];
static int    gInkN = 0;

static void uiInk(HFONT f, const wchar_t *s, int *x0, int *y0, int *x1, int *y1)
{
    int i;
    HDC dc; SIZE sz; int W, H, x, y, found = 0, ex0 = 0, ey0 = 0, ex1 = 0, ey1 = 0;
    BITMAPINFO bi; void *bits = NULL; HBITMAP bmp, old; RECT r;

    *x0 = *y0 = *x1 = *y1 = 0;
    if (!s || !*s || wcslen(s) >= 96) return;
    for (i = 0; i < gInkN; i++) {
        if (gInk[i].f == f && wcscmp(gInk[i].s, s) == 0) {
            *x0 = gInk[i].x0; *y0 = gInk[i].y0; *x1 = gInk[i].x1; *y1 = gInk[i].y1;
            return;
        }
    }
    dc = CreateCompatibleDC(NULL);
    if (!dc) return;
    SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    W = sz.cx + 24; H = sz.cy + 24;
    if (W < 8) W = 8;
    if (H < 8) H = 8;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;               /* 负 = 顶向下，扫描时好懂 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    old = SelectObject(dc, bmp);
    r.left = 0; r.top = 0; r.right = W; r.bottom = H;
    FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    r.left = 8; r.top = 8;                          /* 留 8px 边，墨迹不会贴边 */
    DrawTextW(dc, s, -1, &r, DT_LEFT | DT_TOP | DT_NOCLIP);
    if (bits) {
        const unsigned char *p = (const unsigned char *)bits;
        for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
            const unsigned char *px = p + (size_t)y * (size_t)W * 4 + (size_t)x * 4;
            if (px[0] < 200 || px[1] < 200 || px[2] < 200) {   /* 非白即墨迹 */
                if (!found) { found = 1; ex0 = ex1 = x; ey0 = ey1 = y; }
                else {
                    if (x < ex0) ex0 = x;
                    if (x > ex1) ex1 = x;
                    if (y < ey0) ey0 = y;
                    if (y > ey1) ey1 = y;
                }
            }
        }
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    if (found) { *x0 = ex0 - 8; *y0 = ey0 - 8; *x1 = ex1 + 1 - 8; *y1 = ey1 + 1 - 8; }
    if (gInkN < 2048) {
        gInk[gInkN].f = f;
        wcscpy(gInk[gInkN].s, s);
        gInk[gInkN].x0 = *x0; gInk[gInkN].y0 = *y0;
        gInk[gInkN].x1 = *x1; gInk[gInkN].y1 = *y1;
        gInkN++;
    }
}

/* ---- 探针色：按调用点行号给一句独有色 ----
   三个乘数分别与各自模数互质，最小公倍数 15200 > pvz.c 的总行数，
   所以源码范围内不会有两个调用点撞色。 */
static int gProbeOn = 1;
static COLORREF probeColor(int src)
{
    int r = 40 + (src * 37) % 190;
    int g = 30 + (src * 61) % 200;
    int b = 90 + (src * 23) % 160;
    return RGB(r, g, b);
}

/* ---- 可见性标记：在文字原位压一块实心小色块 ----
   为什么不用「把文字改成探针色再找精确色匹配」：
   字体是 CLEARTYPE_QUALITY，字形边缘全是抗锯齿混色；世界层又要经过
   SetStretchBltMode(gFrameDC, HALFTONE) 的加权平均降采样。两次混色之后，
   精确的探针色几乎一个像素都不剩，等于判不出可见性（试过，全 0）。
   改成画实心矩形：纯色填充不经抗锯齿，2x->1x 的平均在矩形内部仍还原成同一个颜色，
   所以「最终帧的这块区域里出现这个独有色」就是硬证据。
   色块从文字左上角起、最大 24x5 逻辑像素：够大能活下来，够小不会大片盖住邻居。 */
/* 色块的几何：与 uiTraceText 打印给分析脚本的必须完全一致，
   所以两边都调这一个函数，避免"画在这、报在那"。
   竖直位置取字身带（行高）的中间 —— 第一版贴在 rc.top 上，结果色块浮在
   墨迹框上方一条缝里，分析脚本按墨迹框去找，14 处明明可见的文字被判成死代码。 */
static void uiMarkGeom(float x, float y, float w, float cy,
                       float *mx, float *my, float *mw, float *mh)
{
    if (w < 8.0f) w = 8.0f;
    if (w > 24.0f) w = 24.0f;
    *mx = x;
    *mw = w;
    *mh = 5.0f;
    *my = y + (cy - *mh) * 0.5f;
    if (*my < y) *my = y;
}

static void uiMark(HDC dc, float x, float y, float w, float cy, int src)
{
    float mx, my, mw, mh;
    if (!gProbeOn) return;
    uiMarkGeom(x, y, w, cy, &mx, &my, &mw, &mh);
    fillRect(dc, mx, my, mx + mw, my + mh, probeColor(src));
}

static FILE *gUiTrace = NULL;
static int   gUiPaint = 0;
static int   gUiLive  = 0;

static void uiTraceFrame(int st, void *worldDC, void *frameDC)
{
    if (!gUiLive) {
        const char *p = getenv("PVZ_UITRACE");
        const char *q = getenv("PVZ_PROBE");
        gUiLive = 1;
        if (p && *p) gUiTrace = fopen(p, "w");
        if (q && *q && q[0] == '0') gProbeOn = 0;
    }
    gUiPaint++;
    if (!gUiTrace) return;
    /* 这里不能引用 gWorldDC / gFrameDC（声明在 1400 行后，本函数在 160 行），
       所以 DC 指针由 render() 调用时传进来。 */
    fprintf(gUiTrace, "F\t%d\t%d\t%llu\t%llu\n", gUiPaint, st,
            (unsigned long long)(size_t)worldDC, (unsigned long long)(size_t)frameDC);
    fflush(gUiTrace);
}

/* 画字前记一笔：谁、画在哪个 DC、什么颜色、墨迹框在哪、探针块在哪 */
static void uiTraceText(HDC dc, HFONT f, COLORREF c, const wchar_t *s,
                        float x, float y, float dw, float dh, int src)
{
    char u8[2048];
    int x0, y0, x1, y1;
    float mx, my, mw, mh;
    if (!gUiTrace) return;
    uiInk(f, s, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return;      /* 空串 / 全空白 */
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, u8, sizeof(u8), NULL, NULL) <= 0) return;
    uiMarkGeom(x, y, dw, dh, &mx, &my, &mw, &mh);
    fprintf(gUiTrace, "T\t%d\t%d\t%llu\t%d,%d,%d\t%.1f\t%.1f\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%s\n",
            gUiPaint, src, (unsigned long long)(size_t)dc,
            (int)GetRValue(c), (int)GetGValue(c), (int)GetBValue(c),
            x + (float)x0, y + (float)y0, x1 - x0, y1 - y0,
            mx, my, mw, mh, u8);
    fflush(gUiTrace);
}

/* ---- 帧导出：把合成后的最终帧写盘 ---- */
static int gDumpLeft = 0;
static int gDumpTag  = -1;

/* 不能直接 GetDIBits(gFrameDC,...)：位图正被 select 在 DC 里，会失败。
   所以先 BitBlt 到一张新建的 DIBSection，再从它的 bits 写盘。 */
static void dumpFrameBMP(HDC dc, const char *path)
{
    HDC mem; HBITMAP bmp, old;
    void *bits = NULL;
    BITMAPINFO bi;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *fp;
    int W = VIEW_W, H = VIEW_H;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;               /* 顶向下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    mem = CreateCompatibleDC(NULL);
    if (!mem) return;
    bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { DeleteDC(mem); return; }
    old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(mem, old);

    fp = fopen(path, "wb");
    if (fp) {
        memset(&fh, 0, sizeof(fh));
        fh.bfType = 0x4D42;                        /* "BM" */
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + (DWORD)W * (DWORD)H * 4;
        memcpy(&ih, &bi.bmiHeader, sizeof(ih));
        ih.biSizeImage = (DWORD)W * (DWORD)H * 4;
        fwrite(&fh, sizeof(fh), 1, fp);
        fwrite(&ih, sizeof(ih), 1, fp);
        fwrite(bits, 1, (size_t)W * (size_t)H * 4, fp);
        fclose(fp);
    }
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* 连拍时每 10 帧落一次盘（省 IO），并把帧号写进同名 txt，
   分析脚本据此精确取出「这一帧」的 T 行。 */
static void dumpMaybe(HDC dc)
{
    char p[600], q[600];
    const char *dir;
    FILE *fp;
    if (gDumpLeft <= 0) return;
    if (gDumpLeft % 10 == 0) {
        dir = getenv("PVZ_DUMPDIR");
        if (dir && *dir) {
            _snprintf(p, sizeof(p) - 1, "%s\\st%02d.bmp", dir, gDumpTag);
            p[sizeof(p) - 1] = 0;
            dumpFrameBMP(dc, p);
            _snprintf(q, sizeof(q) - 1, "%s\\st%02d.txt", dir, gDumpTag);
            q[sizeof(q) - 1] = 0;
            fp = fopen(q, "w");
            if (fp) { fprintf(fp, "%d\n", gUiPaint); fclose(fp); }
        }
    }
    gDumpLeft--;
}
'''

MACROS = r'''
/* 把调用点的行号带进钩子 —— 报告里每一条都能直接指回 pvz.c 的行。
   （putTextS/putTextCS 内部改用 *_impl，所以行号不会被中间层吃掉） */
#define putText(dc, f, c, s, x, y, a)      putText_impl(dc, f, c, s, x, y, a, __LINE__)
#define putTextS(dc, f, c, sh, s, x, y, a) putTextS_impl(dc, f, c, sh, s, x, y, a, __LINE__)
#define putTextCS(dc, f, c, sh, s, cx, cy) putTextCS_impl(dc, f, c, sh, s, cx, cy, __LINE__)
'''

HOTKEYS = r'''        else if (wp == VK_F1)  { gState = ST_PLAY;     gDumpTag = 1;  gDumpLeft = 60; }
        else if (wp == VK_F2)  { gState = ST_PAUSE;    gDumpTag = 2;  gDumpLeft = 60; }
        else if (wp == VK_F3)  { gState = ST_WIN;      gDumpTag = 3;  gDumpLeft = 60; }
        else if (wp == VK_F4)  { gState = ST_LOSE;     gDumpTag = 4;  gDumpLeft = 60; }
        else if (wp == VK_F5)  { openDraft();          gDumpTag = 5;  gDumpLeft = 60; }
        else if (wp == VK_F6)  { gState = ST_LEVELS;   gDumpTag = 6;  gDumpLeft = 60; }
        else if (wp == VK_F7)  { gState = ST_TALENTS;  gDumpTag = 7;  gDumpLeft = 60; }
        else if (wp == VK_F8)  { gState = ST_ACH;      gDumpTag = 8;  gDumpLeft = 60; }
        else if (wp == VK_F9)  { gState = ST_GACHA;    gDumpTag = 9;  gDumpLeft = 60; }
        else if (wp == VK_F10) { gState = ST_LOADOUT;  gDumpTag = 10; gDumpLeft = 60; }
        else if (wp == VK_F12) { rewardPrepare(); gState = ST_REWARD; gDumpTag = 11; gDumpLeft = 60; }
        else if (wp == VK_HOME){ gState = ST_DAILY;    gDumpTag = 12; gDumpLeft = 60; }
        else if (wp == VK_END) { gState = ST_MENU;     gDumpTag = 0;  gDumpLeft = 60; }
'''


def patch(text, old, new, what, count=1):
    global CUM
    n = text.count(old)
    if n != count:
        sys.exit("[X] 锚点不匹配：%s（期望 %d 处，实际 %d 处）" % (what, count, n))
    idx = text.index(old)
    delta = new.count("\n") - old.count("\n")
    at = text[:idx].count("\n") + 1
    CUM += delta
    BOUNDS.append([at, CUM])
    return text.replace(old, new, count)


def main():
    src = open(SRC, encoding="utf-8").read()
    n0 = len(src)

    # 1. 钩子
    src = patch(src,
                "static HFONT gF15, gF18, gF22, gF30, gF54, gF72;\n",
                "static HFONT gF15, gF18, gF22, gF30, gF54, gF72;\n" + HOOK,
                "字体全局变量声明")

    # 2. putText -> putText_impl（多一个 src 行号参数）
    src = patch(src,
                "static void putText(HDC dc, HFONT f, COLORREF c, const wchar_t *s, "
                "float x, float y, int align)\n{\n    SIZE sz; RECT rc;\n",
                "static void putText_impl(HDC dc, HFONT f, COLORREF c, const wchar_t *s, "
                "float x, float y, int align, int src)\n{\n    SIZE sz; RECT rc;\n",
                "putText 函数签名")

    # 2b. 记录原始颜色 + 文字画完后压一块探针色标记
    src = patch(src,
                "    rc.left = (int)x; rc.top = (int)y; "
                "rc.right = rc.left + sz.cx + 4; rc.bottom = rc.top + sz.cy + 4;\n"
                "    SetTextColor(dc, c);\n"
                "    DrawTextW(dc, s, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP);\n",
                "    rc.left = (int)x; rc.top = (int)y; "
                "rc.right = rc.left + sz.cx + 4; rc.bottom = rc.top + sz.cy + 4;\n"
                "    uiTraceText(dc, f, c, s, (float)rc.left, (float)rc.top, "
                "(float)sz.cx, (float)sz.cy, src);\n"
                "    SetTextColor(dc, c);\n"
                "    DrawTextW(dc, s, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP);\n"
                "    uiMark(dc, (float)rc.left, (float)rc.top, "
                "(float)sz.cx, (float)sz.cy, src);\n",
                "putText 内的 rc 赋值 + SetTextColor + DrawTextW")

    # 3. putTextS -> putTextS_impl
    src = patch(src,
                "static void putTextS(HDC dc, HFONT f, COLORREF c, COLORREF sh, "
                "const wchar_t *s, float x, float y, int align)\n"
                "{\n"
                "    putText(dc, f, sh, s, x + 2, y + 2, align);\n"
                "    putText(dc, f, c, s, x, y, align);\n"
                "}\n",
                "static void putTextS_impl(HDC dc, HFONT f, COLORREF c, COLORREF sh, "
                "const wchar_t *s, float x, float y, int align, int src)\n"
                "{\n"
                "    putText_impl(dc, f, sh, s, x + 2, y + 2, align, src);\n"
                "    putText_impl(dc, f, c, s, x, y, align, src);\n"
                "}\n",
                "putTextS 函数体")

    # 4. putTextCS -> putTextCS_impl，并在其后补三个宏
    src = patch(src,
                "static void putTextCS(HDC dc, HFONT f, COLORREF c, COLORREF sh, "
                "const wchar_t *s, float cx, float cy)\n"
                "{\n"
                "    SIZE sz;\n"
                "    SelectObject(dc, f);\n"
                "    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);\n"
                "    putText(dc, f, sh, s, cx - sz.cx * 0.5f + 2, cy - sz.cy * 0.5f + 2, 0);\n"
                "    putText(dc, f, c, s, cx - sz.cx * 0.5f, cy - sz.cy * 0.5f, 0);\n"
                "}\n",
                "static void putTextCS_impl(HDC dc, HFONT f, COLORREF c, COLORREF sh, "
                "const wchar_t *s, float cx, float cy, int src)\n"
                "{\n"
                "    SIZE sz;\n"
                "    SelectObject(dc, f);\n"
                "    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);\n"
                "    putText_impl(dc, f, sh, s, cx - sz.cx * 0.5f + 2, cy - sz.cy * 0.5f + 2, 0, src);\n"
                "    putText_impl(dc, f, c, s, cx - sz.cx * 0.5f, cy - sz.cy * 0.5f, 0, src);\n"
                "}\n" + MACROS,
                "putTextCS 函数体")

    # 5. render() 每帧开头打帧标记
    src = patch(src,
                "static void render(HDC out)\n{\n    XFORM id, xf;\n",
                "static void render(HDC out)\n{\n"
                "    uiTraceFrame(gState, gWorldDC, gFrameDC);\n    XFORM id, xf;\n",
                "render 函数开头")

    # 6. render() 结尾导出最终帧
    src = patch(src,
                "                BitBlt(out, 0, 0, VIEW_W, VIEW_H, gFrameDC, 0, 0, SRCCOPY);\n"
                "            }\n        }\n    }\n}\n",
                "                BitBlt(out, 0, 0, VIEW_W, VIEW_H, gFrameDC, 0, 0, SRCCOPY);\n"
                "            }\n        }\n    }\n"
                "    dumpMaybe(gFrameDC);\n}\n",
                "render 函数结尾")

    # 7. 强制换界面热键
    src = patch(src,
                "        else if (wp == VK_ESCAPE) { gSel = -1; gShovel = 0; }\n",
                HOTKEYS + "        else if (wp == VK_ESCAPE) { gSel = -1; gShovel = 0; }\n",
                "WM_KEYDOWN 的 ESC 分支")

    open(DST, "w", encoding="utf-8", newline="\n").write(src)
    json.dump({"bounds": BOUNDS, "src": SRC, "dst": DST},
              open(LINEMAP, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("[1/2] %s 已生成（%d -> %d 字节）；行号映射 %s：%s"
          % (DST, n0, len(src), LINEMAP, BOUNDS))

    cmd = ["gcc", "-O2", "-mwindows", "-static-libgcc", "-o", "_uiproof.exe", DST,
           "-lgdi32", "-luser32", "-lmsimg32", "-lwinmm", "-lm", "-Wall", "-Wextra"]
    print("[2/2] " + " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    if out:
        print(out[:4000])
    if r.returncode != 0:
        sys.exit("[X] 编译失败 returncode=%d" % r.returncode)
    print("[OK] _uiproof.exe 编译成功")


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)) or ".")
    main()
