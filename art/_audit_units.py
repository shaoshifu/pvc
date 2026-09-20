# -*- coding: utf-8 -*-
"""量【归档后 BMP】的边缘残留 —— 这才是玩家真正看到的东西。

为什么不能只看 _build_units.py 报的"离散"：
  那个数是**源图四边框**的方差，描述的是"输入背景有多花"，
  不是"输出抠得干不干净"。P26 的源图是径向渐变（边框离散 16.1），
  但泛洪抠完之后可能一点都不剩 —— 用一个输入指标去判输出，
  会把已经修好的图继续标红，也会放过"源图很纯但边缘留了一圈洋红"的图。

真正该量的量（本脚本）：
  1. 边缘残留：最外圈 6 px（必定是背景区）里 alpha > 40 的像素占比。
     抠得干净 -> 0；留有半透明光晕 -> 明显大于 0。
  2. 洋红/紫粉残留：整图里「色相接近出图背景、且 alpha 在 40~220」的像素数。
     这是模型把角色光晕烘焙进背景后最常见的脏边形态。
  3. 内容占比：alpha>200 的比例，用来发现"抠过头把角色也吃了"。

用法：python art/_audit_units.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _unitlib as U                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DST = os.path.join(os.path.dirname(HERE), "assets")


def audit(path):
    """返回 (边缘残留%, 洋红脏边px, 不透明%, 尺寸)。读不出的返回 None。"""
    try:
        a, w, h = U.read_bmp32(path)
    except Exception as e:
        return ("读失败:%s" % e, None, None, None)

    al = a[:, :, 3].astype(np.float32)
    rgb = a[:, :, :3].astype(np.float32)
    # 预乘过：反预乘回来看真实颜色，否则半透明像素的 rgb 被压暗，判色相会偏
    a3 = np.maximum(al[:, :, None] / 255.0, 1e-3)
    true = np.clip(rgb / a3, 0, 255)

    r, g, b = true[:, :, 0], true[:, :, 1], true[:, :, 2]

    ring = 6
    edge = np.zeros_like(al, bool)
    edge[:ring, :] = edge[-ring:, :] = edge[:, :ring] = edge[:, -ring:] = True
    edge_res = float(((al > 40) & edge).sum()) / max(1, int(edge.sum())) * 100.0

    # 洋红/紫粉：R 高、B 高、G 明显低
    magenta = (r > 120) & (b > 90) & (r - g > 45) & (b - g > 12)
    dirty = int((magenta & (al > 40) & (al < 220)).sum())

    return (edge_res, dirty, float((al > 200).mean() * 100.0), "%dx%d" % (w, h))


def main():
    keys = (["rgplant_%02d" % i for i in range(50)] +
            ["rgzombie_%02d" % i for i in range(50)])
    bad = []
    print("%-16s %8s %8s %8s  %s" % ("文件", "边缘残留%", "洋红脏边", "不透明%", "尺寸"))
    for k in keys:
        p = os.path.join(DST, k + ".bmp")
        if not os.path.exists(p):
            print("%-16s 缺失" % k)
            bad.append((k, "缺失"))
            continue
        e, d, o, sz = audit(p)
        if d is None:
            print("%-16s %s" % (k, e))
            bad.append((k, str(e)))
            continue
        flag = ""
        if e > 0.35:
            flag += " !!边缘残留"
        if d > 900:
            flag += " !!洋红脏边"
        if o < 12:
            flag += " !!抠过头"
        if flag:
            bad.append((k, flag.strip()))
        print("%-16s %8.2f %8d %8.1f  %s%s" % (k, e, d, o, sz, flag))

    print("\n" + "=" * 62)
    if bad:
        print("需要处理 %d 项：" % len(bad))
        for k, why in bad:
            print("  %-16s %s" % (k, why))
    else:
        print("100 张全部干净：无边缘残留、无洋红脏边、无抠过头")
    return 0


if __name__ == "__main__":
    sys.exit(main())
