/* =========================================================================
   _ab_scenario.h —— 跨平台渲染比对用的**共享场景**
   -------------------------------------------------------------------------
   目的：让"Windows 构建"与"网页后端（本机 gcc 编译）"跑**逐字相同**的
   初始化与帧序列，然后比对世界层的像素。

   为什么必须共享同一份代码（而不是各写一份）：
     两边只要有任何一处步骤差异（少调一次、顺序不同、dt 不同），
     比对就会失败，而失败原因看起来像"渲染实现不一致"——
     排查方向直接被带偏。这个项目里"手抄副本漂移"已经翻过三次车
     （artLoadAll / 回归旧二进制 / 测试手抄加载循环），
     所以比对场景**必须**是同一个 .h。

   确定性从三处保证：
     ① `gDailyMode=1` —— resetGame 默认用 `srand(time(NULL))` 播种，
        不切成每日模式的话每次开局都不一样，比对根本做不了；
     ② 固定 `dt` —— 不读墙钟，帧序列完全可复现；
     ③ 不注入任何输入 —— 没有点击就没有分支差异。

   输出格式与 _test_render_baseline.c 一致：GLB1 + 宽 + 高 + BGRA 像素。
   ========================================================================= */
#ifndef AB_SCENARIO_H
#define AB_SCENARIO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 引擎侧入口（pvz.c 提供） */
void  gameSetAssetDir(const char *dir);
void  gameInitAll(void);
void  gameStep(float dt, HDC out);
const void *gameWorldBits(int *w, int *h, int *stride);
void  gameSetDeterministic(unsigned seed);

static int abWriteRaw(const char *path, const void *bits, int w, int h, int stride)
{
    FILE *f = fopen(path, "wb");
    int y;
    if (!f) return 0;
    fwrite("GLB1", 1, 4, f);
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    for (y = 0; y < h; y++)
        fwrite((const unsigned char *)bits + (size_t)y * stride, 1, (size_t)stride, f);
    fclose(f);
    return 1;
}

/* 跑固定场景，导出世界层。返回 0 表示成功。
   `frames` 与 `seed` 由调用方给，两端必须一致。 */
static int abRunScenario(const char *assetdir, const char *outpath,
                         int frames, unsigned seed, int doFrames)
{
    HDC out = NULL;
    int i, w = 0, h = 0, stride = 0;
    const void *bits;

    /* 存档隔离：绝不能碰本机真档（pvz_save.dat） */
    _wputenv(L"PVZ_SAVE_FILE=_ab_scenario_save.dat");

    gameSetAssetDir(assetdir);

    /* ★ 固定随机性：必须在 resetGame 之前设好（resetGame 内部会 srand） */
    gameSetDeterministic(seed);

    gameInitAll();          /* createLayers + 字体 + 载图 + resetGame + ST_MENU */

    if (doFrames) {
        out = GetDC(NULL);
        for (i = 0; i < frames; i++)
            gameStep(1.0f / 60.0f, out);
    }

    bits = gameWorldBits(&w, &h, &stride);
    if (!bits || w <= 0 || h <= 0) return 1;
    if (!abWriteRaw(outpath, bits, w, h, stride)) return 2;
    return 0;
}

#endif  /* AB_SCENARIO_H */
