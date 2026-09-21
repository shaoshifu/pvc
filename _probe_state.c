#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
void gameInitAll(void); void gameSetAssetDir(const char *d);
void gameStep(float dt, HDC out); const void *gameWorldBits(int*,int*,int*);
int platWebInit(int,int,const char*);
int gameStateId(void); int gameMenuBgOk(void);
static const char *SN(int s){ static const char *n[]={
  "ST_MENU","ST_PLAY","ST_PAUSE","ST_WIN","ST_LOSE","ST_DRAFT","ST_REWARD",
  "ST_LEVELS","ST_LOADOUT","ST_TALENTS","ST_ACH","ST_GACHA","ST_DAILY" };
  return (s>=0 && s<13)? n[s] : "?"; }
int main(int argc,char**argv){
    int N=(argc>1)?atoi(argv[1]):60, i, last=-1, w=0,h=0,st=0;
    const void *bits;
    _wputenv(L"PVZ_SAVE_FILE=_probe_state.dat");
    gameSetAssetDir("assets"); platWebInit(1000,650,"st"); gameInitAll();
    printf("  menuBg.ok=%d background.ok=%d  初始状态=%s\n",
           gameMenuBgOk(), -1, SN(gameStateId()));
    for(i=0;i<N;i++){
        gameStep(1.0f/60.0f, GetDC(NULL));
        if (gameStateId()!=last){ last=gameStateId();
            printf("    第 %3d 帧后 → gState=%s (%d)\n", i+1, SN(last), last); }
    }
    bits=gameWorldBits(&w,&h,&st);
    printf("  最终 gState=%s  世界层 %dx%d\n", SN(gameStateId()), w, h);
    return 0;
}
