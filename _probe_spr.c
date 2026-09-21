#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
void gameInitAll(void); void gameSetAssetDir(const char *d);
void gameStep(float dt, HDC out); const void *gameWorldBits(int*,int*,int*);
int platWebInit(int,int,const char*);
int gameStateId(void); int gameMenuBgOk(void);
const unsigned char *gameSpriteProbe(int which, int *w, int *h);
static void dump(const char *tag, int which, int fx, int fy){
    int w=0,h=0;
    const unsigned char *b = gameSpriteProbe(which,&w,&h);
    if(!b){ printf("    %-14s bits=NULL\n", tag); return; }
    if(fx<0) fx=w/2; if(fy<0) fy=h/2;
    const unsigned char *p = b + ((size_t)fy*w + fx)*4;
    printf("    %-14s %4dx%-5d @(%4d,%4d) BGR=(%3d,%3d,%3d) A=%3d\n",
           tag, w,h,fx,fy, p[0],p[1],p[2],p[3]);
}
int main(int argc,char**argv){
    int N=(argc>1)?atoi(argv[1]):60, i;
    _wputenv(L"PVZ_SAVE_FILE=_probe_spr.dat");
    gameSetAssetDir("assets"); platWebInit(1000,650,"spr"); gameInitAll();
    for(i=0;i<=N;i++){
        if(i==0||i==1||i==10||i==30||i==59||i==60||i==61||i==120||i==N){
            printf("  第 %3d 帧  state=%d menuBg.ok=%d\n", i, gameStateId(), gameMenuBgOk());
            dump("menuBg 天空", 0, 200, 60);
            dump("background 天空", 1, 200, 60);
            dump("btn_dark 中心", 2, -1, -1);
        }
        gameStep(1.0f/60.0f, GetDC(NULL));
    }
    return 0;
}
