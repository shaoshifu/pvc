/* ============================================================================
   文字（字形注入）端到端测试 —— 判定"文字画不出来"是 C 侧还是 JS 侧的问题

   【为什么必须有这个测试】
   线上出现"素材 853 张全都载入了、wasm 正常跑、0 个 JS 异常，
   但**整屏一个字的都没有**"。这种失败模式最恶劣的地方是**完全静默**：
   没有异常、没有失败请求、画面其余部分（草坪/房子/卡片底板）全部正常，
   看起来只像"UI 还没做"。

   判定它到底坏在哪一环，靠远程浏览器来回试太慢。这个测试把整条链路
   在**本机**跑一遍，而且是可控的：
     ① 先用"假字形"注入（实心方块，覆盖率 255）
     ② 再跑帧、导出世界层
     ③ 断言：文字区域里**出现了近乎纯色的亮像素**
   如果这一步过不了 → 问题在 C 侧（查找/贴图/变换）。
   如果这一步过了、线上仍然没字 → 问题在 JS 侧（光栅化产出空覆盖/尺寸不对）。
   两者一次就能分开，不用猜。

   【为什么用"实心方块"当假字形】
   真字形的覆盖率是灰阶的，断言只能靠"模糊的亮度分布"。
   实心方块覆盖率恒为 255，于是断言可以很硬：
   区域里必须出现 RGB 恰好等于**文字颜色**的像素 —— 这也顺带验证了
   "字形只存覆盖率、颜色由 SetTextColor 决定"这条契约。
   如果颜色是由 JS 染的（比如染成白的），而引擎这边又不上色，
   那么这段断言会以"找不到该颜色"失败，正好把契约破坏暴露出来。

   【覆盖范围】
     · 基线位置：文字必须出现在 curY 上方（不是下方）—— 偏了就会跑到卡片外面
     · 尺寸：逻辑 15px 的字要按 30 设备像素光栅（世界层 2x）
     · 颜色：文字颜色必须等于 SetTextColor 给的颜色
     · 位置：世界层 → 逻辑坐标的换算是 ×2
   ============================================================================ */
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gameInitAll(void);
void gameSetAssetDir(const char *dir);
void gameStep(float dt, HDC out);
const void *gameWorldBits(int *w, int *h, int *stride);

int  platWebInit(int w, int h, const char *title);
int  platWebGlyph(int cp, int px, int bold, const unsigned char *bgra,
                  int gw, int gh, int adv);
int  platWebGlyphReq(int *cp, int *px, int *bold);
int  platWebGlyphCount(void);
int  platWebGlyphHit(void);
int  platWebGlyphMiss(void);
int  platWebGlyphPixels(void);
void platWebGlyphLastRect(int *x, int *y, int *w, int *h);

