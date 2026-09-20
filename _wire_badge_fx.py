# -*- coding: utf-8 -*-
"""把本轮新增的 14 张素材（8 枚档位徽章 + 6 张开牌光效）接进游戏。

【幂等】重复执行安全：开头检查 drawTierBadge 是否已存在。
  这不是洁癖 —— 本环境的 heredoc 会把脚本执行两次（已实测多次），
  补丁脚本必须自己扛住重复执行，否则会出现"槽位声明两份、编译报重复定义"。

【锚点选择】一定要用**函数体内部**的独有语句做锚点。
  第一版用 `static void drawCelebFront(...)\n{\n` 当锚点，结果是：
  那段插入内容落在了**上一层函数的闭括号之后**，变成文件级语句，
  C 直接报 `expected identifier or '(' before '{'`。
  正确做法是锚到"上一层函数体最后一段独有代码"，插在它的闭括号之前。

用法：python _wire_badge_fx.py
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "pvz.c")


HELPERS = '''
/* ==================== 档位徽章 + 光效贴图（2026-09-21 新增素材的接入） ====================
   两条原则：
     ① **缺图为纯回退**：徽章没加载出来就只画文字（原来的样子）；
        光效没加载出来，庆典依然完整 —— 光扇/冲击波/火花本来就是程序化画的。
        所以整包缺这 14 张也跑得起来，不会出现"少一张图界面就空掉"。
     ② 位置/尺寸只有一处定义：徽章边长用 BADGE_ICON，档位名文字的 x 由它推出来，
        避免两处写死导致徽章和文字错位。 */
#define BADGE_ICON 20.0f        /* 档位徽章的显示边长（逻辑像素） */

static void drawTierBadge(HDC dc, int tier, float x, float y, float size)
{
    Sprite *s;
    if (tier < 0 || tier >= RGQ_COUNT) return;
    s = &gSprBadge[tier];
    if (!s->ok) return;
    /* spriteBlit 把贴图**底边**对齐到 baseY、水平居中到 cx，
       所以这里要把 (x, y) 这个"上左角"换算成 (中心, 底边)。 */
    spriteBlit(dc, s, x + size * 0.5f, y + size,
               size / ((float)s->w / (float)SS),
               size / ((float)s->h / (float)SS), -1);
}

/* 光效贴图叠加：以 (cx, cy) 为中心、边长 size、整体透明度 alpha。
   素材是"黑底 luma key"抠出来的（见 art/_gen_badges.py 的说明），
   边缘自带渐隐，直接 AlphaBlend 就能融进背景，不需要加色混合。 */
static void fxOverlay(HDC dc, int idx, float cx, float cy, float size, int alpha)
{
    Sprite *s;
    if (idx < 0 || idx >= FSX_N || alpha <= 0) return;
    s = &gSprFx[idx];
    if (!s->ok) return;
    spriteBlit(dc, s, cx, cy + size * 0.5f,
               size / ((float)s->w / (float)SS),
               size / ((float)s->h / (float)SS), alpha);
}

'''


# ---- 挂在 drawCelebBack 末尾（卡片之下：光柱 / 光扇 / 符文环）
BACK_ANCHOR = """                fillCircle(dc, cx + cosf(a) * rr, cy + sinf(a) * rr * 0.80f,
                           3.0f + 5.0f * f2, tint(tc, 0.15f + 0.40f * f2));
            }
        }
    }
}
"""
BACK_FX = """                fillCircle(dc, cx + cosf(a) * rr, cy + sinf(a) * rr * 0.80f,
                           3.0f + 5.0f * f2, tint(tc, 0.15f + 0.40f * f2));
            }
        }
    }

    /* ---- 贴图光效：在程序化光扇/光环之上再叠一层 ------------
       每层都在自己的作用域块里，fade 互不干扰；
       alpha 归零时 fxOverlay 自己 return，省掉一堆"要不要画"的判断。
       半径仍然收在卡片尺度附近（早先光扇铺满全屏、把卡片糊掉的教训）。 */
    {
        float f = 1.0f - CLAMP((p - 0.22f) / 0.40f, 0.0f, 1.0f);
        if (f > 0.02f)
            fxOverlay(dc, FSX_PILLAR, cx, cy, CARD_W * 1.08f, (int)(120.0f * f));
    }
    {
        float grow = celebEaseOut(p / 0.45f);
        float f = 1.0f - CLAMP((p - 0.30f) / 0.36f, 0.0f, 1.0f);
        if (f > 0.02f)
            fxOverlay(dc, FSX_RAYS, cx, cy,
                      150.0f + (gCelebPrim ? 260.0f : 190.0f) * grow,
                      (int)((gCelebPrim ? 150.0f : 110.0f) * f));
    }
    if (gCelebPrim) {                      /* 始祖多一层符文环 */
        float f = 1.0f - CLAMP((p - 0.34f) / 0.42f, 0.0f, 1.0f);
        if (f > 0.02f)
            fxOverlay(dc, FSX_RUNES, cx, cy,
                      150.0f + 220.0f * celebEaseOut(p / 0.50f), (int)(140.0f * f));
    }
}
"""

# ---- 挂在 drawCelebFront 末尾（卡片之上：闪斑 / 冲击波 / 火花）
FRONT_ANCHOR = """            fillCircle(dc, cx + cosf(a) * d * 0.78f, cy + sinf(a) * d * 0.78f * 0.72f,
                       3.2f * f + 1.0f, tint(tc, 0.30f));
        }
    }
}
"""
FRONT_FX = """            fillCircle(dc, cx + cosf(a) * d * 0.78f, cy + sinf(a) * d * 0.78f * 0.72f,
                       3.2f * f + 1.0f, tint(tc, 0.30f));
        }
    }

    /* ---- 贴图光效（压在卡片与彩幕之上）----
       顺序：闪斑（开场一击）→ 冲击波（外扩）→ 火花（迸射）。
       闪斑刻意做得大（180→400）：它和全屏彩幕一起构成"过曝"那一瞬，
       是整个演出里最有冲击力的一帧。 */
    {
        float f = 1.0f - CLAMP(p / 0.20f, 0.0f, 1.0f);
        if (f > 0.02f)
            fxOverlay(dc, FSX_FLARE, cx, cy, 180.0f + 220.0f * (1.0f - f),
                      (int)((gCelebPrim ? 235.0f : 190.0f) * f));
    }
    {
        float ph = CLAMP(p / 0.46f, 0.0f, 1.0f);
        if (ph > 0.0f && ph < 1.0f)
            fxOverlay(dc, FSX_SHOCK, cx, cy,
                      60.0f + (gCelebPrim ? 420.0f : 320.0f) * celebEaseOut(ph),
                      (int)(190.0f * (1.0f - ph)));
    }
    {
        float lp = CLAMP((p - 0.06f) / 0.50f, 0.0f, 1.0f);
        if (lp > 0.0f && lp < 1.0f)
            fxOverlay(dc, FSX_SPARK, cx, cy,
                      80.0f + (gCelebPrim ? 380.0f : 300.0f) * celebEaseOut(lp),
                      (int)(175.0f * (1.0f - lp)));
    }
}
"""

CARD_OLD = """            rc = RGQ_COL[tier];
            wsprintfW(buf, L"%s", RGQ_NAME[tier]);
            putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), buf,
                     x + 12.0f, y + 2.0f, 0);"""
CARD_NEW = """            rc = RGQ_COL[tier];
            /* 档位徽章画在档位名左侧，文字跟着右移同一个宽度；
               两个坐标都由 BADGE_ICON 推出来（写死两次迟早会错位）。 */
            drawTierBadge(dc, tier, x + 11.0f, y + 1.0f, BADGE_ICON);
            wsprintfW(buf, L"%s", RGQ_NAME[tier]);
            putTextS(dc, gF15, RGB(255, 255, 255), tint(rc, -0.5f), buf,
                     x + 11.0f + BADGE_ICON + 4.0f, y + 2.0f, 0);"""


def main():
    s = io.open(SRC, encoding="utf-8").read()
    if "drawTierBadge" in s:
        print("  已接过线，跳过（幂等）")
        return 0
    steps = [
        ("助手函数（徽章 + 光效叠加）", None, HELPERS),
        ("庆典底层光效（光柱/光扇/符文环）", BACK_ANCHOR, BACK_FX),
        ("庆典上层光效（闪斑/冲击波/火花）", FRONT_ANCHOR, FRONT_FX),
        ("卡片档位徽章", CARD_OLD, CARD_NEW),
    ]
    for name, old, new in steps:
        if old is None:
            # 助手函数挂在 drawCelebBack 定义之前（同层，安全）
            sig = ("static void drawCelebBack(HDC dc)\n{\n"
                   "    float p, x, y, cx, cy, rot, fade;\n")
            assert s.count(sig) == 1, "助手函数锚点不唯一: %d" % s.count(sig)
            s = s.replace(sig, new + sig)
        else:
            assert s.count(old) == 1, "%s 锚点不唯一: %d" % (name, s.count(old))
            s = s.replace(old, new)
        print("  ✔ %s" % name)
    io.open(SRC, "w", encoding="utf-8", newline="\n").write(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
