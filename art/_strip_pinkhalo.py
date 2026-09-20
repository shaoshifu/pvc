# -*- coding: utf-8 -*-
"""清掉「贴轮廓的粉雾」——出图模型在最高档提示词下必画的一层洋红泛光。

【为什么需要这个脚本】
  _unitlib 的注释里写着「贴轮廓的粉雾属源图缺陷，用重生成解决」。
  殿堂五尊（P100~P104）实测**重生成三轮都压不住**：
    · 第一轮：脚下补棕色泥土台；
    · 第二轮：加「no dirt mound / no ground disc」→ 泥土没了，改成脚下一圈白色发光底盘；
    · 第三轮：把「radiance / glowing」全部换成纯材质描述 → 光环还在，还多了一层洋红泛光。
  模型对"最高档角色"就是会补光环。继续赌提示词是在烧配额，
  所以这里改成**确定性后处理**：粉雾和暗影的 alpha 都不是满值、
  且色相与角色本体完全不重合，可以靠判据精确剔除。

【判据】
  粉雾 = 色相属于洋红/粉族 + 色度明显：
         `g < r - 20 且 g < b - 20 且 (mx - mn) > 50`
         即「G 是最低通道」。本体是 绿/青/黄白/暗绿/棕金，没有一个是粉紫系。
  为什么还要 `al < 250`：角色本体是不透明的（255），
         粉雾是背景色和泛光的混合，alpha 落在中间段。
         这一条把"角色自身的粉紫色装饰"排除在外（本批没有，但以后可能加）。
  下半部的粉族即使 alpha=255 也清 —— 模型有时把雾画成不透明的一块。

【用法】
    python art/_strip_pinkhalo.py P100 P101        # 直接从原图重跑并入库
    python art/_strip_pinkhalo.py --report P108 P109   # 只看判据命中量
    python art/_strip_pinkhalo.py --report P104    # 只看判据命中量，不写文件
"""
import os
import sys

import numpy as np
from scipy import ndimage as ndi      # ② 贴剪影光环用连通域判定

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                    # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "gen2")


def fix(arr):
    """arr: (h,w,4) uint8（matte 之后、裁切之前）。返回 (arr, 命中像素数)。"""
    a = arr.astype(np.float32)
    rgb = a[:, :, :3]
    al = a[:, :, 3].copy()
    r, g, b = rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]
    mx, mn = rgb.max(2), rgb.min(2)

    pink = (g < r - 20) & (g < b - 20) & ((mx - mn) > 50)
    h = al.shape[0]
    lower = np.zeros_like(pink)
    lower[int(h * 0.45):] = True

    # ② 上半部的"贴剪影光环"：判据是**结构**而不是颜色 ——
    #    与透明背景（8 邻域）相邻、且是亮品红的粉色连通域，就是贴着轮廓画的雾。
    #    为什么需要补这一条（2026-09-21 第三批 50 张实测）：
    #      上半部有一部分粉雾的 alpha 是 255（模型直接画成不透明的一块），
    #      原来 `al < 250` 那一条放过了它们 → 角色身上顶着粉紫色矩形块，
    #      在草坪绿底上非常显眼。P138/P142 残留量分别在 1.9%/5.0%。
    #    为什么必须带 mx > 150：
    #      本批有**深紫系本体**（P152 暗夜兰、P155 橙木守御的暗紫花冠，
    #      残留像素 mx 中位只有 67），它们的紫色装饰同样满足"G 最低通道"。
    #      只清亮品红（mx > 150）能把"雾"和"本体配色"分开 ——
    #      雾是发光的浅洋红，本体是暗紫。
    #    为什么加"邻接透明区"：
    #      本体上的紫色装饰被身体包围，不与背景相邻；贴轮廓的雾必然相邻。
    #    两条一起用，误伤概率极低；且它**只缩小**清除范围（比 v1 更保守的方向
    #    是相反的那条，所以这里独立成规则、与 v1 并列而不是替换）。
    lab, _n = ndi.label(pink, structure=np.ones((3, 3), bool))
    near_bg = ndi.binary_dilation(al == 0, structure=np.ones((3, 3), bool))
    touching = set(np.unique(lab[near_bg & pink]).tolist()) - {0}
    halo = np.isin(lab, list(touching)) & (mx > 150) if touching else np.zeros_like(pink)

    kill = pink & (al > 0) & ((al < 250) | lower | halo)
    n = int(kill.sum())
    al[kill] = 0.0
    a[:, :, 3] = al
    return a.astype(np.uint8), n


def run(key, report_only=False):
    src = os.path.join(SRC, "u_%s.jpg" % key.lower())
    if not os.path.exists(src):
        return key, False, "缺源图 %s" % src
    im, c, spread = U.matte_file(src)
    arr = np.array(im)
    arr, n = fix(arr)
    frac = n / float(arr.shape[0] * arr.shape[1]) * 100
    if report_only:
        return key, True, "命中粉雾 %.1f%%（背景 %s 离散 %.1f）" % (frac, c.round(0).astype(int), spread)
    # 清完必须重裁 bbox，否则角色会顶着一条透明带悬浮（view 层按底边对齐）
    ys, xs = np.where(arr[:, :, 3] > 0)
    if len(xs) == 0:
        return key, False, "清完什么都没了"
    arr2 = arr[ys.min():ys.max() + 1, xs.min():xs.max() + 1]

    # 交给和 _build_units 完全一致的后续步骤：控尺寸 -> 预乘 -> 写盘 -> 回读校验
    import _build_units as B
    arr2 = B.limit(arr2, B.MAX_PX)
    name = "rgplant_%s" % key[1:]
    dst = os.path.join(ROOT, "assets", name + ".bmp")
    U.save_bmp32(B.premultiply(arr2), dst)
    err = B.verify(dst, arr2.shape[1], arr2.shape[0])
    if err:
        return key, False, "%s.bmp 校验失败：%s" % (name, err)
    return key, True, ("%s.bmp %dx%d 清粉雾%.1f%% 不透明%.0f%% 背景离散%.1f"
                       % (name, arr2.shape[1], arr2.shape[0], frac,
                          (arr2[:, :, 3] > 200).mean() * 100, spread))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    report = "--report" in sys.argv
    keys = args or ["P%d" % i for i in range(100, 105)]
    bad = 0
    for k in keys:
        k, ok, msg = run(k, report)
        print("  %s %s %s" % (k, "OK " if ok else "X  ", msg))
        if not ok:
            bad += 1
    print("\n%s %d / %d" % ("报告" if report else "入库", len(keys) - bad, len(keys)))


if __name__ == "__main__":
    main()
