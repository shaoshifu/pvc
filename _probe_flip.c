/* 定位"菜单背景从 ui_menu_bg_v1（夜景）变成 background（白天）"发生在第几帧。
   做法：跑不同帧数，各导出一份世界层，然后比较天空区域的像素。
   本机跑既快又可复现，比在浏览器里反复试有效得多。 */
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
void gameInitAll(void); void gameSetAssetDir(const char *d);
void gameStep(float dt, HDC out); const void *gameWorldBits(int*,int*,int*);
int platWebInit(int,int,const char*);
int gameMenuBgOk(void);
int main(int argc, char **argv){
    int frames = (argc>1)? atoi(argv[1]) : 1;
    const char *out = (argc>2)? argv[2] : "_flip.raw";
    int w=0,h=0,s=0,i;
    const void *bits;
    _wputenv(L"PVZ_SAVE_FILE=_probe_flip.dat");
    gameSetAssetDir("assets");
    platWebInit(1000,650,"flip");
    gameInitAll();
    for (i=0;i<frames;i++) gameStep(1.0f/60.0f, GetDC(NULL));
    bits = gameWorldBits(&w,&h,&s);
    printf("  frames=%-4d  menuBg.ok=%d  ", frames, gameMenuBgOk());
    if (bits) {
        const unsigned char *B = (const unsigned char *)bits;
        /* 天空采样点：world (200,60) 与 (1800,60)，以及按钮区 (300,700) */
        const unsigned char *p1 = B + ((size_t)60*w + 200)*4;
        const unsigned char *p2 = B + ((size_t)60*w + 1800)*4;
        const unsigned char *p3 = B + ((size_t)700*w + 300)*4;
        printf("天空L=(%d,%d,%d) 天空R=(%d,%d,%d) 按钮区=(%d,%d,%d)\n",
               p1[2],p1[1],p1[0], p2[2],p2[1],p2[0], p3[2],p3[1],p3[0]);
        FILE *f=fopen(out,"wb");
        if(f){ fwrite("GLB1",1,4,f); fwrite(&w,4,1,f); fwrite(&h,4,1,f);
               fwrite(bits,1,(size_t)s*h,f); fclose(f); }
    } else printf("bits=NULL\n");
    return 0;
}
