# -*- coding: utf-8 -*-
"""批量生成 5 个新关卡的美术素材（mmx image）。

设计约束（让 AI 出图能直接进游戏）：
  · 统一风格锚点 —— 同一个游戏里风格必须一致，所以每条 prompt 都带同一段
    风格描述；否则每张图各画各的，拼在一个画面里会明显违和。
  · 场景图用 16:9（对应游戏 1000×650 的逻辑画面）。
  · 角色图强调"正面全身 / 纯色背景"，便于后续抠图。
  · 不写文字 —— AI 生成的字基本都是乱码，游戏里文字都是程序画的。

用法：
  python _gen_art.py            # 生成全部
  python _gen_art.py lv6        # 只生成某一关
"""
import os
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(D, "assets_new")
# ⚠️ Windows 上 mmx 是 .cmd 批处理，Python 的 subprocess 不能靠 PATH 找到
#    "mmx" 这个名字（WinError 2）—— 必须给出完整路径。
#    另外 .cmd 不能直接当可执行文件传给 CreateProcess，要显式走 cmd.exe /c。
MMX = r"C:\nvm4w\nodejs\mmx.cmd"

# 风格锚点：所有 prompt 共用，保证 5 个关卡画风一致
STYLE = ("stylized cartoon tower defense game art, clean vector-like shapes with bold "
         "outlines, vibrant saturated colors, soft shading, playful casual game style, "
         "no text, no letters, no watermark")

# (子目录, 文件名, 提示词, 宽高比)
JOBS = [
    # ================= 关 7：霜牙隘口（冰川）=================
    ("lv6_ice", "lawn_ice",
     "top-down view of a frozen ice lawn battlefield for a tower defense game, "
     "9 columns by 5 rows grid of cracked blue-white ice tiles, frost patterns, "
     "subtle embedded pebbles, even lighting, tileable game background, " + STYLE, "16:9"),
    ("lv6_ice", "hazard_thin_ice",
     "a single square thin ice tile with spider-web cracks, viewed from above, "
     "pale translucent blue, isolated on plain dark background, game tile asset, " + STYLE, "1:1"),
    ("lv6_ice", "zombie_frostfang",
     "full body front view of a cartoon zombie with an icicle crown on its head, "
     "pale blue frozen skin, tattered winter coat, arms outstretched, "
     "isolated on plain flat green background, game character sprite, " + STYLE, "1:1"),
    ("lv6_ice", "zombie_glacier",
     "full body front view of a huge hulking cartoon zombie made of glacier ice, "
     "crystal spikes on shoulders, glowing cold blue core, arms outstretched, "
     "isolated on plain flat green background, game boss sprite, " + STYLE, "1:1"),
    ("lv6_ice", "fx_snowstorm",
     "seamless snowstorm overlay texture, scattered snowflakes and wind streaks, "
     "white particles on pure black background for additive blending, game vfx, " + STYLE, "16:9"),

    # ================= 关 8：熔心裂谷（炼狱）=================
    ("lv7_magma", "lawn_magma",
     "top-down view of a scorched volcanic battlefield for a tower defense game, "
     "9 columns by 5 rows grid of dark basalt rock tiles with glowing orange lava "
     "seams between them, ash dust, even lighting, tileable game background, " + STYLE, "16:9"),
    ("lv7_magma", "hazard_rift",
     "a single square volcanic fissure tile erupting with molten lava and sparks, "
     "viewed from above, glowing orange and red, isolated on plain dark background, "
     "game tile asset, " + STYLE, "1:1"),
    ("lv7_magma", "zombie_magmaw",
     "full body front view of a cartoon zombie with cracked magma skin, glowing "
     "orange lava veins, charred black armor plates, arms outstretched, "
     "isolated on plain flat green background, game character sprite, " + STYLE, "1:1"),
    ("lv7_magma", "zombie_ashwalker",
     "full body front view of a lean cartoon zombie covered in grey volcanic ash, "
     "glowing ember eyes, ragged burnt clothes, arms outstretched, "
     "isolated on plain flat green background, game character sprite, " + STYLE, "1:1"),
    ("lv7_magma", "fx_ember",
     "seamless rising ember particles overlay texture, orange sparks and ash flecks, "
     "on pure black background for additive blending, game vfx, " + STYLE, "16:9"),

    # ================= 关 9：幽影回廊（虚空）=================
    ("lv8_void", "lawn_void",
     "top-down view of a dark void corridor battlefield for a tower defense game, "
     "9 columns by 5 rows grid of deep purple black stone tiles with faint glowing "
     "rune lines, eerie atmosphere, even lighting, tileable game background, " + STYLE, "16:9"),
    ("lv8_void", "fx_fog",
     "seamless dense purple-grey fog cloud overlay texture, soft billowing mist, "
     "on pure black background for alpha blending, game vfx, " + STYLE, "16:9"),
    ("lv8_void", "zombie_phantom",
     "full body front view of a translucent ghostly cartoon zombie, wispy purple "
     "smoke body, hollow glowing eyes, tattered cloak, arms outstretched, "
     "isolated on plain flat green background, game character sprite, " + STYLE, "1:1"),
    ("lv8_void", "zombie_blinker",
     "full body front view of a cartoon zombie with glitchy pixelated body parts "
     "dissolving into purple squares, one arm missing mid-teleport, glowing violet "
     "eyes, isolated on plain flat green background, game character sprite, " + STYLE, "1:1"),

    # ================= 关 10：机枢要塞（电路）=================
    ("lv9_circuit", "lawn_circuit",
     "top-down view of a mechanical circuit board battlefield for a tower defense "
     "game, 9 columns by 5 rows grid of dark green metal tiles with glowing cyan "
     "circuit traces and rivets, even lighting, tileable game background, " + STYLE, "16:9"),
    ("lv9_circuit", "node_power",
     "a single square power generator node, brass and steel with glowing cyan "
     "energy core, viewed from above, isolated on plain dark background, "
     "game tile asset, " + STYLE, "1:1"),
    ("lv9_circuit", "zombie_cogwork",
     "full body front view of a cartoon steampunk mechanical zombie, brass gears "
     "and pistons for limbs, exposed copper wiring, single glowing lens eye, "
     "arms outstretched, isolated on plain flat green background, game character sprite, " + STYLE, "1:1"),
    ("lv9_circuit", "zombie_saboteur",
     "full body front view of a cartoon zombie carrying oversized wire cutters and "
     "a sparking severed cable, goggles pushed up on forehead, "
     "arms outstretched, isolated on plain flat green background, game character sprite, " + STYLE, "1:1"),

    # ================= 关 11：星界王座（星界 · 终局）=================
    ("lv10_astral", "lawn_astral",
     "top-down view of a celestial astral throne battlefield for a tower defense "
     "game, 9 columns by 5 rows grid of dark indigo tiles inlaid with glowing "
     "constellation star lines, nebula sheen, even lighting, tileable game background, " + STYLE, "16:9"),
    ("lv10_astral", "boss_apostle",
     "full body front view of a towering cosmic boss creature, armored star knight "
     "with a crown of floating runes, glowing violet eyes, cape made of galaxy "
     "nebula, imposing stance, isolated on plain flat green background, "
     "game boss sprite, " + STYLE, "1:1"),
    ("lv10_astral", "fx_meteor",
     "a single falling star meteor with fiery tail, viewed from above, purple and "
     "gold cosmic trail, isolated on plain dark background, game vfx asset, " + STYLE, "1:1"),
    # ================= 始祖主题（无关卡绑定，仅始祖级出现后使用）=================
    ("primordial", "lawn_primordial",
     "top-down view of a primordial creation-era battlefield for a tower defense game, "
     "9 columns by 5 rows grid of polished obsidian tiles veined with molten gold seams, "
     "faint ancient runes glowing softly inside the stone, dawn-of-the-world atmosphere, "
     "even lighting, tileable game background, " + STYLE, "16:9"),
]


