# -*- coding: utf-8 -*-
"""给 plat.h 补上「可移植侧」的 56 个 Win32 函数声明，并允许本机强制走该分支。

【为什么必须先做这一步】
  plat.h 原来只声明了 5 个 plat* 函数（platInit/platShutdown/...），
  但那 56 个 GDI/Win32 符号**一个都没声明** —— 可移植侧实际的声明面是空的。
  前几轮 iOS 移植之所以停在半路，一部分原因就在这里：
  写 plat_ios.m 时没有任何"契约"可依，全靠猜。

【本机验证的关键开关：PLAT_PORTABLE】
  原来分支条件是 `#ifdef _WIN32`，在 Windows 上永远走直通分支，
  于是 plat_web.c 在本机**根本编译不了**，也就无法用 gcc 做逐像素验证。
  改成 `#if defined(_WIN32) && !defined(PLAT_PORTABLE)` 之后：
      gcc -DPLAT_PORTABLE ... plat_web.c pvz.c
  就能在 Windows 上用软件光栅后端跑同一份 pvz.c，
  与 Windows 构建的结果逐像素比对 —— 这就是 P2 阶段的验证手段。

【幂等】开头检查标记。
用法：python _add_plat_decls.py
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "plat.h")

MARK = "/* ---- 可移植侧：Win32 符号的声明面"

DECLS = '''
/* ---- 可移植侧：Win32 符号的声明面 -------------------------------------------------
   清单来自对 pvz.c 的**实测统计**（56 个函数），不是照抄 windows.h。
   只声明真正用到的 —— 声明面越小，后端实现越不容易缺项。

   ★ 语义契约（实现方必须遵守，否则行为会静默偏离）：
     · 坐标全部是**设备像素**（调用方已经乘过 SS）；
     · `FillRect` 是 [left,right) × [top,bottom)，右下开区间（GDI 语义）；
     · 位图一律 32 位 **BGRA 且 alpha 已预乘**；
     · `SetWorldTransform` 只用到 eM11/eM22/eDx/eDy（缩放+平移，无旋转），
       但**必须**按完整 2x3 仿射实现，否则以后加旋转会错得莫名其妙；
     · `AlphaBlend` 的 `SourceConstantAlpha` 是**额外**的整体不透明度，
       与像素自身的 alpha 相乘（这是 AC_SRC_ALPHA 的语义）；
     · `SetGraphicsMode(GM_ADVANCED)` 必须真正生效 —— 引擎依赖它让世界变换
       对 FillRect/Ellipse 等"图元"也起作用。GM_COMPATIBLE 下只有 BitBlt 受影响。
   ------------------------------------------------------------------------- */
#define GM_COMPATIBLE 1
#define GM_ADVANCED   2
#define PS_SOLID      0
#define FW_NORMAL     400
#define FW_BOLD       700
#define DEFAULT_CHARSET   1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY   0
#define DEFAULT_PITCH     0
#define MCIERR_BASE   0
typedef int MCIERROR;
typedef void *HMENU;
typedef void *HMODULE;
typedef struct { unsigned short Red, Green, Blue, Alpha; LONG x, y; } TRIVERTEX;
typedef struct { DWORD UpperLeft, LowerRight; } GRADIENT_RECT;
#define GRADIENT_FILL_RECT_V 1
#define NULLREGION  1
#define SIMPLEREGION 2
#define COMPLEXREGION 3
#define ERROR       0

/* 绘制 */
HDC      CreateCompatibleDC(HDC hdc);
HBITMAP  CreateCompatibleBitmap(HDC hdc, int w, int h);
HBITMAP  CreateDIBSection(HDC hdc, const BITMAPINFO *bi, UINT usage,
                          void **bits, HANDLE section, DWORD offset);
HGDIOBJ  SelectObject(HDC hdc, HGDIOBJ obj);
BOOL     DeleteObject(HGDIOBJ obj);
BOOL     DeleteDC(HDC hdc);
HDC      GetDC(HWND hwnd);
int      ReleaseDC(HWND hwnd, HDC hdc);
BOOL     BitBlt(HDC dst, int x, int y, int w, int h,
                HDC src, int sx, int sy, DWORD rop);
BOOL     StretchBlt(HDC dst, int x, int y, int w, int h,
                    HDC src, int sx, int sy, int sw, int sh, DWORD rop);
BOOL     AlphaBlend(HDC dst, int x, int y, int w, int h,
                    HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION bf);
int      FillRect(HDC hdc, const RECT *rc, HBRUSH br);
BOOL     Ellipse(HDC hdc, int l, int t, int r, int b);
BOOL     Rectangle(HDC hdc, int l, int t, int r, int b);
BOOL     RoundRect(HDC hdc, int l, int t, int r, int b, int ew, int eh);
BOOL     Polygon(HDC hdc, const POINT *pts, int n);
BOOL     MoveToEx(HDC hdc, int x, int y, POINT *old);
BOOL     LineTo(HDC hdc, int x, int y);
HPEN     CreatePen(int style, int w, COLORREF c);
HBRUSH   CreateSolidBrush(COLORREF c);
HGDIOBJ  GetStockObject(int idx);
BOOL     SetWorldTransform(HDC hdc, const XFORM *xf);
BOOL     GetWorldTransform(HDC hdc, XFORM *xf);
int      SetStretchBltMode(HDC hdc, int mode);
int      SetGraphicsMode(HDC hdc, int mode);
int      SetBrushOrgEx(HDC hdc, int x, int y, POINT *old);
HRGN     CreateRectRgn(int l, int t, int r, int b);
HRGN     CreateRoundRectRgn(int l, int t, int r, int b, int ew, int eh);
int      SelectClipRgn(HDC hdc, HRGN rgn);
BOOL     GradientFill(HDC hdc, TRIVERTEX *vt, unsigned long nv,
                      GRADIENT_RECT *gr, unsigned long ng, unsigned long mode);

/* 文字 */
HFONT    CreateFontW(int h, int w, int esc, int orient, int weight,
                     DWORD italic, DWORD under, DWORD strike, DWORD charset,
                     DWORD prec, DWORD clip, DWORD quality, DWORD pitch,
                     const wchar_t *face);
COLORREF SetTextColor(HDC hdc, COLORREF c);
int      SetBkMode(HDC hdc, int mode);
int      DrawTextW(HDC hdc, const wchar_t *s, int n, RECT *rc, UINT fmt);
BOOL     GetTextExtentPoint32W(HDC hdc, const wchar_t *s, int n, SIZE *sz);

/* 音频 */
MCIERROR mciSendStringW(const wchar_t *cmd, wchar_t *ret, UINT retLen, HWND cb);
BOOL     PlaySound(const wchar_t *name, HMODULE h, DWORD flags);

/* 输入 / 时间 */
SHORT    GetAsyncKeyState(int vk);
DWORD    GetTickCount(void);
void     Sleep(DWORD ms);

/* 杂项 */
int      wsprintfW(wchar_t *out, const wchar_t *fmt, ...);
FILE    *_wfopen(const wchar_t *path, const wchar_t *mode);
BOOL     GetClientRect(HWND hwnd, RECT *rc);
void     SetCursor(HCURSOR c);
HCURSOR  LoadCursorW(HINSTANCE h, const wchar_t *name);
DWORD    GetModuleFileNameW(HINSTANCE h, wchar_t *out, DWORD n);
DWORD    GetEnvironmentVariableW(const wchar_t *name, wchar_t *out, DWORD n);
BOOL     SetWindowTextW(HWND hwnd, const wchar_t *s);

/* 窗口 / 消息（网页版用 PLAT_HAS_OWN_MAIN=0 绕开，但符号要存在才能编过） */
BOOL     ShowWindow(HWND hwnd, int cmd);
BOOL     UpdateWindow(HWND hwnd);
BOOL     TranslateMessage(const MSG *msg);
LRESULT  DispatchMessageW(const MSG *msg);
BOOL     PeekMessageW(MSG *msg, HWND hwnd, UINT lo, UINT hi, UINT remove);
LRESULT  DefWindowProcW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
ATOM     RegisterClassExW(const WNDCLASSEXW *wc);
HWND     CreateWindowExW(DWORD ex, const wchar_t *cls, const wchar_t *title,
                         DWORD style, int x, int y, int w, int h,
                         HWND parent, HMENU menu, HINSTANCE inst, void *param);
BOOL     SetProcessDpiAwarenessContext(HANDLE ctx);
BOOL     SetForegroundWindow(HWND hwnd);
int      GetSystemMetrics(int idx);
BOOL     AdjustWindowRectEx(RECT *rc, DWORD style, BOOL menu, DWORD exStyle);

/* 网页后端额外暴露给外壳的入口（Windows 侧不需要，故声明在可移植分支内） */
int  platWebInit(int w, int h, const char *title);
/* 由 JS 侧注入字形位图：见 plat_web.c 的说明（文字用 Canvas2D 光栅一次后缓存） */
int  platWebGlyph(int cp, int px, int bold, const unsigned char *bgra,
                  int gw, int gh, int adv);
/* 把一帧的指针事件交给游戏：type 0=move 1=down 2=up 3=right-up */
void platWebPointer(int type, float x, float y);
void platWebKey(int vk, int down);
/* 每帧结束后外壳取一次像素（BGRA 预乘，尺寸 = 设备像素） */
const void *platWebFrame(int *w, int *h, int *stride);
void platWebResize(int cssW, int cssH, float dpr);

'''

OLD_GUARD = "#ifdef _WIN32\n\n/* ======================= Windows：直通，不加任何包装 ======================= */"
NEW_GUARD = '''#if defined(_WIN32) && !defined(PLAT_PORTABLE)

/* ======================= Windows：直通，不加任何包装 ======================= */'''


def main():
    s = io.open(SRC, encoding="utf-8").read()
    if MARK in s:
        print("  已加过，跳过（幂等）")
        return 0

    # ① 分支条件：允许本机强制走可移植分支
    assert s.count(OLD_GUARD) == 1, "Windows 分支锚点不唯一: %d" % s.count(OLD_GUARD)
    s = s.replace(OLD_GUARD, NEW_GUARD)
    print("  ✔ 分支条件改为 `_WIN32 && !PLAT_PORTABLE`")

    # ② 在可移植分支的末尾（#endif 之前）插入声明
    tail = "#endif  /* _WIN32 */\n#endif  /* PLAT_H */"
    assert s.count(tail) == 1, "文件末尾锚点不唯一"
    s = s.replace(tail, DECLS + tail)
    print("  ✔ 插入 56 个函数声明 + 网页后端接口")

    # ③ 顶部的说明注释同步（它原来写的是"26 类型 + 52 函数"）
    s = s.replace("iOS 侧要实现的符号（共 26 类型 + 52 函数，清单来自对 pvz.c 的实测统计）：",
                  "可移植侧要实现的符号（共 26 类型 + 56 函数，清单来自对 pvz.c 的实测统计）：\n"
                  "   （2026-09-21 由 gcc 实测校正：原注释写 52，实际是 56。\n"
                  "     用途也从\"只给 iOS\"扩展为\"iOS + 网页版共用\"。）")
    io.open(SRC, "w", encoding="utf-8", newline="\n").write(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
