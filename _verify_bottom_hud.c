/* ------------------------------------------------------------------
   底部状态条排版校验器（无窗口，纯测量）

   为什么需要它：本环境的模型看不了图，肉眼验收截图这条路走不通。
   与其"感觉差不多了"，不如用 GDI 自己把字量出来，硬算矩形有没有交叠。

   校验对象是 pvz.c 里底部那三行状态文字 + 波次进度条：
     - 波次条（含 3px 描边）实际占据 x 175~825 / y 617~643，
       另外 BAR_Y-13 起还有危险段三角箭头，所以从 607 就算被占。
     - 草坪底边 LAWN_Y+ROWS*CELL_H = 604，它以上是种植区，
       状态文字压上去会盖住植物 —— 这也一并校验。
   ------------------------------------------------------------------ */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define VIEW_W 1000
#define VIEW_H 650
#define LAWN_Y 104
#define ROWS 5
#define CELL_H 100

/* pvz.c 里的 putText：align 0=左对齐 1=居中 2=右对齐 */
typedef struct { int l, t, r, b; const char *name; } Rect;

static HFONT mkFont(int px, int bold, const wchar_t *face)
{
    return CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static Rect measure(HDC dc, HFONT f, const wchar_t *s, float x, float y, int align,
                    const char *name)
{
    SIZE sz; Rect r;
    SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    if (align == 1)      x -= sz.cx * 0.5f;
    else if (align == 2) x -= (float)sz.cx;
    r.l = (int)x; r.t = (int)y;
    r.r = r.l + sz.cx; r.b = r.t + sz.cy;
    r.name = name;
    return r;
}

static int hit(Rect a, Rect b)
{
    return !(a.r <= b.l || b.r <= a.l || a.b <= b.t || b.b <= a.t);
}

static void dump(const char *tag, Rect r)
{
    printf("  %-28s x %4d..%4d  y %4d..%4d  (%dx%d)\n",
           tag, r.l, r.r, r.t, r.b, r.r - r.l, r.b - r.t);
}

int main(void)
{
    HDC dc = CreateCompatibleDC(NULL);
    HFONT f15 = mkFont(15, 0, L"\u5fae\u8f6f\u96c5\u9ed1");
    HFONT f18 = mkFont(18, 1, L"\u5fae\u8f6f\u96c5\u9ed1");
    int bad = 0, i, j, k;
    const wchar_t *toggles15[2] = { L"\u81ea\u52a8\u62fe\u53d6 [A] \u5f00",
                                     L"\u98d8\u843d\u7279\u6548 [W] \u5f00" };
    Rect rs[5];
    /* 代表值：0 位与 4 位击杀数都量一遍，别只测好看的那个 */
    const wchar_t *kills[3] = { L"\u51fb\u6740 0", L"\u51fb\u6740 42",
                                L"\u51fb\u6740 1234" };
    Rect bar, lawn;

    printf("=== 1. 现状排版（pvz.c:5804 起的坐标）===\n");
    rs[0] = measure(dc, f18, kills[1], 18, 614, 0, "kill@left");
    rs[1] = measure(dc, f15, toggles15[0], 982, 607, 2, "autosun@right");
    rs[2] = measure(dc, f15, toggles15[1], 982, 627, 2, "weather@right");
    for (i = 0; i < 3; i++) dump(rs[i].name, rs[i]);

    bar.l = 175; bar.r = 825; bar.t = 607; bar.b = 643; bar.name = "wavebar";
    lawn.l = 0; lawn.r = VIEW_W; lawn.t = 0; lawn.b = LAWN_Y + ROWS * CELL_H;
    lawn.name = "lawn";
    dump("wavebar(含描边/发光)", bar);
    printf("  lawn  底边 y=%d（以上为种植区）\n", lawn.b);

    printf("\n=== 2. 两两交叠检查 ===\n");
    for (i = 0; i < 3; i++)
        for (j = i + 1; j < 3; j++)
            if (hit(rs[i], rs[j])) { printf("  [FAIL] %s 撞 %s\n", rs[i].name, rs[j].name); bad++; }
    printf("  三行状态文字互不交叠 ...... %s\n", bad ? "FAIL" : "OK");

    k = 0;
    for (i = 0; i < 3; i++) if (hit(rs[i], bar)) { printf("  [FAIL] %s 压在波次条上\n", rs[i].name); k++; }
    printf("  状态文字不压波次条 ........ %s\n", k ? "FAIL" : "OK"); bad += k;

    k = 0;
    for (i = 0; i < 3; i++) if (hit(rs[i], lawn)) { printf("  [FAIL] %s 侵入草坪种植区\n", rs[i].name); k++; }
    printf("  状态文字不侵入草坪 ........ %s\n", k ? "FAIL" : "OK"); bad += k;

    printf("\n=== 3. 边界与自检 ===\n");
    k = 0;
    for (i = 0; i < 3; i++) {
        if (rs[i].l < 0 || rs[i].r > VIEW_W) { printf("  [FAIL] %s 超出画面宽度\n", rs[i].name); k++; }
        if (rs[i].t < 604 || rs[i].b > VIEW_H) { printf("  [FAIL] %s 超出底部条带 604..%d\n", rs[i].name, VIEW_H); k++; }
    }
    printf("  文字都在 604..%d 条带内 ..... %s\n", VIEW_H, k ? "FAIL" : "OK"); bad += k;

    printf("\n=== 4. 击杀数变宽（0 / 42 / 1234 位）===\n");
    for (i = 0; i < 3; i++) {
        Rect t = measure(dc, f18, kills[i], 18, 616, 0, "kill");
        dump("kill", t);
        if (hit(t, bar)) { printf("  [FAIL] 位数变多后压到波次条\n"); bad++; }
        if (t.r > bar.l + 3) { printf("  [WARN] 右边界 %d 已贴到波次条左边 %d\n", t.r, bar.l); }
    }

    printf("\n=== 5. 右侧开关提示与波次条的最小水平间距 ===\n");
    for (i = 1; i < 3; i++) {
        int gap = rs[i].l - bar.r;
        printf("  %-16s 左边界 %4d，距波次条右边 %d px  %s\n",
               rs[i].name, rs[i].l, gap, gap > 8 ? "OK" : "过近");
        if (gap <= 0) { printf("  [FAIL] 水平方向已相交\n"); bad++; }
    }

    printf("\n=== 6. 底部条带 ASCII 位图（模型看不了图，用这个来看）===\n");
    printf("    列 0..999 每格 10px，行 600..650 每格 2px\n");
    printf("    图例: '#' 状态文字  '=' 波次条(含描边/发光)  '~' 草坪底边以下  ' ' 空白\n");
    {
        int row, col;
        printf("      +");
        for (col = 0; col < 100; col++) putchar(col % 10 == 0 ? '|' : '-');
        printf("+\n");
        for (row = 600; row < 650; row += 2) {
            printf(" %4d |", row);
            for (col = 0; col < 100; col++) {
                int x0 = col * 10, x1 = x0 + 10;
                int y0 = row, y1 = row + 2;
                char ch = ' ';
                Rect cell; cell.l = x0; cell.r = x1; cell.t = y0; cell.b = y1;
                if (hit(cell, bar)) ch = '=';
                for (i = 0; i < 3; i++) if (hit(cell, rs[i])) ch = '#';
                if (ch == ' ' && row >= 604) ch = '~';
                putchar(ch);
            }
            printf("|\n");
        }
        printf("      +");
        for (col = 0; col < 100; col++) putchar(col % 10 == 0 ? '|' : '-');
        printf("+\n");
        printf("      0         1         2         3         4         5         6         7         8         9\n");
        printf("      0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789\n");
    }

    DeleteObject(f15); DeleteObject(f18); DeleteDC(dc);
    printf("\n===== 结论: %s（%d 项未通过）=====\n", bad ? "不通过" : "全部通过", bad);
    return bad ? 1 : 0;
}
