# -*- coding: utf-8 -*-
"""为 5 个新关卡生成 BGM。

为什么自己合成而不是调 API：
  · mmx CLI 1.0.25 只暴露 text/speech/image/video/search/vision，**没有 music**；
  · MiniMax 平台的 /v1/music_generation 对本账号返回 410
    「no longer available to new users」；
  · ACE Studio 需要 GUI 常驻且以歌声合成为主。
  所以这里用纯 Python 合成 —— 好处是**完全可控**：每关的情绪、
  和声走向、配器都能精确对齐关卡设计，而不是靠 prompt 碰运气。

输出：5 个 44.1kHz 立体声 WAV，首尾可无缝循环，再由 ffmpeg 转 MP3。

用法： python _gen_music.py
"""
import os
import struct
import subprocess
import sys
import wave

import numpy as np

SR = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets_new", "bgm")

# ============================================================================
# 基础合成单元
# ============================================================================


def env_adsr(n, a, d, s, r, sustain_level=0.7):
    """ADSR 包络。a/d/r 单位秒，s 单位秒。返回长度 n 的数组。"""
    a_n, d_n, r_n = int(a * SR), int(d * SR), int(r * SR)
    s_n = max(0, n - a_n - d_n - r_n)
    parts = []
    if a_n:
        parts.append(np.linspace(0, 1, a_n, endpoint=False) ** 1.5)
    if d_n:
        parts.append(np.linspace(1, sustain_level, d_n, endpoint=False))
    if s_n:
        parts.append(np.full(s_n, sustain_level))
    if r_n:
        parts.append(np.linspace(sustain_level, 0, r_n) ** 1.8)
    e = np.concatenate(parts) if parts else np.zeros(n)
    if len(e) < n:
        e = np.pad(e, (0, n - len(e)))
    return e[:n]


def env_perc(n, attack=0.004, decay=0.35, power=2.2):
    """打击/拨奏类包络：极快起音 + 指数衰减。"""
    t = np.arange(n) / SR
    e = np.exp(-t / max(decay, 1e-4)) ** power
    a_n = max(1, int(attack * SR))
    e[:a_n] *= np.linspace(0, 1, a_n)
    return e


def osc(wave, freq, n, phase=0.0):
    """生成一段波形。freq 可以是标量或长度 n 的数组（用于滑音/颤音）。"""
    t = np.arange(n) / SR
    ph = 2 * np.pi * (np.cumsum(np.broadcast_to(freq, n)) / SR) + phase
    if wave == "sin":
        return np.sin(ph)
    if wave == "tri":
        return 2 / np.pi * np.arcsin(np.sin(ph))
    if wave == "saw":
        return 2 * (ph / (2 * np.pi) % 1.0) - 1
    if wave == "sqr":
        return np.sign(np.sin(ph))
    if wave == "pulse":
        return np.where((ph / (2 * np.pi) % 1.0) < 0.3, 1.0, -1.0)
    raise ValueError(wave)


def noise(n, rng):
    return rng.uniform(-1, 1, n)


def lowpass(x, cutoff):
    """一阶低通。cutoff 单位 Hz（近似），用于给锯齿/噪声去毛刺。"""
    a = np.exp(-2 * np.pi * cutoff / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a) * x[i] + a * acc
        y[i] = acc
    return y


def lowpass_fast(x, cutoff):
    """用 FFT 做低通 —— 比逐样本递归快几百倍，长音色必须用这个。"""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X *= 1.0 / (1.0 + (f / max(cutoff, 20.0)) ** 2)
    return np.fft.irfft(X, len(x))


def highpass_fast(x, cutoff):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X *= 1.0 - 1.0 / (1.0 + (f / max(cutoff, 20.0)) ** 2)
    return np.fft.irfft(X, len(x))


def midi(m):
    """MIDI 音高 → 频率。"""
    return 440.0 * 2 ** ((m - 69) / 12.0)


# ============================================================================
# 混音总线
# ============================================================================


