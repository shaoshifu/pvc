/* 新增 50 张卡的**实机视觉验证**：把新卡强行塞进三选一，渲染整屏倒出来。
   ------------------------------------------------------------------------
   为什么必须做这一步（纯数值测试不够）：
     ① 贴图命名 rgplant_108 … rgplant_157 是三位数，而加载循环写的是
        wsprintfW(nm, L"rgplant_%02d", k) —— %02d 是"最少两位"，
        108 会输出 "108"、8 会输出 "08"，恰好兼容。但这种"恰好"必须实测，
        不能靠读格式串推断。
     ② 抠图后的贴图在**草坪底色**上是什么样，只有合成到实际背景才看得出来
        （预乘 alpha 出错时不会报错，只会让角色发黑或发白）。
     ③ 卡片文字层要能容下始祖卡的长名字与新副标题。

   输出：_cardshot 目录下的 BMP（按 SS 的原生尺寸，逐卡一张）

   编译：
     gcc -O2 -o _test_cards50_visual.exe _test_cards50_visual.c \
         -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
   运行：
     ./_test_cards50_visual.exe assets _cardshot
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

#include <stdio.h>

static int fails;
#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

/* render(out) 的收尾会把画面呈现回 out，所以必须给一块独立 DC，
   否则世界层会被自己的 1x 缩略图覆盖（这是踩过的坑，见
   _test_primordial_buff.c 的注释）。 */
static HDC     gShotDC = NULL;
static HBITMAP gShotBM = NULL;

static void shotEnsure(void)
{
    if (gShotDC) return;
    gShotDC = CreateCompatibleDC(NULL);
    gShotBM = CreateCompatibleBitmap(gShotDC, VIEW_W, VIEW_H);
    SelectObject(gShotDC, gShotBM);
}

static void dumpBmp(const char *path)
{
    FILE *f = fopen(path, "wb");
    int y;
    unsigned char hdr[54];
    unsigned char *row;
    int W = VIEW_W * SS, H = VIEW_H * SS;
    unsigned int sz = (unsigned int)(54 + W * H * 4);
    if (!f) return;
    memset(hdr, 0, 54);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &sz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &W, 4);
    memcpy(hdr + 22, &H, 4);          /* 正高度：自下而上 */
    hdr[26] = 1; hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    row = (unsigned char *)malloc((size_t)W * 4);
    /* gWorldBits 是自上而下的 DIB（biHeight 为负），BMP 要自下而上 → 倒序写 */
    for (y = H - 1; y >= 0; y--) {
        memcpy(row, (unsigned char *)gWorldBits + (size_t)y * W * 4, (size_t)W * 4);
        fwrite(row, 1, (size_t)W * 4, f);
    }
    free(row);
    fclose(f);
}

/* 把三选一的内容换成指定的三张卡，然后渲染一帧 */
static void shotDraft(const char *outdir, const char *tag, int a, int b, int c)
{
    char path[512];
    wchar_t wdir[512];

    gDraft[0] = RG_CARD_BASE + a;
    gDraft[1] = RG_CARD_BASE + b;
    gDraft[2] = RG_CARD_BASE + c;
    /* 悬停中间那张：让卡面高亮态也进入截图（高亮路径与常态是两套绘制分支） */
    gDraftHover = 1;
    gState = ST_DRAFT;

    /* ★ 必须把翻牌动画拨到"全部展开"。
       flipK() = |2t-1|，t 由 gScreenT 推出来：
         t=0（gScreenT 默认值 0）→ flipK=1 → 卡片是**侧立**的，画出来是一条线；
         实测症状就是"三张卡的贴图缩成几个像素点、卡面底板干脆没画"，
         而且文字照常显示 —— 很容易误判成贴图坏了。
       要三张都 t>=1：gScreenT >= FLIP_DUR + (DRAFT_N-1)*FLIP_LEAD。
       多加 0.5s 余量，落在"翻完但还没自动进入选卡"的窗口里。 */
    gScreenT = FLIP_DUR + (float)(DRAFT_N - 1) * FLIP_LEAD + 0.5f;

    /* ★ 必须把翻牌动画拨到"全部展开"。
       flipK() = |2t-1|，t 由 gScreenT 推出来：
         t=0（gScreenT 默认值 0）→ flipK=1 → 卡片是**侧立**的，画出来是一条线；
         实测症状就是"三张卡的贴图缩成几个像素点、卡面底板干脆没画"，
         而且文字照常显示 —— 很容易误判成贴图坏了。
       要三张都 t>=1：gScreenT >= FLIP_DUR + (DRAFT_N-1)*FLIP_LEAD。
       多加 0.5s 余量，落在"翻完但还没自动进入选卡"的窗口里。 */
    gScreenT = FLIP_DUR + (float)(DRAFT_N - 1) * FLIP_LEAD + 0.5f;

    shotEnsure();
    render(gShotDC);

    MultiByteToWideChar(CP_UTF8, 0, outdir, -1, wdir, 512);
    _snprintf(path, sizeof(path), "%s\\%s.bmp", outdir, tag);
    dumpBmp(path);
    printf("  截图 %-16s ← P%d / P%d / P%d\n", tag, a, b, c);
}

