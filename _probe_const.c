/* 探针：打印关键常量 + 指定植物 id 的定义。
   手数枚举容易 off-by-one，而 PT_COUNT / PT_HERO_FLAME 直接决定
   牌组、候选统计、存档数组长度三者的边界 —— 必须问编译器。 */
#include <stdio.h>
#define main pvz_main_unused
#include "pvz.c"
#undef main

int main(int argc, char **argv)
{
    int i;
    printf("PT_COUNT         = %d\n", PT_COUNT);
    printf("PT_HERO_FLAME    = %d\n", PT_HERO_FLAME);
    printf("gCandidateSlots  = %d\n", gCandidateSlots);
    printf("gCardSlots       = %d\n", gCardSlots);
    printf("sizeof(SaveData) = %d 字节 = %d int\n",
           (int)sizeof(SaveData), (int)(sizeof(SaveData) / 4));
    printf("loadout    偏移(int) = %d\n",
           (int)(((char *)gSave.loadout    - (char *)&gSave) / 4));
    printf("plantOwned 偏移(int) = %d\n",
           (int)(((char *)gSave.plantOwned - (char *)&gSave) / 4));
    printf("\n植物定义（用于核对卡片上显示的是什么）：\n");
    for (i = 1; i < argc; i++) {
        int id = atoi(argv[i]);
        if (id < 0 || id >= PT_COUNT) { printf("  id %d 越界\n", id); continue; }
        printf("  id %-3d cost=%-4d name=%ls\n",
               id, (int)plantCost(id), plantDefs[id].name);
    }
    return 0;
}
