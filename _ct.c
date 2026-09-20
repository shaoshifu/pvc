#define WinMain pvz_winmain_unused
#include "pvz.c"
#include <stdio.h>
/* MinGW 控制台里 printf("%ls") 常静默输出空串，所以自己转 UTF-8 再按窄串打。 */
static void pn(const wchar_t *w){
    char b[128];
    if (WideCharToMultiByte(CP_UTF8,0,w,-1,b,sizeof(b),NULL,NULL) <= 0) b[0] = 0;
    fputs(b, stdout);
}
int main(void){
    SetConsoleOutputCP(CP_UTF8);
    printf("PT_HERO_FLAME=%d  PT_COUNT=%d  PT_COLLECTIBLE=%d\n",
           (int)PT_HERO_FLAME,(int)PT_COUNT,(int)PT_COLLECTIBLE);
    printf("RG_PLANT_N=%d  RGQ_COUNT=%d  HERO_RARITY_COUNT=%d\n",
           (int)RG_PLANT_N,(int)RGQ_COUNT,(int)HERO_RARITY_COUNT);
    {
        int q;
        printf("RGQ_NAME: ");
        for (q = 0; q < RGQ_COUNT; q++) { printf("%d=", q); pn(RGQ_NAME[q]); printf(" "); }
        printf("\nRGQ_DMG : ");
        for (q = 0; q < RGQ_COUNT; q++) printf("%.1f ", RGQ_DMG[q]);
        printf("\nRGQ_W   : ");
        for (q = 0; q < RGQ_COUNT; q++) printf("%d ", RGQ_W[q]);
        printf("\nRGQ_TRAITN: ");
        for (q = 0; q < RGQ_COUNT; q++) printf("%d ", RGQ_TRAITN[q]);
        printf("\n");
    }
    return 0;
}
