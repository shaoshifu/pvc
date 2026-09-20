# -*- coding: utf-8 -*-
"""程序化合成游戏音效（打击感 / 种植 / 铲除 / UI …）。

为什么用合成而不是下载素材：
  1. 音效没有现成的 CC0 大包能一次凑齐"豌豆命中""铲子挖土"这种细分动作；
  2. 合成出来的每个参数（频率、衰减、噪声量）都能调，手感不对就改数字重跑；
  3. 零依赖、零版权，改完立刻能听。

合成配方的基本套路：
  · 打击感 = 短噪声爆发 + 快速指数衰减 + 一点低频"body"
  · 金属/晶体 = 多个非谐波正弦叠加 + 长尾衰减
  · 挖掘/摩擦 = 带通噪声 + 幅度抖动
  · 上升/下降提示 = 频率扫频 + 三角包络
所有音效统一 22050Hz / 16bit / 单声道，够用且文件小。

用法： python art/_gen_sfx.py [--list]
"""
import math
import os
import sys
import wave

import numpy as np

SR = 22050
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "assets", "sfx")
RNG = np.random.default_rng(20260917)      # 固定种子：保证每次生成结果一致


# ---------------------------------------------------------------- 基础工具
def n_of(sec):
    return max(1, int(SR * sec))


def noise(n):
    return RNG.uniform(-1.0, 1.0, n)


def sine(f, n, phase=0.0):
    t = np.arange(n) / SR
    return np.sin(2.0 * np.pi * f * t + phase)


def sweep(f0, f1, n, kind="exp"):
    """扫频：kind=exp 听起来更"自然"，lin 更适合做机械感。"""
    t = np.arange(n) / SR
    if kind == "exp":
        f0 = max(1.0, f0)
        f1 = max(1.0, f1)
        k = (f1 / f0) ** (1.0 / max(1e-6, t[-1]))
        ph = 2.0 * np.pi * f0 * (k ** t - 1.0) / math.log(k)
    else:
        ph = 2.0 * np.pi * (f0 * t + 0.5 * (f1 - f0) / max(1e-6, t[-1]) * t * t)
    return np.sin(ph)


def env_ad(n, attack=0.004, curve=4.0):
    """attack 线性起音 + 指数衰减。curve 越大衰减越陡（打击感越"干"）。"""
    a = max(1, int(SR * attack))
    e = np.ones(n)
    e[:a] = np.linspace(0.0, 1.0, a)
    e[a:] = np.exp(-curve * np.linspace(0.0, 1.0, n - a))
    return e


def env_tri(n, peak=0.35):
    """三角包络：适合扫频提示音（起-峰-落）。"""
    p = int(n * peak)
    e = np.ones(n)
    e[:p] = np.linspace(0.0, 1.0, p)
    e[p:] = np.linspace(1.0, 0.0, n - p)
    return e


def lowpass(x, cutoff):
    """一阶 IIR 低通。用来把生硬的噪声磨圆，避免刺耳。"""
    a = math.exp(-2.0 * math.pi * cutoff / SR)
    y = np.empty_like(x)
    prev = 0.0
    for i in range(len(x)):
        prev = (1.0 - a) * x[i] + a * prev
        y[i] = prev
    return y


def highpass(x, cutoff):
    return x - lowpass(x, cutoff)


def mix(*parts):
    n = max(len(p) for p in parts)
    out = np.zeros(n)
    for p in parts:
        out[:len(p)] += p
    return out


def norm(x, peak=0.85):
    m = float(np.max(np.abs(x))) if len(x) else 0.0
    return x if m < 1e-9 else x * (peak / m)


def write_wav(name, x):
    os.makedirs(OUT, exist_ok=True)
    x = norm(np.clip(x, -1.0, 1.0))
    pcm = (x * 32000.0).astype("<i2")
    path = os.path.join(OUT, name + ".wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print("  %-22s %5.0f ms  %5.1f KB" % (name, len(x) / SR * 1000.0,
                                          os.path.getsize(path) / 1024.0))


# ---------------------------------------------------------------- 音效配方
def sfx_plant():
    """种植：泥土的"噗"。低频闷响 + 短噪声。"""
    n = n_of(0.22)
    body = sweep(320, 90, n) * env_ad(n, 0.006, 3.5)
    dirt = lowpass(noise(n), 1400) * env_ad(n, 0.002, 9.0) * 0.5
    return mix(body, dirt)


def sfx_shovel():
    """铲除：铲子挖土的刮擦。带通噪声 + 抖动。"""
    n = n_of(0.30)
    grit = highpass(lowpass(noise(n), 3200), 400)
    wobble = 1.0 + 0.5 * np.sin(2 * np.pi * 26.0 * np.arange(n) / SR)
    return grit * wobble * env_ad(n, 0.010, 5.5)