def gen(job):
    sub, name, prompt, ar = job
    d = os.path.join(OUT, sub)
    os.makedirs(d, exist_ok=True)
    # mmx 按序号命名，生成后改名成语义化名字
    cmd = ["cmd", "/c", MMX, "image", "generate", "--prompt", prompt,
           "--aspect-ratio", ar, "--out-dir", d]
    env = dict(os.environ)
    before = set(os.listdir(d))
    r = subprocess.run(cmd, capture_output=True, text=True, env=env,
                       cwd=D, timeout=420)
    if r.returncode != 0:
        return False, (r.stderr or r.stdout or "")[-200:]
    new = [f for f in os.listdir(d) if f not in before]
    if not new:
        return False, "没有新文件"
    src = os.path.join(d, sorted(new)[-1])
    dst = os.path.join(d, name + os.path.splitext(src)[1])
    if os.path.exists(dst):
        os.remove(dst)
    os.replace(src, dst)
    kb = os.path.getsize(dst) // 1024
    return True, "%s (%dKB)" % (os.path.basename(dst), kb)


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else None
    jobs = [j for j in JOBS if not want or j[0].startswith(want)]
    print("共 %d 张待生成\n" % len(jobs))
    ok = fail = 0
    for i, job in enumerate(jobs, 1):
        sub, name = job[0], job[1]
        t0 = time.time()
        good, msg = gen(job)
        if good:
            ok += 1
            print("[%2d/%2d] ✔ %-14s %-18s %5.1fs" % (i, len(jobs), sub, msg, time.time() - t0))
        else:
            fail += 1
            print("[%2d/%2d] ✗ %-14s %-18s %s" % (i, len(jobs), sub, name, msg))
        sys.stdout.flush()
    print("\n完成：成功 %d / 失败 %d" % (ok, fail))
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