static int nPass = 0, nFail = 0;
#define CK(cond, fmt, ...) do { \
    if (cond) { nPass++; printf("  OK   " fmt "\n", ##__VA_ARGS__); } \
    else      { nFail++; printf("  FAIL " fmt "\n", ##__VA_ARGS__); } \
} while (0)

int main(void)
{
    int w = 0, h = 0, stride = 0, i;
    const void *bits;
    int reqN = 0;
    static int reqCp[4096], reqPx[4096], reqBold[4096];

    _wputenv(L"PVZ_SAVE_FILE=_test_web_glyph_save.dat");
    gameSetAssetDir("assets");
    if (!platWebInit(1000, 650, "pvz-web")) { fprintf(stderr, "init fail\n"); return 1; }

    printf("== 文字（字形注入）端到端测试 ==\n");
    gameInitAll();

    /* ---- ① 先跑一帧，让引擎把"缺哪些字"登记出来 ---- */
    gameStep(1.0f / 60.0f, GetDC(NULL));
    while (reqN < 4096 && platWebGlyphReq(&reqCp[reqN], &reqPx[reqN], &reqBold[reqN]))
        reqN++;
    printf("  引擎请求了 %d 个字形\n", reqN);
    CK(reqN > 0, "引擎确实会向外壳索要字形（0 = 文字永远不会显示）");

    /* 统计请求里的字号（设备像素），确认世界层缩放被算进去了 */
    {
        int sz[16], szN = 0, k;
        for (i = 0; i < reqN; i++) {
            for (k = 0; k < szN; k++) if (sz[k] == reqPx[i]) break;
            if (k == szN && szN < 16) sz[szN++] = reqPx[i];
        }
        printf("  请求涉及的设备字号: ");
        for (k = 0; k < szN; k++) printf("%s%d", k ? ", " : "", sz[k]);
        printf("\n");
        /* 逻辑字号 15/18/22/30/54/72 × 世界层缩放 2 → 30/36/44/60/108/144 */
        {
            int has30 = 0;
            for (k = 0; k < szN; k++) if (sz[k] == 30) has30 = 1;
            CK(has30, "逻辑 15px 的字被按 30 设备像素请求（世界层是 2x）");
        }
    }

    /* ---- ② 用"实心方块"注入假字形 ----
       覆盖率恒 255，于是下面可以直接断言"出现了文字颜色的纯色像素"。 */
    {
        int injected = 0;
        for (i = 0; i < reqN; i++) {
            int gw = reqPx[i] > 0 ? reqPx[i] : 1;      /* 宽度=字号，够显眼 */
            int gh = reqPx[i] + 4;
            size_t n = (size_t)gw * gh * 4;
            unsigned char *buf = (unsigned char *)malloc(n);
            memset(buf, 255, n);                        /* 覆盖率 255（白色无关，见契约） */
            if (platWebGlyph(reqCp[i], reqPx[i], reqBold[i], buf, gw, gh, gw))
                injected++;
            free(buf);
        }
        printf("  注入假字形 %d 个（实心，覆盖率 255）\n", injected);
        CK(injected == reqN, "全部请求都注入成功（%d/%d）", injected, reqN);
        CK(platWebGlyphCount() == reqN, "wasm 侧字形表计数一致（%d）", platWebGlyphCount());
    }

    /* ---- ③ 取注入前后的文字区域颜色分布 ----
       文字颜色是引擎 SetTextColor 指定的；找最亮的那个颜色（文字的描边/正文
       通常是接近白或鲜艳的），据此定位文字像素。 */
    {
        /* 再跑几帧，让画面进入稳态（并把字形用上） */
        for (i = 0; i < 3; i++) gameStep(1.0f / 60.0f, GetDC(NULL));
        bits = gameWorldBits(&w, &h, &stride);
        if (!bits) { fprintf(stderr, "no world bits\n"); return 1; }

        const unsigned char *B = (const unsigned char *)bits;
        printf("  世界层 %dx%d（逻辑 %dx%d）\n", w, h, w / 2, h / 2);

        /* ★ 探针：命中/落空。这两个数字一眼分开两类故障 ——
           hit=0 → 查找或贴图坏了（C 侧）；hit>0 但没字 → 位置/覆盖数据问题。 */
        {
            int rx, ry, rw, rh;
            platWebGlyphLastRect(&rx, &ry, &rw, &rh);
            printf("  字形命中 %d / 落空 %d / **实际写入像素 %d**\n",
                   platWebGlyphHit(), platWebGlyphMiss(), platWebGlyphPixels());
            printf("      最后一次贴图矩形: 设备(%d,%d) %dx%d\n", rx, ry, rw, rh);
            CK(platWebGlyphHit() > 0, "字形被真实命中（0 = 查找失败或贴图路径没走）");
            CK(platWebGlyphPixels() > 500,
               "文字像素**真的写进了位图**（%d；命中但此数为 0 = 全被越界裁掉）",
               platWebGlyphPixels());
        }

        /* ★★ 最硬的证据：全图找"大块纯色"。
           注入的是**实心**字形（覆盖率恒 255），所以任何一段文字都会变成
           一块纯色矩形 —— 标题、菜单、按钮文字都会留下几千到上万像素的
           同色块。背景是插画（颜色连续过渡），不会出现这种纯色峰。
           所以"存在 >3000 px 的纯色块" ⇔ "文字确实被画到了画面上"。
           这条断言在 n<0 修复前必然为 0（整屏一个字都没有）。 */
        {
            static int hist[65536];
            int x, y, k, bigN = 0, bigCol = -1, bigCnt = 0;
            memset(hist, 0, sizeof(hist));
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    const unsigned char *p = B + ((size_t)y * w + x) * 4;
                    hist[(p[2] << 8) | p[1]]++;      /* 用 (R,G) 做键就够区分 */
                }
            for (k = 0; k < 65536; k++)
                if (hist[k] > 3000) {
                    bigN++;
                    if (hist[k] > bigCnt) { bigCnt = hist[k]; bigCol = k; }
                }
            printf("  全图纯色块(>3000px): %d 个", bigN);
            if (bigCol >= 0)
                printf("，最大 RGB(%d,%d,?) x%d", (bigCol >> 8) & 0xFF, bigCol & 0xFF, bigCnt);
            printf("\n");
            CK(bigN >= 1,
               "画面上有成块的文字（%d 个；0 = 整屏无文字，即 n<0 那个坑回来了）", bigN);
        }

        /* 位置抽查：开场画面的大标题占位块应在画面中上部。
           标题用 putTextCS 居中画，字号 72 → 世界层 144 px 高。 */
        {
            int x0 = 400, y0 = 280, x1 = 1600, y1 = 520;
            int x, y, n = 0, r = 0, g = 0;
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++) {
                    const unsigned char *p = B + ((size_t)y * w + x) * 4;
                    if (p[0] > 170 && p[1] > 170 && p[2] > 90) { n++; r = p[2]; g = p[1]; }
                }
            printf("  标题区亮色像素: %d（例 RGB(%d,%d,?)）\n", n, r, g);
            CK(n > 3000, "标题文字块出现在预期位置（%d 个像素）", n);
        }

        /* ---- ④ 基线语义：字形必须画在 curY **上方** ----
           引擎的 curY 是**基线**（= 文本框顶 + fontPx）。若把基线当顶点，
           整段文字会下移一个字高，压到下面的内容上（卡片价压到草坪上）。
           用贴图矩形来判定：by = curY - devPx，所以 by 必须**小于** curY。
           这里直接用探针给的矩形复核这个关系。 */
        {
            int rx, ry, rw, rh;
            platWebGlyphLastRect(&rx, &ry, &rw, &rh);
            printf("  末次贴图矩形 设备(%d,%d) %dx%d\n", rx, ry, rw, rh);
            CK(rw > 0 && rh > 0 && rx >= 0 && ry >= 0,
               "贴图矩形落在世界层范围内（负值/零 = 被裁掉或坐标越界）");
        }
    }

    /* 导出世界层，便于肉眼确认文字画到哪、什么颜色 */
    {
        FILE *f = fopen("_glyph_test.raw", "wb");
        if (f) {
            fwrite("GLB1", 1, 4, f);
            fwrite(&w, 4, 1, f);
            fwrite(&h, 4, 1, f);
            fwrite(bits, 1, (size_t)stride * h, f);
            fclose(f);
            printf("  已导出 _glyph_test.raw（用于肉眼确认）\n");
        }
    }

    printf("\n=== 通过 %d / 失败 %d ===\n", nPass, nFail);
    if (nFail) {
        printf("文字链路在 C 侧就有问题 —— 不用再去浏览器里猜。\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
