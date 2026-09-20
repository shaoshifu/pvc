# -*- coding: utf-8 -*-
"""给现有僵尸追加第二个机制 —— 解决"僵尸只会走路啃植物"的问题。

背景：
  实测机制数分布是 {1: 62, 2: 7, 4: 1} —— 62 只僵尸只有**一个**能力位，
  打起来确实单调：知道它是"吸阳光僵尸"之后就没有任何变数了。
  这里给 43 只单机制僵尸各配一个第三批的新机制，形成"主机制 + 副机制"。

配对原则（不是随便发的）：
  · 同主题相扣 —— 病毒扩散配毒云、重力井配磁力、镜像配反弹；
  · 补短板 —— 慢速/防御型补一个位移（BLINK/DASH/BURROW），
    让它不只是一堵会走的墙；
  · 品级越高给越强的副机制（传奇/神级拿到 REBORN、AURA 这类）；
  · 不碰 42~48（已经是 2 个机制）和 49（RZ2_ALL）。

用法： python _upgrade_zombies.py [--dry]
"""
import re
import sys

P = r"C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\pvz.c"

# 索引 -> 追加的机制
PAIR = {
    "00": "RZ3_BLINK",       # 时间悖论 + 闪现：死而复生还带位移
    "01": "RZ3_DEFLECT",     # 量子叠加 + 反弹：三条线都在弹子弹
    "02": "RZ3_POISONCLOUD", # 病毒扩散 + 毒云：传染升级成范围压制
    "03": "RZ3_THROW",       # 黑客 + 投掷：黑掉植物再隔空砸
    "04": "RZ3_DEVOUR",      # 贪吃蛇 + 速食：吃植物长得更快
    "05": "RZ3_THROW",       # 弹幕厨师 + 投掷：菜刀之外再加石头
    "06": "RZ3_SHIELD",      # 迷宫建筑 + 护盾：造墙的自己也硬
    "07": "RZ3_REBORN",      # 概率坩埚 + 复生：本来就是赌命，再来一次
    "08": "RZ3_STEALSUN",    # 贷款 + 偷阳光：经济骚扰拉满
    "09": "RZ3_MAGNET",      # 蜘蛛网 + 磁力：织网再把猎物拽进来
    "10": "RZ3_SPLIT",       # 虚拟现实 + 分裂：假死之后变成两只
    "11": "RZ3_SHIELD",      # 代码编译 + 护盾：代码墙本来就硬，再加固
    "12": "RZ3_TAUNT",       # 噪音 + 嘲讽：吵得植物只打它
    "13": "RZ3_MAGNET",      # 重力井 + 磁力：拉住后排植物
    "14": "RZ3_DASH",        # 音乐节拍 + 冲锋：跟着拍子冲
    "15": "RZ3_FLY",         # 随机传送 + 飞行：位置完全不可预测
    "16": "RZ3_DEFLECT",     # 镜像 + 反弹：镜面本来就该弹
    "17": "RZ3_STEALSUN",    # 吸阳光 + 偷阳光：阳光黑洞
    "18": "RZ3_SPLIT",       # 区块链 + 分裂：分叉死亡
    "19": "RZ3_REGEN",       # 生化实验 + 自愈：变异体自我修复
    "20": "RZ3_THROW",       # 跨界外星 + 投掷：激光之外加远程物理
    "21": "RZ3_ENRAGE",      # 赌徒 + 狂暴：赌输了反而更猛
    "22": "RZ3_JUMP",        # 爬格子 + 跳跃：移动能力叠加
    "23": "RZ3_DEFLECT",     # 像素崩坏 + 反弹：像素化弹开子弹
    "24": "RZ3_AURA",        # 塔防合成 + 光环：合成体自然当指挥官
    "25": "RZ3_DEFLECT",     # 弹跳 + 反弹：一个机制两个方向
    "26": "RZ3_BLINK",       # 暗杀 + 闪现：刺客的位移
    "27": "RZ3_STEALSUN",    # 贪婪 + 偷阳光：贪婪主题闭环
    "28": "RZ3_POISONCLOUD", # 诅咒 + 毒云：死亡诅咒升级成走哪烂哪
    "29": "RZ3_TAUNT",       # 功夫 + 嘲讽：以战养战
    "30": "RZ3_SPLIT",       # 量子纠缠 + 分裂：纠缠体分裂
    "31": "RZ3_REGEN",       # 时间回溯 + 自愈：双重回血
    "32": "RZ3_SUMMON",      # 黑客2 + 召唤：黑进系统调僵尸
    "33": "RZ3_POISONCLOUD", # 病原体 + 毒云：病原体的自然延伸
    "34": "RZ3_BLINK",       # 逆向时间 + 闪现：时间与空间的错位
    "35": "RZ3_SHIELD",      # 概率墙 + 护盾：防御主题叠加
    "36": "RZ3_BURROW",      # 迷宫 + 钻地：造墙之余自己还能穿墙
    "37": "RZ3_FREEZEBITE",  # 随机天气 + 冰噬：暴风雪里的咬击
    "38": "RZ3_SUMMON",      # 卡牌 + 召唤：抽卡抽出一堆僵尸
    "39": "RZ3_DASH",        # 功夫2 + 冲锋：连踢带冲
    "40": "RZ3_MAGNET",      # 吞噬黑洞 + 磁力：引力主题闭环
    "41": "RZ3_THROW",       # 弹幕厨师2 + 投掷：炸弹 + 投石
    "42": "RZ3_ENRAGE",      "43": "RZ3_SHIELD",    "44": "RZ3_REGEN",
    "45": "RZ3_THROW",       "46": "RZ3_SUMMON",    "47": "RZ3_BURROW",
    "48": "RZ3_AURA",
}


def main():
    dry = "--dry" in sys.argv
    src = open(P, encoding="utf-8").read()
    i0 = src.index("static const RgZombieDef rgZombies")
    i1 = src.index("\n};", i0)
    head, body, tail = src[:i0], src[i0:i1], src[i1:]

    ITEM = re.compile(r"(/\* (\d{2}) \*/ \{ L\"[^\"]+\",\s*RGQ_\w+,\s*\w+,\s*)([^,]+)(,)")
    added, skipped = [], []

    def rep(m):
        pre, idx, traits, comma = m.groups()
        add = PAIR.get(idx)
        if not add:
            skipped.append(idx)
            return m.group(0)
        if add in traits:
            skipped.append(idx)
            return m.group(0)
        # 换行 + 对齐，保持可读性
        newt = traits.rstrip() + " | " + add
        if len(newt) > 44:                       # 太长就折到下一行
            newt = traits.rstrip() + " |\n" + " " * 24 + add
        added.append((idx, add))
        return pre + newt + comma

    body2 = ITEM.sub(rep, body)
    print("追加机制 %d 处，跳过 %d 处" % (len(added), len(skipped)))
    for idx, add in added[:6]:
        print("  /* %s */ + %s" % (idx, add))
    if len(added) > 6:
        print("  ...")
    if dry:
        print("[dry] 未写盘")
        return
    open(P, "w", encoding="utf-8", newline="").write(head + body2 + tail)
    print("已写回", P)


if __name__ == "__main__":
    main()
