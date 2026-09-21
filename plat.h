/* =========================================================================
   plat.h —— 平台抽象层
   -------------------------------------------------------------------------
   目的：让 pvz.c（12300 行游戏逻辑 + 400 行绘制）在 Windows 与 iOS 上
         编译同一份源码，差异只落在这个头文件背后的实现里。

   ★ 设计取向：**Windows 侧是"直通"**。
     也就是 `plat.h` 在 _WIN32 下只是把 windows.h/mmsystem.h 引进来，
     不包一层、不改造、不重命名 —— 这样 Windows 版的行为**一个字节都不变**，
     现有的 12 个测试与抓帧基线全部继续有效，
     而 iOS 侧去实现同一批符号。两端共用同一份 pvz.c。

   为什么能这么做（Phase 1 实测的依据）：
     · 游戏逻辑没有任何平台依赖；
     · 全部绘制都收在 22 个原语里，HDC 只是个"画布"参数
       → iOS 上 `HDC` 就是"一个带 CGContext 的位图"；
     · 坐标变换只用了缩放 + 平移（只用 eM11/eM22/eDx/eDy，没有旋转）
       → 直接对应 CGAffineTransform；
     · 引擎本来就是三层离屏 DC（gWorldDC → gFrameDC → 屏幕）
       → 与"位图上下文链"天然同构。

   可移植侧要实现的符号（共 26 类型 + 56 函数，清单来自对 pvz.c 的实测统计）：
   （2026-09-21 由 gcc 实测校正：原注释写 52，实际是 56。
     用途也从"只给 iOS"扩展为"iOS + 网页版共用"。）
     类型  HDC HWND HBITMAP HBRUSH HFONT HRGN HGDIOBJ HINSTANCE HPEN
           WPARAM LPARAM LRESULT MSG RECT POINT SIZE
           BITMAPINFO BITMAPINFOHEADER XFORM COLORREF DWORD UINT
           WNDCLASSEXW PAINTSTRUCT HRESULT BYTE
     绘制  CreateDIBSection CreateCompatibleDC CreateCompatibleBitmap
           SelectObject DeleteObject DeleteDC GetDC ReleaseDC
           BitBlt StretchBlt AlphaBlend FillRect Ellipse Rectangle Polygon
           LineTo MoveToEx CreatePen CreateSolidBrush
           SetWorldTransform GetWorldTransform SetStretchBltMode SetGraphicsMode
           CreateRectRgn SelectClipRgn CreateRoundRectRgn
     文字  CreateFontW SetTextColor SetBkMode DrawTextW GetTextExtentPoint32W
     音频  mciSendStringW PlaySound
     输入  GetAsyncKeyState
     杂项  GetTickCount Sleep wsprintfW _wfopen GetLocalRect GetClientRect
           LoadCursorW LoadIconW SetWindowTextW GetLocalTime
   ========================================================================= */
#ifndef PLAT_H
#define PLAT_H

#if defined(_WIN32) && !defined(PLAT_PORTABLE)

/* ======================= Windows：直通，不加任何包装 ======================= */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <mmsystem.h>   /* mciSendStringW：背景音乐。编译需加 -lwinmm */

#define PLAT_DESKTOP 1
/* 主循环由 pvz.c 自己的 WinMain 驱动（窗口消息泵），iOS 侧才需要外部驱动 */
#define PLAT_HAS_OWN_MAIN 1

/* 资源目录：exe 同级的 assets\。
   ★ 这里**内联实现**而不是另开一个 plat_win32.c —— 为了让 Windows 侧继续
     "单文件编译"，现有构建命令（gcc ... pvz.c）一个字都不用改。
     平台差异只有这一个函数，放在头里比多一个编译单元更省事。 */
static void platAssetDir(wchar_t *out, int cap)
{
    wchar_t *p;
    (void)cap;
    GetModuleFileNameW(NULL, out, MAX_PATH);
    p = wcsrchr(out, L'\\');
    if (p) p[1] = 0; else out[0] = 0;
    wcscat(out, L"assets\\");
}

#else   /* ========================= iOS / 可移植侧 ========================= */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include <time.h>

#define PLAT_DESKTOP 0
/* iOS 没有窗口消息泵：主循环由 plat_ios.m 的 main() 驱动，
   pvz.c 里的 WinMain 会被条件编译掉，改为暴露 gameInit/gameFrame 供调用。 */