class Mix:
    def __init__(self, seconds):
        self.n = int(seconds * SR)
        self.buf = np.zeros((self.n, 2), dtype=np.float64)

    def add(self, sig, start_s, pan=0.0, gain=1.0):
        i0 = int(start_s * SR)
        if i0 >= self.n:
            return
        seg = sig[: self.n - i0]
        l = np.sqrt(0.5 * (1 - np.clip(pan, -1, 1)))
        r = np.sqrt(0.5 * (1 + np.clip(pan, -1, 1)))
        self.buf[i0:i0 + len(seg), 0] += seg * l * gain
        self.buf[i0:i0 + len(seg), 1] += seg * r * gain

    def finish(self, peak=0.86):
        """软限幅 + 归一化。软限幅比硬裁剪自然，不会有刺耳的削波声。"""
        b = np.tanh(self.buf * 1.15)
        m = np.max(np.abs(b))
        if m > 1e-9:
            b *= peak / m
        return b


# ============================================================================
# 配器音色
# ============================================================================


def inst_bell(freq, dur, amp=1.0, rng=None):
    """铃 / 钟琴：冰凉通透，用于冰原关。"""
    n = int(dur * SR)
    e = env_perc(n, 0.002, dur * 0.30, 2.6)
    s = osc("sin", freq, n) * 1.0
    s += osc("sin", freq * 2.76, n) * 0.34 * np.exp(-np.arange(n) / SR / (dur * 0.10))
    s += osc("sin", freq * 5.40, n) * 0.13 * np.exp(-np.arange(n) / SR / (dur * 0.05))
    return s * e * amp


def inst_pad(freqs, dur, amp=1.0, cutoff=1800, detune=0.006):
    """弦垫：多个失谐锯齿叠加 + 慢起音，用于铺和声底色。"""
    n = int(dur * SR)
    e = env_adsr(n, dur * 0.30, 0.15, dur * 0.40, dur * 0.30, 0.85)
    s = np.zeros(n)
    for f in freqs:
        for d in (-detune, 0.0, detune):
            s += osc("saw", f * (1 + d), n)
    s = lowpass_fast(s / max(len(freqs) * 3, 1), cutoff)
    return s * e * amp


def inst_choir(freqs, dur, amp=1.0):
    """合唱：正弦谐波堆叠 + 共振峰感，用于终局史诗感。"""
    n = int(dur * SR)
    e = env_adsr(n, dur * 0.35, 0.20, dur * 0.45, dur * 0.35, 0.9)
    s = np.zeros(n)
    for f in freqs:
        for h, w in ((1, 1.0), (2, 0.45), (3, 0.28), (4, 0.14), (5, 0.07)):
            s += osc("sin", f * h, n) * w
    s = lowpass_fast(s, 2600)
    return s * e * amp


def inst_strings(freq, dur, amp=1.0):
    n = int(dur * SR)
    e = env_adsr(n, 0.09, 0.12, dur * 0.5, 0.28, 0.8)
    s = sum(osc("saw", freq * (1 + d), n) for d in (-0.008, -0.003, 0.003, 0.008))
    s = lowpass_fast(s / 4, 2400)
    return s * e * amp


def inst_bass(freq, dur, amp=1.0, drive=1.0):
    """贝斯：正弦基频 + 少量锯齿提供泛音，带一点过载。"""
    n = int(dur * SR)
    e = env_adsr(n, 0.006, 0.10, dur * 0.6, 0.12, 0.85)
    s = osc("sin", freq, n) + 0.42 * osc("saw", freq, n) + 0.18 * osc("sqr", freq * 0.5, n)
    s = np.tanh(s * drive)
    s = lowpass_fast(s, 900)
    return s * e * amp


def inst_lead(freq, dur, amp=1.0, wave="sqr", vib=5.0):
    n = int(dur * SR)
    e = env_adsr(n, 0.012, 0.10, dur * 0.55, 0.16, 0.75)
    t = np.arange(n) / SR
    f = freq * (1 + 0.004 * np.sin(2 * np.pi * vib * t))
    s = osc(wave, f, n)
    s = lowpass_fast(s, 3200)
    return s * e * amp


def inst_pluck(freq, dur, amp=1.0, rng=None):
    """拨弦：噪声激励 + 快速衰减，机械关的序列音色。"""
    n = int(dur * SR)
    e = env_perc(n, 0.001, dur * 0.22, 2.4)
    s = osc("tri", freq, n) * 0.7 + osc("saw", freq * 1.005, n) * 0.3
    s = lowpass_fast(s, 4200)
    return s * e * amp


