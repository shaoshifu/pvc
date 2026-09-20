/* 跨平台比对：**Windows/GDI** 侧的驱动。
   与 _ab_web.c 共用 _ab_scenario.h —— 场景必须逐字相同。
   这一侧不需要 platWebInit（窗口层由 createLayers 自己建）。 */
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

/* ⚠️ Windows 侧**不要**链接 plat_web.c —— 它定义的是软件光栅版 GDI，
   与本机的真实 gdi32 会同名冲突。这一侧就是要用真 GDI，才形成对照。 */
#include "_ab_scenario.h"

int main(int argc, char **argv)
{
    const char *assetdir = (argc > 1) ? argv[1] : "assets";
    extern int gameArtCount(void); extern int gameStateId(void);
    extern int gameMenuBgOk(void); extern int gameLawnOk(void);
    const char *outpath  = (argc > 2) ? argv[2] : "_ab_win.raw";
    int frames           = (argc > 3) ? atoi(argv[3]) : 60;
    unsigned seed        = (argc > 4) ? (unsigned)strtoul(argv[4], NULL, 10) : 20260921u;
    int rc;
    SetConsoleOutputCP(CP_UTF8);
    rc = abRunScenario(assetdir, outpath, frames, seed, 1);
    if (rc) { fprintf(stderr, "场景失败 rc=%d\n", rc); return 1; }
    printf("  [win] 状态=%d 菜单底图=%d 草坪=%d 资源=%d\n", gameStateId(), gameMenuBgOk(), gameLawnOk(), gameArtCount());
    printf("  [win] 已导出 %s（%d 帧，seed=%u）\n", outpath, frames, seed);
    return 0;
}
