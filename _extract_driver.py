# -*- coding: utf-8 -*-
"""把 pvz.c 的"驱动接口"从 WinMain 里抽出来，供网页版 / iOS 复用。

【为什么要抽】
  网页版（以及被暂停的 iOS 版）都没有 Win32 消息泵，它们的驱动方式完全不同：
    桌面端  while(running) { PeekMessage; dt; render; Sleep }
    网页端  requestAnimationFrame(t) { dt; step; putImageData }
    iOS      CADisplayLink { dt; step; present }
  但"每一帧要做什么"是同一件事：推进界面计时 → 更新逻辑 → 渲染。
  所以把它抽成 `gameStep(dt, out)`，各平台只写自己的循环外壳。

【抽出的三件东西】
  gameBoot()       —— 字体 + 载图 + 开局（不含窗口，窗口是平台自己的事）
  gameStep(dt,out) —— 一帧：计时 / 更新 / 渲染，并记录渲染耗时
  gameShutdown()   —— 释放贴图
  外加 gameWorldBits() —— 让平台层取回世界层像素（web 要上传到 canvas）

【为什么 out 是参数而不是内部 GetDC】
  桌面端是 `GetDC(hWnd)`；网页版没有窗口，需要传一块"假的"输出 DC，
  让 render() 的呈现步骤写进它、而不是污染 gWorldDC
  （这个坑在 _test_cards50_visual.c 里踩过：把 gWorldDC 当 out 传，
   世界层会被自己的 1x 缩略图覆盖，画面出现两套尺度）。

【幂等】开头检查 gameStep 是否已存在。本环境的 heredoc 会执行两次，
  所有补丁脚本都必须扛住重复执行。

用法：python _extract_driver.py
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "pvz.c")

MARK = "int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)"

# ---------------------------------------------------------------- 抽取出来的驱动
DRIVER = '''/* ==================== 平台无关的驱动接口 ====================
   见 _extract_driver.py 的说明：桌面 / 网页 / iOS 三端共用这三个函数，
   各自的循环外壳（消息泵 / requestAnimationFrame / CADisplayLink）在外层。

   ⚠️ 这三个必须是**非 static**：网页版把 pvz.c 与 plat_web.c 编成两个
      编译单元，plat_web.c 要能链接到它们。（桌面端用不到跨单元调用，
      但接口一致比省一个符号重要。） */

/* 开局：字体 + 美术资源 + 重置到主菜单。
   不含窗口创建 —— 窗口是平台的私事（网页版根本没有窗口）。 */
void gameBoot(void)
{
    /* ---------- 字体 ---------- */
    gF15 = mkFont(15, 0, L"微软雅黑");
    gF18 = mkFont(18, 1, L"微软雅黑");
    gF22 = mkFont(22, 1, L"微软雅黑");
    gF30 = mkFont(30, 1, L"微软雅黑");
    gF54 = mkFont(54, 1, L"微软雅黑");
    gF72 = mkFont(72, 1, L"微软雅黑");

    gArtLoaded = artLoadAll();
    resetGame();
    gState = ST_MENU;
}

/* 平台层用来给"上一帧渲染花了多久"做节流（桌面端据此 Sleep 到 60fps）。 */
float gLastFrameUsed = 0.0f;

/* 一帧：界面计时 → 更新 → 渲染。
   `out` 是呈现目标 DC：桌面端传窗口 DC，网页端传一块假 DC
   （这样 render 的呈现步骤不会覆盖 gWorldDC，调用方随后自己取像素上传）。 */
void gameStep(float dt, HDC out)
{
    float used;
    DWORD t0;

    /* 界面计时：切状态就归零，入场动画（翻牌等）依赖它 */
    if (gState != gScreenPrev) {
        gScreenPrev = gState;
        gScreenT = 0.0f;
        /* 卡片选中态（gSel / gShovel）**只属于战斗**。
           一旦切到战斗以外的界面就作废，免得它跨界面存活 ——
           表现就是"没人点过、卡片却亮着"（实测：菜单按数字键把 gSel
           设成编组里的植物 id，进战斗后那张卡就自己亮了）。
           ST_PLAY ↔ ST_PAUSE 之间不清：暂停时卡片栏仍然画着，
           选中态该保留，恢复后接着种。
           这一道是兜底 —— 真正的根因修复在 WndProc 的数字键分支。 */
        if (gState != ST_PLAY && gState != ST_PAUSE) { gSel = -1; gShovel = 0; }
    }
    else                          gScreenT += dt;

    if (gState == ST_PLAY) updateGame(dt);
    else {
        gTime += dt; weatherUpdate(dt); if (gLvUpT > 0.0f) gLvUpT -= dt;
        /* 庆典演出发生在 ST_DRAFT（属于"非战斗"分支），
           所以必须在这里推进计时 —— 放 updateGame 里的话演出根本不会走。 */
        celebUpdate(dt);
    }

    t0 = GetTickCount();
    render(out);
    used = (float)((DWORD)(GetTickCount() - t0)) / 1000.0f;

    /* 记录渲染耗时给诊断用（见 _mk_framedump.py 的 perf 行）。 */
    gDbgRenderMs = used * 1000.0f;
    if (gDbgFrameCnt == 0) gDbgPerfT0 = GetTickCount();
    gDbgFrameCnt++;
    if (gDbgFrameCnt > 5) { gDbgSumRender += (double)gDbgRenderMs; gDbgPerfN++; }
    gLastFrameUsed = used;
}

/* 让平台层取回"世界层"的像素与尺寸 —— 网页版每帧把它上传到 canvas。
   返回的是 gWorldBits（BGRA、自上而下、行距 w*4）；w/h 是**设备像素**
   （= 逻辑尺寸 × SS），与 spriteBlit 的内部约定一致。 */
const void *gameWorldBits(int *w, int *h, int *stride)
{
    if (w)      *w = VIEW_W * SS;
    if (h)      *h = VIEW_H * SS;
    if (stride) *stride = VIEW_W * SS * 4;
    return gWorldBits;
}

'''

# WinMain 里要删掉的部分（已经搬进 gameBoot）
BOOT_BLOCK_OLD = '''    /* ---------- 字体 ---------- */
    gF15 = mkFont(15, 0, L"微软雅黑");
    gF18 = mkFont(18, 1, L"微软雅黑");
    gF22 = mkFont(22, 1, L"微软雅黑");
    gF30 = mkFont(30, 1, L"微软雅黑");
    gF54 = mkFont(54, 1, L"微软雅黑");
    gF72 = mkFont(72, 1, L"微软雅黑");

    /* ---------- 载入美术资源（抽象成 artLoadAll，见其定义处的说明） ---------- */
    gArtLoaded = artLoadAll();

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    resetGame();
    gState = ST_MENU;
'''
BOOT_BLOCK_NEW = '''    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    gameBoot();          /* 字体 + 载图 + 开局（与网页版共用，见其定义处） */
'''

LOOP_OLD = '''        if (dt > 0.05f) dt = 0.05f;
        if (dt < 0.0005f) dt = 0.0005f;

        /* 界面计时：切状态就归零，入场动画（翻牌等）依赖它 */
        if (gState != gScreenPrev) {
            gScreenPrev = gState;
            gScreenT = 0.0f;
            /* 卡片选中态（gSel / gShovel）**只属于战斗**。
               一旦切到战斗以外的界面就作废，免得它跨界面存活 ——
               表现就是"没人点过、卡片却亮着"（实测：菜单按数字键把 gSel
               设成编组里的植物 id，进战斗后那张卡就自己亮了）。
               ST_PLAY ↔ ST_PAUSE 之间不清：暂停时卡片栏仍然画着，
               选中态该保留，恢复后接着种。
               这一道是兜底 —— 真正的根因修复在 WndProc 的数字键分支。 */
            if (gState != ST_PLAY && gState != ST_PAUSE) { gSel = -1; gShovel = 0; }
        }
        else                          gScreenT += dt;

        if (gState == ST_PLAY) updateGame(dt);
        else {
            gTime += dt; weatherUpdate(dt); if (gLvUpT > 0.0f) gLvUpT -= dt;
            /* 庆典演出发生在 ST_DRAFT（属于"非战斗"分支），
               所以必须在这里推进计时 —— 放 updateGame 里的话演出根本不会走。 */
            celebUpdate(dt);
        }

        t0 = GetTickCount();
        {
            HDC hdc = GetDC(hWnd);
            render(hdc);
            ReleaseDC(hWnd, hdc);
        }

        used = (float)((DWORD)(GetTickCount() - t0)) / 1000.0f;
        /* 记录渲染耗时给诊断用（见 _mk_framedump.py 的 perf 行）。
           升到 1661×1080 之后这一步会不会成为瓶颈，只看代码判断不出来。 */
        gDbgRenderMs = used * 1000.0f;
        if (gDbgFrameCnt == 0) gDbgPerfT0 = GetTickCount();
        gDbgFrameCnt++;
        if (gDbgFrameCnt > 5) { gDbgSumRender += (double)gDbgRenderMs; gDbgPerfN++; }
        {
            float rest = 1.0f / 60.0f - used;
            if (rest > 0.001f) Sleep((DWORD)(rest * 1000.0f));
            else if (rest > 0.0f) Sleep(1);
        }
    }
'''
LOOP_NEW = '''        if (dt > 0.05f) dt = 0.05f;
        if (dt < 0.0005f) dt = 0.0005f;

        /* 一帧的全部工作都在 gameStep 里 —— 网页版 / iOS 调的是同一个函数，
           区别只是它们的循环外壳（rAF / CADisplayLink）与呈现方式。 */
        {
            HDC hdc = GetDC(hWnd);
            gameStep(dt, hdc);
            ReleaseDC(hWnd, hdc);
        }
        {
            float rest = 1.0f / 60.0f - gLastFrameUsed;
            if (rest > 0.001f) Sleep((DWORD)(rest * 1000.0f));
            else if (rest > 0.0f) Sleep(1);
        }
    }
'''


def main():
    s = io.open(SRC, encoding="utf-8").read()
    if "void gameStep(float dt, HDC out)" in s:
        print("  已抽过，跳过（幂等）")
        return 0

    # ① 驱动接口：插在 WinMain 之前
    assert s.count(MARK) == 1, "WinMain 签名不唯一: %d" % s.count(MARK)
    s = s.replace(MARK, DRIVER + MARK)
    print("  ✔ 插入 gameBoot / gameStep / gameShutdown / gameWorldBits")

    # ② WinMain 里删掉已搬走的部分
    assert s.count(BOOT_BLOCK_OLD) == 1, "启动块锚点不唯一"
    s = s.replace(BOOT_BLOCK_OLD, BOOT_BLOCK_NEW)
    print("  ✔ WinMain 的启动块改为调用 gameBoot()")

    # ③ 主循环体换成 gameStep
    assert s.count(LOOP_OLD) == 1, "主循环块锚点不唯一"
    s = s.replace(LOOP_OLD, LOOP_NEW)
    print("  ✔ 主循环体改为调用 gameStep()")

    # ④ WinMain 加条件编译：网页/iOS 自己提供 main。
    #    ⚠️ 收尾锚点不能用 `return (int)msg.wParam;` —— 这个 WinMain 的收尾
    #    其实是 `return 0;`（前面是一串 DeleteObject），第一版就是踩了这个。
    #    改用"字体释放 + return 0 + 收尾花括号"这一整段，唯一且稳定。
    s = s.replace(MARK, "#if PLAT_HAS_OWN_MAIN\n" + MARK)
    tail = ("    DeleteObject(gF15); DeleteObject(gF18); DeleteObject(gF22);\n"
            "    DeleteObject(gF30); DeleteObject(gF54); DeleteObject(gF72);\n"
            "    return 0;\n}\n")
    assert s.count(tail) == 1, "WinMain 收尾不唯一: %d" % s.count(tail)
    s = s.replace(tail, tail + "\n#endif  /* PLAT_HAS_OWN_MAIN */\n")
    print("  ✔ WinMain 包进 #if PLAT_HAS_OWN_MAIN")

    io.open(SRC, "w", encoding="utf-8", newline="\n").write(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
