/* 资源格式切换（BMP → PNG/JPEG）后的加载器正确性验证。
   ------------------------------------------------------------------------
   为什么要单独测：贴图加载错了**不会崩**，只会让画面悄悄变形 ——
   通道顺序搞反 = R/B 互换（草地变紫、僵尸变蓝），
   上下翻转搞反 = 所有单位倒立。这两种都不会报错，只有肉眼在实机里才看得出来。

   判据：
     · **PNG 路径**（带透明的精灵）：`spriteLoad` 产出的 bits 必须与源 BMP
       逐字节一致（只允许 BGRA↔RGBA 的字节交换，且已交换回来）。
     · **JPEG 路径**（整屏不透明背景）：有损，所以只保证
       尺寸一致 + alpha 全 255 + 平均色差很小。
   比较在 Python 侧做（它读 BMP 最方便），本程序只负责把 bits 原样倒出来。

   编译：
     gcc -O2 -o _test_asset_load.exe _test_asset_load.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
   用法：
     ./_test_asset_load.exe <输出目录>      # 产出 <name>.bin（原始 BGRA）
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static int fails;
#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

/* 代表性样本：两条加载路径 + 不同尺寸/透明度 */
static const wchar_t *SAMPLES[] = {
    L"lawn",                /* 1728x1000 不透明 → JPEG 路径 */
    L"background",          /* 2000x1300 不透明 → JPEG 路径 */
    L"lawn_primordial",     /* 始祖草坪，同样走 JPEG */
    L"rgplant_00",          /* 带 alpha 精灵 → PNG 路径 */
    L"rgplant_105",         /* 始祖三尊，PNG 路径 */
    L"rgzombie_00",         /* 肉鸽僵尸，PNG 路径 */
    L"rgzombie_00_dead0",   /* 倒地帧 */
    L"zombie_ashwalker",
    L"plant_51",
    L"tile_web",
    L"bullet_pea",
    L"ui_btn_blue_normal",  /* UI 按钮，带 alpha */
};

int main(int argc, char **argv)
{
    int i;
    /* argv[1] = 资源目录（要有 assets/ 下的贴图），argv[2] = bits 输出目录。
       两者不能混：第一版把 gAssetDir 指成了输出目录，9 个样本全部加载失败。 */
    const char *assetdir = (argc > 1) ? argv[1] : "assets";
    const char *outdir   = (argc > 2) ? argv[2] : ".";
    wchar_t wdir[MAX_PATH];

    _wputenv(L"PVZ_SAVE_FILE=_test_asset_load_save.dat");
    SetConsoleOutputCP(CP_UTF8);
    MultiByteToWideChar(CP_UTF8, 0, assetdir, -1, wdir, MAX_PATH);
    wsprintfW(gAssetDir, L"%s\\", wdir);
    /* gAssetDir 末尾必须是反斜杠：spriteLoadName 直接拼 "%s%s.png" */

    printf("== 用真实的 spriteLoad 加载 %d 个样本，把原始 bits 倒出来 ==\n",
           (int)(sizeof(SAMPLES) / sizeof(SAMPLES[0])));
    for (i = 0; i < (int)(sizeof(SAMPLES) / sizeof(SAMPLES[0])); i++) {
        Sprite sp;
        char name[128], path[512];
        FILE *fp;
        int ok = spriteLoadName(&sp, SAMPLES[i]);
        WideCharToMultiByte(CP_UTF8, 0, SAMPLES[i], -1, name, sizeof(name), NULL, NULL);
        printf("  %-14s load=%d  %dx%d  ok=%d\n", name, ok, sp.w, sp.h, sp.ok);
        CHECK(ok == 1 && sp.ok == 1 && sp.w > 0 && sp.h > 0, name);
        if (!ok || !sp.ok) continue;

        /* 自检：倒出去之前先确认 bits 不是全 0（否则"通过"毫无意义） */
        {
            const unsigned char *b = (const unsigned char *)sp.bits;
            long nz = 0, k, n = (long)sp.w * sp.h * 4;
            for (k = 0; k < n; k += 977) if (b[k]) nz++;
            CHECK(nz > 0, "sanity: pixels are non-zero (not an empty buffer)");
        }

        snprintf(path, sizeof(path), "%s/%s.bin", outdir, name);
        fp = fopen(path, "wb");
        if (!fp) { printf("  [!] 写不出 %s\n", path); fails++; continue; }
        fwrite(sp.bits, 1, (size_t)sp.w * (size_t)sp.h * 4, fp);
        fclose(fp);
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    remove("_test_asset_load_save.dat");
    return fails ? 1 : 0;
}
