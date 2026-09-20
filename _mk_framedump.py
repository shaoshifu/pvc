# -*- coding: utf-8 -*-
"""
从 pvz.c 生成「能把渲染结果导出成 BMP」的诊断副本 _dump.c / _dump.exe。

为什么需要它：
  PrintWindow + GetDIBits 拿到的是「整个窗口」而不是客户区，含标题栏/边框的偏移，
  用它判坐标会系统性错位（实测：连阳性对照的纯色块都找不见）。
  最可靠的真值来源是让程序自己把 gFrameDC（1x 合成后的最终帧）写成文件。

副本比 pvz.c 多出的东西（全部只在诊断版）：
  1. dumpFrameBMP()：把任意 HDC 的像素写成 32bpp 顶向下 BMP。
  2. render() 里 GPU 提前返回点之前：若 gDumpLeft>0，每帧把 gWorldDC 写到
     %PVZ_DUMPDIR%\\st<NN>.bmp 并递减。
     （不要挪到 render() 结尾 —— GPU 模式在那里已经 return，钩子永不执行。）
  3. F1~F12 强制切到 ST_PLAY..ST_DAILY（12 个状态），Home 回 ST_MENU；
     切状态时自动连拍 90 帧，够动画稳定后取样。
     ST_DRAFT / ST_REWARD 走 openDraft()/rewardPrepare()，否则界面是空的。

用法：
  PVZ_DUMPDIR=C:\\...\\_frames  _dump.exe
  驱动器发 F 键 → 隔 ~1.5s 读 _frames\\st<NN>.bmp
"""

import json
import os
import subprocess
import sys

SRC = "pvz.c"
DST = "_dump.c"
BOUNDS = []
CUM = 0

HOOK = r'''
/* ================== 帧缓冲导出（仅诊断版） ================== */

static void dumpStats(void);   /* 前向声明：实现放在 render 之前 */
static void dumpAuto(void);    /* 前向声明：实现放在 render 之前（那里才能调游戏函数） */

static int gDumpLeft = 0;      /* 还要连拍几帧（>0 时每帧写一次盘） */
static int gDumpTag  = -1;     /* 当前状态号，拼进文件名 */

/* 把 dc 的像素写成 32bpp、顶向下（biHeight 为负）的 BMP。
   不能直接 GetDIBits(gFrameDC, ...)：位图正被 select 在 DC 里，GetDIBits 会失败。
   所以先 BitBlt 到一张新建的 DIBSection，再从它的 bits 写盘。 */
static void dumpFrameBMP(HDC dc, const char *path)
{
    HDC mem; HBITMAP bmp, old;
    void *bits = NULL;
    BITMAPINFO bi;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *fp;
    int W = VIEW_W, H = VIEW_H;

    /* 抓帧尺寸改成**查位图实际尺寸**，不再写死 VIEW_W×VIEW_H。
       逻辑分辨率改造后 gFrameDC 是按视图尺寸（如 1662×1080）分配的，
       写死 1000×650 只会抓到左上角一角，看起来像"画面被放大了"。
       这里用 GetObject 问一下当前选中的位图有多宽 ——
       顺便也不用操心 gViewW 的声明顺序（它在文件更下方）。 */
    {
        BITMAP bm;
        HGDIOBJ hb = GetCurrentObject(dc, OBJ_BITMAP);
        if (hb && GetObject(hb, sizeof(bm), &bm) == sizeof(bm) &&
            bm.bmWidth > 0 && bm.bmHeight > 0) {
            W = bm.bmWidth;
            H = bm.bmHeight;
        }
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;               /* 顶向下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    mem = CreateCompatibleDC(NULL);
    if (!mem) return;
    bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { DeleteDC(mem); return; }
    old = SelectObject(mem, bmp);
    /* ★ 抓帧前必须把源 DC 的 world transform 复位成 identity。
       BitBlt 的**源坐标**受源 DC 变换影响，而 1x 层为了清晰化装了
       `gFrameXF`（缩放 = gViewW/1000）。带着它去读，GDI 会把小得多的
       源矩形映射过去，结果只复制到左上角一小块 ——
       看起来就像"画面没被放大"，实际渲染完全正常。
       这里用内联 identity（gIdentXF / gFrameXF 都声明在注入点之后，
       直接引用会编译不过），复位后**不需要恢复**：
       dumpMaybe 是 render 的最后一步，本帧不再绘制，
       下一帧的 render 会在降采样后重新装上 gFrameXF。 */
    {
        XFORM ident;
        ident.eM11 = 1; ident.eM12 = 0; ident.eM21 = 0; ident.eM22 = 1;
        ident.eDx  = 0; ident.eDy  = 0;
        SetWorldTransform(dc, &ident);
    }
    BitBlt(mem, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(mem, old);

    fp = fopen(path, "wb");
    if (fp) {
        memset(&fh, 0, sizeof(fh));
        fh.bfType = 0x4D42;                        /* "BM" */
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + (DWORD)W * (DWORD)H * 4;
        memcpy(&ih, &bi.bmiHeader, sizeof(ih));
        ih.biSizeImage = (DWORD)W * (DWORD)H * 4;
        fwrite(&fh, sizeof(fh), 1, fp);
        fwrite(&ih, sizeof(ih), 1, fp);
        fwrite(bits, 1, (size_t)W * (size_t)H * 4, fp);
        fclose(fp);
    }
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* 不能直接引用 gFrameDC —— 它声明在 1400 行后，本函数在 160 行附近。
   和审计版一样，DC 由 render() 调用时传进来。 */
static void dumpMaybe(HDC dc)
{
    char p[600];
    const char *dir;
    dumpAuto();          /* 必须在 early-return 之前：否则空闲帧里永远发不出按键 */
    if (gDumpLeft <= 0) return;
    dir = getenv("PVZ_DUMPDIR");
    if (dir && *dir) {
        _snprintf(p, sizeof(p) - 1, "%s\\st%02d.bmp", dir, gDumpTag);
        p[sizeof(p) - 1] = 0;
        dumpFrameBMP(dc, p);
    }
    gDumpLeft--;
    dumpStats();          /* 每帧再写一份精确状态，像素统计不可靠 */
}
'''

