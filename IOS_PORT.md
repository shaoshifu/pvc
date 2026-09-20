# iOS / IPA 移植方案（进行中）

目标：把这个 Windows 版（C + GDI + D3D9 + MCI）做成能在 iPad 上跑的 iOS 应用，
产出**未签名 IPA**，由用户用自己的 Apple ID 签名安装。

---

## 0. 先讲清两件不可能的事

| 需求 | 能不能 | 说明 |
|---|---|---|
| 在这台 Windows 上产出 IPA | **不能** | IPA 需要 Apple 的 iOS SDK，官方只在 macOS 上提供。没有任何 Windows 工具链能合法产生 .ipa |
| 我替你签名 | **不能** | 签名需要你的证书 + iPad 的 UDID，这是 Apple 的设计，只能你来 |

**所以路线是**：我负责移植 + 构建流程 → 你负责在手机上签名安装。

```
[C 源码 + 资源] --(我: 移植到可移植层)--> [Xcode 工程]
   --(GitHub Actions macOS 免费编译)--> [未签名 .ipa]
   --(你: Sideloadly / AltStore)--> [签你的 Apple ID] --> [iPad 上运行]
```

---

## 1. 为什么这是「换后端」而不是重写（实测数据）

| 项 | 实测 |
|---|---|
| 源码总量 | 13353 行 |
| 需要替换的平台代码 | 约 **900 行**（WinMain / 窗口 / 音频 / 输入） |
| 需要重写的绘制原语 | **22 个**（`fillRect` / `fillEllipse` / `poly` / `spriteBlit` / `putText*` / `pushRoundClip` …） |
| 游戏逻辑 | 约 **12300 行，纯 C + 浮点，零平台依赖** |
| 需要 shim 的 Win32 符号 | **35 个** |

关键点：
- 所有绘制都收在那 22 个原语里，`HDC` 只是个"画布"参数 →
  **`typedef HDC = CGContextRef`** 就能把整条渲染链换成 CoreGraphics。
- 坐标变换只用**缩放 + 平移**（只用 `eM11`/`eDx`，没有旋转）→ 直接对应 CG 的 CTM。
- 已经是三层离屏缓冲（`gWorldDC` / `gFrameDC` / `gScreenDC`）→ 天然适配 CG bitmap context。
- `d3d9` 只是可选呈现后端，**iOS 直接丢掉**，不需要移植。

---

## 2. 阶段划分

### Phase 1 ✔ 资源压缩（已完成并验证）

BMP 是未压缩格式，795 张贴图占了整个包的大头。

| | 之前 | 之后 |
|---|---|---|
| 贴图 | 259.3 MB（795 张 BMP） | **47.6 MB**（774 PNG + 21 JPEG） |
| 加上音频（54 MB，mp3 压不动） | 约 313 MB | 约 **102 MB** |

规则（`art/_pack_assets.py`）：
- **带透明**的精灵 → PNG（必须无损）
- **整屏不透明**的图（草坪 / 菜单背景，21 张）→ JPEG q92（PNG 对照片类纹理压不动）

⚠️ **预乘 alpha 陷阱**：这些 BMP 是 32 位 BGRA 且 **alpha 已预乘**（引擎用 `AlphaBlend`，
它要求源是预乘的）。如果按常规做法"反预乘 → 存 PNG → 加载时再预乘"，**会丢数据**：
`A=1, RGB=200 → 反预乘 51000 → 截到 255 → 预乘回 1`，低 alpha 描边整片变暗。
所以 PNG 里**直接存预乘后的字节**（PNG 不校验 RGB ≤ A），加载时只做一次 BGRA↔RGBA 交换，**全程零误差**。

加载器改造：内嵌 `stb_image`（单头文件，公共领域，只开 PNG/JPEG 分支）+
`spriteLoadName` 先试 `.png` 再试 `.jpg`。

验证（`_test_asset_load.c`）：
- **9 张 PNG 路径：与源 BMP 逐字节一致**
- 3 张 JPEG 路径：平均色差 1.1~2.5（肉眼不可分），
  且"上下翻转 / R-B 互换"的差值大 **10~80 倍** → 方向与通道顺序都没错
- 实机抓帧：电路草坪（JPEG）+ UI + 植物贴图（PNG）全部正确渲染
- 全部回归测试保持通过

### Phase 2 可移植层（下一步）

新增 `plat.h`（声明）+ 两个后端：
- `plat_win32.c`：把现有 Win32 代码搬过来（行为不变，Windows 版继续可用）
- `plat_ios.m`：CoreGraphics 绘制 + CoreText 文字 + AVAudioPlayer/AVAudioEngine 音频 + 触摸
- 资源读取从宽字符路径改成 UTF-8 + `NSBundle`/`fopen`（iOS 沙箱）
- **Windows 上就能证明移植没走样**：同一个 `plat.h` 两套后端，
  用现有抓帧链路**逐像素比对同一帧**。比对通过再交给 macOS 编译，
  iOS 那边第一次编译成功的概率就高得多。

### Phase 3 Xcode 工程 + GitHub Actions

- 生成 `project.yml`（XcodeGen）或直接手写 `project.pbxproj`
- GitHub Actions `macos-latest`：编译 → `xcodebuild archive` → **未签名 IPA** → 上传 artifact
- 你下载后用 Sideloadly 签自己的 Apple ID

---

## 3. 三个你必须知道的现实问题

| 问题 | 影响 | 对策 |
|---|---|---|
| **免费 Apple ID 签名有效期 7 天** | 7 天后 App 打不开，要重签 | Sideloadly 重签一条命令；付费开发者账号（$99/年）可一年一签 |
| **鼠标键盘 → 触摸** | 真正的 UX 工作量（不是技术难点） | 点按=点击、拖动=移动、长按=悬停；快捷键（空格/W/M/E）做成一排屏幕按钮 |
| **性能** | iPad 跑这套 2D + 数百精灵**应该没问题**，但需实机验证 | 保留 SSAA 开关；必要时降采样 |

---

## 4. 不做的事（明确边界）

- 不碰游戏逻辑（12300 行一行不改）——所有改动都在平台层
- 不下架 Windows 版；两套后端共用同一份 `pvz.c`
- 不做 App Store 上架（需要开发者账号 + 审核，与"自己签名自用"是两条路）
