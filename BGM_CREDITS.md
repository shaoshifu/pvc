# 背景音乐来源与许可

全部曲目均为 **CC0（公共领域）** —— 可商用、可修改、**无需署名**、且不可撤销。
本文件只作来源存档，方便日后追溯或替换。

| 关卡 | 文件 | 曲目 | 作者 | 许可 | 来源 |
| --- | --- | --- | --- | --- | --- |
| 0 前院 · 白天 | `assets/bgm_level1.wav` | Jazz AI（用户指定） | — | 用户自有 | 本地文件 |
| 1 后院 · 泳池 | `assets/bgm/lv1.mp3` | Plains of Luminescence (loop) | vitalezzz | CC0 | OpenGameArt |
| 2 月夜 · 墓园 | `assets/bgm/lv2.mp3` | Tempo | pauliuw | CC0 | OpenGameArt |
| 3 屋顶 · 天台 | `assets/bgm/lv3.mp3` | Four Loop | pauliuw | CC0 | OpenGameArt |
| 4 沙漠 · 遗迹 | `assets/bgm/lv4.mp3` | Dumus | pauliuw | CC0 | OpenGameArt |
| 5 无尽 · 挑战 | `assets/bgm/lv5.mp3` | Fast Background | pauliuw | CC0 | OpenGameArt |

原始页面：
- https://opengameart.org/content/plains-of-luminescence
- https://opengameart.org/node/4232 （Music Loops，含 tempo / four_loop / dumus / fast_background）

## 为什么选 CC0 而不是 CC-BY

- **CC0 不产生署名义务**：CC-BY 要求在游戏内可见位置放版权行（如 Incompetech 的
  Kevin MacLeod 曲目就强制要求 "Music by Kevin MacLeod (incompetech.com)"）。
  这个项目目前没有 credits 界面，用 CC-BY 就得先补一个，属于额外工作量。
- **CC0 不可撤销**：公共领域声明一旦发出不能反悔，长期发行（移植、续作、上架）最安全。
- Pixabay 虽然也免署名，但它的 License 是**自有的简化许可**而非 CC，条款允许平台
  单方面调整，长期项目不如 CC0 稳。FreePD（原 CC0 首选站）已于 2026 年关站。

## 注意：这些是 MP3，必须用 mpegvideo 解码器

Windows MCI 的 `waveaudio` 类型**只认 WAV**，喂 MP3 会返回错误 296
「无法在指定的 MCI 设备上播放指定的文件」。所以 `bgmPlayFile()` 会按扩展名
自动切到 `mpegvideo`（实测 `status mode` 返回 `playing`）。
**如果要换成 .ogg，这条路走不通** —— MCI 在无第三方 DirectShow 滤镜时解不了 OGG，
需要改用其他播放后端。

## 换曲方法

改 `pvz.c` 里的 `BGM_BY_LEVEL[]` 即可，下标 = `gCurLevel`：

```c
static const wchar_t *BGM_BY_LEVEL[LV_COUNT] = {
    L"bgm_level1.wav",   /* 0 前院 · 白天 */
    L"bgm/lv1.mp3",      /* 1 后院 · 泳池 */
    ...
};
```

路径相对 `assets/`（`bgmPlayFile` 会拼上 `gAssetDir`）。
填 `NULL` 表示该关不放音乐，进关时会静默跳过。