#define PLAT_HAS_OWN_MAIN 0

typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef unsigned int        UINT;
typedef unsigned long       DWORD;
typedef long                LONG;
typedef long long           LRESULT;
typedef uintptr_t           WPARAM;
typedef intptr_t            LPARAM;
typedef unsigned int        COLORREF;
typedef int                 HRESULT;
typedef void               *HANDLE;
typedef void               *HINSTANCE;
typedef void               *HGDIOBJ;
typedef void               *HPEN;
typedef void               *HBRUSH;
typedef void               *HFONT;
typedef void               *HBITMAP;
typedef void               *HRGN;
typedef void               *HCURSOR;
typedef void               *HICON;

/* HDC = "带 CGContext 的位图"。定义在 plat_ios.m，这里只做不透明指针。
   ⚠️ 不能直接 typedef 成 void* —— 引擎里会做 `if (sp->dc)` 之类的判空，
   还需要区分"屏幕 DC"与"离屏 DC"（呈现行为不同），所以走不透明结构体。 */
typedef struct PlatDC     *HDC;

/* ⚠️ 这几个是补的：原来只 typedef 了 HDC/HDC 系列的句柄，
   HWND / SHORT / ATOM 漏了 —— 而 MSG 结构体（在下面几十行）就用到了 HWND，
   于是可移植侧**根本编译不过**。前几轮 iOS 停在半路，这也是原因之一。 */
typedef void               *HWND;
typedef short               SHORT;
typedef unsigned short      ATOM;

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG cx, cy; } SIZE;

typedef struct {
    DWORD  biSize; LONG biWidth, biHeight;
    WORD   biPlanes, biBitCount;
    DWORD  biCompression, biSizeImage;
    LONG   biXPelsPerMeter, biYPelsPerMeter;
    DWORD  biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef struct { BITMAPINFOHEADER bmiHeader; DWORD bmiColors[3]; } BITMAPINFO;

/* 世界变换：引擎只用缩放 + 平移（无旋转），与 CGAffineTransform 一一对应 */
typedef float FLOAT;
typedef struct { float eM11, eM12, eM21, eM22, eDx, eDy; } XFORM;

typedef struct { UINT BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat; } BLENDFUNCTION;

typedef struct { UINT cbSize, style; void *lpfnWndProc; int cbClsExtra, cbWndExtra;
                 HINSTANCE hInstance, hIcon, hCursor, hbrBackground;
                 const wchar_t *lpszMenuName, *lpszClassName;
                 HICON hIconSm; } WNDCLASSEXW;
typedef struct { HDC hdc; int fErase; RECT rcPaint; int fRestore, fIncUpdate;
                 BYTE rgbReserved[32]; } PAINTSTRUCT;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam;
                 DWORD time; POINT pt; } MSG;

/* ---- 常量：取值必须与 Windows 一致，否则行为会静默偏离 ---- */
#define TRUE  1
#define FALSE 0
/* ⚠️ 取值必须是 Windows 的真实值（wingdi.h：NULL_BRUSH=5, NULL_PEN=8）。
   ★ 原来这两个都被定义成 NULL —— 那是个**真 bug**：
     `GetStockObject(NULL_PEN)` 与 `GetStockObject(NULL_BRUSH)` 变成同一个调用，
     后端**无法区分**"只填充不描边"和"只描边不填充"。
     引擎里 NULL_PEN 用了 4 处、NULL_BRUSH 用了 8 处，语义完全不同：
       fillRound  = 选 NULL_PEN   → 只填
       strokeRound= 选 NULL_BRUSH → 只描
     两个都传 0 的话，所有带描边的形状（按钮、卡片边框、圆形标尺）
     都会变成"实心块"或"什么都不画"，而且**不报错**。 */
#define NULL_BRUSH 5
#define NULL_PEN   8

#define WM_DESTROY      0x0002
#define WM_SIZE         0x0005
#define WM_PAINT        0x000F
#define WM_CLOSE        0x0010
#define WM_QUIT         0x0012
#define WM_ERASEBKGND   0x0014
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_RBUTTONDOWN  0x0204
#define WM_KEYDOWN      0x0100
#define WM_KEYUP        0x0101
#define WM_SYSKEYDOWN   0x0104
#define WM_SYSCOMMAND   0x0112
#define WM_DPICHANGED   0x02E0

