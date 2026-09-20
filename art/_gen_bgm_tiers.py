# -*- coding: utf-8 -*-
"""合成「殿堂 / 始祖」两首开牌庆典 stinger（不需要外部音乐模型）。

为什么自己合成而不是调用 mmx：
  本机的 mmx CLI **没有音乐生成能力**（只有 text / speech / image / video /
  search / vision 六个子命令，"音乐/音频生成"不在其中），
  所以"每个等级专属的音乐素材"这条路只能靠程序化合成 ——
  好在它零依赖、零版权，而且每个参数都能改、改完立刻重跑。

七个档位各一首（凑齐"每个等级专属的音乐素材"）：

  · draft_rare.mp3    ~1.0s   稀有：两记木琴，短促清亮，不打断节奏。
  · draft_epic.mp3    ~1.5s   史诗：三记，pad 略厚。
  · draft_legend.mp3  ~2.0s   传说：四记，开始有低频。
  · draft_myth.mp3    ~2.5s   神级：五记 + 高频闪光 + 落地。
  · draft_ultra.mp3   ~3.0s   传奇：六记，接近殿堂的厚重感。
  · celeb_hall.mp3    ~3.6s   殿堂：七记满琶音 + 铜管 pad + 曝光闪光。
  · celeb_primordial.mp3 ~6.4s 始祖：次低音 + 创世大钟 + 圣咏 + 过曝撞击。

  ★ 五首低档 stinger 由**同一个阶梯函数**生成（`build_ladder`），
    差别只在三个量：琶音记数（2→6）、时长、pad 的低音厚度。
    为什么这样做：用户要求"风格统一"，同一段琶音动机逐级加长加厚，
    听起来就是"同一套音乐语言按档位升级"，而不是七首各自为政的曲子。
    五首都落在 C 大调上（复用殿堂那串 C4 E4 G4 C5 E5 G5 C6 的子集），
    所以连抽两张也能自然衔接。

两首庆典曲的分工：
  · celeb_hall.mp3        ~3.6s  殿堂降临：明亮、昂扬、金属光泽。
                                  大三和弦上行琶音 + 铜管感 pad + 高频闪光，
                                  情绪是"强力援军到场"。
  · celeb_primordial.mp3  ~6.2s  始祖降临：更低、更宽、更庄严。
                                  次低音 drone + 创世大钟 + 圣咏式 pad，
                                  中段一次明显的"过曝"撞击，收尾留长混响尾巴，
                                  情绪是"世界被改写"。

接入点：celebBegin()（殿堂/始祖开牌演出开始时播放），
所以它们不是循环 BGM，而是**一次性 stinger** —— 这点决定了：
  · 必须自带淡入淡出（两端各 60~90ms），MCI 播完不能有爆音；
  · 长度要和演出时长匹配（殿堂演出 2.8s / 始祖 4.2s，stinger 略长一点）。

用法： python art/_gen_bgm_tiers.py
"""
import os
import shutil
import subprocess
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# 复用既有的合成原语（同一套音色，两首曲子才像是"同一个世界观"）
from _gen_bgm import n_of, t_axis, sine, env_attack, lowpass, reverb, bell  # noqa: E402

OUTDIR = os.path.join(ROOT, "assets", "bgm")      # mp3 留档（可试听）
SFXDIR = os.path.join(ROOT, "assets", "sfx")      # wav —— 游戏真正播的那份
TMPDIR = os.path.join(HERE, "gen2")
RNG = np.random.default_rng(20260921)


def fade(x, fin=0.02, fout=0.06):
    """两端淡入淡出。stinger 播完不能有"啪"的一声。"""
    n = len(x)
    a = min(n, n_of(fin))
    b = min(n, n_of(fout))
    e = np.ones(n)
    if a:
        e[:a] = np.linspace(0.0, 1.0, a)
    if b:
        e[-b:] = np.linspace(1.0, 0.0, b)
    return x * e


