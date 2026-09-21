#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
void gameInitAll(void); void gameSetAssetDir(const char *d);
void gameStep(float dt, HDC out);
int platWebInit(int,int,const char*);
int gameStateId(void);
void gameRenderCounters(int*,int*,int*,int*);
int main(int argc,char**argv){
    int N=(argc>1)?atoi(argv[1]):60, i, bg,blk,txt,blit, pb=-1;
    _wputenv(L"PVZ_SAVE_FILE=_probe_cnt.dat");
    gameSetAssetDir("assets"); platWebInit(1000,650,"cnt"); gameInitAll();
    for(i=1;i<=N;i++){
        gameStep(1.0f/60.0f, GetDC(NULL));
        gameRenderCounters(&bg,&blk,&txt,&blit);
        if(bg!=pb){ printf("  第%3d帧  bgRuns=%-4d menuBlock=%-4d menuText=%-4d menuBgBlit=%-4d state=%d\n",
                           i,bg,blk,txt,blit,gameStateId()); pb=bg; }
        if(i==10||i==60||i==120){ printf("  [核对] 第%3d帧 bg=%d blk=%d txt=%d blit=%d\n",i,bg,blk,txt,blit); }
    }
    return 0;
}
