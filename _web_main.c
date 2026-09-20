/* 网页版 main —— 用来确认 `pvz.c + plat_web.c` 能**真正链接**成可执行文件，
   并在本机跑起来（这就是 P2 阶段的验证载体，不需要 Emscripten）。

   这不是最终交付的 web 入口（那个会由 plat_web.c 暴露给 JS，
   由 requestAnimationFrame 驱动），而是一个**本地验证外壳**：
     · 能链接 → 说明 56 个 Win32 符号一个不缺、签名都对得上；
     · 能跑 → 说明软件光栅后端真的在产出像素；
     · 导出世界层 → 可以和 Windows 构建做逐像素比对。

   编译：
     gcc -DPLAT_PORTABLE -O2 -I. -o _web_verify.exe _web_main.c plat_web.c pvz.c -lm
   运行：
     ./_web_verify.exe assets _web_out.raw [帧数]
*/
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gameInitAll(void);
void gameSetAssetDir(const char *dir);
void gameStep(float dt, HDC out);
const void *gameWorldBits(int *w, int *h, int *stride);

/* plat_web.c 提供 */
int  platWebInit(int w, int h, const char *title);

static int writeRaw(const char *path, const void *bits, int w, int h, int stride)
{
    FILE *f = fopen(path, "wb");
    int y;
    if (!f) return 0;
    /* 头部与 Windows 侧的 _test_render_baseline 保持一致（GLB1 + 宽高），
       这样同一个 Python 脚本能直接比对两个平台的产物。 */
    fwrite("GLB1", 1, 4, f);
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    for (y = 0; y < h; y++)
        fwrite((const unsigned char *)bits + (size_t)y * stride, 1, (size_t)stride, f);
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    const char *assetdir = (argc > 1) ? argv[1] : "assets";
    const char *outpath  = (argc > 2) ? argv[2] : "_web_out.raw";
    int frames           = (argc > 3) ? atoi(argv[3]) : 60;
    int i, w = 0, h = 0, stride = 0;
    const void *bits;

    /* 存档隔离：与本机真档分开，避免测试污染进度 */
    _wputenv(L"PVZ_SAVE_FILE=_web_verify_save.dat");

    /* 资源目录：走 pvz.c 暴露的入口（gAssetDir 是 static，外部拿不到） */
    gameSetAssetDir(assetdir);

    if (!platWebInit(1000, 650, "pvz-web")) {
        fprintf(stderr, "platWebInit 失败\n");
        return 1;
    }

    printf("== 网页版后端本地验证 ==\n");
    gameInitAll();                       /* createLayers + 字体 + 载图 + 开局 */
    printf("  开局完成，gState 已就绪\n");

    /* 跑若干帧。第一帧负责把所有素材解码进内存，之后是稳态帧。 */
    for (i = 0; i < frames; i++)
        gameStep(1.0f / 60.0f, GetDC(NULL));

    bits = gameWorldBits(&w, &h, &stride);
    printf("  世界层：%dx%d，行距 %d，指针 %s\n", w, h, stride, bits ? "非空" : "**空**");
    if (!bits) return 1;

    if (!writeRaw(outpath, bits, w, h, stride)) {
        fprintf(stderr, "写 %s 失败\n", outpath);
        return 1;
    }
    printf("  已导出 %s（%d 字节像素数据）\n", outpath, w * h * 4);
    printf("\nOK\n");
    return 0;
}