def stack(dur, events):
    """把一堆 (起始秒, 波形) 叠到一条长缓冲上。越界的部分自动裁掉。"""
    n = n_of(dur)
    mix = np.zeros(n, dtype=np.float64)
    for t0, y in events:
        i = n_of(t0)
        if i >= n:
            continue
        k = min(len(y), n - i)
        mix[i:i + k] += y[:k]
    return mix


# =========================================================================
# 殿堂：明亮上行琶音 + 铜管 pad
# =========================================================================
def build_hall():
    DUR = 3.6
    n = n_of(DUR)
    # C 大调（C4 E4 G4 C5 E5 G5 C6）：五声音阶感强、昂扬
    arp = [261.63, 329.63, 392.00, 523.25, 659.26, 783.99, 1046.50]
    ev = []
    # ① 上行琶音钟：每 0.16s 一记，共 7 记，力度递减（"冲上去"）
    for i, f in enumerate(arp):
        _, y = bell(n_of(0.0), f, dur=2.0, amp=0.30 * (1.0 - 0.07 * i))
        ev.append((0.16 * i, y))
    # ② 铜管 pad：C 大三和弦，起音 0.25s（"铺垫"，不能抢琶音）
    for f, a in ((130.81, 0.16), (196.00, 0.12), (261.63, 0.10), (329.63, 0.08)):
        y = sine(f, n) * 0.5 + sine(f * 1.005, n) * 0.5      # 轻微失谐 = 厚重
        y *= np.clip(np.linspace(0.0, 1.6, n), 0.0, 1.0)
        ev.append((0.0, y * a * env_attack(n, 0.25, 0.5)))
    # ③ 高频闪光：只在 0.35~1.2s，制造"曝光"瞬间
    sh = np.zeros(n)
    for f in (2093.0, 2637.0, 3136.0):
        seg = sine(f, n) * np.exp(-np.maximum(t_axis(n) - 0.35, 0.0) * 2.6)
        seg[t_axis(n) < 0.35] = 0.0
        sh += seg
    ev.append((0.0, sh * 0.045))
    # ④ 低频落地：一记 soft impact，让"降临"有重量
    imp = sine(55.0, n) * np.exp(-t_axis(n) * 4.0)
    ev.append((0.0, imp * 0.22))
    x = stack(DUR, ev)
    x = reverb(x, mix=0.26)
    return fade(x, 0.02, 0.10), DUR


# =========================================================================
# 始祖：次低音 drone + 创世大钟 + 圣咏 pad + 过曝撞击
# =========================================================================
def build_primordial():
    DUR = 6.4
    n = n_of(DUR)
    t = t_axis(n)
    ev = []
    # ① 次低音：A1 55Hz + 27.5Hz 垫底，极慢呼吸 —— "巨大的东西在靠近"
    sub = sine(55.0, n) * 0.42 + sine(27.5, n) * 0.30
    sub *= 1.0 + 0.35 * sine(0.11, n)
    ev.append((0.0, sub * env_attack(n, 0.35, 1.2)))
    # ② 失谐 drone：A2/E2，制造"广袤"
    for f, a in ((110.0, 0.16), (110.6, 0.14), (82.41, 0.15), (82.9, 0.12)):
        ev.append((0.0, sine(f, n) * a * env_attack(n, 0.5, 1.4)))
    # ③ 圣咏 pad：A 小调九和弦（A C E G B），长起音，像唱诗班
    for f, a in ((220.0, 0.085), (261.63, 0.070), (329.63, 0.065),
                 (392.0, 0.055), (493.88, 0.045)):
        ev.append((0.0, sine(f, n) * a * np.clip((t - 0.5) / 1.8, 0.0, 1.0)
                   * env_attack(n, 0.8, 1.0)))
    # ④ 创世大钟：A2 的低钟，第 0.15s 与第 2.9s 各一记（第二记轻）
    for t0, amp in ((0.15, 0.34), (2.90, 0.20)):
        sh = n_of(t0)
        _, y = bell(sh, 110.0, dur=4.2, amp=amp)
        # bell() 内部按 dur 生成并以 n0 为起点，所以这里直接接上
        ev.append((0.0, np.concatenate([np.zeros(sh), y])))
    # ⑤ 过曝撞击：3.3s 一次短促的白噪 + 低频，模拟"世界被改写"
    hit = n_of(0.5)
    nt = np.linspace(0.0, 1.0, hit)
    noise = RNG.normal(0.0, 1.0, hit) * np.exp(-nt * 9.0)
    noise = lowpass(noise, 6)
    boom = sine(41.2, hit) * np.exp(-nt * 5.0)
    ev.append((3.30, (noise * 0.30 + boom * 0.42)))
    # ⑥ 上行微光：4.0s 起一串很轻的高频，收尾"亮起来"
    for i, f in enumerate((1318.5, 1760.0, 2093.0, 2637.0)):
        ev.append((4.05 + 0.14 * i, sine(f, n) * 0.030))
    x = stack(DUR, ev)
    x = reverb(x, mix=0.40)          # 比殿堂更长的尾巴：更宏大
    return fade(x, 0.03, 0.22), DUR


