# -*- coding: utf-8 -*-
"""把 assets/ 裁剪成"网页版真正需要"的一套，放进 web/assets/。

【为什么必须裁剪】
  原始 assets/ 有 **375MB**，其中大部分是 **BMP 与 WAV 母版**：
    · 845 个 .bmp（未压缩，单张最大 10MB）—— 它们是打包流程的中间产物，
      引擎在正常发行包里根本不读（spriteLoadName 先找 .png / .jpg，
      .bmp 只是"还没跑打包"时的兜底）；
    · 34MB 的 bgm_level1.wav —— 已转成 3.1MB 的 .mp3。
  直接 `--preload-file assets` 会把这 375MB 全塞进 .data，
  网页版首屏要下几分钟，完全不可接受。

  裁剪规则（与引擎的加载顺序严格对应）：
    .png / .jpg            → 保留（引擎的首选路径）
    .bmp                   → **丢弃**（除非同名的 png/jpg 不存在 —— 见下）
    sfx/*.wav              → 保留（音效，约 1.3MB）
    *.mp3 / bgm/*.mp3      → 保留（关卡 BGM，走 HTMLAudio 流式）

  ⚠️ 例外：**没有 PNG/JPG 替代的 BMP 必须保留**。
     实测有 50 个这样的文件（rgplant_108..157，第三批新卡当时漏跑了打包）。
     漏掉它们的话，那 50 张卡在网页版会静默退回程序化绘制 ——
     而这个坑本项目已经踩过一次（见 _build_web_local.sh 与 CARDS_BATCH3.md）。

用法：
    python _prepare_web_assets.py            # 输出到 web/assets
    python _prepare_web_assets.py --check    # 只报告体积，不复制
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "assets")
DST = os.path.join(HERE, "web", "assets")

KEEP_DIRS = ("sfx", "bgm")
KEEP_EXT = (".png", ".jpg", ".jpeg", ".mp3")


def collect():
    """返回 (要复制的文件列表, 统计信息)"""
    keep = []
    stats = {"png": 0, "jpg": 0, "bmp_kept": 0, "bmp_dropped": 0,
             "audio": 0, "other_dropped": 0, "bytes": 0, "dropped_mb": 0.0}

    # ① 顶层文件
    names = set()
    for fn in os.listdir(SRC):
        p = os.path.join(SRC, fn)
        if not os.path.isfile(p):
            continue
        base, ext = os.path.splitext(fn)
        ext = ext.lower()
        names.add(base)
        if ext in KEEP_EXT:
            keep.append((p, fn))
            stats["png" if ext == ".png" else "jpg"] += 1
            stats["bytes"] += os.path.getsize(p)
        elif ext == ".bmp":
            # 有同名 png/jpg 才丢；否则必须留（见文件头说明）
            has_alt = (os.path.exists(os.path.join(SRC, base + ".png")) or
                       os.path.exists(os.path.join(SRC, base + ".jpg")))
            if has_alt:
                stats["bmp_dropped"] += 1
                stats["dropped_mb"] += os.path.getsize(p) / 1048576.0
            else:
                # BMP 不能直接给浏览器用，转成 PNG 再放（体积也小得多）
                keep.append((p, base + ".png", True))
                stats["bmp_kept"] += 1
                stats["bytes"] += os.path.getsize(p) // 4   # 估：PNG 约为 BMP 的 1/4
        else:
            stats["other_dropped"] += 1
            stats["dropped_mb"] += os.path.getsize(p) / 1048576.0

    # ② 子目录（sfx / bgm）
    for d in KEEP_DIRS:
        dp = os.path.join(SRC, d)
        if not os.path.isdir(dp):
            continue
        for fn in sorted(os.listdir(dp)):
            p = os.path.join(dp, fn)
            if not os.path.isfile(p):
                continue
            if os.path.splitext(fn)[1].lower() in KEEP_EXT:
                keep.append((p, d + "/" + fn))
                stats["audio"] += 1
                stats["bytes"] += os.path.getsize(p)

    return keep, stats


def main():
    check_only = "--check" in sys.argv
    keep, st = collect()

    print("  保留：PNG %d / JPG %d / BMP 转 PNG %d / 音频 %d" %
          (st["png"], st["jpg"], st["bmp_kept"], st["audio"]))
    print("  丢弃：BMP（有 PNG/JPG 替代的）%d 个，其它 %d 个，合计约 %.1f MB" %
          (st["bmp_dropped"], st["other_dropped"], st["dropped_mb"]))
    print("  预计输出体积：约 %.1f MB（%d 个文件）" % (st["bytes"] / 1048576.0, len(keep)))

    if check_only:
        return 0

    if os.path.isdir(DST):
        shutil.rmtree(DST)
    os.makedirs(os.path.join(DST, "sfx"), exist_ok=True)
    os.makedirs(os.path.join(DST, "bgm"), exist_ok=True)

    n = 0
    for item in keep:
        if len(item) == 3:
            srcp, name, need_conv = item
            dstp = os.path.join(DST, name)
            if need_conv:
                convert_bmp_to_png(srcp, dstp)
            n += 1
        else:
            srcp, name = item
            dstp = os.path.join(DST, name)
            os.makedirs(os.path.dirname(dstp), exist_ok=True)
            shutil.copy2(srcp, dstp)
            n += 1

    total = sum(os.path.getsize(os.path.join(r, f))
                for r, _, fs in os.walk(DST) for f in fs)
    print("  已写入 %s：%d 个文件，%.1f MB" % (DST, n, total / 1048576.0))
    return 0


def convert_bmp_to_png(src, dst):
    """把引擎用的 32 位 BMP 转成 PNG。

    ⚠️ 不做任何 alpha 变换：BMP 里存的是**已预乘**的 BGRA
      （引擎的约定），PNG 也存预乘字节，只换通道顺序。
      顺手做预乘会双重乘、颜色发暗。
    """
    import struct
    import numpy as np
    from PIL import Image
    b = open(src, "rb").read()
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bh = abs(h)
    a = np.frombuffer(b[off:off + w * bh * 4], np.uint8).reshape(bh, w, 4)
    if h > 0:
        a = a[::-1]                       # 自下而上 → 自上而下
    Image.fromarray(a[:, :, [2, 1, 0, 3]], "RGBA").save(dst, optimize=True)


if __name__ == "__main__":
    sys.exit(main())
