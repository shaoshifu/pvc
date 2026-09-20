# -*- coding: utf-8 -*-
"""分析 _shot_topbar.png（客户区 1:1 对应游戏坐标）：
1) 确认真的进了战斗（顶栏木条 + 左上阳光面板 + 黄色太阳）
2) 数出卡面（米色卡座）个数与位置
3) 检查铲子卡右侧是否还有残留的价格文字（本次 bug 的核心表现）
"""
from PIL import Image
import numpy as np

im = np.array(Image.open("_shot_topbar.png").convert("RGB")).astype(int)
H, W, _ = im.shape
print("尺寸 %dx%d" % (W, H))

def frac(mask, y0, y1):
    return mask[y0:y1].mean()

# ---- 1) 顶栏结构确认 ----
wood   = (np.abs(im - np.array([104, 70, 42])).sum(2) < 45)
dkline = (np.abs(im - np.array([74, 48, 28])).sum(2) < 45)
ltline = (np.abs(im - np.array([146, 104, 62])).sum(2) < 45)
strip_wood = frac(wood, 2, 88)
row_dk = dkline[86:92].mean(1).max() if H > 92 else 0
print("顶栏(2..88行)木色占比 %.2f  底边线占比 %.2f" % (strip_wood, row_dk))

# 阳光面板：米色圆角面板 RGB(222,200,152) + 太阳 RGB(255,220,70)
panel = (np.abs(im[10:88, 10:130] - np.array([222, 200, 152])).sum(2) < 60).mean()
sunpx = (np.abs(im[20:70, 10:64] - np.array([255, 220, 70])).sum(2) < 90).mean()
print("阳光面板米色占比 %.2f  太阳黄色占比 %.2f" % (panel, sunpx))
# 木色占比不会超过 0.5：阳光面板 + 卡面本身就盖掉顶栏约 55% 的面积。
# 战斗判定看三个确定性指标：底边深色线、阳光面板、太阳黄圆。
in_battle = row_dk > 0.5 and panel > 0.3 and sunpx > 0.1
print("=> %s" % ("【战斗画面】" if in_battle else "【不是战斗画面，R 没生效】"))

if not in_battle:
    # 看看前 92 行到底是什么颜色，辅助判断卡在哪
    top = im[0:92]
    print("顶区平均色", top.reshape(-1, 3).mean(0).astype(int))
    raise SystemExit(0)

# ---- 2) 卡面统计（米色卡座 RGB(238,224,178)，含变暗版 196,186,162）----
beige = ((np.abs(im[8:84] - np.array([238, 224, 178])).sum(2) < 60) |
         (np.abs(im[8:84] - np.array([196, 186, 162])).sum(2) < 60))
colfrac = beige.mean(0)
cards = np.where(colfrac > 0.15)[0]
# 分组连续区段
groups = []
if len(cards):
    s = cards[0]; prev = cards[0]
    for x in cards[1:]:
        if x - prev > 4:
            groups.append((s, prev)); s = x
        prev = x
    groups.append((s, prev))
print("检测到 %d 个卡面:" % len(groups))
for g in groups:
    print("   game-x %d..%d  中心 %d" % (g[0], g[1], (g[0] + g[1]) // 2))

# ---- 3) 文字像素分区检查 ----
strip = im[4:88]
bright = (strip[:, :, 0] > 185) & (strip[:, :, 1] > 185) & (strip[:, :, 2] > 170)
red    = (strip[:, :, 0] > 195) & (strip[:, :, 1] < 175) & (strip[:, :, 2] < 175)
txt = bright | red
xs = np.where(txt.any(0))[0]
gx = xs
last_card_end = groups[-1][1] if groups else 566
print("文字像素 game-x 范围: %d..%d" % (gx.min(), gx.max()) if len(gx) else "无文字")
zones = [
    ("阳光面板 8..132",        8, 132),
    ("卡片区 144..%d" % last_card_end, 144, last_card_end),
    ("铲子右侧 %d..995" % (last_card_end + 8), last_card_end + 8, 995),
]
for name, a, b in zones:
    sel = gx[(gx >= a) & (gx <= b)]
    print("  %-24s %4d 个文字像素" % (name, sel.size))
bad = gx[(gx >= last_card_end + 8) & (gx <= 995)]
if bad.size:
    print("  右侧残留落点:", np.unique(bad)[:40])
    print("  => 仍有残留！")
else:
    print("  => 铲子右侧干净，无残留价格文字")