int main(int argc, char **argv)
{
    const char *assetdir = (argc > 1) ? argv[1] : "assets";
    const char *outdir   = (argc > 2) ? argv[2] : ".";
    wchar_t wdir[MAX_PATH];

    SetConsoleOutputCP(CP_UTF8);
    _wputenv(L"PVZ_SAVE_FILE=_cardshot.dat");

    MultiByteToWideChar(CP_UTF8, 0, assetdir, -1, wdir, MAX_PATH);
    wsprintfW(gAssetDir, L"%s\\", wdir);

    createLayers(NULL);
    srand(20260921u);
    resetGame();

    /* ★ 必须用 artLoadAll()，**不要**手抄一份加载循环。
       踩过：这个测试原来手抄了 `rgplant_%02d` 那段循环，看起来"加载成功了"，
       但漏掉了徽章/光效/其它所有素材 —— 于是卡片上的档位徽章静默不显示，
       而测试依然全绿、输出的截图看不出任何异常。
       artLoadAll() 就是为这件事抽出来的（见它在 pvz.c 里的注释）。 */
    artLoadAll();

    printf("== 新卡贴图加载检查 ==\n");
    /* 顺带核对徽章与光效也真的加载到了 ——
       上一版这里只查了植物贴图，于是"徽章没加载"完全逃过检查。 */
    {
        int nb = 0, nf = 0, k;
        for (k = 0; k < RGQ_COUNT; k++) if (gSprBadge[k].ok) nb++;
        for (k = 0; k < FSX_N; k++)      if (gSprFx[k].ok)    nf++;
        printf("  徽章 %d/%d，光效 %d/%d\n", nb, RGQ_COUNT, nf, FSX_N);
        CHECK(nb == RGQ_COUNT, "assets: all tier badges loaded (badge_common..primordial)");
        CHECK(nf == FSX_N, "assets: all celebration fx loaded (fx_rays..fx_runes)");
    }
    {
        int missNew = 0, missOld = 0, i;
        for (i = 0; i < 108; i++)  if (!gSprRgPlant[i].bmp) missOld++;
        for (i = 108; i < RG_PLANT_N; i++) if (!gSprRgPlant[i].bmp) missNew++;
        printf("  旧卡(0..107) 缺图 %d / 108；新卡(108..157) 缺图 %d / 50\n", missOld, missNew);
        CHECK(missNew == 0, "assets: all 50 new plant sprites loaded (rgplant_108..157)");
        CHECK(gSprRgPlant[108].bmp != NULL && gSprRgPlant[157].bmp != NULL,
              "assets: first and last new sprite both present");
        /* 尺寸合理：抠图后应小于上限、大于 0，且不是"整块空白" */
        for (i = 108; i < RG_PLANT_N; i++) {
            Sprite *s = &gSprRgPlant[i];
            if (!s->bmp || s->w < 20 || s->h < 20 || s->w > 512 || s->h > 512) {
                printf("FAIL: P%d 尺寸异常 %dx%d\n", i, s->w, s->h);
                fails++;
            }
        }
        CHECK(1, "assets: new sprite dimensions all within 20..512");
    }

    printf("\n== 三选一实机截图 ==\n");
    /* ① 两张殿堂 + 一张始祖：日常最可能出现的组合 */
    shotDraft(outdir, "draft_mix", 108, 128, 113);
    /* ② 三张始祖：最强组合，顺便看庆典是否同时触发 */
    shotDraft(outdir, "draft_allprim", 128, 138, 157);
    /* ③ 殿堂 + 始祖 + 成长卡的常规波次形态 */
    shotDraft(outdir, "draft_oneplant", 100, 120, 149);
    /* ④ 长名字的始祖（看卡片文字层会不会溢出） */
    shotDraft(outdir, "draft_longname", 148, 152, 155);

    printf("\n== 图集：把新卡逐个渲染到草坪上（抠图质量肉眼验收）==\n");
    {
        int i;
        char path[512];
        /* 直接画在草坪上：不经过卡片 UI，纯粹看预乘 alpha 与尺寸 */
        for (i = 0; i < 50; i++) {
            int idx = 108 + i;
            int col = i % 5, row = i / 5;
            float x = 120.0f + col * 105.0f;
            float y = 150.0f + row * 150.0f;
            const RgPlantDef *d = &rgPlants[idx];
            /* 先清成草坪绿，再贴一个单位 */
            fillRect(gWorldDC, 0, 0, VIEW_W, VIEW_H, RGB(34, 78, 40));
            spriteBlit(gWorldDC, &gSprRgPlant[idx], x, y, 1.0f, 1.0f, -1);
            (void)d;
            _snprintf(path, sizeof(path), "%s\\unit_%03d.bmp", outdir, idx);
            shotEnsure();
            render(gShotDC);           /* 走完整呈现路径，验证贴合 */
            MultiByteToWideChar(CP_UTF8, 0, outdir, -1, wdir, MAX_PATH);
            dumpBmp(path);
        }
        printf("  已输出 50 张 unit_108..157.bmp\n");
    }

    printf("\n%s（失败 %d）\n", fails ? "★ 有失败项" : "全部通过", fails);
    return fails ? 1 : 0;
}
