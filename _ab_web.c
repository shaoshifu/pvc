/* 跨平台比对：**网页后端**侧的驱动（本机 gcc 编译）。
   与 _ab_win.c 共用 _ab_scenario.h —— 场景必须逐字相同。
   编译：见 _ab_compare.sh */
#include "plat.h"
#include "_ab_scenario.h"

int main(int argc, char **argv)
{
    const char *assetdir = (argc > 1) ? argv[1] : "assets";
    extern int gameArtCount(void); extern int gameStateId(void);
    extern int gameMenuBgOk(void); extern int gameLawnOk(void);
    const char *outpath  = (argc > 2) ? argv[2] : "_ab_web.raw";
    int frames           = (argc > 3) ? atoi(argv[3]) : 60;
    unsigned seed        = (argc > 4) ? (unsigned)strtoul(argv[4], NULL, 10) : 20260921u;
    int rc;

    if (!platWebInit(1000, 650, "pvz-web-ab")) { fprintf(stderr, "platWebInit 失败\n"); return 1; }
    rc = abRunScenario(assetdir, outpath, frames, seed, 1);
    if (rc) { fprintf(stderr, "场景失败 rc=%d\n", rc); return 1; }
    printf("  [web] 状态=%d 菜单底图=%d 草坪=%d 资源=%d\n", gameStateId(), gameMenuBgOk(), gameLawnOk(), gameArtCount());
    printf("  [web] 已导出 %s（%d 帧，seed=%u）\n", outpath, frames, seed);
    return 0;
}
