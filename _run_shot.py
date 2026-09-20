# -*- coding: utf-8 -*-
"""跑 _shot.exe 抓三张图：点击前的编组页 / 点击后的编组页 / 开局战场。

用 PVZ_SAVE_FILE 把存档隔离到测试档 —— shotAuto() 会走真实的
onClick → loadoutToggle → saveFlush()，不隔离就会写玩家的 pvz_save.dat。
"""
import os
import subprocess
import time

D = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(D, "_frames_loadout1")

env = dict(os.environ)
env["PVZ_SAVE_FILE"] = os.path.join(D, "_test_loadout1_save.dat")

p = subprocess.Popen([os.path.join(D, "_shot.exe")], cwd=D, env=env)
time.sleep(9)
p.kill()
p.wait()

files = sorted(os.listdir(OUT))
print("files:", files)
for f in files:
    fp = os.path.join(OUT, f)
    print("  %-14s %8d bytes" % (f, os.path.getsize(fp)))
    if f.endswith(".txt"):
        with open(fp, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith(("alive", "sel ", "state=", "loadout ")):
                    print("      | " + line.rstrip())