def inst_brass(freq, dur, amp=1.0):
    """铜管感的低音号：厚、带侵略性，用于熔岩关。"""
    n = int(dur * SR)
    e = env_adsr(n, 0.035, 0.12, dur * 0.5, 0.22, 0.8)
    s = osc("saw", freq, n) * 1.0 + osc("saw", freq * 1.003, n) * 0.7
    s = lowpass_fast(s, 1400)
    s = np.tanh(s * 1.4)
    return s * e * amp


# ---- 打击组 ----
def dr_kick(amp=1.0, dur=0.34):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = 118 * np.exp(-t * 26) + 44
    e = np.exp(-t * 7.5)
    return np.tanh(osc("sin", f, n) * 1.7) * e * amp


def dr_timpani(freq, amp=1.0, dur=1.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = freq * (1 + 0.05 * np.exp(-t * 16))
    e = np.exp(-t * 3.0)
    return (osc("sin", f, n) + 0.3 * osc("sin", f * 1.5, n)) * e * amp


def dr_snare(amp=1.0, rng=None, dur=0.20):
    n = int(dur * SR)
    rng = rng or np.random.default_rng(7)
    e = np.exp(-np.arange(n) / SR * 22)
    body = osc("sin", 195, n) * 0.5
    nz = highpass_fast(noise(n, rng), 900)
    return (nz * 0.85 + body) * e * amp


def dr_hat(amp=1.0, rng=None, dur=0.075, open_=False):
    n = int(dur * SR * (3 if open_ else 1))
    rng = rng or np.random.default_rng(11)
    e = np.exp(-np.arange(n) / SR * (9 if open_ else 46))
    return highpass_fast(noise(n, rng), 6200) * e * amp * 0.55


def dr_metal(amp=1.0, dur=0.45, rng=None):
    """金属敲击：机械关的工业打击。用不谐和正弦簇模拟。"""
    n = int(dur * SR)
    e = np.exp(-np.arange(n) / SR * 8)
    s = np.zeros(n)
    rng = rng or np.random.default_rng(3)
    for f in (420, 631, 948, 1520):
        s += osc("sin", f * (1 + rng.uniform(-0.01, 0.01)), n)
    return lowpass_fast(s / 4, 5200) * e * amp


def dr_riser(dur, amp=1.0, rng=None):
    """上行提示音：阶段切换/波次警报。"""
    n = int(dur * SR)
    rng = rng or np.random.default_rng(5)
    t = np.arange(n) / SR
    f = 180 * 2 ** (t / dur * 2.4)
    e = np.linspace(0.15, 1.0, n) ** 1.6
    s = osc("saw", f, n) * 0.5 + highpass_fast(noise(n, rng), 2000) * 0.4
    return lowpass_fast(s, 4200) * e * amp


# ============================================================================
# 五首曲子
# ============================================================================

def track_frostbite():
    """关6 霜牙隘口：冷冽疏离。铃铛主导，稀疏的节奏，像风穿过冰谷。"""
    bpm, bars = 92, 16
    beat = 60 / bpm
    dur = bars * 4 * beat
    mx = Mix(dur)
    rng = np.random.default_rng(21)

    # Am - F - C - G  每 2 小节换一次
    prog = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]
    for bar in range(bars):
        ch = prog[(bar // 2) % 4]
        t0 = bar * 4 * beat
        mx.add(inst_pad([midi(m) for m in ch], 4 * beat * 1.05, 0.16, 1500), t0)
        # 贝斯：每小节头拍
        mx.add(inst_bass(midi(ch[0] - 12), beat * 1.8, 0.34, 0.9), t0)
        mx.add(inst_bass(midi(ch[0] - 12), beat * 1.2, 0.22, 0.9), t0 + 2.5 * beat)

    # 铃铛旋律：五声音阶，留白多
    melody = [76, 79, 81, 79, 76, 72, 74, 76,  # 前 4 小节
              77, 81, 84, 81, 79, 76, 74, 72]
    for i, m in enumerate(melody):
        t0 = i * 2 * beat
        mx.add(inst_bell(midi(m), beat * 1.6, 0.30, rng), t0, pan=-0.25)
        if i % 4 == 2:
            mx.add(inst_bell(midi(m + 12), beat * 1.2, 0.12, rng), t0 + beat, pan=0.3)

    # 中段加一层高音冰晶
    for i in range(32):
        t0 = 8 * 4 * beat + i * beat
        m = 88 + [0, 3, 7, 10][i % 4]
        mx.add(inst_bell(midi(m), beat * 0.7, 0.11, rng), t0, pan=0.4)

    # 稀疏打击：只在后半段进
    for bar in range(8, bars):
        t0 = bar * 4 * beat
        mx.add(dr_kick(0.42), t0)
        mx.add(dr_kick(0.30), t0 + 2.5 * beat)
        mx.add(dr_hat(0.34, rng), t0 + 1 * beat)
        mx.add(dr_hat(0.30, rng), t0 + 3 * beat)
    return mx.finish(), dur


def track_emberrift():
    """关7 熔心裂谷：沉重压迫。铜管 + 驱动鼓组，像岩浆在脚下翻滚。"""
    bpm, bars = 120, 16
    beat = 60 / bpm
    dur = bars * 4 * beat
    mx = Mix(dur)
    rng = np.random.default_rng(33)

    # Dm - Bb - Gm - A（自然小调 + 大调属和弦，推动感强）
    prog = [(50, 53, 57), (46, 50, 53), (43, 46, 50), (45, 49, 52)]
    for bar in range(bars):
        ch = prog[bar % 4]
        t0 = bar * 4 * beat
        mx.add(inst_pad([midi(m) for m in ch], 4 * beat, 0.13, 1100), t0)
        # 驱动贝斯：八分音符脉冲
        for k in range(8):
            if k == 3 and bar % 2 == 1:
                continue                      # 偶尔留一个空，避免死板
            mx.add(inst_bass(midi(ch[0] - 12), beat * 0.46, 0.34, 1.5),
                   t0 + k * beat * 0.5)

    # 铜管主题：低沉、有推进力
    theme = [(57, 2), (60, 1), (62, 1), (60, 2), (57, 2),
             (55, 2), (57, 1), (58, 1), (57, 4)]
    t = 0.0
    for m, b in theme:
        mx.add(inst_brass(midi(m - 12), b * beat * 0.95, 0.20), t)
        mx.add(inst_brass(midi(m), b * beat * 0.95, 0.13), t)
        t += b * beat
    t = 8 * 4 * beat
    for m, b in theme:
        mx.add(inst_brass(midi(m - 12), b * beat * 0.95, 0.22), t)
        mx.add(inst_brass(midi(m), b * beat * 0.15, 0.15), t)
        mx.add(inst_brass(midi(m + 3), b * beat * 0.95, 0.10), t)
        t += b * beat

    # 鼓组：四四拍 + 后段加密
    for bar in range(bars):
        t0 = bar * 4 * beat
        mx.add(dr_kick(0.8), t0)
        mx.add(dr_kick(0.62), t0 + 2 * beat)
        if bar >= 4:
            mx.add(dr_kick(0.5), t0 + 3.5 * beat)
        mx.add(dr_snare(0.5, rng), t0 + beat)
        mx.add(dr_snare(0.55, rng), t0 + 3 * beat)
        for k in range(8):
            mx.add(dr_hat(0.3 if k % 2 else 0.42, rng), t0 + k * beat * 0.5)
        if bar % 4 == 3:
            mx.add(dr_riser(beat * 1.8, 0.28, rng), t0 + 2.2 * beat)
    return mx.finish(), dur


def track_phantom():
    """关8 幽影回廊：诡异悬疑。低频嗡鸣 + 不谐和钟声，像在雾里摸索。"""
    bpm, bars = 78, 16
    beat = 60 / bpm
    dur = bars * 4 * beat
    mx = Mix(dur)
    rng = np.random.default_rng(47)

    # 持续低音嗡鸣（Drone）：整曲铺底，制造不安
    total = int(dur * SR)
    t = np.arange(total) / SR
    drone = (osc("sin", 55, total) * 0.5 +
             osc("sin", 55 * 1.005, total) * 0.4 +
             osc("sin", 82.5, total) * 0.22)
    drone *= 0.16 * (1 + 0.25 * np.sin(2 * np.pi * 0.06 * t))
    mx.buf[:, 0] += drone
    mx.buf[:, 1] += drone * 0.96

    # Em - Cmaj7 - Am - B7：小调里加大七与属七，悬疑感来源
    prog = [(40, 52, 55, 59), (36, 48, 52, 55, 59), (33, 45, 48, 52), (35, 47, 51, 54, 57)]
    for bar in range(bars):
        ch = prog[bar % 4]
        t0 = bar * 4 * beat
        mx.add(inst_pad([midi(m) for m in ch], 4 * beat * 1.1, 0.14, 1200), t0)

    # 不谐和钟声：小二度摩擦
    hits = [(0, 63), (1.5, 64), (3, 63), (5, 70), (6.5, 69), (8, 63),
            (9.5, 75), (11, 74), (12, 63), (13.5, 64), (15, 70)]
    for b, m in hits:
        mx.add(inst_bell(midi(m), beat * 2.4, 0.22, rng), b * beat, pan=rng.uniform(-0.5, 0.5))

    # 心跳式底鼓：慢、闷
    for bar in range(bars):
        t0 = bar * 4 * beat
        mx.add(dr_kick(0.55), t0)
        if bar >= 6:
            mx.add(dr_kick(0.34), t0 + 2 * beat)
        if bar >= 10:
            mx.add(dr_hat(0.16, rng, open_=True), t0 + 3 * beat)
    # 后段加入不安的金属摩擦
    for i in range(6):
        mx.add(dr_metal(0.16, 0.7, rng), 10 * 4 * beat + i * 2.4 * beat, pan=0.3)
    return mx.finish(), dur


def track_cogwork():
    """关9 机枢要塞：机械律动。序列琶音 + 金属打击，像活塞不停运转。"""
    bpm, bars = 132, 16
    beat = 60 / bpm
    dur = bars * 4 * beat
    mx = Mix(dur)
    rng = np.random.default_rng(59)

    # Gm - Eb - Bb - F
    prog = [(43, 46, 50), (39, 43, 46), (46, 50, 53), (41, 45, 48)]
    for bar in range(bars):
        ch = prog[bar % 4]
        t0 = bar * 4 * beat
        mx.add(inst_pad([midi(m) for m in ch], 4 * beat, 0.10, 1400), t0)
        # 活塞贝斯：十六分音符，机械感的核心
        for k in range(16):
            if k % 4 == 3:
                continue
            mx.add(inst_bass(midi(ch[0] - 12), beat * 0.22, 0.30, 1.6),
                   t0 + k * beat * 0.25)

    # 序列琶音（八分音符循环，机械关的灵魂）
    for bar in range(bars):
        ch = prog[bar % 4]
        t0 = bar * 4 * beat
        pattern = [0, 1, 2, 1, 0, 2, 1, 2]
        for k, idx in enumerate(pattern):
            m = ch[idx % len(ch)] + 12
            mx.add(inst_pluck(midi(m), beat * 0.30, 0.20, rng),
                   t0 + k * beat * 0.5, pan=-0.3 if k % 2 else 0.3)
        if bar >= 8:
            for k in range(4):
                mx.add(inst_pluck(midi(ch[(k) % 3] + 24), beat * 0.25, 0.10, rng),
                       t0 + k * beat, pan=0.45)

    # 主旋律：方波，工整的机械主题
    lead = [(70, 1), (72, 1), (74, 1.5), (70, 0.5), (67, 1), (70, 1), (72, 2),
            (67, 1), (70, 1), (72, 1.5), (74, 0.5), (75, 1), (74, 1), (70, 2)]
    for rep in range(2):
        t = 4 * 4 * beat + rep * 4 * 4 * beat
        for m, b in lead:
            mx.add(inst_lead(midi(m), b * beat * 0.85, 0.15, "pulse"), t)
            t += b * beat

    # 工业鼓组
    for bar in range(bars):
        t0 = bar * 4 * beat
        mx.add(dr_kick(0.85), t0)
        mx.add(dr_kick(0.70), t0 + 2 * beat)
        mx.add(dr_snare(0.48, rng), t0 + beat)
        mx.add(dr_snare(0.52, rng), t0 + 3 * beat)
        for k in range(8):
            mx.add(dr_hat(0.26 if k % 2 else 0.36, rng), t0 + k * beat * 0.5)
        if bar % 2 == 1:
            mx.add(dr_metal(0.30, 0.4, rng), t0 + 3.5 * beat)
    return mx.finish(), dur


def track_astral():
    """关10 星界王座：史诗终局。合唱 + 定音鼓 + 弦乐，庄严而有压迫感。"""
    bpm, bars = 108, 16
    beat = 60 / bpm
    dur = bars * 4 * beat
    mx = Mix(dur)
    rng = np.random.default_rng(71)

    # Cm - Ab - Eb - Bb（经典史诗走向，明亮与沉重交替）
    prog = [(48, 51, 55), (44, 48, 51), (51, 55, 58), (46, 50, 53)]
    for bar in range(bars):
        ch = prog[bar % 4]
        t0 = bar * 4 * beat
        mx.add(inst_choir([midi(m) for m in ch], 4 * beat * 1.02, 0.15),
               t0, pan=-0.15)
        mx.add(inst_strings(midi(ch[0] - 12), 4 * beat, 0.16), t0, pan=0.15)
        mx.add(inst_bass(midi(ch[0] - 24), 4 * beat, 0.40, 0.8), t0)
        # 定音鼓：主拍 + 后半段加密
        mx.add(dr_timpani(midi(ch[0] - 24), 0.50, 0.9), t0)
        mx.add(dr_timpani(midi(ch[0] - 24), 0.34, 0.7), t0 + 2.5 * beat)
        if bar >= 8:
            mx.add(dr_timpani(midi(ch[0] - 19), 0.26, 0.6), t0 + 1.5 * beat)
            mx.add(dr_timpani(midi(ch[0] - 19), 0.26, 0.6), t0 + 3.5 * beat)

    # 主旋律：弦乐齐奏，庄重
    theme = [(75, 3), (72, 1), (70, 2), (72, 2),
             (70, 3), (68, 1), (67, 4),
             (70, 3), (72, 1), (75, 2), (77, 2),
             (75, 4), (72, 4)]
    for rep in range(2):
        t = (2 if rep == 0 else 10) * 4 * beat
        for m, b in theme:
            mx.add(inst_strings(midi(m), b * beat * 0.92, 0.13), t, pan=-0.2)
            mx.add(inst_strings(midi(m - 12), b * beat * 0.92, 0.07), t, pan=0.2)
            t += b * beat

    # 星界琶音：铺在高频，制造"星辰闪烁"感
    for i in range(int(dur / (beat * 0.5))):
        t0 = i * beat * 0.5
        ch = prog[int(t0 / (4 * beat)) % 4]
        m = ch[i % 3] + 24 + (12 if i % 8 >= 4 else 0)
        mx.add(inst_bell(midi(m), beat * 1.1, 0.075, rng), t0,
               pan=0.5 * np.sin(i * 0.7))
    return mx.finish(), dur


TRACKS = [
    ("lv6_frostbite.mp3", track_frostbite, "关6 霜牙隘口 · 冷冽疏离"),
    ("lv7_emberrift.mp3", track_emberrift, "关7 熔心裂谷 · 沉重压迫"),
    ("lv8_phantom.mp3",   track_phantom,   "关8 幽影回廊 · 诡异悬疑"),
    ("lv9_cogwork.mp3",   track_cogwork,   "关9 机枢要塞 · 机械律动"),
    ("lv10_astral.mp3",   track_astral,    "关10 星界王座 · 史诗终局"),
]


def write_wav(path, buf):
    """写 16bit 立体声 WAV。首尾各做 8ms 淡入淡出，避免循环接缝爆音。"""
    b = buf.copy()
    f = int(0.008 * SR)
    ramp = np.linspace(0, 1, f)
    b[:f] *= ramp[:, None]
    b[-f:] *= ramp[::-1][:, None]
    data = (np.clip(b, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(data.tobytes())


def main():
    os.makedirs(OUT, exist_ok=True)
    ffmpeg = (r"C:\Users\Administrator\AppData\Local\Microsoft\WinGet\Packages"
              r"\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe"
              r"\ffmpeg-8.1.1-full_build\bin\ffmpeg.exe")
    print("输出目录: %s\n" % OUT)
    for name, fn, desc in TRACKS:
        buf, dur = fn()
        wav = os.path.join(OUT, os.path.splitext(name)[0] + ".wav")
        write_wav(wav, buf)
        mp3 = os.path.join(OUT, name)
        r = subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", wav,
                            "-codec:a", "libmp3lame", "-b:a", "160k", mp3],
                           capture_output=True, text=True, timeout=300)
        if r.returncode == 0:
            os.remove(wav)
            print("✔ %-22s %-22s %5.1fs  %dKB"
                  % (name, desc, dur, os.path.getsize(mp3) // 1024))
        else:
            print("✗ %-22s 转码失败: %s" % (name, (r.stderr or "")[-160:]))
        sys.stdout.flush()
    print("\n完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
