/* 真存档加载校验：把玩家真档**副本**喂给 saveLoad()，确认能正确读出，
   并观察「只能选 1 株」的收敛是否按预期发生。

   为什么必须用副本：saveLoad() 末尾在收敛后会 saveFlush() 落盘，
   直接指真档会在校验的同时把它改掉 —— 这类"校验顺手写盘"正是
   今天真档被清成新档的同一类原因，不能再犯。

   编译：
     gcc -O2 -o _check_save.exe _check_save.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm
   用法（先自己拷一份副本）：
     cp pvz_save.dat _chk_save.dat && ./_check_save.exe
*/
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

int main(void)
{
    int i, owned = 0, unlocked = 0, stars = 0, picks = 0;
    char nb[128];

    /* MinGW 控制台里 printf("%ls") 静默输出空串，自己转 UTF-8 再打。 */
    SetConsoleOutputCP(CP_UTF8);

    /* 只读副本：绝不指向 pvz_save.dat */
    _wputenv(L"PVZ_SAVE_FILE=_chk_save.dat");

    memset(&gSave, 0, sizeof(gSave));
    saveLoad();
    /* 必须镜像真实启动路径：saveLoad() 之后紧跟 loadoutNormalize()。
       收敛刻意不放在 saveLoad 内部（它有多条 return 分支，容易漏），
       所以只调 saveLoad 是看不到收敛效果的 —— 那是测试不完整，不是游戏没收敛。 */
    loadoutNormalize();
    loadoutBuild();

    printf("ver    = %d (期望 %d)\n", (int)gSave.ver, (int)SAVE_VER);
    printf("stars  = %d\n", gSave.stars);
    printf("coins  = %d\n", gSave.coins);
    printf("xp/lv  = %d / %d\n", gSave.xp, gSave.level);

    for (i = 0; i < PT_COLLECTIBLE; i++)
        if (!plantRetired(i) && gSave.plantOwned[i]) owned++;
    for (i = 0; i < LV_COUNT; i++) {
        if (gSave.lvUnlocked[i]) unlocked++;
        stars += gSave.lvStars[i];
    }
    for (i = 0; i < PT_COUNT; i++) if (gSave.loadout[i]) picks++;

    printf("owned  = %d 株（不含已下架的 4 株）\n", owned);
    printf("关卡   = %d 关解锁，星数合计 %d（lvStars 和）\n", unlocked, stars);
    printf("loadout= %d 个入选（单选收敛后必须是 1）\n", picks);
    for (i = 0; i < PT_COUNT; i++)
        if (gSave.loadout[i]) {
            nb[0] = 0;
            if (WideCharToMultiByte(CP_UTF8, 0, plantDefs[i].name, -1,
                                    nb, sizeof(nb), NULL, NULL) <= 0) nb[0] = 0;
            printf("         出战 -> #%d %s\n", i, nb);
        }

    /* 收敛后落盘的副本内容，供人工比对（真档不受影响） */
    printf("\n[校验结论] %s\n",
           (picks <= 1 && gSave.ver == SAVE_VER) ? "PASS" : "FAIL");
    return (picks <= 1 && gSave.ver == SAVE_VER) ? 0 : 1;
}
