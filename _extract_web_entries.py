# -*- coding: utf-8 -*-
"""抽出「平台无关的输入/生命周期入口」，供网页版直接复用。

【为什么必须抽】
  WndProc 里那几个鼠标分支不是"Windows 消息处理"，而是**游戏逻辑**：
     WM_MOUSEMOVE  → 更新三选一的悬停高亮（gDraftHover）
     WM_LBUTTONDOWN→ mDown=1 并调 onClick(x, y)（命中卡片/卡槽/铲子/按钮）
  网页版要点击卡片、要拖拽种植，就必须走同一份逻辑。
  如果网页版另写一套点击处理，必然漂移 —— 这个项目里"手抄副本漂移"
  已经翻过两次车（artLoadAll、回归旧二进制）。

  所以抽成 `gamePointerMove/Down/Up`（坐标是**逻辑游戏坐标**），
  WndProc 与 plat_web.c 都调它。

【顺带解决三个"函数未被使用"告警】
  createLayers / WndProc / spriteRelease / gpuRelease 在可移植构建里
  只被 WinMain 用到，而 WinMain 被 `#if PLAT_HAS_OWN_MAIN` 排除了。
  处理方式不是压告警，而是**把它们的其它调用者补上**：
    · createLayers  → gameInitAll()
    · spriteRelease → gameShutdownAll()
    · gpuRelease    → gameShutdownAll()
    · WndProc       → 确实是 Windows 专有（消息分发），
                      用 `#if PLAT_HAS_OWN_MAIN` 排掉才是正确的表达。

用法：python _extract_web_entries.py
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "pvz.c")

MARK = "void gamePointerMove(float gx, float gy)"

# ---------------------------------------------------------------- 输入入口
INPUT_ENTRIES = '''/* ==================== 平台无关的输入入口 ====================
   坐标是**逻辑游戏坐标**（0..VIEW_W / 0..VIEW_H）。
   桌面端由 WndProc 经 windowToGame() 换算后调用；
   网页版由外壳把"触摸点 → 逻辑坐标"换算后调用（外壳那一步还要处理
   letterbox 黑边与 CSS 缩放，见 web/shell.js 的 mapPointer）。
   两条路走同一份命中判定，不会出现"桌面点得中、平板点不中"。 */

void gamePointerMove(float gx, float gy)
{
    mMouseX = (int)gx;
    mMouseY = (int)gy;
    if (gState == ST_DRAFT) {
        int k;
        gDraftHover = -1;
        for (k = 0; k < DRAFT_N; k++) {
            float cx0, cy0;
            draftCardRect(k, &cx0, &cy0);
            if (mMouseX >= cx0 && mMouseX <= cx0 + CARD_W &&
                mMouseY >= cy0 && mMouseY <= cy0 + CARD_H) gDraftHover = k;
        }
    }
}

void gamePointerDown(float gx, float gy)
{
    mDown = 1;
    mMouseX = (int)gx;
    mMouseY = (int)gy;
    onClick(mMouseX, mMouseY);
}

void gamePointerUp(void) { mDown = 0; }

/* ==================== 平台无关的生命周期入口 ====================
   createLayers / artLoadAll 都是 static，平台层（plat_web.c / plat_ios.m）
   在另一个编译单元里，所以这里包一层暴露出去。 */

void gameInitAll(void)
{
    createLayers(NULL);
    gameBoot();          /* 字体 + 载图 + 开局，见其定义处 */
}

/* 释放全部贴图与 GPU 资源。桌面端在 WinMain 收尾调用；
   网页版不需要（页面卸载时整块内存随 wasm 实例回收），
   但 iOS 需要 —— 那边切后台可能被回收内存。 */
void gameShutdownAll(void)
{
    int i, k;
    for (i = 0; i < PT_COUNT; i++) spriteRelease(&gSprPlant[i]);
    for (i = 0; i < ZT_COUNT; i++) {
        spriteRelease(&gSprZombie[i]);
        spriteRelease(&gSprZombieFlash[i]);
        spriteRelease(&gSprZombieBlue[i]);
        spriteRelease(&gSprZombieEat[i]);
        spriteRelease(&gSprZombieEatFlash[i]);
        spriteRelease(&gSprZombieEatBlue[i]);
        for (k = 0; k < 5; k++) spriteRelease(&gSprZombieDead[i][k]);
    }
    spriteRelease(&gSprSun);
    spriteRelease(&gSprBackground);
    spriteRelease(&gSprLawn);
    spriteRelease(&gSprLawnNight);
    spriteRelease(&gSprLawnWater);
    spriteRelease(&gSprLawnRoof);
    spriteRelease(&gSprLawnDesert);
    spriteRelease(&gSprLawnIce);
    spriteRelease(&gSprLawnMagma);
    spriteRelease(&gSprLawnVoid);
    spriteRelease(&gSprLawnCircuit);
    spriteRelease(&gSprLawnAstral);
    spriteRelease(&gSprLawnPrimordial);
    { int rp; for (rp = 0; rp < RG_PLANT_N; rp++) spriteRelease(&gSprRgPlant[rp]); }
    { int rz; for (rz = 0; rz < RG_ZOMBIE_N; rz++) spriteRelease(&gSprRgZombie[rz]); }
    { int b; for (b = 0; b < RGQ_COUNT; b++) spriteRelease(&gSprBadge[b]); }
    { int f; for (f = 0; f < FSX_N; f++) spriteRelease(&gSprFx[f]); }
    gpuRelease();
}

'''

# WndProc 的鼠标分支 → 改调新入口
MOUSE_OLD = '''    case WM_MOUSEMOVE:
        windowToGame((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &mMouseX, &mMouseY);
        if (gState == ST_DRAFT) {
            int k;
            gDraftHover = -1;
            for (k = 0; k < DRAFT_N; k++) {
                float cx0, cy0;
                draftCardRect(k, &cx0, &cy0);
                if (mMouseX >= cx0 && mMouseX <= cx0 + CARD_W &&
                    mMouseY >= cy0 && mMouseY <= cy0 + CARD_H) gDraftHover = k;
            }
        }
        SetCursor(LoadCursorW(NULL, uiPointerHot(mMouseX, mMouseY) ? IDC_HAND : IDC_ARROW));
        return 0;
    case WM_LBUTTONDOWN:
        mDown = 1;
        windowToGame((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &mMouseX, &mMouseY);
        onClick(mMouseX, mMouseY);
        return 0;
    case WM_LBUTTONUP: mDown = 0; return 0;'''
MOUSE_NEW = '''    case WM_MOUSEMOVE: {
        /* 换算成逻辑坐标后交给平台无关入口 —— 网页版走的是同一个函数
           （见 gamePointerMove 的说明）。 */
        int gx, gy;
        windowToGame((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &gx, &gy);
        gamePointerMove((float)gx, (float)gy);
        SetCursor(LoadCursorW(NULL, uiPointerHot(mMouseX, mMouseY) ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int gx, gy;
        windowToGame((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &gx, &gy);
        gamePointerDown((float)gx, (float)gy);
        return 0;
    }
    case WM_LBUTTONUP: gamePointerUp(); return 0;'''


def main():
    s = io.open(SRC, encoding="utf-8").read()
    if MARK in s:
        print("  已抽过，跳过（幂等）")
        return 0

    # ① 插入输入/生命周期入口（放在 WndProc 之前，与其它平台入口相邻）
    anchor = "/* ---- 键盘处理：抽成独立函数"
    assert s.count(anchor) == 1, "输入入口锚点不唯一"
    s = s.replace(anchor, INPUT_ENTRIES + anchor)
    print("  ✔ 插入 gamePointerMove/Down/Up + gameInitAll/gameShutdownAll")

    # ② WndProc 的鼠标分支改调新入口
    assert s.count(MOUSE_OLD) == 1, "WndProc 鼠标段锚点不唯一: %d" % s.count(MOUSE_OLD)
    s = s.replace(MOUSE_OLD, MOUSE_NEW)
    print("  ✔ WndProc 的鼠标分支改为调用平台无关入口")

    # ③ WndProc 包进 PLAT_HAS_OWN_MAIN（它本质是消息分发，Windows 专有）
    wa = "static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)"
    assert s.count(wa) == 1
    s = s.replace(wa, "#if PLAT_HAS_OWN_MAIN\n" + wa)
    # 找 WndProc 的收尾：后面紧跟的是 gameBoot（handleKey 在 WndProc **之前**，
    # 第一版拿 handleKey 当锚点所以没找到 —— 这里用 gameBoot 作界标）
    i = s.index(wa)
    j = s.index("\nvoid gameBoot(void)", i)
    s = s[:j] + "\n#endif  /* PLAT_HAS_OWN_MAIN */" + s[j:]
    print("  ✔ WndProc 包进 #if PLAT_HAS_OWN_MAIN")

    # ④ WinMain 收尾改调 gameShutdownAll()
    k = s.index("    /* ---------- 清理 ---------- */")
    m = s.index("    gpuRelease();", k)
    m = s.index("\n", m) + 1
    s = s[:k] + "    /* ---------- 清理 ---------- */\n    gameShutdownAll();\n" + s[m:]
    print("  ✔ WinMain 收尾改为 gameShutdownAll()")

    # ⑤ WinMain 启动改调 gameInitAll()
    old_boot = "    createLayers(hWnd);"
    if s.count(old_boot) == 1:
        s = s.replace(old_boot, "    createLayers(hWnd);        /* 窗口相关的层建立留在桌面端 */")
    old_gb = "    gameBoot();          /* 字体 + 载图 + 开局（与网页版共用，见其定义处） */"
    if s.count(old_gb) == 1:
        s = s.replace(old_gb, "    gameBoot();          /* 字体 + 载图 + 开局（与网页版共用，见其定义处） */")

    io.open(SRC, "w", encoding="utf-8", newline="\n").write(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