HOTKEYS = r'''        /* P：只把**当前**状态写盘，不改任何游戏状态。
           验证"点击命中"必须用这个 —— 其它键都会先切状态，
           那样就分不清"状态变了"是因为点中了还是因为键本身。 */
        else if (wp == 'P') { gDumpTag = 30; gDumpLeft = 30; }
        /* V：依次进入第 6~10 关（每按一次前进一关，循环）。
           为什么要专用键而不是走选关界面：F6 已经被「肉鸽选卡」占用
           （同一段里两个 VK_F6 分支，先绑定的胜出），选关界面进不去；
           而 UI 点击路径又依赖按钮坐标，验证脚本容易假阳性。
           直接切关卡最可靠，也让这个脚本只测"规则有没有装上"这一件事。 */
        else if (wp == 'V') {
            static int vn = 0;
            int lv = 6 + (vn % 5);
            vn++;
            gCurLevel = lv;
            gLawnVariant = levelDefs[lv].lawnVariant;
            /* ⚠️ 必须和真实选关路径（onClick 的 ST_LEVELS 分支）保持一致。
               之前这里漏了 bgmForLevel，于是"新关卡没有 BGM"看起来像 bug，
               实际只是测试键没走完整流程 —— 测出来的问题会误导排查方向。 */
            bgmForLevel(lv);
            resetGame();
            gState = ST_PLAY;
            gDumpTag = 40; gDumpLeft = 60;
        }
        /* B：把 8 种新僵尸每种刷一只到场上（每行一只）。
           用来验证「贴图能不能加载 + 行为有没有跑起来」——
           正常出怪要靠波次推进，跑一次要几十秒，不适合做验收。 */
        /* K：给第 0 行断电 8 秒（模拟短路僵尸的效果）。
           为什么不用"刷个短路僵尸等它走到植物"来测：那条路径依赖
           僵尸移动速度、关卡路径长度、波次进度，脚本化验证会非常脆。
           直接置状态，测的才是真正要验的东西 —— 断电期间植物停不停工。 */
        else if (wp == 'K') {
            /* 断电验证场景。要点：
               ① 先切到战斗状态 —— updateGame() 只在 ST_PLAY 里跑，
                  不在战斗中断电计时器根本不会推进（测试会假失败）；
               ② 自带被试对象：在第 0 行第 0 列种一株向日葵。第 0 列直接接电源，
                  不受关 9「必须贴着已有植物种」的连链规则限制 ——
                  用 X 键摆放会被那条规则全部拒绝（实测 plants n=0）；
               ③ 断 20 秒而不是 8 秒，给测试留出充裕的观察窗口。 */
            if (gState != ST_PLAY) gState = ST_PLAY;
            /* ④ 把波次计时器推到很远：否则测试中途会弹三选一（gState=ST_DRAFT），
               updateGame 停跑、所有计时器一起冻结 —— 那会让"断电冻结植物"
               这个断言变成假阳性（实测就踩过，+12s 时进了 DRAFT）。 */
            waveTimer = 9999.0f;
            plantIt(0, 0, PT_SUNFLOWER);
            gPowerCutRow[0] = 20.0f;
            gDumpTag = 30; gDumpLeft = 30;
        }
        /* Y：给场上的冰锥僵尸造成 1 点伤害（触发受击溅射），
           并在相邻行放一只普通僵尸当"被试"。
           为什么要专用键：冰锥的溅射是"受击时"触发，靠玩家实战命中来测
           要等豌豆飞行、命中判定，时序完全不可控。直接施加伤害，
           测的才是真正要验的 —— 受击有没有把冰冻写到相邻行的僵尸身上。 */
        else if (wp == 'Q') {
            int k, placed = 0;
            if (gState != ST_PLAY) gState = ST_PLAY;
            for (k = 0; k < MAX_ZOMBIES; k++) {
                Zombie *z = &zombies[k];
                if (!z->active || z->dead || z->type != ZT_FROSTFANG) continue;
                /* 在它上下相邻行各放一只被试僵尸，位置对齐 */
                if (z->row >= 1 && placed < 1) {
                    spawnZombieAt(ZT_NORMAL, z->row - 1, z->x);
                    placed++;
                } else if (z->row + 1 < ROWS && placed < 2) {
                    spawnZombieAt(ZT_NORMAL, z->row + 1, z->x);
                    placed++;
                }
                /* 扣血**必须同时设 flash** —— 冰锥的溅射是挂在
                   「flash 上升沿」上判定的（和冰面击退、机械反弹同一套判据）。
                   只减 hp 不会触发 flash，溅射自然不生效：
                   我第一次写这个测试键就漏了 flash，结果误判成"溅射没实现"。 */
                z->hp -= 1.0f;
                z->flash = 0.20f;
                break;
            }
            gDumpTag = 30; gDumpLeft = 45;
        }
        /* R：刷死亡事件能力僵尸，验证复生 / 死亡分叉 / 死亡诅咒。 */
        else if (wp == 'R') { int k;
            static const int RZEV[3] = { 0, 18, 28 };
            for (k = 0; k < 3; k++) {
                gPendingRgZ = RZEV[k];
                spawnZombieAt(ZT_NORMAL, k, 460.0f);
                gPendingRgZ = -1;
            }
            for (k = 0; k < MAX_ZOMBIES; k++)
                if (zombies[k].active) { zombies[k].hp = 1.0f; zombies[k].maxhp = 1.0f; }
            gDumpTag = 23; gDumpLeft = 180;
        }
        /* C：模拟"打通当前关卡"。
           做法是把波次推到终点并清场，让**真实的通关结算流程**跑一遍 ——
           如果直接去改 lvUnlocked，就测不到"结算里有没有正确解锁下一关"
           这条真正要验的路径。 */
        else if (wp == 'C') {
            int k;
            gWave = levelClearWaves(gCurLevel);
            spawnQN = 0;
            for (k = 0; k < MAX_ZOMBIES; k++) { zombies[k].active = 0; zombies[k].dead = 0; }
            waveTimer = 0.0f;
            gDumpLeft = 60;
        }
        else if (wp == 'B') {
            int k;
            static const int NZ8[8] = {
                ZT_FROSTFANG, ZT_GLACIER, ZT_MAGMAW, ZT_ASHWALKER,
                ZT_PHANTOM, ZT_BLINKER, ZT_COGWORK, ZT_SABOTEUR };
            for (k = 0; k < 8; k++) {
                int row = k % ROWS;
                float atx = (float)LAWN_X + (float)(6 + (k / ROWS) * 2) * (float)CELL_W;
                spawnZombieAt(NZ8[k], row, atx);
            }
            gState = ST_PLAY;
            gDumpTag = 41; gDumpLeft = 90;
        }
        else if (wp == VK_F1)  { gState = ST_PLAY;     gDumpTag = 1;  gDumpLeft = 90; }
        else if (wp == VK_F2)  { gState = ST_PAUSE;    gDumpTag = 2;  gDumpLeft = 90; }
        else if (wp == VK_F3)  { gState = ST_WIN;      gDumpTag = 3;  gDumpLeft = 90; }
        else if (wp == VK_F4)  { gState = ST_LOSE;     gDumpTag = 4;  gDumpLeft = 90; }
        /* F5 = 第一波奖励；F6 = 第二波奖励。
           两次都必须免费可选，防止第二次重新退化成阳光不足时点不动的货架。 */
        else if (wp == VK_F5)  { gWave = 0; rgOpenDraft(gWave); gDumpTag = 5;  gDumpLeft = 90; }
        else if (wp == VK_F6)  { gWave = 1; rgOpenDraft(gWave); gDumpTag = 6;  gDumpLeft = 90; }
        /* Z：刷三只肉鸽僵尸，验证生成、品质倍率、推进与击杀。 */
        else if (wp == 'Z') { gPendingRgZ = 49; spawnZombieAt(ZT_GIANT, 2, 700.0f); gPendingRgZ = -1;
                                 gPendingRgZ = 42; spawnZombieAt(ZT_NORMAL, 1, 760.0f); gPendingRgZ = -1;
                                 gPendingRgZ = 7;  spawnZombieAt(ZT_NORMAL, 3, 820.0f); gPendingRgZ = -1;
                                 gDumpTag = 13; gDumpLeft = 90; }
        /* Y：铺满各类地块 + 强制炼狱主题，验证「地形随能力变化」的渲染 */
        else if (wp == 'Y') { int cc2, rr2;
                                 terrainThemeSet(TH_INFERNO, L"末日花领主", 40.0f);
                                 for (rr2 = 0; rr2 < ROWS; rr2++)
                                   for (cc2 = 0; cc2 < COLS; cc2++) {
                                     unsigned bb = TF_SCORCH;
                                     if (cc2 % 5 == 1) bb = TF_ICE;
                                     else if (cc2 % 5 == 2) bb = TF_WEB;
                                     else if (cc2 % 5 == 3) bb = TF_CORRODE;
                                     else if (cc2 % 5 == 4) bb = TF_BLOOM;
                                     terrainTileAdd(rr2, cc2, bb, 90.0f);
                                   }
                                 gDumpTag = 14; gDumpLeft = 90; }
        /* U：刷一只神级僵尸，验证「神级以上单位自然触发主题切换」 */
        else if (wp == 'U') { gPendingRgZ = 40; spawnZombieAt(ZT_GIANT, 2, 700.0f); gPendingRgZ = -1;
                                 gDumpTag = 15; gDumpLeft = 90; }
        /* X：摆上各品质肉鸽植物 + 肉鸽僵尸，验证品质光环与接触影 */
        else if (wp == 'X') { int q;                                 static const int RGS[6] = { 0, 7, 10, 27, 39, 49 };
                                 for (q = 0; q < 6; q++) {
                                     int rr3 = q % ROWS, cc3 = 1 + q;
                                     gPendingRg = RGS[q];
                                     plantIt(rr3, cc3, (int)rgPlants[RGS[q]].base);
                                     gPendingRg = -1;
                                 }
                                 gPendingRgZ = 40; spawnZombieAt(ZT_GIANT, 2, 600.0f); gPendingRgZ = -1;
                                 gPendingRgZ = 7;  spawnZombieAt(ZT_NORMAL, 3, 690.0f); gPendingRgZ = -1;
                                 gPendingRgZ = 0;  spawnZombieAt(ZT_NORMAL, 1, 780.0f); gPendingRgZ = -1;
                                 gDumpTag = 16; gDumpLeft = 90; }
        /* D：殿堂五尊（rgPlants 100~104）—— 验证最高档的专属贴图与品质光环。
           为什么单开一个键：X 键摆的那 6 株是旧批次索引（0/7/10/27/39/49），
           完全没覆盖 100+，新增的最高档此前没有任何可视验证入口。
           每株占一行，从 col1 起错开一列，五尊的基座差异一眼能看出来。 */
        else if (wp == 'D') { int q;
                                 /* 必须先退出可能还挂着的三选一面板：
                                    否则截图里是一块奖励面板，草坪被完全盖住，
                                    看起来像"贴图没加载"。 */
                                 gState = ST_PLAY;
                                 for (q = 0; q < 5; q++) {
                                     gPendingRg = 100 + q;
                                     plantIt(q, 1 + q, (int)rgPlants[100 + q].base);
                                     gPendingRg = -1;
                                 }
                                 gPendingRgZ = 49; spawnZombieAt(ZT_GIANT,  2, 640.0f); gPendingRgZ = -1;
                                 gPendingRgZ = 7;  spawnZombieAt(ZT_NORMAL, 4, 760.0f); gPendingRgZ = -1;
                                 gDumpTag = 23; gDumpLeft = 150; }
        /* T：始祖三尊 + 始祖主题（背景换成 lawn_primordial、BGM 换 primordial.mp3）。
           与 D 键同一套思路，但它还会把主题打开 —— 用来肉眼确认
           "始祖出现后背景确实变了"，而不是只看代码里那几行。 */
        else if (wp == 'T') { int q;
                                 gState = ST_PLAY;
                                 primordialThemeApply();
                                 for (q = 0; q < 3; q++) {
                                     gPendingRg = 105 + q;
                                     plantIt(q, 2 + q, (int)rgPlants[105 + q].base);
                                     gPendingRg = -1;
                                 }
                                 gPendingRgZ = 49; spawnZombieAt(ZT_GIANT,  3, 640.0f); gPendingRgZ = -1;
                                 gPendingRgZ = 7;  spawnZombieAt(ZT_NORMAL, 0, 760.0f); gPendingRgZ = -1;
                                 gDumpTag = 24; gDumpLeft = 150; }
        /* M：机制验证场景 —— 5 株各代表一类机制，同行各配一只僵尸。
           读 stat 明细里的 slow / x / hp 就能判断：
           减速场有没有写进 z->slow、击退有没有真的推动 x、处决有没有秒杀。
           这是"静态审计说接通了"之后的第二道验证：接通 ≠ 生效。 */
        else if (wp == 'M') { int q; static const int RM[5] = { 67, 50, 59, 81, 60 };
                                 for (q = 0; q < 5; q++) {
                                     gPendingRg = RM[q];
                                     plantIt(q, 1, (int)rgPlants[RM[q]].base);
                                     gPendingRg = -1;
                                     /* ⚠️ 僵尸必须紧贴植物摆（+46px）。
                                        影响半径 R = rgRangeMul(tier) * 52，
                                        普通品质只有 52px —— 第一版把僵尸摆在
                                        x=420（离 col1 有 250px），机制根本够不到，
                                        结果只有"基座的普通攻击"打死了僵尸，
                                        却会被误读成"机制生效"。 */
                                     spawnZombieAt(ZT_NORMAL, q, cellCX(1) + 46.0f);
                                 }
                                 /* 把血放大 6 倍：贴脸的僵尸会被 5 株植物集火，
                                    实测 0.5 秒内就全灭 —— 那样只能证明"有伤害"，
                                    看不到 slow / 击退 这类需要停留观察的持续状态。 */
                                 for (q = 0; q < MAX_ZOMBIES; q++)
                                     if (zombies[q].active) {
                                         zombies[q].maxhp *= 6.0f;
                                         zombies[q].hp = zombies[q].maxhp;
                                         zombies[q].basehp = zombies[q].maxhp;
                                     }
                                 gDumpTag = 17; gDumpLeft = 90; }
        /* V：验证神级植物的穿透 + 溅射 + 半径 —— 一株神级射手 + 一排抱团的僵尸
           ⚠️ 这条是**死分支**：本段最前面已有一个 'V'（切第 6~10 关），
           else-if 链里先绑定的胜出，所以按 V 永远进的是关卡切换，不是这个场景。
           留在这里会让人以为"按 V 就该看到神级植物"，白费一轮排查。
           要用这个场景请换键（例如 'B'），并确认新键在全段里只出现一次。 */
        else if (wp == 'V') { int q;
                                 gPendingRg = 7;                 /* 概率坩埚（神级，豌豆基座） */
                                 plantIt(2, 1, (int)rgPlants[7].base);
                                 gPendingRg = -1;
                                 for (q = 0; q < 6; q++) {       /* 同一行挤 6 只 */
                                     gPendingRgZ = -1;
                                     spawnZombieAt(ZT_NORMAL, 2, 560.0f + (float)q * 24.0f);
                                 }
                                 /* ⚠️ 这个 V 键原本也用 tag 17，和 M 键撞车 ——
                                    两个键写了同一个文件名，后写的把先写的覆盖掉，
                                    采样会读到另一套场景的数据。改成 19 避开。 */
                                 gDumpTag = 19; gDumpLeft = 90; }
        /* N：机制验证第二批 —— 护盾光环 / 标记 / 轨道轰炸 / 召唤 / 阳光光环。
           与 M 键同构，换 5 株，用来覆盖"不直接改僵尸 x/hp"的那半边机制。 */
        else if (wp == 'N') { int q; static const int RN[5] = { 63, 55, 78, 74, 64 };
                                 for (q = 0; q < 5; q++) {
                                     gPendingRg = RN[q];
                                     plantIt(q, 1, (int)rgPlants[RN[q]].base);
                                     gPendingRg = -1;
                                     spawnZombieAt(ZT_NORMAL, q, cellCX(1) + 46.0f);
                                 }
                                 for (q = 0; q < MAX_ZOMBIES; q++)
                                     if (zombies[q].active) {
                                         zombies[q].maxhp *= 6.0f;
                                         zombies[q].hp = zombies[q].maxhp;
                                         zombies[q].basehp = zombies[q].maxhp;
                                     }
                                 gDumpTag = 18; gDumpLeft = 90; }
        /* G / H：直接触发某一关的 BGM，用来验证 MCI 通路是否真的出声。
           为什么需要：F1 只是把 gState 设成 ST_PLAY，**不经过选关**，
           所以 bgmForLevel() 根本不会被调用 —— 光按 F1 永远测不到 BGM。 */
        /* J：刷第三批新僵尸（飞 / 跳 / 召唤 / 闪现 / 钻地 / 尸王）。
           挑的这几只都带**位移类**机制，x 会明显变化 ——
           读 stat 的 detail 行就能判断"机制到底跑没跑"，
           不像回血类要靠长时间采样才看得出来。 */
        else if (wp == 'J') { int q; static const int NZ[6] = { 50, 51, 53, 54, 68, 69 };
                              for (q = 0; q < 6; q++) {
                                  gPendingRgZ = NZ[q];
                                  spawnZombieAt(ZT_NORMAL, q % ROWS, 660.0f + (float)q * 30.0f);
                                  gPendingRgZ = -1;
                              }
                              /* ⚠️ 这里必须给足时长：90 帧只有 1.5 秒，
                                 而僵尸机制的脉冲周期是 `4.6 - 0.42*tier` 秒
                                 （普通 4.6s / 传奇 2.1s）—— 采样还没跑到机制触发，
                                 文件就已经停写了，于是永远看到"机制没生效"。
                                 给 900 帧（≈15 秒）。 */
                              gDumpTag = 22; gDumpLeft = 900; }
        else if (wp == 'G') { bgmForLevel(0); gDumpTag = 20; gDumpLeft = 90; }
        else if (wp == 'H') { bgmForLevel(1); gDumpTag = 21; gDumpLeft = 90; }
        /* ⚠️ 这里原来还有一条 `VK_F6 -> ST_LEVELS`：F6 已被上面的「第二波三选一」
           绑定，else-if 链里先绑定的胜出，那条永远执行不到 —— 删掉，
           否则以后会照着它去按 F6 找选关界面，白费一轮排查。
           选关界面请用下面的 'V' 键（那是它自己的 handler，不是死分支）。 */
        else if (wp == VK_F7)  { gState = ST_TALENTS;  gDumpTag = 7;  gDumpLeft = 90; }
        else if (wp == VK_F8)  { gState = ST_ACH;      gDumpTag = 8;  gDumpLeft = 90; }
        else if (wp == VK_F9)  { gState = ST_GACHA;    gDumpTag = 9;  gDumpLeft = 90; }
        /* F10：编组界面。用户报的"已选 N/M 与实际绿框对不上"要看这个界面，
           原来没有对应热键，没法单独抓它出来比对。
           （这里也曾重复绑一次 F10，后者是死分支，已删。） */
        else if (wp == VK_F10) { gState = ST_LOADOUT;  gDumpTag = 10; gDumpLeft = 180; }
        else if (wp == VK_F12) { rewardPrepare(); gState = ST_REWARD; gDumpTag = 11; gDumpLeft = 90; }
        else if (wp == VK_HOME){ gState = ST_DAILY;    gDumpTag = 12; gDumpLeft = 90; }
        else if (wp == VK_END) { gState = ST_MENU;     gDumpTag = 0;  gDumpLeft = 90; }
'''