#define VK_LBUTTON 0x01
#define VK_ESCAPE  0x1B
#define VK_RETURN  0x0D
#define VK_SPACE   0x20
#define VK_F11     0x7A

#define DIB_RGB_COLORS 0
#define BI_RGB         0
#define HALFTONE       4
#define SRCCOPY        0x00CC0020
#define TRANSPARENT    1
#define AC_SRC_OVER    0
#define AC_SRC_ALPHA   1
#define CS_HREDRAW     0x0002
#define CS_VREDRAW     0x0001
#define CS_OWNDC       0x0020
#define SWP_NOACTIVATE 0x0010
#define SWP_NOZORDER   0x0004
#define SC_MAXIMIZE    0xF030
/* ⚠️ 这两个必须是**指针**，不能是整数 0/1。
   引擎写的是 `LoadCursorW(NULL, cond ? IDC_HAND : IDC_ARROW)`，
   如果它们是 int，三元表达式的结果是 int，传给 `const wchar_t*` 形参会报
   -Wint-conversion。Windows 的做法正是 MAKEINTRESOURCE（把整数塞进指针）。 */
#define IDC_ARROW      ((const wchar_t *)0)
#define IDC_HAND       ((const wchar_t *)1)
#define SW_SHOW        5
#define DT_LEFT        0
#define DT_TOP         0
#define DT_NOCLIP      0x0100
#define FAILED(hr)     ((hr) < 0)

/* 自绘位图的像素格式：BGRA 且 alpha 预乘 —— 与引擎现有资产完全一致 */
enum { PLAT_PF_BGRA_PREMUL = 1 };

/* 平台初始化/收尾（由 main 或 WinMain 调用；iOS 侧用来建窗口与音频会话） */
int  platInit(int winW, int winH, const char *title);
void platShutdown(void);

/* 消息泵：桌面端是 PeekMessage 循环；iOS 侧返回 0（由 main 直接驱动帧） */
int  platPumpMessages(void);

/* 资源目录：把"assets 目录的宽字符完整路径（含结尾反斜杠/斜杠）"写进 out。
   桌面端 = exe 同级的 assets\；iOS = App Bundle 内的 assets/。
   ⚠️ 签名必须是宽字符：引擎全程用 wchar_t 路径（_wfopen、wsprintfW）。 */
void platAssetDir(wchar_t *out, int cap);

/* 键盘事件注入：让 iOS 的屏幕按钮能复用同一条按键处理逻辑 */
void platInjectKey(int vk, int isRepeat);

/* 引擎侧需要暴露给平台层的两个入口（iOS main 调用） */
#define PLAT_EXPORT_INIT_FRAME 1


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
#define OUT_TT_PRECIS     4
#define CLEARTYPE_QUALITY 5
#define FF_DONTCARE       0
#define RGN_DIFF          4
#define MCIERR_BASE   0
#define MAX_PATH      260
/* 调用约定：Windows 上是 __stdcall 之类；可移植侧清空即可。
   ⚠️ 必须定义 —— 否则 `static LRESULT CALLBACK WndProc(...)` 会因为
      "CALLBACK 未定义"变成语法错误（编译器报的是 `expected '=' before 'WndProc'`，
      提示信息完全指不到根因，很费时间）。 */
#define CALLBACK
#define WINAPI
#define APIENTRY
#define WINGDIAPI
#define STDCALL
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define MONITOR_DEFAULTTONEAREST 2
#define GWL_STYLE     (-16)
#define SW_MAXIMIZE   3
#define SW_RESTORE    9
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED_(hr)   ((hr) < 0)
typedef unsigned short COLOR16;
typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;
typedef struct {
    DWORD cbSize;                 /* ⚠️ 引擎会写 mi.cbSize —— 不能省 */
    RECT  rcMonitor, rcWork;
    DWORD dwFlags;
} MONITORINFO;
/* 窗口样式/位置常量：取 Windows 的真实值，便于将来对照 */
#define WS_POPUP        0x80000000L
#define WS_VISIBLE      0x10000000L
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define HWND_TOP        ((HWND)0)
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW   0x0040
#define SWP_NOACTIVATE_  0x0010
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

