# -*- coding: utf-8 -*-
"""
从 pvz.c 生成一个「带 UI 文本审计钩子」的临时副本 _uiaudit.c，并编译成 _uiaudit.exe。

为什么用副本而不是改 pvz.c：
  - 钩子只是诊断设施，不该进入出货源码（生产版保持零调试代码）。
  - 副本是 pvz.c 的定点插入版，渲染逻辑逐字节一致，
    所以审计结论对生产版成立。

副本比 pvz.c 多出的东西（全部只在审计版里）：
  1. putText / putTextS / putTextCS 换成带 src 行号的 *_impl，外壳用宏把
     __LINE__ 传进去 —— 报告里的每一条都能直接指回 pvz.c 的行号。
  2. 每次画字之前，把该字符串「真渲染一次」量出墨迹矩形（不是字体行高！
     微软雅黑 15px 的 cy=20，拿行高当占位会把正常行距全判成重叠），
     结果按 (字体, 字符串) 缓存，只在第一次遇到时算。
  3. render() 每帧开头写一条 F 行（帧号 + gState），用来按帧/界面分组。
  4. F1~F4 强制切到 三选一 / 胜利 / 失败 / 抽卡结算 四个状态 ——
     这四个平时要清关或打几分钟才看得到，审计走不到。

输出 TSV（设 PVZ_UITRACE=<路径> 启用）：
    F <帧号> <gState> <gWorldDC> <gFrameDC>
    T <调用行号> <align> <墨迹x> <墨迹y> <墨迹w> <墨迹h> <文本>
"""

import json
import os
import subprocess
import sys

SRC = "pvz.c"
DST = "_uiaudit.c"
LINEMAP = "_uiaudit_lines.json"

# 审计副本里 __LINE__ 报的是副本行号，而副本比 pvz.c 多插了东西。
# 这里按「补丁应用的先后顺序」累计行数偏移，产出 副本行号 -> 源码行号 的映射表，
# 分析脚本据此把报告里的行号换回 pvz.c 的真实行号。
BOUNDS = []          # [[副本行号, 该行起的累计偏移], ...]
CUM = 0

HOOK = r'''
/* ================== UI 文本审计钩子（仅审计版） ================== */

/* ---- 量字形墨迹：把字符串真画一次，扫出非白像素的范围 ----
   为什么要这么麻烦：GetTextExtentPoint32W 返回的是字体「行高」，
   微软雅黑 15px 时 cy=20、18px 时 cy=25，比字号大不少。用行高当占位框
   去判重叠，20px 行距的两行字会被判成压字（实测就误报过）。
   只有真渲染取墨迹，才是眼睛看到的那块。 */
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
    bi.bmiHeader.biHeight      = -H;            /* 负 = 顶向下，扫描时好懂 */
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

static FILE *gUiTrace = NULL;
static int   gUiPaint = 0;
static int   gUiLive  = 0;

/* 这里不能引用 gWorldDC / gFrameDC（声明在 1400 行后，本函数在 160 行），
   所以 DC 指针由 render() 调用时传进来。 */
static void uiTraceFrame(int st, void *worldDC, void *frameDC)
{
    if (!gUiLive) {                       /* 第一次进 render 才读环境变量 */
        const char *p = getenv("PVZ_UITRACE");
        gUiLive = 1;
        if (p && *p) gUiTrace = fopen(p, "w");
    }
    if (!gUiTrace) return;
    fprintf(gUiTrace, "F\t%d\t%d\t%llu\t%llu\n", gUiPaint++, st,
            (unsigned long long)(size_t)worldDC, (unsigned long long)(size_t)frameDC);
    fflush(gUiTrace);                     /* 每帧刷一次：崩了也不丢前面的数据 */
}

static void uiTraceText(HFONT f, const wchar_t *s, int align, float x, float y, int src)
{
    char u8[2048];
    int x0, y0, x1, y1;
    if (!gUiTrace) return;
    uiInk(f, s, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return;      /* 空串 / 全空白 */
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, u8, sizeof(u8), NULL, NULL) <= 0) return;
    fprintf(gUiTrace, "T\t%d\t%d\t%.1f\t%.1f\t%d\t%d\t%s\n",
            src, align, x + (float)x0, y + (float)y0, x1 - x0, y1 - y0, u8);
}
'''

# 审计版专用强制换状态热键：F1=三选一 F2=胜利 F3=失败 F4=抽卡结算
HOTKEYS = r'''        else if (wp == VK_F1) { openDraft(); }
        else if (wp == VK_F2) { gState = ST_WIN; }
        else if (wp == VK_F3) { gState = ST_LOSE; }
        else if (wp == VK_F4) { rewardPrepare(); gState = ST_REWARD; }
'''