# =========================================================================
# 低档位阶梯（稀有 → 传奇）：同一动机，逐级加长加厚
# =========================================================================
def build_ladder(notes, dur, pad, impact, shimmer, rev, step):
    """一档一首。所有可变量都从参数进来，保证五首"只差规模、不差性格"。"""
    n = n_of(dur)
    ev = []
    # ① 上行琶音钟：与殿堂同一动机的前 N 个子集（殿下的由 build_hall 走满 7 记）
    for i, f in enumerate(notes):
        _, y = bell(0, f, dur=min(2.0, dur * 0.8), amp=0.30 - 0.02 * i)
        ev.append((step * i, y))
    # ② 铜管 pad：起音比殿堂更短，否则低档位会"听起来很隆重"、与档位不符
    for f, a in pad:
        y = sine(f, n) * 0.5 + sine(f * 1.005, n) * 0.5
        y *= np.clip(np.linspace(0.0, 1.6, n), 0.0, 1.0)
        ev.append((0.0, y * a * env_attack(n, 0.14, 0.35)))
    # ③ 高频闪光：档位越高越多，制造"亮起来"的差别（殿堂为 0.045）
    if shimmer > 0.0:
        sh = np.zeros(n)
        t = t_axis(n)
        for f in (2093.0, 2637.0, 3136.0):
            seg = sine(f, n) * np.exp(-np.maximum(t - 0.30, 0.0) * 2.8)
            seg[t < 0.30] = 0.0
            sh += seg
        ev.append((0.0, sh * shimmer))
    # ④ 低频落地：殿堂是 0.22，低档位按比例给，"重量"随档位增长
    if impact > 0.0:
        ev.append((0.0, sine(55.0, n) * np.exp(-t_axis(n) * 4.0) * impact))
    x = stack(dur, ev)
    x = reverb(x, mix=rev)
    return fade(x, 0.02, min(0.12, dur * 0.12)), dur


# 档位 → 参数。琶音序列都是 build_hall 那串的子集，逐级多一记。
LADDER = (
    ("draft_rare",   [523.25, 659.26],                                 1.00,
     ((130.81, 0.09), (196.00, 0.06)),                                 0.00, 0.000, 0.15, 0.12),
    ("draft_epic",   [523.25, 659.26, 783.99],                          1.50,
     ((130.81, 0.11), (196.00, 0.08), (261.63, 0.06)),                 0.07, 0.015, 0.18, 0.13),
    ("draft_legend", [392.00, 523.25, 659.26, 783.99],                  2.00,
     ((130.81, 0.13), (196.00, 0.10), (261.63, 0.07), (329.63, 0.05)), 0.12, 0.025, 0.21, 0.14),
    ("draft_myth",   [329.63, 392.00, 523.25, 659.26, 783.99],          2.50,
     ((98.00, 0.14), (130.81, 0.12), (196.00, 0.09), (261.63, 0.07)),  0.16, 0.035, 0.23, 0.15),
    ("draft_ultra",  [261.63, 329.63, 392.00, 523.25, 659.26, 783.99],  3.00,
     ((65.41, 0.14), (98.00, 0.13), (130.81, 0.11), (196.00, 0.08),
      (261.63, 0.06)),                                                  0.19, 0.042, 0.25, 0.16),
)


