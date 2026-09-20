/* 渲染基线：把"一帧画面"变成可逐像素比对的字节流。
   ------------------------------------------------------------------------
   为什么需要它（而不是拿两次抓帧比）：
     抓帧链路的输入注入是按**墙钟**计时的（Sleep + SendMessage），
     两次运行里"按键落在第几帧"根本不同；dt 也来自 GetTickCount。
     实测直接比两次抓帧会得到"96% 一致、4% 像素不同、连 sun 都对不上"的
     假差异 —— 那不是回归，是两次运行时间不同步。

   本测试把时间因素全部消掉：
     · srand(固定种子) —— rnd() 完全可复现
     · 场景手工摆好（种植物 / 放僵尸 / 设状态）—— 不依赖任何输入时序
     · 不推进游戏逻辑，只调用一次 render()
     · 直接把 gWorldDC 的像素倒出来

   于是"同一份源码在不同构建/不同平台下渲染是否一致"可以被**逐像素证明**。
   用法：
     ./_test_render_baseline.exe <资源目录> <输出文件>
   比对：
     两个构建各跑一次，把两个 .bin 按 GLB 头里的 w/h 切成 BGRA 逐字节比。

   编译：
     gcc -O2 -o _test_render_baseline.exe _test_render_baseline.c \
         -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int fails;
#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

#define SEED 20260920u

int main(int argc, char **argv)
{
    const char *assetdir = (argc > 1) ? argv[1] : "assets";
    const char *outpath  = (argc > 2) ? argv[2] : "render_baseline.bin";
    wchar_t wdir[MAX_PATH];
    FILE *fp;
    int i;

    SetConsoleOutputCP(CP_UTF8);
    _wputenv(L"PVZ_SAVE_FILE=_test_render_baseline_save.dat");

    MultiByteToWideChar(CP_UTF8, 0, assetdir, -1, wdir, MAX_PATH);
    wsprintfW(gAssetDir, L"%s\\", wdir);

    /* ---- 先建三层离屏缓冲：没有它 gWorldBits 是 NULL，
           倒出来的文件只有 12 字节头 —— 那就成了"两个空文件互相比较"的假通过。
           传 NULL 即可（GetDC(NULL) 会拿到屏幕兼容 DC）。

           ⚠️ 下面这段是"内联版"的同一份逻辑，存在的唯一理由是：
           本测试要能同时编译在**平台层重构前后**的 pvz.c 上，才能逐像素证明
           "重构没改变渲染"。老版本没有 createLayers()，所以必须自带一份。
           新版本走 createLayers()，保证测的是产品代码本身。
           两条路建出来的缓冲区规格完全一致（VIEW_W*SS，负高度顶向 DIB）。 ---- */
#ifdef PLAT_OLD_SOURCE_TEST
    {
        BITMAPINFO bi;
        gScreenDC = GetDC(NULL);
        gFrameDC  = CreateCompatibleDC(gScreenDC);
        gFrameBM  = CreateCompatibleBitmap(gScreenDC, VIEW_W, VIEW_H);
        SelectObject(gFrameDC, gFrameBM);
        gWorldDC  = CreateCompatibleDC(gScreenDC);
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = VIEW_W * SS;
        bi.bmiHeader.biHeight      = -(VIEW_H * SS);
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        gWorldBM = CreateDIBSection(gScreenDC, &bi, DIB_RGB_COLORS, &gWorldBits, NULL, 0);
        SelectObject(gWorldDC, gWorldBM);
        SetGraphicsMode(gWorldDC, GM_ADVANCED);
        gScaleXF.eM11 = (FLOAT)SS; gScaleXF.eM12 = 0;
        gScaleXF.eM21 = 0;         gScaleXF.eM22 = (FLOAT)SS;
        gScaleXF.eDx  = 0;         gScaleXF.eDy  = 0;
        gIdentXF = gScaleXF;
        gCurXF   = gScaleXF;
        SetWorldTransform(gWorldDC, &gScaleXF);
    }
#else
    createLayers(NULL);
