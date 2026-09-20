# -*- coding: utf-8 -*-
"""程序化合成「始祖主题」BGM —— 不需要任何外部音乐模型。

为什么自己合成：
  1. 本机的 mmx CLI **没有音乐生成能力**（只有 text/speech/image/video/search/vision），
     五关那批 mp3 是外部工具出的，脚本没有留下；
  2. 始祖主题是**一次性、情绪极特殊**的一首（太初 / 压迫 / 崇高），
     现成素材库很难刚好命中；
  3. 合成出来的每个参数都能调，不满意改数字重跑，零版权零依赖。

作曲思路（76 秒，A 小调，循环用）：
  层 1 sub    : 55Hz(A1) + 27.5Hz 垫底，0.06Hz 的极慢呼吸
  层 2 drone  : 82.41Hz(E2) 与 110Hz(A2) 微失谐叠加，制造"巨大物体在远处"
  层 3 pad    : A 小调和弦（110/130.8/164.8/220）各三份失谐，长起音
  层 4 shimmer: 1318/1760/2093Hz 的极轻高频簇，慢速颤音，只在后半段出现
  层 5 bell   : FM 钟（载波+调制器），A 五声音阶，14s 起每 6 秒一记
  层 6 pulse  : 48Hz 心跳式低频脉冲，8s 起每 3.2 秒一次
  层 7 swell  : 低通噪声的缓慢涨落，做"能量在聚集"的听感
  收尾：Schroeder 混响（4 comb + 2 allpass）把各层粘在一起，
        两端各 1.5 秒淡入淡出，避免 MCI 循环时爆音。

输出：assets/bgm/primordial.mp3（走 ffmpeg 编码）。
      **必须用 mp3 而不是 wav**：本项目实测 MCI 播 WAV 时
      `play ... repeat` 会返回错误 259（打开成功但不出声），
      靠 bgmTick 续播会有循环缝；mp3 走 mpegvideo 解码器，repeat 正常。

用法： python art/_gen_bgm.py
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import wave

import numpy as np

SR = 22050
DUR = 76.0
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUTDIR = os.path.join(ROOT, "assets", "bgm")
TMP = os.path.join(HERE, "gen2", "_primordial.wav")
RNG = np.random.default_rng(20260920)


def n_of(sec):
    return max(1, int(SR * sec))


def t_axis(n):
    return np.arange(n, dtype=np.float64) / SR


def sine(f, n, phase=0.0):
    return np.sin(2.0 * np.pi * f * t_axis(n) + phase)


def env_attack(n, atk, rel):
    """起音/释音包络（秒）。pad 这类长音全靠它避免"啪"的一声。"""
    e = np.ones(n)
    a = min(n, n_of(atk))
    r = min(n, n_of(rel))
    if a > 0:
        e[:a] = np.linspace(0.0, 1.0, a)
    if r > 0:
        e[-r:] = np.linspace(1.0, 0.0, r)
    return e


def lowpass(x, k):
    """滑动平均当低通（够用且零依赖）：k 越大越闷。"""
    if k <= 1:
        return x
    c = np.cumsum(np.insert(x, 0, 0.0))
    y = (c[k:] - c[:-k]) / float(k)
    return np.concatenate([np.full(k - 1, y[0]), y])


def reverb(x, mix=0.34):
    """Schroeder 简易混响：4 个并联 comb + 2 个串联 allpass。

    没有它，七八层正弦叠在一起会像"电子琴试音"；
    有它才有"在巨大空间里"的感觉 —— 这正是始祖主题要的。
    """
    combs = [1116, 1188, 1277, 1356]
    acc = np.zeros_like(x)
    for d in combs:
        b = np.zeros_like(x)
        for i in range(d, len(x)):
            b[i] = x[i] + 0.77 * b[i - d]
        acc += b / len(combs)
    y = acc
    for d, g in ((225, 0.5), (556, 0.5)):
        out = np.zeros_like(y)
        for i in range(d, len(y)):
            out[i] = -g * y[i] + y[i - d] + g * out[i - d]
        y = out
    return x * (1.0 - mix) + y * mix


def bell(n0, f, dur=2.6, amp=0.22):
    """FM 钟：载波 + 非整数倍调制器，指数衰减。金属感来自非谐波比。"""
    n = n_of(dur)
    t = t_axis(n)
    mod = np.sin(2.0 * np.pi * f * 1.41 * t) * 2.2 * np.exp(-t * 2.4)
    y = np.sin(2.0 * np.pi * f * t + mod)
    y *= np.exp(-t * 1.9)
    return n0, (y * amp * env_attack(n, 0.004, 0.05)).astype(np.float64)


def build():
    n = n_of(DUR)
    mix = np.zeros(n, dtype=np.float64)
    t = t_axis(n)

    # 层 1+2：sub 与 drone（全程）
    breath = 0.72 + 0.28 * np.sin(2.0 * np.pi * 0.06 * t - 1.2)
    sub = (sine(55.0, n) * 0.42 + sine(27.5, n) * 0.30) * breath
    drone = (sine(82.41, n) * 0.26 + sine(82.41 * 1.0013, n) * 0.22
             + sine(110.0, n) * 0.16 + sine(110.0 * 0.9987, n) * 0.14)
    mix += (sub + drone) * env_attack(n, 6.0, 5.0)

    # 层 3：pad（12 秒后进入）
    pad = np.zeros(n)
    for f in (110.0, 130.81, 164.81, 220.0):
        for det in (0.9975, 1.0, 1.0025):
            pad += sine(f * det, n, RNG.uniform(0, 6.28)) * 0.055
    pad *= 0.55 + 0.45 * np.sin(2.0 * np.pi * 0.045 * t + 0.7)
    pad_env = np.zeros(n)
    k = n_of(12.0)
    pad_env[k:] = env_attack(n - k, 10.0, 6.0)
    mix += pad * pad_env

    # 层 4：高频微光（38 秒后加入，只在后半段）
    shim = np.zeros(n)
    for f in (1318.51, 1760.0, 2093.0):
        shim += sine(f, n) * 0.018
    gate = 0.5 + 0.5 * np.sin(2.0 * np.pi * 0.13 * t)
    s_env = np.zeros(n)
    k = n_of(38.0)
    s_env[k:] = env_attack(n - k, 8.0, 8.0)
    mix += shim * gate * s_env

    # 层 5：钟（14 秒起，每 6 秒一记，A 五声）
    notes = [440.0, 523.25, 659.25, 880.0, 329.63, 587.33, 440.0, 784.0]
    for i, at in enumerate([14, 20, 26, 32, 44, 50, 56, 68]):
        n0 = n_of(float(at))
        if n0 >= n:
            continue
        _, y = bell(0, notes[i % len(notes)], amp=0.20)
        m = min(len(y), n - n0)
        mix[n0:n0 + m] += y[:m]

    # 层 6：心跳脉冲（8 秒起）
    for at in np.arange(8.0, DUR - 4.0, 3.2):
        n0 = n_of(float(at))
        nk = n_of(0.55)
        if n0 + nk > n:
            break
        tt = t_axis(nk)
        thump = np.sin(2.0 * np.pi * (48.0 - 14.0 * tt) * tt) * np.exp(-tt * 11.0)
        mix[n0:n0 + nk] += thump * 0.34

    # 层 7：低频噪声涨落（"能量在聚集"）
    sw = lowpass(RNG.uniform(-1, 1, n), 90) * 2.2
    sw *= np.clip((t - 26.0) / 30.0, 0.0, 1.0) * np.clip((DUR - 6.0 - t) / 12.0, 0.0, 1.0)
    mix += sw * 0.12

    # 收尾：混响 -> 归一化 -> 两端淡入淡出
    mix = reverb(mix, mix=0.34)
    peak = float(np.max(np.abs(mix))) or 1.0
    mix = mix / peak * 0.92
    f = n_of(1.5)
    mix[:f] *= np.linspace(0.0, 1.0, f)
    mix[-f:] *= np.linspace(1.0, 0.0, f)
    return mix


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    os.makedirs(os.path.dirname(TMP), exist_ok=True)
    x = build()
    pcm = np.clip(x, -1.0, 1.0)
    data = (pcm * 32767.0).astype("<i2").tobytes()
    with wave.open(TMP, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(data)
    print("[1/2] WAV 已合成 %s（%.1f 秒，%.1f MB）"
          % (TMP, DUR, len(data) / 1048576.0))

    ff = shutil.which("ffmpeg")
    if not ff:
        print("[!] 找不到 ffmpeg：只留 WAV。"
              "注意 MCI 播 WAV 时 repeat 会失败（错误 259），循环会有缝。")
        return 0
    dst = os.path.join(OUTDIR, "primordial.mp3")
    cmd = [ff, "-y", "-loglevel", "error", "-i", TMP,
           "-codec:a", "libmp3lame", "-b:a", "160k", dst]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("[X] ffmpeg 编码失败：%s" % (r.stderr or "")[-400:])
        return 1
    print("[2/2] MP3 已输出 %s（%.1f MB）"
          % (dst, os.path.getsize(dst) / 1048576.0))
    print("      峰值 %.3f，音频长度 %.1f 秒" % (float(np.max(np.abs(x))), DUR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