def sfx_shoot():
    """豌豆发射：短促"啵"。"""
    n = n_of(0.09)
    return mix(sweep(900, 380, n) * env_ad(n, 0.001, 16.0),
               noise(n) * env_ad(n, 0.001, 26.0) * 0.35)


def sfx_hit():
    """子弹命中僵尸：打击感核心音。噪声瞬态 + 中频 body。"""
    n = n_of(0.13)
    snap = noise(n) * env_ad(n, 0.0005, 34.0)
    body = sweep(520, 180, n) * env_ad(n, 0.001, 13.0) * 0.8
    return mix(snap, body)


def sfx_hit_hard():
    """重击 / 爆炸命中：更低的 body、更长的尾巴。"""
    n = n_of(0.26)
    snap = noise(n) * env_ad(n, 0.001, 22.0)
    body = sweep(260, 60, n) * env_ad(n, 0.002, 7.0)
    tail = lowpass(noise(n), 700) * env_ad(n, 0.010, 5.0) * 0.6
    return mix(snap, body, tail)


def sfx_explode():
    """爆炸：噪声爆发 + 低频冲击。"""
    n = n_of(0.55)
    boom = sweep(180, 38, n) * env_ad(n, 0.003, 5.0)
    blast = lowpass(noise(n), 2200) * env_ad(n, 0.002, 7.0)
    return mix(boom * 1.1, blast)


def sfx_zombie_bite():
    """僵尸啃植物：低频咀嚼，两声。"""
    n = n_of(0.26)
    x = np.zeros(n)
    for k, off in enumerate((0.0, 0.11)):
        s = int(SR * off)
        seg = n_of(0.09)
        bite = lowpass(noise(seg), 900) * env_ad(seg, 0.003, 12.0)
        x[s:s + seg] += bite * (1.0 if k == 0 else 0.8)
    return x


def sfx_zombie_die():
    """僵尸倒地：下滑音 + 闷响。"""
    n = n_of(0.42)
    fall = sweep(300, 70, n) * env_ad(n, 0.005, 4.5)
    thud = lowpass(noise(n), 500) * env_ad(n, 0.008, 6.0) * 0.7
    return mix(fall, thud)


def sfx_sun_collect():
    """收阳光：清脆"叮"。两个纯五度泛音，长尾。"""
    n = n_of(0.34)
    return mix(sine(1046.5, n) * env_ad(n, 0.001, 6.5),
               sine(1568.0, n) * env_ad(n, 0.001, 8.5) * 0.6,
               sine(2093.0, n) * env_ad(n, 0.001, 12.0) * 0.3)


def sfx_sun_drop():
    """阳光落地：柔和"咚"。"""
    n = n_of(0.20)
    return mix(sweep(700, 420, n) * env_ad(n, 0.003, 8.0),
               sine(520, n) * env_ad(n, 0.002, 10.0) * 0.5)


def sfx_card():
    """选卡：干脆的"嗒"。"""
    n = n_of(0.07)
    return mix(noise(n) * env_ad(n, 0.0005, 40.0),
               sine(1800, n) * env_ad(n, 0.001, 22.0) * 0.4)


def sfx_draft():
    """三选一 / 翻牌：上滑提示音。"""
    n = n_of(0.30)
    return mix(sweep(420, 980, n) * env_tri(n, 0.4),
               sine(660, n) * env_ad(n, 0.004, 7.0) * 0.4)


def sfx_buy():
    """购买成功：金币叮当（两连音）。"""
    n = n_of(0.36)
    x = np.zeros(n)
    for k, (off, f) in enumerate(((0.0, 1318.5), (0.10, 1760.0))):
        s = int(SR * off)
        seg = n_of(0.22)
        x[s:s + seg] += sine(f, seg) * env_ad(seg, 0.001, 8.0) * (1.0 - 0.25 * k)
    return x


def sfx_deny():
    """阳光不足 / 操作无效：低沉"嗡"。"""
    n = n_of(0.22)
    return mix(sine(150, n) * env_ad(n, 0.004, 7.0),
               sine(149, n) * env_ad(n, 0.004, 7.0) * 0.8)   # 轻微拍频 = 不适感


def sfx_wave():
    """新波次警报：两声短促上扬。"""
    n = n_of(0.60)
    x = np.zeros(n)
    for off in (0.0, 0.24):
        s = int(SR * off)
        seg = n_of(0.30)
        tone = mix(sine(560, seg), sine(840, seg) * 0.5)
        x[s:s + seg] += tone * env_tri(seg, 0.3)
    return x


def sfx_win():
    """胜利：三音上行大调分解。"""
    notes = (523.25, 659.25, 783.99, 1046.5)
    seg = n_of(0.34)
    n = n_of(0.34 * len(notes) + 0.45)
    x = np.zeros(n)
    for i, f in enumerate(notes):
        s = int(SR * 0.28 * i)
        x[s:s + seg] += sine(f, seg) * env_ad(seg, 0.004, 4.5) * 0.8
    return x