#endif

    /* ---- 固定种子 + 固定开局：这是"可复现"的全部前提 ---- */
    srand(SEED);
    gCurLevel = 0;
    resetGame();
    srand(SEED);          /* resetGame 内部会用 time(NULL) 重新播种，必须再压一次 */

    /* ---- 手工摆一个确定的场景：植物 / 僵尸 / 资源 / 界面状态 ---- */
    {
        static const int P[6][3] = {
            { 0, 0, PT_PEASHOOTER }, { 1, 1, PT_SUNFLOWER },
            { 2, 2, PT_WALLNUT },    { 3, 3, PT_CACTUS },
            { 4, 4, PT_MELONPULT },  { 2, 6, PT_SPIKEWEED },
        };
        for (i = 0; i < 6; i++) {
            gPendingRg = -1;
            plantIt(P[i][0], P[i][1], P[i][2]);
        }
        /* 肉鸽植物：覆盖高品级贴图与能力位 */
        gPendingRg = 105; plantIt(0, 4, (int)rgPlants[105].base); gPendingRg = -1;
        gPendingRg = 100; plantIt(4, 2, (int)rgPlants[100].base); gPendingRg = -1;
    }
    {
        static const int Z[5][3] = {
            { 0, ZT_NORMAL, 900 }, { 1, ZT_CONE, 1020 }, { 2, ZT_BUCKET, 1140 },
            { 3, ZT_FOOTBALL, 1260 }, { 4, ZT_GIANT, 1380 },
        };
        for (i = 0; i < 5; i++) {
            gPendingRgZ = -1;
            spawnZombieAt(Z[i][1], Z[i][0], (float)Z[i][2]);
        }
        /* 一只肉鸽僵尸：覆盖它的贴图路径 */
        gPendingRgZ = 5;  spawnZombieAt((int)rgZombies[5].base, 2, 1320.0f); gPendingRgZ = -1;
        gPendingRgZ = 66; spawnZombieAt((int)rgZombies[66].base, 1, 1440.0f); gPendingRgZ = -1;
    }
    /* 阳光 / 金气 / 资源栏 / 波次条 / 选卡态 */
    gSun = 1234; gGold = 7; gKillStack = 3;
    gWave = 7;
    gSel = 0; gShovel = 0;
    gState = ST_PLAY;
    gTime = 12.5f;                 /* 固定时刻：粒子/脉冲相位随之固定 */
    gWaveStallT = 3.25f;

    /* ---- 只渲染，不推进逻辑 ---- */
    render(gWorldDC);

    /* ---- 倒出 gWorldDC 的像素 ---- */
    fp = fopen(outpath, "wb");
    if (!fp) { printf("[!] 写不出 %s\n", outpath); fails++; return 1; }
    {
        /* 头：魔数 + 宽 + 高，方便比对脚本切分 */
        const char magic[4] = { 'G', 'L', 'B', '1' };
        int w = VIEW_W * SS, h = VIEW_H * SS;
        fwrite(magic, 1, 4, fp);
        fwrite(&w, 4, 1, fp);
        fwrite(&h, 4, 1, fp);
        fwrite(gWorldBits, 1, (size_t)w * (size_t)h * 4, fp);
        printf("  输出 %s：%dx%d BGRA，%zu 字节\n",
               outpath, w, h, (size_t)w * (size_t)h * 4 + 12);
        CHECK(w > 0 && h > 0, "render: produced a non-empty canvas");
        /* ★ 必须断言"真的有像素"，不能只看尺寸算术 ——
           上一版就是只报了计算出来的字节数，而 gWorldBits 为 NULL、
           fwrite 一个字节都没写，两次运行"逐字节一致"纯属两个空文件相等。 */
        CHECK(gWorldBits != NULL, "sanity: world layer was really allocated");
        CHECK(ftell(fp) == (long)((size_t)w * (size_t)h * 4 + 12),
              "sanity: the pixel payload was really written");
    }
    fclose(fp);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_render_baseline_save.dat");
    return fails ? 1 : 0;
}