STATS_BODY = r'''/* ================== 精确状态输出（仅诊断版） ==================
   像素统计容易被纹理淹没（实测"偏离度"在僵尸全灭前后只差 0.3），
   所以直接写数字 —— 存活数 / 总血量 / 击杀数。 */
static void dumpStats(void)
{
    char p[1200];
    char det[700];
    const char *dir = getenv("PVZ_DUMPDIR");
    FILE *fp;
    int i, n = 0, pl = 0;
    float hp = 0.0f;
    if (!dir || !*dir) return;
    for (i = 0; i < MAX_ZOMBIES; i++)
        if (zombies[i].active && !zombies[i].dead) { n++; hp += zombies[i].hp; }
    for (i = 0; i < ROWS; i++) { int c; for (c = 0; c < COLS; c++) {
        Plant *q = plantAt(i, c); if (q && q->alive) pl++; } }
    /* 逐只僵尸明细。为什么必须有：
       总数掉血只能证明"有东西在打"，分不清是哪个机制在起作用。
       x 用来验证击退/拉拽（钩爪把 x 拉小、弹簧把 x 推大），
       slow/rooted 用来验证减速场与定身，hp 用来验证伤害/处决。 */
    det[0] = 0;
    {
        int k = 0, shown = 0;
        for (i = 0; i < MAX_ZOMBIES && shown < 8; i++) {
            char one[96];
            Zombie *z = &zombies[i];
            if (!z->active) continue;
            _snprintf(one, sizeof(one) - 1,
                      "z%d(%d,%.0f,%.0f,%.1f,%.1f%s) ", i, z->row, z->x, z->hp,
                      z->slow, z->rooted, z->dead ? ",D" : "");
            one[sizeof(one) - 1] = 0;
            if (k + (int)strlen(one) >= (int)sizeof(det) - 2) break;
            strcpy(det + k, one);
            k += (int)strlen(one);
            shown++;
        }
    }
    _snprintf(p, sizeof(p) - 1, "%s\\stat%02d.txt", dir, gDumpTag);
    p[sizeof(p) - 1] = 0;
    fp = fopen(p, "w");
    if (fp) {
        fprintf(fp, "alive=%d hp=%.0f plants=%d killed=%d sun=%d theme=%d\n",
                n, hp, pl, gKilled, gSun, gTheme);
        fprintf(fp, "detail %s\n", det);
        /* 各僵尸类型在场数量：验证新僵尸有没有真的出场。
           只看 detail 那行分不清「没刷出来」和「刷出来了但没动」。 */
        {
            int tc[ZT_COUNT], q, total = 0;
            memset(tc, 0, sizeof(tc));
            for (q = 0; q < MAX_ZOMBIES; q++)
                if (zombies[q].active) { tc[zombies[q].type]++; total++; }
            fprintf(fp, "ztypes total=%d", total);
            for (q = 0; q < ZT_COUNT; q++)
                if (tc[q]) fprintf(fp, " %d:%d", q, tc[q]);
            fprintf(fp, "\n");
        }
        /* BGM 状态。MCI 是**异步**的：open 成功不代表能放出声
           （解码器类型选错、文件损坏都会在 play 阶段才暴露），
           所以这里查的是 status mode，而不是"有没有调过 bgmPlayFile"。
           用 %ls 而不是 %S —— MinGW 下 %S 的含义和 MSVC 相反。 */
        {
            /* 必须是 wchar_t 缓冲：mciSendStringW 的第二个参数是 LPWSTR，
               传 char[] 会直接编译不过（不是 warning，是 error）。
               打印用 %ls 而不是 %S —— MinGW 下这两个的含义与 MSVC 相反。 */
            wchar_t bm[64] = L"";
            mciSendStringW(L"status " BGM_ALIAS L" mode", bm, 64, NULL);
            fprintf(fp, "bgm open=%d mode=%ls vol=%d\n", gBgmOpen, bm, BGM_VOLUME);
            /* 视图尺寸 + 1x 层变换是否真的装上 —— 排查"画面没放大"这类问题
               光看代码是看不出 SetWorldTransform 静默失败的 */
            /* 当前界面状态 + 鼠标逻辑坐标：验证"点击是否命中"要看这两个。
               state 变了说明按钮真的被按下去了。 */
            fprintf(fp, "state=%d mouse=%d,%d\n", gState, mMouseX, mMouseY);
            /* 关卡解锁状态：验证「通关即解锁」要看这两个。
               unlocked 每一位对应一关能不能进；playable 是实际判定结果。 */
            {
                int q;
                fprintf(fp, "lv cur=%d clearw=%d stars=%d unlocked=",
                        gCurLevel, levelClearWaves(gCurLevel), gSave.stars);
                for (q = 0; q < LV_COUNT; q++) fprintf(fp, "%d", gSave.lvUnlocked[q] ? 1 : 0);
                fprintf(fp, " playable=");
                for (q = 0; q < LV_COUNT; q++) fprintf(fp, "%d", levelPlayable(q) ? 1 : 0);
                fprintf(fp, "\n");
                /* 各关通关波数：单独一行列出，防止"截断所有关卡"这类错误
                   （曾经把所有 >20 波的关卡都压成 20，界面显示才发现）。
                   ⚠️ 必须另起一行 —— 插在上一行中间会把 unlocked= 和它的值拆开，
                      解析脚本就读不到了。 */
                fprintf(fp, "clearwaves ");
                for (q = 0; q < LV_COUNT; q++)
                    fprintf(fp, "%d:%d ", q, levelClearWaves(q));
                fprintf(fp, "\n");
            }
            /* 新五关的规则状态：验证"每关真的有独立规则"要看这几个 */
            {
                int thin = 0, rr, cc, rift = 3;
                for (rr = 0; rr < ROWS; rr++) for (cc = 0; cc < COLS; cc++)
                    if (gThinIce[rr][cc]) thin++;
                if (gRuleKind != RULE_RIFT) rift = 0;
                fprintf(fp, "rule kind=%d level=%d phase=%d thinice=%d rifts=%d "
                            "heat=%.2f fogcol=%d grava=%.2f\n",
                        gRuleKind, gCurLevel, gPhase, thin, rift,
                        gHeatWave, gFogFromCol, gGravAcc);
            }
            /* 卡片选中态 + 编组 + 阳光：排查"没选却出现选取"要看这几个。
               gSel 是战斗中的卡片选中项（植物 id），必须是 -1 或某张卡的 id。 */
            fprintf(fp, "sel gSel=%d shovel=%d N=%d bonus=%d rg=%d sun=%d cd0=%.1f cd1=%.1f\n",
                    gSel, gShovel, gLoadoutN, gBonusN, gRgN, gSun,
                    cardCD[0], cardCD[1]);
            fprintf(fp, "loadout %d %d %d %d\n",
                    gLoadout[0], gLoadout[1], gLoadout[2], gLoadout[3]);
            /* 断电 + 新僵尸行为：验证 P0-1 / P0-2 要看这几个 */
            {
                int q, ready = 0, powered = 0;
                for (q = 0; q < ROWS; q++) {
                    if (gPowerCutRow[q] > 0.0f) fprintf(fp, "cutrow %d=%.1f ", q, gPowerCutRow[q]);
                    if (newLevelRowPowered(q)) powered++;
                }
                fprintf(fp, "\npower powered=%d/5 ", powered);
                for (q = 0; q < ROWS; q++) fprintf(fp, "%d", newLevelRowPowered(q) ? 1 : 0);
                fprintf(fp, " riftT=%.1f\n", gRiftT);
                /* 场上植物的 timer 之和：断电应当让它不再增长（向日葵不产阳光） */
                {
                    float tsum = 0.0f; int npl = 0;
                    int rr, cc, sl;
                    for (rr = 0; rr < ROWS; rr++) for (cc = 0; cc < COLS; cc++)
                        for (sl = 0; sl < 2; sl++) {
                            Plant *pp = (sl == 0) ? &grid[rr][cc] : &grid2[rr][cc];
                            if (pp->alive) { tsum += pp->timer; npl++; }
                        }
                    fprintf(fp, "plants n=%d timersum=%.2f\n", npl, tsum);
                    /* 断电验证的直接观察点：被试植物的计时器 */
                    {
                        Plant *t0 = &grid[0][0];
                        fprintf(fp, "probe state=%d alive=%d timer=%.3f\n",
                                gState, t0->alive, t0->timer);
                    }
                }
                /* 减速僵尸数：冰锥溅射后应增加 */
                {
                    int nslow = 0, nmag = 0, nfrost = 0;
                    for (q = 0; q < MAX_ZOMBIES; q++) {
                        if (!zombies[q].active || zombies[q].dead) continue;
                        if (zombies[q].slow > 0.0f) nslow++;
                        if (zombies[q].type == ZT_MAGMAW)   nmag++;
                        if (zombies[q].type == ZT_FROSTFANG) nfrost++;
                    }
                    fprintf(fp, "zstate slow=%d magmaw=%d frostfang=%d ready=%d\n",
                            nslow, nmag, nfrost, ready);
                    /* 按行列出僵尸（type 与 slow）：冰锥溅射要对照相邻行看 */
                    fprintf(fp, "zrows");
                    for (q = 0; q < MAX_ZOMBIES; q++) {
                        if (!zombies[q].active || zombies[q].dead) continue;
                        /* slow 是递减计时器，可以为负；显示时 clamp 到 0，免得读出 s-0.0 */
                        {
                            float sl = zombies[q].slow;
                            if (sl < 0.0f) sl = 0.0f;
                            fprintf(fp, " r%d:t%d,s%.1f", zombies[q].row,
                                    zombies[q].type, sl);
                        }
                    }
                    fprintf(fp, "\n");
                }
            }
        }
        {
            /* 性能：单帧 render 耗时 + 累计帧率。
               60fps 的预算是 16.7ms，render 超过它就会掉帧。 */
            DWORD el = GetTickCount() - gDbgPerfT0;
            double fps = (el > 0) ? (double)gDbgFrameCnt * 1000.0 / (double)el : 0.0;
            fprintf(fp, "perf render=%.1fms fps=%.1f frames=%d\n",
                    gDbgRenderMs, fps, gDbgFrameCnt);
            if (gDbgPerfN > 0)
                fprintf(fp, "avg render=%.1fms n=%d\n",
                        gDbgSumRender / gDbgPerfN, gDbgPerfN);
        }
        /* 音效槽占用数：能直接证明"音效到底有没有被触发过"。
           0/N 表示一次都没播过（触发点没接上），而不是"播了但听不见"。 */
        {
            int k, used = 0;
            for (k = 0; k < SFX_POOL; k++) if (gSfx[k].used) used++;
            fprintf(fp, "sfx slots=%d/%d on=%d\n", used, SFX_POOL, gSfxOn);
        }
        fclose(fp);
    }
}

/* ================== 进程内自动驱动（仅诊断版） ==================
   为什么需要：从**外部进程** PostMessage 到本项目窗口实测不可靠 ——
   返回值是 1、窗口也活着，WndProc 却收不到键，表现就是"进程 Alive、
   一张图都没有"。所以改成进程内按帧号直接调游戏函数，不依赖任何按键。

   为什么定义在这里（而不是上面抓帧钩子那块）：要用到 rgOpenDraft /
   spawnZombieAt / ZT_* / gWave，它们在文件更下方才声明。

   序列（帧号 : 动作 -> 产物）：
      20 : 第一波三选一      -> st05.bmp
      70 : 回战场            -> st01.bmp
      80 : 第二波三选一      -> st06.bmp
     130 : 回战场            -> st01.bmp
     140 : 刷三只肉鸽僵尸    -> st13.bmp
   190 帧之后不再有动作；进程继续存活，由调用脚本自行终止。 */
static int gAutoFrame = 0;
static void dumpAuto(void)
{
    static const struct { int at; int act; } SEQ[] = {
        {  20, 5 }, {  70, 1 }, {  80, 6 }, { 130, 1 }, { 140, 13 }, { 230, 23 }
    };
    int i;
    const char *seq = getenv("PVZ_AUTOSEQ");
    if (!seq) return;
    /* PVZ_AUTOSEQ=2：始祖专项序列。单独一支是为了**不拖慢** _rg_verify
       （它用的是序列 1；把始祖场景塞进同一支会让每次验证都多等十几秒）。 */
    if (seq[0] == '2') {
        if (gAutoFrame == 20) {
            int q;
            gState = ST_PLAY;
            primordialThemeApply();
            for (q = 0; q < 3; q++) {
                gPendingRg = 105 + q;
                plantIt(q, 2 + q, (int)rgPlants[105 + q].base);
                gPendingRg = -1;
            }
            gPendingRgZ = 49; spawnZombieAt(ZT_GIANT,  3, 640.0f); gPendingRgZ = -1;
            gPendingRgZ = 7;  spawnZombieAt(ZT_NORMAL, 0, 760.0f); gPendingRgZ = -1;
            gDumpTag = 24; gDumpLeft = 150;
        }
        gAutoFrame++;
        return;
    }
    for (i = 0; i < (int)(sizeof(SEQ) / sizeof(SEQ[0])); i++) {
        if (SEQ[i].at != gAutoFrame) continue;
        switch (SEQ[i].act) {
        case 5:  gWave = 0; rgOpenDraft(gWave); gDumpTag = 5;  gDumpLeft = 90; break;
        case 6:  gWave = 1; rgOpenDraft(gWave); gDumpTag = 6;  gDumpLeft = 90; break;
        case 1:  gState = ST_PLAY;              gDumpTag = 1;  gDumpLeft = 90; break;
        case 13: gPendingRgZ = 49; spawnZombieAt(ZT_GIANT,  2, 700.0f); gPendingRgZ = -1;
                 gPendingRgZ = 42; spawnZombieAt(ZT_NORMAL, 1, 760.0f); gPendingRgZ = -1;
                 gPendingRgZ = 7;  spawnZombieAt(ZT_NORMAL, 3, 820.0f); gPendingRgZ = -1;
                 gDumpTag = 13; gDumpLeft = 90; break;
        /* 23：殿堂五尊。和 'D' 键同一套动作 —— 新加的最高档必须有自动化的
           可视验证入口，否则只能靠人肉按键，而外部按键在本项目不可靠。 */
        case 23: { int q;
                 gState = ST_PLAY;   /* 退掉上一幕遗留的三选一面板，否则草坪被盖住 */
                 for (q = 0; q < 5; q++) {
                     gPendingRg = 100 + q;
                     plantIt(q, 1 + q, (int)rgPlants[100 + q].base);
                     gPendingRg = -1;
                 }
                 gPendingRgZ = 49; spawnZombieAt(ZT_GIANT,  2, 640.0f); gPendingRgZ = -1;
                 gPendingRgZ = 7;  spawnZombieAt(ZT_NORMAL, 4, 760.0f); gPendingRgZ = -1;
                 gDumpTag = 23; gDumpLeft = 150; break; }
        }
    }
    gAutoFrame++;
}
'''

