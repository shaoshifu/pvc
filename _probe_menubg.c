#include "plat.h"
#include <stdio.h>
void gameInitAll(void); void gameSetAssetDir(const char *d);
void gameStep(float dt, HDC out); const void *gameWorldBits(int*,int*,int*);
int platWebInit(int,int,const char*);
int gameMenuBgOk(void);
extern int gameStateId_(void) __attribute__((weak));
int main(void){
    _wputenv(L"PVZ_SAVE_FILE=_probe_menubg.dat");
    gameSetAssetDir("assets");
    platWebInit(1000,650,"probe");
    gameInitAll();
    for (int i=0;i<3;i++) gameStep(1.0f/60.0f, GetDC(NULL));
    printf("  gSprMenuBg.ok = %d   (0 = 菜单底图没加载 → 回退纯色)\n", gameMenuBgOk());
    return 0;
}
