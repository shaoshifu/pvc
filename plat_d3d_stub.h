/* =========================================================================
   plat_d3d_stub.h —— 可移植侧（网页 / iOS）的 Direct3D9 桩头
   -------------------------------------------------------------------------
   为什么不直接把 D3D 那段代码从 pvz.c 里删掉或条件编译掉：
     那是一段 200 行的设备创建/纹理上传/呈现代码，切割它要动 6 个函数，
     每切一处都有引错括号的风险（这个项目里已经因为类似操作翻过车）。
     而**留一个"设备创建必然失败"的桩**更省事、更安全：

       Direct3DCreate9() 返回 NULL
         → gpuInit() 失败
         → gEngine 走**既有的** GDI 回退路径（gFrameDC → BitBlt → 呈现）

     这条回退路径本来就是引擎为了"远程桌面 / 旧驱动 / 设备丢失"准备的，
     久经使用（`PVZ_GDI=1` 就是强制走它）。网页版要的正是它 ——
     因为网页版根本不该碰 D3D：画面靠软件光栅 + canvas 上传。

   所以这个头文件里所有函数都是"失败/空操作"，唯一有意义的返回值是
   Direct3DCreate9 的 NULL。
   ========================================================================= */
#ifndef PLAT_D3D_STUB_H
#define PLAT_D3D_STUB_H

#include <stddef.h>

/* ---- 常量：取值随意，只要自洽（永远不会真的传给驱动）---- */
#define D3D_SDK_VERSION            32
#define D3DADAPTER_DEFAULT         0
#define D3DDEVTYPE_HAL             1
#define D3DCREATE_FPU_PRESERVE     0x00000002L
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040L
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020L
#define D3DERR_DEVICELOST          ((long)0x88760868)
#define D3DERR_DEVICENOTRESET      ((long)0x88760869)
#define D3D_OK                     0
#define D3DFMT_UNKNOWN             0
#define D3DFMT_X8R8G8B8            22
#define D3DPOOL_DEFAULT            0
#define D3DUSAGE_DYNAMIC           0x00000200L
#define D3DLOCK_DISCARD            0x00002000L
#define D3DSWAPEFFECT_DISCARD      1
#define D3DPRESENT_INTERVAL_ONE    0x00000001L
#define D3DCLEAR_TARGET            0x00000001L
#define D3DCULL_NONE               1
#define D3DRS_ZENABLE              7
#define D3DRS_LIGHTING             137
#define D3DRS_CULLMODE             22
#define D3DSAMP_MINFILTER          5
#define D3DSAMP_MAGFILTER          6
#define D3DSAMP_ADDRESSU           1
#define D3DSAMP_ADDRESSV           2
#define D3DTEXF_LINEAR             2
#define D3DTADDRESS_CLAMP          3
#define D3DTSS_COLOROP             1
#define D3DTSS_COLORARG1           2
#define D3DTSS_ALPHAOP             4
#define D3DTOP_DISABLE             1
#define D3DTOP_SELECTARG1          2
#define D3DTA_TEXTURE              2
#define D3DPT_TRIANGLESTRIP        5
#define D3DFVF_XYZRHW              0x004
#define D3DFVF_TEX1                0x100
#define D3DCOLOR_XRGB(r,g,b)       ((unsigned long)(0xFF000000u | ((r) << 16) | ((g) << 8) | (b)))

/* ---- 类型：不透明指针 + 两个结构体（成员被 pvz.c 直接赋值）---- */
/* ⚠️ 这三行必须是"不透明**结构体**"，不能写成"指向结构体的指针"。
   pvz.c 里写的是 `static IDirect3D9 *gD3D;` —— 真实 d3d9.h 里
   IDirect3D9 就是结构体本身（LPDIRECT3D9 才是指针），
   所以 `IDirect3D9 *` 是"指向接口的指针"。若这里 typedef 成指针，
   `IDirect3D9 *` 会变成二级指针，赋值立刻报 incompatible pointer type。 */
typedef struct PlatD3D9        IDirect3D9;
typedef struct PlatD3DDevice9  IDirect3DDevice9;
typedef struct PlatD3DBaseTex9 IDirect3DBaseTexture9;
typedef struct PlatD3DBaseTex9 IDirect3DTexture9;

typedef struct {
    unsigned int  Width;
    unsigned int  Height;
    long          Pitch;
    void         *pBits;
} D3DLOCKED_RECT;

typedef struct {
    unsigned int  BackBufferWidth;
    unsigned int  BackBufferHeight;
    int           BackBufferFormat;
    unsigned int  BackBufferCount;
    int           MultiSampleType;
    unsigned int  MultiSampleQuality;
    int           SwapEffect;
    void         *hDeviceWindow;
    int           Windowed;
    int           EnableAutoDepthStencil;
    int           AutoDepthStencilFormat;
    unsigned long Flags;
    unsigned int  FullScreen_RefreshRateInHz;
    unsigned int  PresentationInterval;
} D3DPRESENT_PARAMETERS;

/* ---- 唯一有意义的函数：返回 NULL 让引擎走 GDI 回退 ---- */
static inline IDirect3D9 *Direct3DCreate9(unsigned int sdk)
{
    (void)sdk;
    return NULL;      /* ★ 就是这一行让引擎走 GDI 回退 */
}

/* ⚠️ 用**可变参数桩函数**而不是 `((long)0x...)` 常量表达式：
   常量表达式不"使用"传入的参数，于是 gpuInit / gpuDrawFrame 里
   那些只为 D3D 调用而存在的局部变量（flags、D3DPRESENT_PARAMETERS 的副本等）
   会触发 -Wunused-but-set-variable / -Wunused-variable。
   可移植构建里这段是**死代码**，但我不想用 pragma 把告警压掉 ——
   压制会让"以后真的少用一个变量"也发现不了。
   桩函数接受任意个任意类型参数，参数即被使用，告警自然消失。 */
static inline long platD3DStubFail(long dummy, ...) { (void)dummy; return (long)0x8876086C; }
static inline void platD3DStubVoid(long dummy, ...) { (void)dummy; }

/* ---- COBJMACROS 风格的方法宏：全部失败/空操作 ----
   pvz.c 用了 `#define COBJMACROS` + `IDirect3DDevice9_Xxx(p, ...)` 的调用形式，
   这里按同名宏提供桩，就不用改 pvz.c 的任何一行调用。 */
#define IDirect3D9_Release(...) platD3DStubVoid(0, __VA_ARGS__)
#define IDirect3D9_CreateDevice(...) platD3DStubFail(0, __VA_ARGS__)   /* INVALIDCALL */
#define IDirect3DDevice9_Release(...) platD3DStubVoid(0, __VA_ARGS__)
#define IDirect3DDevice9_Reset(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_TestCooperativeLevel(...)     ((long)0x88760868)   /* DEVICELOST */
#define IDirect3DDevice9_CreateTexture(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_BeginScene(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_EndScene(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_Clear(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_SetTexture(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_SetTextureStageState(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_SetSamplerState(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_SetRenderState(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_SetFVF(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_DrawPrimitiveUP(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DDevice9_Present(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DTexture9_Release(...) platD3DStubVoid(0, __VA_ARGS__)
#define IDirect3DTexture9_LockRect(...) platD3DStubFail(0, __VA_ARGS__)
#define IDirect3DTexture9_UnlockRect(...) platD3DStubFail(0, __VA_ARGS__)

#endif  /* PLAT_D3D_STUB_H */