/* ---- 常用宏（取值与 windows.h 完全一致）----
   ⚠️ 这些**必须**自己定义：Windows 侧由 windows.h 提供，可移植侧没有。
      漏一个的症状是"编译报 implicit declaration"，但更麻烦的是
      如果哪里顺手写了个近似实现，颜色会静默错位（RGB 与 BGR 差一个字节顺序）。 */
#define RGB(r,g,b)   ((COLORREF)(((BYTE)(r)) | (((WORD)((BYTE)(g))) << 8) | (((DWORD)((BYTE)(b))) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)(((WORD)(c)) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))
#define LOWORD(l)    ((WORD)(((DWORD)(l)) & 0xFFFF))
#define HIWORD(l)    ((WORD)((((DWORD)(l)) >> 16) & 0xFFFF))
#define MAKEWORD(a,b) ((WORD)(((BYTE)(a)) | (((WORD)((BYTE)(b))) << 8)))
#define MAKELONG(a,b) ((LONG)(((WORD)(a)) | (((DWORD)((WORD)(b))) << 16)))
#define ZeroMemory(p,n)  memset((p), 0, (n))
#define CopyMemory(d,s,n) memcpy((d), (s), (n))

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
int      GetClipRgn(HDC hdc, HRGN rgn);
BOOL     Polyline(HDC hdc, const POINT *pts, int n);
int      GetRgnBox(HRGN rgn, RECT *rc);
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
void     GetLocalTime(SYSTEMTIME *st);
void     GetSystemTime(SYSTEMTIME *st);
SHORT    GetAsyncKeyState(int vk);
DWORD    GetTickCount(void);
void     Sleep(DWORD ms);

/* 杂项 */
int      wsprintfW(wchar_t *out, const wchar_t *fmt, ...);
#ifndef _WIN32
/* MinGW 的 stdio.h 已经带 dllimport 声明过 _wfopen；
   本机用 -DPLAT_PORTABLE 验证时若再声明一次，会报 -Wattributes 警告。 */
FILE    *_wfopen(const wchar_t *path, const wchar_t *mode);
#endif

/* ---- MSVCRT 专有函数（标准 C 与 musl 都没有）----
   ★ 这一组漏一个，本机（MinGW）也能链上 —— 因为 kernel32/msvcrt 会兜底；
     但 emcc 到 musl 上就会**链接失败**。属于"本地过、目标平台失败"的经典形态。
     清单由 `_check_web_symbols.py` 实测得出（nm 求差集），加进了 CI。
     ⚠️ 以后在 pvz.c 里用到任何 `_` 开头的 libc 风格函数，都要先跑一次那个脚本。

   ⚠️ 整组包在 `#ifndef _WIN32` 里：本机（MinGW）的 msvcrt 已经用 dllimport
      声明过这些符号，我们再声明一次只会产生 4 条 -Wattributes 警告，
      把真正的告警淹掉。而真正需要这份声明的只有可移植目标（emcc/musl）。
      —— 和上面 _wfopen 同一处理，同一个理由。 */
#ifndef _WIN32
int      _wcsicmp(const wchar_t *a, const wchar_t *b);
int      _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n);
int      _stricmp(const char *a, const char *b);
wchar_t *_wgetenv(const wchar_t *name);
int      _wputenv(const wchar_t *envstr);
#endif
BOOL     GetClientRect(HWND hwnd, RECT *rc);
/* ★ GdiFlush：GDI 会把绘制**批处理**在内部命令缓冲里，不一定立刻落到 DIB 位。
   任何"画完立刻读位图"的代码（抓帧、逐像素比对）都必须先调它，
   否则读到的是**陈旧或全零**的内存 —— 症状是"两边 100% 不同"，
   而且连最简单的底色填充都比不上，极易被误判成"混合公式写错了"。
   （引擎自己的 render() 里有 GdiFlush，所以 pvz.c 路径没暴露这个问题；
     是 _ab_prim.c 这种"纯原语"测试才踩到。） */
BOOL     GdiFlush(void);

void     SetCursor(HCURSOR c);
HCURSOR  LoadCursorW(HINSTANCE h, const wchar_t *name);
DWORD    GetModuleFileNameW(HINSTANCE h, wchar_t *out, DWORD n);
DWORD    GetEnvironmentVariableW(const wchar_t *name, wchar_t *out, DWORD n);
BOOL     SetWindowTextW(HWND hwnd, const wchar_t *s);