def encode(name, x, dur, **kw):
    os.makedirs(OUTDIR, exist_ok=True)
    os.makedirs(TMPDIR, exist_ok=True)
    tmp = os.path.join(TMPDIR, "_%s.wav" % name)
    pk = float(np.max(np.abs(x))) or 1.0
    pcm = np.clip(x / pk * 0.92, -1.0, 1.0)          # 归一化，避免削顶
    with wave.open(tmp, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(22050)
        w.writeframes((pcm * 32767.0).astype("<i2").tobytes())
    # ① 游戏真正播的那份：assets/sfx/<name>.wav（sfxPlay 的路径约定）
    os.makedirs(SFXDIR, exist_ok=True)
    wav = os.path.join(SFXDIR, name + ".wav")
    shutil.copyfile(tmp, wav)
    print("  ✔ %-26s %.1fs  %6.1f KB  峰值 %.3f  ← sfxPlay 用这份"
          % ("sfx/" + name + ".wav", dur, os.path.getsize(wav) / 1024.0, pk))
    # ② 留档 mp3（可试听 / 可分享）
    ff = shutil.which("ffmpeg")
    if not ff:
        print("      （没有 ffmpeg，跳过 mp3 留档）")
        return wav
    dst = os.path.join(OUTDIR, name + ".mp3")
    r = subprocess.run([ff, "-y", "-loglevel", "error", "-i", tmp,
                        "-codec:a", "libmp3lame", "-b:a", "160k", dst],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("      [X] mp3 留档失败：%s" % (r.stderr or "")[-200:])
        return wav
    print("  ✔ %-26s %.1fs  %6.1f KB  ← 留档"
          % ("bgm/" + name + ".mp3", dur, os.path.getsize(dst) / 1024.0))
    return wav


JOBS = ([("draft_rare",   lambda a=LADDER[0]: build_ladder(a[1], a[2], a[3], a[4], a[5], a[6], a[7])),
         ("draft_epic",   lambda a=LADDER[1]: build_ladder(a[1], a[2], a[3], a[4], a[5], a[6], a[7])),
         ("draft_legend", lambda a=LADDER[2]: build_ladder(a[1], a[2], a[3], a[4], a[5], a[6], a[7])),
         ("draft_myth",   lambda a=LADDER[3]: build_ladder(a[1], a[2], a[3], a[4], a[5], a[6], a[7])),
         ("draft_ultra",  lambda a=LADDER[4]: build_ladder(a[1], a[2], a[3], a[4], a[5], a[6], a[7])),
         ("celeb_hall",   build_hall),
         ("celeb_primordial", build_primordial)])


def main():
    print("== 合成七档开牌 stinger（稀有 / 史诗 / 传说 / 神级 / 传奇 / 殿堂 / 始祖）==")
    if not shutil.which("ffmpeg"):
        print("  [!] 找不到 ffmpeg，只能出 WAV（mp3 留档会跳过）")
    ok = 0
    for name, fn in JOBS:
        try:
            x, d = fn()
        except Exception as e:                     # noqa: BLE001
            print("  [X] %-18s 合成失败：%s" % (name, e))
            continue
        if encode(name, x, d):
            ok += 1
    print("\n  完成 %d / %d" % (ok, len(JOBS)))
    return 0 if ok == len(JOBS) else 1


if __name__ == "__main__":
    sys.exit(main())