def patch(text, old, new, what, count=1):
    global CUM
    n = text.count(old)
    if n != count:
        sys.exit("[X] 锚点不匹配：%s（期望 %d 处，实际 %d 处）" % (what, count, n))
    idx = text.index(old)
    delta = new.count("\n") - old.count("\n")
    at = text[:idx].count("\n") + 1
    CUM += delta
    BOUNDS.append([at, CUM])
    return text.replace(old, new, count)


def main():
    src = open(SRC, encoding="utf-8").read()
    n0 = len(src)

    src = patch(src,
                "static HFONT gF15, gF18, gF22, gF30, gF54, gF72;\n",
                "static HFONT gF15, gF18, gF22, gF30, gF54, gF72;\n" + HOOK,
                "字体全局变量声明")

    src = patch(src,
                "        if (gpuDrawFrame(cw, ch, dx, dy, dw, dh)) return;\n",
                "        /* 诊断版：抓帧必须挂在 GPU 提前返回点**之前**。\n"
                "           render() 结尾在 GPU 模式下永远到不了（那里直接 return），\n"
                "           钩子挂末尾的表现就是「进程正常、一张图都没有」——\n"
                "           以前误判成外部热键没送到，其实是抓帧代码不在执行路径上。\n"
                "           抓帧源用 gWorldDC：它是 VIEW_W*SS 的 2x 画布，草坪、HUD、\n"
                "           三选一面板全都画在它上面，而 GPU 路径上传的也正是这个位图；\n"
                "           gFrameDC 只在 GDI 回退分支被填充，拿它截会得到空图。 */\n"
                "        dumpMaybe(gWorldDC);\n"
                "        if (gpuDrawFrame(cw, ch, dx, dy, dw, dh)) return;\n",
                "GPU 提前返回点之前插入抓帧钩子")

    src = patch(src,
                "static void render(HDC out)\n",
                STATS_BODY + "static void render(HDC out)\n",
                "render 之前插入 dumpStats 实现")

    # 锚点必须挑不会随功能演进而改的那一行。
    # 以前锚在 VK_ESCAPE 分支上，后来 Esc 被改成多行的 goBack() 分流，
    # 锚点当场失效（生成器直接报"锚点不匹配"）。
    # A 键（自动收阳光）是个纯开关，最不容易被动到，用它。
    # 固定步长：让"第 N 帧"完全可复现。
    # 必要性（踩过）：dt 来自 GetTickCount（墙钟），两次运行的动画相位与累计
    # 游戏时间都不同。直接拿两次抓帧比对，会得到"96% 一致、4% 像素不同"的
    # 假差异，还会看到 sun 数值都不一样 —— 那不是回归，是时间不同步。
    # 设 PVZ_FIXED_DT=0.0166667 后，帧 N 在任一构建上都可复现，
    # 于是"重构/换平台有没有改变渲染"才能被逐像素证明。
    src = patch(src,
                "        if (dt < 0.0005f) dt = 0.0005f;\n",
                "        if (dt < 0.0005f) dt = 0.0005f;\n"
                "        {\n"
                "            static float gFixedDt = -1.0f;\n"
                "            if (gFixedDt < 0.0f) {\n"
                "                const char *fs = getenv(\"PVZ_FIXED_DT\");\n"
                "                gFixedDt = fs ? (float)atof(fs) : 0.0f;\n"
                "            }\n"
                "            if (gFixedDt > 0.0f) dt = gFixedDt;\n"
                "        }\n",
                "dt 钳位之后插入固定步长开关")

    src = patch(src,
                "        else if (wp == 'A') { gAutoSun = !gAutoSun; }\n",
                HOTKEYS + "        else if (wp == 'A') { gAutoSun = !gAutoSun; }\n",
                "WM_KEYDOWN 的按键分支")

    open(DST, "w", encoding="utf-8", newline="\n").write(src)
    json.dump({"bounds": BOUNDS, "src": SRC, "dst": DST},
              open("_dump_lines.json", "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)
    print("[1/2] %s 已生成（%d -> %d 字节）；行映射 %s" % (DST, n0, len(src), BOUNDS))

    cmd = ["gcc", "-O2", "-mwindows", "-static-libgcc", "-o", "_dump.exe", DST,
           "-lgdi32", "-luser32", "-lmsimg32", "-lwinmm", "-ld3d9", "-lm", "-Wall", "-Wextra"]
    print("[2/2] " + " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    if out:
        print(out[:4000])
    if r.returncode != 0:
        sys.exit("[X] 编译失败 returncode=%d" % r.returncode)
    print("[OK] _dump.exe 编译成功")


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)) or ".")
    main()