/* ---- 全屏 / 监视器 / 文件属性 ----
   这几组是 `toggleFullscreen` 与资源存在性判断用的，**全是 Windows 专有概念**。
   网页版由外壳用 Fullscreen API + fetch 替代，所以这里只提供"无害的失败值"：
     · GetFileAttributesW 返回 INVALID_FILE_ATTRIBUTES → 引擎认为"文件不存在"，
       走它既有的回退分支（例如缺图就退回程序化绘制）—— 正是我们想要的；
     · 全屏相关的全部空操作。 */
/* 宽/窄字符转换。引擎用它们拼接资源路径与存档路径。
   ⚠️ 网页版的路径全是 ASCII（"assets/rgplant_108.png"），
      所以按 Latin-1 直转即可 —— 不需要真的做 UTF-8 解码。
      但如果哪天有中文文件名，这里会静默截断，所以实现里带了长度校验。 */
int      MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int srcLen,
                             wchar_t *dst, int dstCap);
int      WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *src, int srcLen,
                             char *dst, int dstCap, const char *defCh, int *used);
#define CP_UTF8 65001
#define CP_ACP  0

/* 资源目录的注入入口（网页版由外壳设置；桌面端用 plat.h 里的内联实现） */
void     gameSetAssetDir(const char *dir);

DWORD    GetFileAttributesW(const wchar_t *path);
wchar_t *lstrcpyW(wchar_t *dst, const wchar_t *src);
wchar_t *lstrcatW(wchar_t *dst, const wchar_t *src);
int      lstrlenW(const wchar_t *s);
HWND     GetActiveWindow(void);
HWND     GetForegroundWindow(void);
BOOL     GetWindowRect(HWND hwnd, RECT *rc);
BOOL     MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint);
LONG     SetWindowLongW(HWND hwnd, int idx, LONG val);
LONG     GetWindowLongW(HWND hwnd, int idx);
typedef void *HMONITOR;
HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags);
BOOL     GetMonitorInfoW(HMONITOR mon, MONITORINFO *mi);
BOOL     Arc(HDC hdc, int l, int t, int r, int b, int sx, int sy, int ex, int ey);
BOOL     SetWindowPos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags);
HDC      BeginPaint(HWND hwnd, PAINTSTRUCT *ps);
BOOL     EndPaint(HWND hwnd, const PAINTSTRUCT *ps);
void     PostQuitMessage(int code);

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
/* 由 JS 侧注入字形位图：见 plat_web.c 的说明（文字用 Canvas2D 光栅一次后缓存）
   ★ 位图契约：**基线在第 px 行**（从位图顶往下数），px 是**设备**像素尺寸
     （= 逻辑字号 × 世界变换缩放）。引擎侧 curY 就是基线，贴图起点 = curY - px。 */
int  platWebGlyph(int cp, int px, int bold, const unsigned char *bgra,
                  int gw, int gh, int adv);
/* ★ 引擎 → 外壳的**反向**通道：取一条"待光栅化"的字符请求。
   引擎查不到字形时会登记，外壳来取并光栅化后回灌 platWebGlyph。
   为什么是拉不是推：引擎不知道哪些字符会被画出来（文案大量是运行时拼的），
   硬编码字符表必然会漏字。返回 0 = 暂时没有了。 */
int  platWebGlyphReq(int *cp, int *px, int *bold);
/* 已注入的字形数量（外壳可用于显示加载进度 / 自检） */
int  platWebGlyphCount(void);
/* 待光栅化队列长度，**不消费**（用 platWebGlyphReq 去探会把请求吞掉）。 */
int  platWebGlyphReqCount(void);
/* 诊断：实际命中字形并贴图的次数 / 落空次数。
   hit 恒为 0 → C 侧的查找或贴图坏了；hit 正常但画面无字 → JS 侧覆盖数据或位置。 */
int  platWebGlyphHit(void);
int  platWebGlyphMiss(void);
/* 把一帧的指针事件交给游戏：type 0=move 1=down 2=up 3=right-up */
void platWebPointer(int type, float x, float y);
void platWebKey(int vk, int down);
/* 每帧结束后外壳取一次像素（BGRA 预乘，尺寸 = 设备像素） */
const void *platWebFrame(int *w, int *h, int *stride);
void platWebResize(int cssW, int cssH, float dpr);

#endif  /* _WIN32 */
#endif  /* PLAT_H */