MACROS = r'''
/* 把调用点的行号带进钩子 —— 报告里每一条都能直接指回 pvz.c 的行。
   （putTextS/putTextCS 内部改用 *_impl，所以行号不会被中间层吃掉） */
#define putText(dc, f, c, s, x, y, a)      putText_impl(dc, f, c, s, x, y, a, __LINE__)
#define putTextS(dc, f, c, sh, s, x, y, a) putTextS_impl(dc, f, c, sh, s, x, y, a, __LINE__)
#define putTextCS(dc, f, c, sh, s, cx, cy) putTextCS_impl(dc, f, c, sh, s, cx, cy, __LINE__)
'''


def patch(text, old, new, what, count=1):
    global CUM
    n = text.count(old)
    if n != count:
        sys.exit("[X] 锚点不匹配：%s（期望 %d 处，实际 %d 处）" % (what, count, n))
    idx = text.index(old)
    delta = new.count("\n") - old.count("\n")
    # 本补丁影响的第一个副本行号
    at = text[:idx].count("\n") + 1
    CUM += delta
    BOUNDS.append([at, CUM])
    return text.replace(old, new, count)


def main():
    src = open(SRC, encoding="utf-8").read()
    orig_len = len(src)

    # --- 1. 钩子插入点：字体全局变量之后、putText 之前 ---
    src = patch(src,
                "static HFONT gF15, gF18, gF22, gF30, gF54, gF72;\n",
                "static HFONT gF15, gF18, gF22, gF30, gF54, gF72;\n" + HOOK,
                "字体全局变量声明")

    # --- 2. putText -> putText_impl（多一个 src 行号参数），并记录墨迹 ---
    src = patch(src,
                "static void putText(HDC dc, HFONT f, COLORREF c, const wchar_t *s, "
                "float x, float y, int align)\n{\n    SIZE sz; RECT rc;\n",
                "static void putText_impl(HDC dc, HFONT f, COLORREF c, const wchar_t *s, "
                "float x, float y, int align, int src)\n{\n    SIZE sz; RECT rc;\n",
                "putText 函数签名")
    anchor_rc = ("    rc.left = (int)x; rc.top = (int)y; "
                 "rc.right = rc.left + sz.cx + 4; rc.bottom = rc.top + sz.cy + 4;\n")
    src = patch(src, anchor_rc,
                "    uiTraceText(f, s, align, x, y, src);\n" + anchor_rc,
                "putText 内的 rc 赋值行")

    # --- 3. putTextS -> putTextS_impl ---
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

    # --- 4. putTextCS -> putTextCS_impl，并在其后补三个宏 ---
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

    # --- 5. render() 每帧开头打帧标记 ---
    src = patch(src,
                "static void render(HDC out)\n{\n    XFORM id, xf;\n",
                "static void render(HDC out)\n{\n"
                "    uiTraceFrame(gState, gWorldDC, gFrameDC);\n    XFORM id, xf;\n",
                "render 函数开头")

    # --- 6. 审计版热键 ---
    src = patch(src,
                "        else if (wp == VK_ESCAPE) {\n",
                HOTKEYS + "        else if (wp == VK_ESCAPE) {\n",
                "WM_KEYDOWN 的 ESC 分支")

    open(DST, "w", encoding="utf-8", newline="\n").write(src)
    json.dump({"bounds": BOUNDS, "src": SRC, "dst": DST},
              open(LINEMAP, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("[1/3] 已生成 %s（%d -> %d 字节，+%d）；行号映射 %s：%s"
          % (DST, orig_len, len(src), len(src) - orig_len, LINEMAP, BOUNDS))

    # --- 7. 自检：宏有没有把原有调用点都接上 ---
    n_impl = src.count("putText_impl(")
    n_macro = src.count("putTextS_impl(") + src.count("putTextCS_impl(") + n_impl
    print("[2/3] 自检：putText_impl 定义/调用 %d 处，宏转发总数 %d 处" % (n_impl, n_macro))

    cmd = ["gcc", "-O2", "-mwindows", "-static-libgcc", "-o", "_uiaudit.exe", DST,
           "-lgdi32", "-luser32", "-lmsimg32", "-lwinmm", "-lm", "-Wall", "-Wextra"]
    print("[3/3] " + " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    if out.strip():
        print(out.strip()[:4000])
    if r.returncode != 0:
        sys.exit("[X] 编译失败，returncode=%d" % r.returncode)
    print("[OK] _uiaudit.exe 编译成功")


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)) or ".")
    main()
