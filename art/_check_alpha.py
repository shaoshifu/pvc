# -*- coding: utf-8 -*-
"""检查所有贴图的抠图质量：透明像素占比 + 边框外圈是否真的透明。

判据：
  · 真正抠好的贴图，外圈 4px 应该全是 alpha=0
  · 透明像素占比过低（比如 < 15%）说明它是"一大块不透明"，即黑底方块
"""
import os
import numpy as np

D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")


def main():
    rows = []
    for f in sorted(os.listdir(D)):
        if not f.endswith(".bmp"):
            continue
        b = open(os.path.join(D, f), "rb").read()
        off = int.from_bytes(b[10:14], "little")
        w = int.from_bytes(b[18:22], "little", signed=True)
        h = int.from_bytes(b[22:26], "little", signed=True)
        a = np.frombuffer(b[off:], np.uint8).reshape(abs(h), abs(w), 4)
        al = a[:, :, 3]
        zf = float((al == 0).mean() * 100)
        edge = np.concatenate([al[:4].ravel(), al[-4:].ravel(),
                               al[:, :4].ravel(), al[:, -4:].ravel()])
        rows.append((f, zf, int(edge.min()), w, abs(h)))

    sprites = [r for r in rows if not r[0].startswith(("background", "lawn"))]
    sprites.sort(key=lambda r: r[1])

    print("贴图总数: %d" % len(sprites))
    print("")
    print("=== 透明占比最低的 35 张 ===")
    for f, zf, emin, w, h in sprites[:35]:
        flag = "   <== 边框不透明" if emin > 0 else ""
        print("  %-32s zfrac=%5.1f%%  edgeAlpha=%3d  %dx%d%s"
              % (f, zf, emin, w, h, flag))

    print("")
    print("=== 抠图正常的（占比最高的 8 张）===")
    for f, zf, emin, w, h in sprites[-8:]:
        print("  %-32s zfrac=%5.1f%%  edgeAlpha=%3d" % (f, zf, emin))

    bad_edge = [r for r in sprites if r[2] > 0]
    low_frac = [r for r in sprites if r[1] < 15.0]
    print("")
    print("边框不透明的贴图数 = %d / %d" % (len(bad_edge), len(sprites)))
    print("透明占比 < 15%% 的贴图数 = %d" % len(low_frac))


if __name__ == "__main__":
    main()
