# -*- coding: utf-8 -*-
import os, glob
from PIL import Image
import numpy as np

OUT = os.path.join(os.path.dirname(__file__), "gen2")
files = sorted(glob.glob(os.path.join(OUT, "u_p*.jpg")))
def spread(p):
    a = np.array(Image.open(p).convert("RGB")).astype(np.float32)
    h,w = a.shape[:2]
    ring=8
    px = np.concatenate([a[:ring].reshape(-1,3), a[-ring:].reshape(-1,3), a[:,:ring].reshape(-1,3), a[:,-ring:].reshape(-1,3)])
    c = np.median(px, axis=0)
    return float(np.mean(np.abs(px-c)))
res=[]
for f in files:
    s=spread(f)
    if s>12:
        res.append((os.path.basename(f), s))
res.sort(key=lambda x:x[1], reverse=True)
for k,v in res:
    print(k, v)
print("总计:", len(res))
