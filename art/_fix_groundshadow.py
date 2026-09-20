# -*- coding: utf-8 -*-
"""剔除「贴地椭圆阴影」—— 从画面底边向上扫出椭圆带，不重生成。

【问题】art/feet_rgplant_00.png 放大可见、art/_diag_ascii.py 逐像素确认：
  模型无视了 STYLE 里的 "no ground shadow, no ellipse under the character"，
  给每只角色脚下烘焙了一片**深洋红/紫红椭圆**（P00 实测 RGB(73,19,81)）。
  它与纯洋红背景 #FF00FF 色距太远 —— 键控不会杀；
  于是成品脚下挂着一圈粉色。游戏里 drawContactShadow() 已自绘接触影，
  这层纯多余且颜色冲突。

【四版演进 —— 前三版都栽了，细节记在 detect() 的 docstring 里，别重踩】
  v1 独立连通域 + 宽高比    → 椭圆与角色同属一个连通域，全 100 张只命中 5 张
  v2 底边泛洪 + 参考色      → 脚踩到底边时参考色取成脚色，Z 系列整批漏清
  v3 逐连通域判形状         → 洋红系角色的装饰与椭圆相连，被 MAX_H 整块拒掉
  v4 底边向上扫带（现行）   → 逐行看"洋红占实体像素比"，连续不达标即停

【清除后必须重裁】spriteBlit() 以底边对齐 baseY。若只清像素不重裁，
  底部会留下一圈透明带，角色被"顶"到空中悬浮。故清完按 alpha bbox 重裁。

【调用位置很重要】_build_units.build() 在 trim 之后、make_fall_frames **之前**
  调用本模块。倒地帧是绕脚底旋转派生的：若先旋转再清，椭圆会被转成一个
  斜的洋红色块，而且不再贴底 —— 本模块的"从底边往上扫"前提直接失效。

用法：
  python art/_fix_groundshadow.py --dry            # 全部 100 张，只报数
  python art/_fix_groundshadow.py --dry P00 P03    # 指定
  python art/_fix_groundshadow.py --sheet          # 只出复核表，不改文件
  python art/_fix_groundshadow.py                  # 真改（重裁 + 覆写）
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DST = os.path.join(ROOT, "assets")

AL = 24            # 计入"实体像素"的 alpha 下限
ASPECT_MAX = 0.40  # 域 高/宽 上限（超过就是纵向的角色本体）
MAX_H = 0.30       # 域高 < 画高 * 该值
MIN_SPAN = 0.45    # 域宽 > 画宽 * 该值（不够宽的不是椭圆）
MAG_MARGIN = 18    # 洋红族判据：R>G+m 且 B>G+m
ROW_MAG_RATIO = 0.30   # 逐行"洋红占实体像素"达到该比例才算椭圆带的一行
ROW_GAP = 5            # 允许连续几行不达标（椭圆被脚/腿挡断时的过渡带）
ROUNDS = 4             # 迭代剥带的最大轮数（见 fix() 的说明）
MAX_KILL_FRAC = 0.09   # 清除面积 / 画布面积 超过该值 -> 判定为"大面积背景光"，整体放弃


def magenta_family(a, m=MAG_MARGIN):
    r = a[:, :, 0].astype(np.int32)
    g = a[:, :, 1].astype(np.int32)
    b = a[:, :, 2].astype(np.int32)
    return (r > g + m) & (b > g + m)


def detect(arr):
    """返回 (kill_mask, stat)。stat=None 表示没有找到贴地椭圆。

    判据演进（三版都试过，前两版都栽了，过程记在这里免得回头重踩）：

    v1 「找独立连通域，量它的宽高比」
       椭圆在 alpha>30 掩码里**和角色连成一片**，永远不是独立连通域 → 全 100 张只命中 5 张。

    v2 「从画面底边泛洪洋红族像素」
       a) 参考色取"最底部 3 行"时，只要僵尸的脚踩到画面底边，取的其实是**脚的颜色**，
          椭圆因为色距太远被排除 —— Z 系列整批漏清；
       b) 换成"最靠下的洋红像素"当种子后仍会漏：椭圆常被脚/腿切成左右两块，
          泛洪只顺着种子那一块走。

    v3 「对每个洋红族连通域各自判形状」（也放弃了）
       洋红系角色（时间悖论僵尸等）身上的装饰与椭圆同色且相连，
       并成一个很高的域，被 MAX_H 闸门整块拒掉。

    v4（现行）「从画面底边**向上扫**出椭圆带」
       椭圆的定义性特征是"它是画面最底部的、横向铺开的洋红块"。
       所以不用连通域，改成沿 y 从最底一行往上走，逐行看"洋红占该行实体像素的比例"：
       比例够高就纳入带，连续几行不达标就停。带内**所有**洋红像素清除。
       角色身上高处的洋红落不到带里，天然安全；
       再叠 MIN_SPAN / MAX_H / 高宽比三道闸门，防住"整只僵尸就是洋红色"的极端情况。
    """
    h, w = arr.shape[:2]
    al = arr[:, :, 3].astype(np.float32)
    solid = al > AL
    mag = solid & magenta_family(arr)

    # 只在下半部找，避免把角色上半身的紫色装饰当椭圆
    half = int(h * 0.5)
    mag[:half] = False
    ys_any = np.where(mag.any(axis=1))[0]
    if len(ys_any) < 2:
        return np.zeros((h, w), bool), None

    y_bot = int(ys_any.max())
    # 从最底部的洋红行往上扩，直到连续 GAP 行"洋红占比不足"
    y_top = y_bot
    gap = 0
    for y in range(y_bot, half - 1, -1):
        n_solid = int(solid[y].sum())
        if n_solid == 0:
            gap += 1
            if gap > ROW_GAP:
                break
            continue
        if int(mag[y].sum()) / float(n_solid) >= ROW_MAG_RATIO:
            y_top = y
            gap = 0
        else:
            gap += 1
            if gap > ROW_GAP:
                break

    band = np.zeros((h, w), bool)
    band[y_top:y_bot + 1] = mag[y_top:y_bot + 1]
    ys, xs = np.where(band)
    if len(xs) == 0:
        return np.zeros((h, w), bool), None

    x0, x1 = int(xs.min()), int(xs.max())
    cw = x1 - x0 + 1
    ch = y_bot - y_top + 1
    asp = ch / float(cw)
    if cw < w * MIN_SPAN:        # 不够宽 -> 只是角色底部的一点紫边
        return np.zeros((h, w), bool), None
    if ch >= h * MAX_H:          # 带太高 -> 整只角色都是洋红色，别动
        return np.zeros((h, w), bool), None
    if asp >= ASPECT_MAX:        # 不够扁 -> 同上
        return np.zeros((h, w), bool), None
    return band, dict(w=cw, h=ch, aspect=asp, area=int(len(xs)), y=y_bot, x0=x0)


def fix(arr, rounds=ROUNDS):
    """迭代清除：一轮只剥掉最底部的一条带，清完再扫，直到没有新目标。

    为什么要迭代：模型给不少僵尸画的不是"薄薄一片影子"，而是**一整块
    紫红色的地面台座**（Z00 实测跨满整个画面底部）。单次扫描从底边往上走，
    很快就被角色的双脚挡住 —— 脚那一行的洋红占比骤降，扫带中断，
    结果台座只被削掉下半截，上半截留在那里，比不清还难看。
    清掉下半截后再扫，新的"最底洋红行"就升到台座腰部，于是继续剥，
    通常 2~3 轮就能剥干净。ROUNDS 是硬上限，配合每轮的 MAX_H 闸门，
    保证不会一路剥到角色身上。
    """
    out = arr.copy()
    total = np.zeros(arr.shape[:2], bool)
    st_last = None
    for _ in range(rounds):
        kill, st = detect(out)
        if st is None:
            break
        out[kill] = 0
        total |= kill
        st_last = st
    if st_last is None:
        return arr, None

    # 最后的保命闸门：清除面积异常大时整体放弃。
    # 为什么需要：提示词里的"爆炸/光环"设定（P45 音乐盒樱桃、P14 时间漩涡）
    # 会让模型画**一整片粉色爆炸铺满背景**。那片粉满足洋红族、也贴底，
    # 前几道闸门都拦不住，迭代几轮会把角色脚下的部分一起啃掉，
    # 留下半截白色圆环 —— 比不清还难看。
    # 真正"贴地椭圆"的清除量实测都在 8% 以下（Z03 7.4% 是上限），
    # 所以 9% 这条线能干净地把"大面积背景光"区分出去。
    if total.sum() > out.shape[0] * out.shape[1] * MAX_KILL_FRAC:
        return arr, None
    st_last["area"] = int(total.sum())
    return out, st_last


def run(keys, dry):
    tot = 0
    for k in keys:
        pre = "rgplant_" if k[0] == "P" else "rgzombie_"
        p = os.path.join(DST, pre + k[1:] + ".bmp")
        if not os.path.exists(p):
            print("%-5s 缺" % k)
            continue
        a, w0, h0 = U.read_bmp32(p)
        na, st = fix(a)
        if st is None:
            if dry:
                print("%-5s  -" % k)
            continue
        tot += 1
        tag = ""
        if not dry:
            ys, xs = np.where(na[:, :, 3] > 0)
            if len(xs):
                na = na[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
            U.save_bmp32(na, p)
            tag = "  -> %dx%d" % (na.shape[1], na.shape[0])
        print("%-5s  %s 域 %dx%d(高宽比%.2f) 面积%4d%s"
              % (k, "将清" if dry else "已清", st["w"], st["h"], st["aspect"],
                 st["area"], tag))
    print("\n%s：%d / %d 张判定有贴地椭圆" % ("干跑" if dry else "已修", tot, len(keys)))


if __name__ == "__main__":
    argv = sys.argv[1:]
    dry = "--dry" in argv
    if "--sheet" in argv:
        import _qa_ellipse_sheet as Q
        Q.sheet("P", ["P%02d" % i for i in range(50)], os.path.join(HERE, "qa_ell_plants.png"))
        Q.sheet("Z", ["Z%02d" % i for i in range(50)], os.path.join(HERE, "qa_ell_zombies.png"))
        sys.exit(0)
    argv = [a for a in argv if not a.startswith("--")]
    ks = [a.upper() for a in argv] or (["P%02d" % i for i in range(50)] +
                                       ["Z%02d" % i for i in range(50)])
    run(ks, dry)