def sfx_lose():
    """失败：三音下行小调。"""
    notes = (523.25, 415.30, 311.13)
    seg = n_of(0.46)
    n = n_of(0.40 * len(notes) + 0.55)
    x = np.zeros(n)
    for i, f in enumerate(notes):
        s = int(SR * 0.40 * i)
        x[s:s + seg] += mix(sine(f, seg), sine(f * 0.5, seg) * 0.5) \
                        * env_ad(seg, 0.006, 3.5) * 0.85
    return x


def sfx_freeze():
    """冰冻：晶体叮声 + 高频闪。"""
    n = n_of(0.40)
    return mix(sine(2637.0, n) * env_ad(n, 0.001, 9.0),
               sine(3520.0, n) * env_ad(n, 0.001, 13.0) * 0.5,
               highpass(noise(n), 5000) * env_ad(n, 0.002, 20.0) * 0.25)


def sfx_fire():
    """火焰：带通噪声的"呼"。"""
    n = n_of(0.40)
    fl = lowpass(highpass(noise(n), 300), 2600)
    wob = 1.0 + 0.45 * np.sin(2 * np.pi * 11.0 * np.arange(n) / SR)
    return fl * wob * env_ad(n, 0.020, 4.0)


def sfx_laser():
    """激光：快速下滑 + 高频。"""
    n = n_of(0.16)
    return mix(sweep(3200, 620, n) * env_ad(n, 0.001, 14.0),
               sine(2400, n) * env_ad(n, 0.001, 18.0) * 0.3)


def sfx_shield():
    """护盾 / 金属：非谐波叠加 + 长尾。"""
    n = n_of(0.45)
    return mix(sine(587, n) * env_ad(n, 0.002, 6.0),
               sine(880, n) * env_ad(n, 0.002, 8.5) * 0.7,
               sine(1409, n) * env_ad(n, 0.002, 12.0) * 0.45)


def sfx_mower():
    """小推车：低频引擎 + 滚动噪声。"""
    n = n_of(0.75)
    eng = mix(sine(72, n), sine(144, n) * 0.5, sine(36, n) * 0.7)
    roll = lowpass(noise(n), 1200) * 0.55
    wob = 1.0 + 0.25 * np.sin(2 * np.pi * 17.0 * np.arange(n) / SR)
    return (eng + roll) * wob * env_tri(n, 0.18)


def sfx_alarm():
    """僵尸进屋 / 危险：急促双音。"""
    n = n_of(0.75)
    x = np.zeros(n)
    for off in (0.0, 0.20, 0.40):
        s = int(SR * off)
        seg = n_of(0.16)
        x[s:s + seg] += mix(sine(880, seg), sine(1320, seg) * 0.4) \
                        * env_ad(seg, 0.004, 9.0)
    return x


def sfx_heal():
    """回血 / 增益：柔和上行。"""
    n = n_of(0.38)
    return mix(sweep(600, 1200, n) * env_tri(n, 0.45) * 0.9,
               sine(900, n) * env_ad(n, 0.004, 6.0) * 0.35)


def sfx_pickup():
    """拾取 / 获得：短促双音上扬。"""
    n = n_of(0.30)      # 第二音起点 0.07 + 段长 0.15 = 0.22，总长必须 ≥ 0.22，
                        # 否则 x[s:s+seg] 会短于 seg，广播失败

    x = np.zeros(n)
    for k, (off, f) in enumerate(((0.0, 987.77), (0.07, 1318.5))):
        s = int(SR * off)
        seg = n_of(0.15)
        x[s:s + seg] += sine(f, seg) * env_ad(seg, 0.001, 12.0)
    return x


RECIPES = [
    ("plant", sfx_plant), ("shovel", sfx_shovel), ("shoot", sfx_shoot),
    ("hit", sfx_hit), ("hit_hard", sfx_hit_hard), ("explode", sfx_explode),
    ("zombie_bite", sfx_zombie_bite), ("zombie_die", sfx_zombie_die),
    ("sun_collect", sfx_sun_collect), ("sun_drop", sfx_sun_drop),
    ("card", sfx_card), ("draft", sfx_draft), ("buy", sfx_buy),
    ("deny", sfx_deny), ("wave", sfx_wave), ("win", sfx_win),
    ("lose", sfx_lose), ("freeze", sfx_freeze), ("fire", sfx_fire),
    ("laser", sfx_laser), ("shield", sfx_shield), ("mower", sfx_mower),
    ("alarm", sfx_alarm), ("heal", sfx_heal), ("pickup", sfx_pickup),
]


def main():
    if "--list" in sys.argv:
        for n, _ in RECIPES:
            print(n)
        return
    print("生成音效 -> %s" % OUT)
    for name, fn in RECIPES:
        write_wav(name, fn())
    print("\n共 %d 个" % len(RECIPES))


if __name__ == "__main__":
    main()
