# -*- coding: utf-8 -*-
"""
生成「自动驱动」的截图版 _shot.c / _shot.exe —— 不需要任何外部按键。

为什么不用 _mk_framedump.py 那套（_dump.exe + PostMessage 热键）：
  实测 PostMessageW(hwnd, WM_KEYDOWN, VK_F1, 0) 返回 1、窗口也健在，
  但 _frames 目录始终为空 —— 外部输入这条路有太多不确定环节
  （进程环境变量有没有传进去、PostMessage 是否被消息循环取到、
   窗口有没有焦点、按键分支前面有没有把消息吞掉的前置 if）。
  截图只需要一次性的证据，不值得在这些环节上耗时间。

  所以这里把「按键」搬进进程内部：render() 每帧调一次 shotAuto()，
  它按帧号走一遍**真实 UI 点击路径**（编组页点卡片 → 选关页点关卡），
  和 _test_loadout1.c 里的断言用的是同一套坐标函数。
  另外把导出目录写成编译期字面量（不再依赖 getenv），
  彻底排除「环境变量没传进子进程」这一类干扰。
"""

import os
import subprocess
import sys

import _mk_framedump as MF

SRC = "pvz.c"
DST = "_shot.c"
OUT_DIR = "_frames_loadout1"

# 编译期写死导出目录（C 字符串转义反斜杠）
_DIR_C = os.path.abspath(OUT_DIR).replace("\\", "\\\\")

# 在 render() 里、dumpMaybe 之前插入自动驱动。
AUTO = r'''
/* ================== 自动驱动（仅截图版） ==================
   按帧号走真实 UI 路径，把「编组页选卡 → 选关页开局」这一串点击
   在进程内部重放一遍；每个阶段留 3 帧写盘窗口。
   同一 tag 写的是同一个文件名，所以每个阶段实际只留最后一张。 */
static void shotAuto(void)
{
    static int f = 0;
    f++;
    if (f == 20) {
        /* 造一个「已经有别的植物被选中」的起始状态，
           这样后面的点击才是真的在验证"顶替"而不是"本来就只有它"。 */
        int i;
        for (i = 0; i < LV_COUNT; i++) gSave.lvUnlocked[i] = 1;
        memset(gSave.loadout, 0, sizeof(gSave.loadout));
        gSave.loadout[3] = 1;
        loadoutBuild();
        gState = ST_LOADOUT;
        gLoadoutPage = 0;
    }
    if (f == 24) { gDumpTag = 11; gDumpLeft = 3; }   /* 点击前的编组页 */
    if (f == 40) {
        float x, y;
        loLayout(0, &x, &y);
        onClick((int)(x + LO_CARD_W * 0.5f), (int)(y + LO_CARD_H * 0.5f));
    }
    if (f == 44) { gDumpTag = 10; gDumpLeft = 3; }   /* 点击后的编组页 */
    if (f == 60) {
        float x, y;
        metaLayoutLevel(0, &x, &y);
        gState = ST_LEVELS;
        onClick((int)(x + LV_CARD_W * 0.5f), (int)(y + LV_CARD_H * 0.5f));
    }
    if (f == 64) { gDumpTag = 1;  gDumpLeft = 3; }   /* 开局战场（卡槽应只有 1 张） */
}
'''


def main():
    src = open(SRC, encoding="utf-8").read()
    n0 = len(src)

    hook = '#define SHOT_DIR "%s"\n' % _DIR_C + MF.HOOK
    # 把两处 getenv("PVZ_DUMPDIR") 换成编译期字面量
    hook = hook.replace('getenv("PVZ_DUMPDIR")', "SHOT_DIR")
    stats = MF.STATS_BODY.replace('getenv("PVZ_DUMPDIR")', "SHOT_DIR")
    if "SHOT_DIR" not in hook or "SHOT_DIR" not in stats:
        sys.exit("[X] getenv 替换失败，dump 钩子结构变了")
    # dumpMaybe / dumpStats 里的局部变量 dir 保留（现在指向字面量）

    src = MF.patch(src,
                   "static HFONT gF15, gF18, gF22, gF30, gF54, gF72;\n",
                   "static HFONT gF15, gF18, gF22, gF30, gF54, gF72;\n" + hook,
                   "字体全局变量声明")

    src = MF.patch(src,
                   "static void render(HDC out)\n",
                   stats + AUTO + "static void render(HDC out)\n",
                   "render 之前插入 dumpStats + shotAuto")

    # ⚠️ 钩子必须挂在 **gpuDrawFrame 之前**，不能挂在 render 末尾。
    #    render 里有一处 `if (gpuDrawFrame(...)) return;`（GPU/D3D 路径），
    #    而本机标题就是「· GPU」—— 也就是说正常情况下 render 从这里就返回了，
    #    末尾那两行永远执行不到。这正是「PostMessage 热键返回 1 但一张图都没有」
    #    的真正原因：不是按键没送到，是抓帧代码根本不在执行路径上。
    #    另外 GPU 路径读的是 gWorldBits（VIEW_W*SS 的 2x 画布），
    #    所以要从 gWorldDC 抓 —— gFrameDC 只在 GDI 回退路径里才被填。
    #    此处 gWorldDC 的变换刚被复位成 identity（见上一行注释），
    #    dumpFrameBMP 再复位一次是幂等的，也不会影响后面 GPU 路径。
    src = MF.patch(src,
                   "        if (gpuDrawFrame(cw, ch, dx, dy, dw, dh)) return;\n",
                   "        shotAuto();\n"
                   "        dumpMaybe(gWorldDC);\n"
                   "        if (gpuDrawFrame(cw, ch, dx, dy, dw, dh)) return;\n",
                   "gpuDrawFrame 提前返回点（抓帧必须挂在这里）")

    src = MF.patch(src,
                   "        else if (wp == 'A') { gAutoSun = !gAutoSun; }\n",
                   MF.HOTKEYS + "        else if (wp == 'A') { gAutoSun = !gAutoSun; }\n",
                   "WM_KEYDOWN 的按键分支（保留手动热键，便于人工复核）")

    open(DST, "w", encoding="utf-8", newline="\n").write(src)
    print("[1/2] %s 已生成（%d -> %d 字节），导出目录写死为 %s"
          % (DST, n0, len(src), _DIR_C))

    cmd = ["gcc", "-O2", "-mwindows", "-static-libgcc", "-o", "_shot.exe", DST,
           "-lgdi32", "-luser32", "-lmsimg32", "-lwinmm", "-ld3d9", "-lm",
           "-Wall", "-Wextra"]
    print("[2/2] " + " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    if out:
        print(out[:4000])
    if r.returncode != 0:
        sys.exit("[X] 编译失败 returncode=%d" % r.returncode)
    print("[OK] _shot.exe 编译成功")


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)) or ".")
    main()
