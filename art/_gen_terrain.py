# -*- coding: utf-8 -*-
"""用 mmx CLI 生成地形贴图（替换原来用 GDI 基元硬画的粗糙版本）。

用法：
    python art/_gen_terrain.py            # 生成全部
    python art/_gen_terrain.py scorch     # 只生成某一张

产物落在 art/gen2/<key>.jpg（纯洋红底，方便抠图）。
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, "gen2")

STYLE = ("cartoon stylized 2D game asset, Plants vs Zombies art style, "
         "vibrant saturated colors, bold clean outlines, crisp edges, "
         "high detail, top-down orthographic, centered filling the frame, "
         "isolated on a solid pure magenta (#FF00FF) background, "
         "no text, no watermark, no border, no frame")

TERRAIN = {
    "th_scorch": ("a square patch of scorched burnt earth, cracked blackened soil "
                  "with glowing orange embers deep in the cracks, thin wisps of smoke"),
    "th_ice":    ("a square patch of thick glossy blue ice, frozen frosted surface "
                  "with sharp crystal shards and internal cracks, cold rim light"),
    "th_web":    ("a square patch of dense white spider webs, radial silk threads "
                  "with concentric rings and dew drops, draped over dark grass"),
    "th_corrode": ("a square patch of acid corroded ground, sickly yellow-green "
                   "bubbling slime pools eating into dark soil, toxic fumes"),
    "th_bloom":  ("a square patch of lush magical flowering moss, pink blossoms "
                  "with glowing yellow centers, emerald vines, soft warm glow"),
}


def gen(key):
    prompt = TERRAIN[key] + ", " + STYLE
    cmd = ["mmx", "image", "generate", "--prompt", prompt,
           "--aspect-ratio", "1:1", "--out-dir", OUT]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, shell=True)
    out = (r.stdout or "") + (r.stderr or "")
    # mmx 输出形如 "  - art\gen2\image_001.jpg"，取最后一行路径
    path = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("- ") and line.lower().endswith((".jpg", ".png", ".jpeg")):
            path = line[2:].strip().replace("\\", "/")
    if not path:
        print("  [X] %-12s 没有解析到产物\n%s" % (key, out[-400:]))
        return False
    src = os.path.join(ROOT, path)
    dst = os.path.join(OUT, key + os.path.splitext(src)[1])
    if os.path.abspath(src) != os.path.abspath(dst):
        shutil.move(src, dst)
    print("  [OK] %-12s -> %s" % (key, os.path.relpath(dst, ROOT)))
    return True


def main():
    os.makedirs(OUT, exist_ok=True)
    want = sys.argv[1:] or list(TERRAIN.keys())
    ok = 0
    for k in want:
        if k not in TERRAIN:
            print("  [X] 未知 key:", k)
            continue
        if gen(k):
            ok += 1
    print("完成 %d / %d" % (ok, len(want)))


if __name__ == "__main__":
    main()
