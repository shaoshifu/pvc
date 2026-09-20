#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核对 assets/ 是否齐备 —— 供 CI 在构建前快速确认资源没漏。

【这个脚本的边界（很重要）】
  它**不做**"引擎引用的每个名字都必须存在"这种断言。原因是引擎里存在
  **设计上就没有美术**的槽位：

      plantFile[PT_COUNT] 里有 plant_01 … plant_50 共 50 个名字，
      而 assets/ 下只有 plant_51 … plant_84。
      前 50 个是"没有专属贴图、走程序化绘制"的植物 —— 这是引擎的既定设计
      （`drawPlantShapeProc()` 那条回退路径），不是缺资源。

  第一版脚本按"名字必须存在"来判，结果报了 56 个"缺失"，全是误报，
  其中还包括 `card` / `gold` 这类**音效名与按钮样式名**。
  一个会误报的检查比没有检查更糟 —— 它会让 CI 长期红着，然后被人忽略。

  所以这里只做两件**确定有意义**的事：
    ① 总量阈值：PNG+JPG 数量、音效数量、BGM 数量（防"整个目录没打进来"）
    ② 少量**逐一验证过的**必需文件（草坪/背景/BGM/音效/关键角色贴图）
  其余的只打印统计，不参与判定。

【退出码】
  0 = 通过    1 = 有硬性缺失
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")

# ── 硬性阈值（取实际值往下留一点余量，防的是"大面积缺失"而不是"少一张"）──
MIN_PNG_JPG = 850      # 实际 859
MIN_SFX     = 30       # 实际 32
MIN_BGM     = 15       # 实际 18

# ── 逐一验证过的必需文件（**后缀都是从实际文件确认过的**）──
# ⚠️ 第一版这里写了 assets/lawn.png 与 assets/ui_menu_bg_v1.png，
#    而实际是 .jpg —— 于是 CI 在这里 exit 1、整个构建失败，
#    报错还看起来像"资源没打包进去"。加文件名前先 ls 确认后缀。
REQUIRED = [
    ("assets/lawn.jpg",              "主草坪（最核心的一张图）"),
    ("assets/background.jpg",        "后院场景背景"),
    ("assets/bgm_level1.mp3",        "第一关 BGM"),
    ("assets/sfx/hit.wav",           "命中音效"),
    ("assets/sfx/draft.wav",         "三选一音效"),
    ("assets/rgplant_00.png",        "肉鸽植物贴图（第 1 号）"),
    ("assets/rgplant_157.png",       "肉鸽植物贴图（最后一张，验证第三批新卡已打包）"),
    ("assets/badge_primordial.png",  "始祖档位徽章"),
    ("assets/fx_rays.png",           "开牌光效"),
]


def count(d):
    """返回 (png, jpg, bmp)"""
    p = j = b = 0
    if not os.path.isdir(d):
        return 0, 0, 0
    for fn in os.listdir(d):
        ext = os.path.splitext(fn)[1].lower()
        if ext == ".png":
            p += 1
        elif ext in (".jpg", ".jpeg"):
            j += 1
        elif ext == ".bmp":
            b += 1
    return p, j, b


def count_audio(d, exts):
    if not os.path.isdir(d):
        return 0
    return sum(1 for f in os.listdir(d)
               if os.path.splitext(f)[1].lower() in exts)


def main():
    quiet = "--quiet" in sys.argv
    fails = []

    if not os.path.isdir(ASSETS):
        print("  ★ 找不到 assets/ 目录")
        return 1

    n_png, n_jpg, n_bmp = count(ASSETS)
    n_sfx = count_audio(os.path.join(ASSETS, "sfx"), (".wav", ".mp3"))
    n_bgm = count_audio(os.path.join(ASSETS, "bgm"), (".wav", ".mp3"))

    if not quiet:
        print("== 资源核对 ==")
        print("  assets/      PNG %d / JPG %d（BMP 母版 %d，发行包不读）"
              % (n_png, n_jpg, n_bmp))
        print("  assets/sfx/  %d 个音频" % n_sfx)
        print("  assets/bgm/  %d 个音频" % n_bgm)
        print()

    # ① 总量阈值
    if n_png + n_jpg < MIN_PNG_JPG:
        fails.append("图片总数 %d < %d（资源可能大面积缺失）"
                     % (n_png + n_jpg, MIN_PNG_JPG))
    if n_sfx < MIN_SFX:
        fails.append("音效数 %d < %d" % (n_sfx, MIN_SFX))
    if n_bgm < MIN_BGM:
        fails.append("BGM 数 %d < %d" % (n_bgm, MIN_BGM))

    # ② 必需文件
    for rel, why in REQUIRED:
        p = os.path.join(HERE, rel)
        ok = os.path.isfile(p)
        if not quiet:
            print("  %s %-32s %s" % ("✓" if ok else "★", rel, why))
        if not ok:
            fails.append("缺 %s（%s）" % (rel, why))

    print()
    if fails:
        print("  ★ 失败 %d 项：" % len(fails))
        for f in fails:
            print("      · %s" % f)
        print("  FAILED")
        return 1

    print("  ✓ 资源核对通过（%d 张图 / %d 个音效 / %d 首 BGM）"
          % (n_png + n_jpg, n_sfx, n_bgm))
    print("  PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
