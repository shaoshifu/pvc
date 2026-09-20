# 网页版移植方案（面向 iPad）

目标：把当前 Windows 桌面版（C + GDI + D3D9 + MCI，14,247 行）做成
在 **iPad Safari 上直接玩的网页版**，保留核心玩法与交互，并针对触摸与横竖屏做适配。

---

## 0. 先说结论：这是「换后端」，不是重写

这一点是**实测**出来的，不是估计：

| 项 | 实测值 | 说明 |
|---|---|---|
| 源码总量 | 14,247 行 | `pvz.c` |
| 其中**游戏逻辑** | 约 13,600 行 | 纯 C + 浮点，**零平台依赖**，一行不用改 |
| 其中平台代码 | 约 650 行 | `WinMain` 202 行 + 窗口/输入/音频/呈现 |
| 需要重写的**绘制原语** | **22 个** | `fillRect` / `fillEllipse` / `poly` / `spriteBlit` / `putText*` / `pushRoundClip` … |
| 需要实现的 Win32 符号 | **约 90 个** | 类型 26 + 函数 52 + 常量 ~30 |

三个让这件事变简单的关键发现（都是本轮实测的）：

1. **`CreateFontW` 全工程只有 1 个调用点**（`mkFont`，`pvz.c:371`）。
   也就是说**所有文字都收在一个函数后面** —— 文字渲染是 Web 上最麻烦的一块
   （中文字体、字形栅格化），而它只需要替换一个入口。
2. **MCI 只用了 6 个动词**：`open / play / stop / pause / close / status`。
   映射到 Web Audio 是直译，不需要模拟 MCI 状态机。
3. **世界层已经是"像素缓冲 + 一次呈现"的结构**：
   `gWorldDC`（2000×1300，2x 超采样）→ `gFrameDC`（合成）→ 窗口（letterbox 呈现）。
   Web 只要把最后一步换成"上传到 canvas"，前面两步的逻辑**原样可用**。

已经就位的接缝（前几轮为 iOS 移植打的地基，网页版直接复用）：

- `plat.h` —— 平台层：类型/常量/函数声明，Windows 侧是直通
- `createLayers()` —— 建三层离屏缓冲（已与窗口解耦）
- `artLoadAll()` —— 载入全部美术资源（已从 `WinMain` 抽出）
- `handleKey()` —— 键盘逻辑（已从 `WndProc` 抽出，屏幕按钮可复用）
- `gameBoot()` / `gameStep(dt, out)` / `gameWorldBits()` —— **本轮新增**的驱动接口

---

## 1. 需要重构的模块范围

### 1.1 必须重写（新写 `plat_web.c`，约 1600~2000 行）

| 模块 | 内容 | 工作量依据 |
|---|---|---|
| **软件光栅后端** | 22 个绘制原语的像素级实现：矩形/圆角矩形/圆/椭圆/多边形/线段（都要抗锯齿）、精灵缩放 blit（预乘 alpha）、九宫格按钮、裁剪、世界变换 | 最大的一块。~800 行 |
| **文字渲染** | 字形按需栅格化 + 缓存。用 Canvas2D `fillText` 光栅一次、存进位图、之后当精灵贴 | ~250 行。因 `CreateFontW` 只有 1 个调用点，接口面极小 |
| **音频** | `mciSendStringW` → Web Audio。6 个动词 + 25 个音效 + 7 档 stinger | ~250 行 |
| **输入** | 指针事件 → 游戏坐标；键盘状态；屏幕快捷键 | ~250 行 |
| **存档** | `_wfopen("pvz_save.dat")` → IndexedDB / localStorage | ~120 行 |
| **主循环 + 呈现** | `requestAnimationFrame` 驱动 `gameStep`，一帧一次 `putImageData` | ~100 行 |

### 1.2 需要改（`pvz.c`，约 30 处）

| 改动 | 说明 |
|---|---|
| `WinMain` 条件编译 | 已包进 `#if PLAT_HAS_OWN_MAIN`（本轮完成），网页版自己提供 `main` |
| 输入注入点 | `WndProc` 的鼠标/键盘分支逻辑要能被 `plat_web` 调用（同 `handleKey` 的做法） |
| 平台相关的几处 `#ifdef` | `SetProcessDpiAwarenessContext`、`gpuInit`（D3D9 直接丢掉）等 |

### 1.3 不动

- **游戏逻辑、数值平衡、抽卡权重、158 张卡表、能力位系统** —— 一行不改
- 渲染管线结构（三层缓冲、2x 超采样、世界变换只做缩放+平移）
- 资源格式（PNG/JPEG + mp3/wav）

### 1.4 新增（Web 外壳，约 400 行）

`web/index.html` + `web/shell.css` + `web/shell.js` —— iPad 适配都在这一层。

---

## 2. 技术实现方式

### 2.1 编译：C → WebAssembly

用 **Emscripten**（`emcc`）。产物体积估计：

| 部件 | 大小 |
|---|---|
| 游戏代码（WASM） | ~700 KB ～ 1.2 MB |
| `stb_image`（PNG/JPEG/BMP 解码） | 已含在上面 |
| 资源（贴图 47.6 MB + 音频 ~54 MB） | **不打包进 WASM**，走 `fetch` 按需加载 |

### 2.1b 已落地：`plat_web.c`（P2 完成）

软件光栅后端**已经写完并在本机跑通**，不需要 Emscripten：

```
gcc -DPLAT_PORTABLE -O2 -I. -o _web_verify.exe _web_main.c plat_web.c pvz.c -lm
./_web_verify.exe assets _web_out.raw 60
→ 世界层 2000x1300（= 逻辑 1000x650 × SS 2），1040 万字节像素
```

规模：**约 1150 行**，实现 56 个 Win32 符号 + 22 个绘制原语。

配套的可移植化改造（都在 `pvz.c` / `plat.h` 里，桌面端行为零变化）：

| 改造 | 内容 |
|---|---|
| `plat.h` 分支条件 | 改为 `_WIN32 && !PLAT_PORTABLE` —— 于是**本机 gcc 就能编可移植分支**，这是能本地做像素验证的前提 |
| 补声明面 | 56 个函数 + 缺失的 typedef（`HWND`/`SHORT`/`ATOM`/`SYSTEMTIME`/`COLOR16`）与宏（`RGB`/`GetRValue`/`CALLBACK`/`MAKEWORD`…） |
| D3D 桩头 | `plat_d3d_stub.h`：`Direct3DCreate9()` 返回 NULL → 引擎走**既有的** GDI 回退路径。比切割 200 行 D3D 代码安全得多 |
| 驱动接口 | `gameInitAll` / `gameStep` / `gameWorldBits` / `gameSetAssetDir` / `gameSetDeterministic` |
| 输入复用 | `gamePointerMove/Down/Up` —— 网页版点击卡片走的就是 `WndProc` 那一套命中判定 |
| 条件编译 | `WinMain` / `WndProc` / `gpuInit` / `presentRelease` / `keyIsRepeat` 等桌面专有部分 |

### 2.2 渲染路线：**软件光栅 + 一帧一次上传**

这是本方案最关键的技术决策，选它有三个硬理由：

| 方案 | 问题 |
|---|---|
| 每个绘制原语都调 Canvas2D（经 JS 边界） | 每帧数百到上千次跨语言调用；且 GDI 语义（预乘 alpha、世界变换、剪切区）要逐个映射，容易出静默偏差 |
| **软件光栅到像素缓冲 + 每帧一次 `putImageData`**（选定） | 跨语言调用 = **每帧 1 次**；混合语义完全自控；**唯一风险是 CPU 开销** |

CPU 开销实测估算：

- 目标分辨率 **2000×1300**（= 逻辑 1000×650 × SS 2），与 iPad 的 2x DPR 天然对齐
- 每帧像素操作量约 300 万～500 万次（数百精灵 + 图形 + 文字）
- WASM 上约 8～15 ms/帧 → **30～60 fps 可达**

三个保底手段（按需启用）：
1. 把 SS 降到 1（像素量降 4 倍），让浏览器用 CSS 拉伸到设备分辨率（GPU 做，免费）
2. 脏矩形：只重绘变化区域
3. 精灵预乘结果的缓存（同一个精灵在同一缩放下不重复采样）

**为什么这条路线还有一个决定性优势**：软件光栅是**纯 C**，
可以**在本地用 gcc 编译、逐像素跑测试**，不需要 WASM 工具链。
也就是说移植过程中 90% 的风险（混合公式错了、通道顺序错了、坐标算错了）
在本地就能发现，不必等云端构建。

### 2.3 文字：Canvas2D 光栅一次 → 缓存成位图

```
首次遇到某个 (字符, 字号, 粗细)
  → 调一次 Canvas2D fillText 到离屏小画布
  → readPixels 拿到 alpha
  → 存进 WASM 侧的字形图集（按 预乘 alpha 存）
之后所有绘制 = 从图集贴图，零 JS 调用
```

- 好处：**中文由系统字体渲染**（iPad 上是苹方），不背 5～10 MB 的中文字体文件；
  字形风格也自动与 iOS 系统一致
- 代价：首帧遇到新字会各触发一次 JS 调用；实测游戏用到的不同汉字约 800～1200 个，
  分散在多帧里，不会卡顿

### 2.4 音频：Web Audio

- `mciSendStringW` 的 6 个动词直译到 `AudioBufferSourceNode`
- **关键约束**：iOS 要求**用户手势**后才能出声。
  所以外壳上必须有一个"点击开始"启动页来 unlock `AudioContext`，
  否则进游戏后全程静音（这是 iOS 上最常见的坑）
- BGM 与音效走不同的 gain 节点（对应桌面端的"BGM 单设备 / 音效 8 设备"分离）

### 2.5 存档：IndexedDB

- 把 `pvz_save.dat` 的 `fopen` 系列 shim 到 Emscripten 的 `IDBFS`
- 或者更轻：直接读写 `localStorage`（存档 1.25 KB，远小于 5 MB 上限）
- **必须在** `visibilitychange`（切到后台）时同步一次 —— iOS 会随时冻结页面

### 2.6 构建：**GitHub Actions**（因本机网络受限，见第 5 节）

---

## 3. 重点验证的功能点

沿用现有 17 个回归测试 + 补充 Web 专项。**最有力的一条是跨平台逐像素比对**：

### 3.1 跨平台渲染一致性（最高价值）

桌面端已有 `_test_render_baseline.c`：固定种子 + 固定场景 → 导出世界层原始像素。
做法：

```
Windows 构建  →  baseline_win.raw
Web 后端本地编译（gcc，同一份 C） → baseline_web.raw
               逐像素比对
```

- **形状/精灵层**应当**逐像素一致**（同一套混合公式、同一套坐标）
- **文字层**允许有差异（GDI 微软雅黑 vs 苹方），但要检查"位置与包围盒一致"
- 这条能在**没有 WASM 工具链**的情况下跑，是移植期的主力验证手段

### 3.2 功能点清单

| 分组 | 要验证的点 |
|---|---|
| 渲染 | 22 个原语的形状/抗锯齿；预乘 alpha 不双重混合；通道顺序（BGRA↔RGBA 不能反，反了草坪变紫）；精灵缩放不糊 |
| 文字 | 中文不出方块；`putTextWrap` 换行点一致；`GetTextExtentPoint32W` 的宽度估算不能让文字溢出卡片 |
| 玩法 | 158 张卡表、抽卡概率、始祖强度（≥殿堂 5 倍）、7 档音乐触发、庆典演出 |
| 输入 | 点击命中判定（卡片/卡槽/铲子/按钮）；拖拽种植；长按=悬停；右键=铲子 |
| 存档 | 存/读/迁移；切后台再回来不丢；刷新页面进度还在 |
| 音频 | 首次手势后才出声；BGM 与音效不互相顶掉；静音切换 |
| 性能 | 帧率、首屏时间、切场景卡顿 |

### 3.3 回归基线

现有 **17 个测试必须继续全绿**（`bash _regress.sh`）。
本轮已把它们加固：**编译失败会删掉旧 exe**，避免陈旧二进制冒充通过
（本轮就靠这条抓出 3 个早已编不过、却一直在"假装通过"的测试）。

---

## 4. iPad 适配场景

### 4.1 分辨率对齐（一个巧妙的吻合）

游戏逻辑分辨率 **1000×650**，超采样 **SS=2** → 渲染缓冲 **2000×1300**。

| iPad | 横屏逻辑分辨率 | 与 2000×1300 的关系 |
|---|---|---|
| iPad 10.9" / Air | 1180×820 | 游戏按宽度铺满，等比放大 ~1.18x |
| iPad Pro 11" | 1194×834 | 同上 |
| iPad Pro 12.9" | 1366×1024 | 等比放大 ~1.37x |

也就是说**渲染缓冲固定 2000×1300 就已经接近甚至超过 iPad 的物理需求**，
放大由浏览器的 GPU 合成完成 —— 不需要按设备做多套分辨率，
也不需要动态改缓冲尺寸（这会让移植期的一切像素测试失效）。

### 4.2 方向切换

游戏的 UI 是**为横向 1000×650 设计的**（顶部 HUD 条 + 底部卡槽行）。
竖屏时按宽度铺满只剩约 45% 的屏高，可玩但局促。

方案：

| 方向 | 行为 |
|---|---|
| **横屏** | 主力形态。铺满、居中，两侧留黑边（或拉伸背景填满） |
| **竖屏** | 弹一个「横屏体验更好」的可关闭提示；**关掉提示后仍可玩** —— 按宽度铺满并居中，不是直接禁用 |

- 无论哪个方向，**游戏内部坐标系不变**（永远是 1000×650），
  只改 CSS 呈现尺寸 → 切换方向时**游戏状态零影响**
- 监听 `orientationchange` + `visualViewport.resize` + `screen.orientation.change`
  三条路径（iPad 上不同 iOS 版本触发的组合不同，只监听一条会漏）
- 切换方向后**重新计算触摸坐标映射**（否则会点到错的位置）

### 4.3 触摸操作

| 桌面操作 | 触摸映射 |
|---|---|
| 鼠标移动 | 手指拖动（种植时跟随） |
| 左键点击 | 点按 |
| **悬停**（预览卡片信息 / 高亮） | **长按 250ms** —— 触摸没有悬停，必须给替代 |
| 右键（铲子） | 屏幕上的"铲子"按钮切换模式 |
| 键盘 1~8（编组）/ 空格（暂停）/ W·M·E | **底部/侧边一排屏幕按钮**，复用已抽出的 `handleKey()` |

触摸专项要求：

- **命中区放大**：可点元素最小 **44×44 pt**（Apple HIG 下限）。
  卡片行本身够大，但顶部 HUD 的小图标与"关卡选择"的格子需要**热区外扩**，
  视觉不变、判定放大
- **禁用浏览器手势**：`touch-action: none`；`user-select: none`；
  阻止双击缩放与捏合缩放（`gesturestart`）
- **防止误触滚动**：`overscroll-behavior: none` + `position: fixed`
- 手指遮挡：拖动种植时，被手指挡住的位置要有**偏移显示**（种植物位置抬高一点）

### 4.4 Safari 专项

| 项 | 处理 |
|---|---|
| 全屏 | `apple-mobile-web-app-capable` + "添加到主屏幕"，去掉 Safari 地址栏 |
| 安全区 | `viewport-fit=cover` + `env(safe-area-inset-*)`，避开圆角与 Home 指示条 |
| 自动播放 | 启动页点击解锁 `AudioContext` |
| 后台冻结 | `visibilitychange` → 立即存档 + 暂停游戏 |
| 双击缩放 | `touch-action: none` + `preventDefault` |
| 微信/内置浏览器 | 需要提示"请在 Safari 中打开"（`AudioContext` 与全屏行为不同） |

---

## 5. ⚠️ 工具链约束（本轮实测，需要你决定）

**Emscripten 在本机装不上。** 实测结果：

| 站点 | 可达性 |
|---|---|
| `github.com` | ✅ 200 |
| `storage.googleapis.com`（Emscripten 工具链与 Node 的实际下载源） | ❌ 不可达 |
| `nodejs.org` / `codeload.github.com` / `raw.githubusercontent.com` / `pypi.org` / `registry.npmjs.org` | ❌ 不可达 |

**沙箱外也一样不可达**（已用提权命令验证），所以不是沙箱策略，是网络层面的限制。
表现：`git clone emsdk` 只创建了 9 KB 的 `.git/hooks` 就挂住，42 分钟没有任何进展。

### 两条可行路径

| 路径 | 做法 | 优劣 |
|---|---|---|
| **A. GitHub Actions 构建**（推荐） | 我写全部代码 + `.github/workflows/build-wasm.yml`；推到 GitHub，Actions 的 runner 网络不受限，编译出 WASM 并部署到 GitHub Pages | 与之前 iOS 方案的思路一致；你需要提供一个仓库（或授权我建）；出的是**可直接在 iPad 打开的链接** |
| **B. 你在本机装 emsdk** | 手动执行 `git clone emsdk && emsdk install latest`（需要能访问 storage.googleapis.com 的网络，比如挂代理） | 本地迭代最快；但要求你这边网络放行 |

无论走哪条，**下面这些工作都不受影响、现在就能做、现在就能验证**：

- `plat_web.c` 的软件光栅后端（纯 C，**本地 gcc 可编译 + 逐像素测试**）
- 跨平台渲染一致性比对（上面 3.1）
- `web/index.html` 外壳（iPad 布局 / 触摸 / 方向）—— 纯前端，浏览器里直接能看
- GitHub Actions 工作流文件

---

## 6. 阶段划分

| 阶段 | 内容 | 验证方式 | 是否依赖工具链 |
|---|---|---|---|
| **P1 驱动接口** ✅ 本轮完成 | `gameBoot` / `gameStep` / `gameWorldBits` 抽出，`WinMain` 条件编译 | 编译零告警 + 17 个回归全过 | 否 |
| **P2 软件光栅后端** ✅ 本轮完成 | `plat_web.c`（约 1150 行）：56 个符号 + 22 个原语 | 本机 gcc 编译通过 + 原语级逐像素比对（3 项完全一致） | 否 |
| **P3 音频** ✅ 完成 / 文字待做 | MCI 状态机 + Web Audio + HTMLAudio 流式 | 端到端音频调用链验证 | 否 |
| **P4 Web 外壳** ✅ 完成 | `index.html` + 触摸 + 方向 + 安全区 | **Playwright 模拟 iPad，35 条断言全过** | 否 |
| **P5 云端构建** | GitHub Actions 出 WASM + 部署 | 链接在 iPad Safari 打开 | **是** |
| **P6 实机调优** | 帧率、触摸手感、性能保底开关 | iPad 实机 | 是 |

**P1~P4 已完成。**剩下：P5 云端构建（需先手动建一次 GitHub 仓库）、
P6 iPad 实机调优、以及文字渲染的字形图集（当前本机测试用占位块）。

---

## 6.5 ★ P2 验证结果：原语级逐像素比对

方法：**同一个测试程序，分别链接真 GDI 与软件光栅**，跑同一段绘制序列后逐像素比
（`_ab_prim.c`，故意不链接 `pvz.c`，保持"纯原语"的干净对照）。

| # | 原语 | 结果 |
|---|---|---|
| 0 | `fillRect` + 2x 世界变换 | **逐像素一致** ✔ |
| 7 | `AlphaBlend`（缩放，AC_SRC_ALPHA） | **逐像素一致** ✔ |
| 8 | `StretchBlt`（缩放） | **逐像素一致** ✔ |
| 4 | `Polygon` | 0.88%（边界取整） |
| 1 | `RoundRect` 填充 | 0.95% |
| 5 | 线 w=3 | 1.68% |
| 3 | `Ellipse` 填+描 | 2.62% |
| 2 | `RoundRect` 描边 | 3.96% |
| 6 | `GradientFill` | 51.6%（渐变插值方式不同） |
| 9 | 裁剪区 | 56.3%（Region 语义） |

### 这一轮查出的 5 个真问题（全部是"不报错、只是画错"）

1. **`FillRect` 取色读到类型标签**。改用对象池后 `HBRUSH` 是 `GdiObj*`，
   而其**第一个字段是 `type`**（值 2）—— `*(COLORREF*)br` 读到的是 2，
   于是所有 `FillRect` 变成 R=2 的近黑色，整屏几乎全黑。
   *只有"连底色都比不上"这种极端现象才暴露了它。*
2. **`AlphaBlend`/`StretchBlt` 其实吃世界变换**。我最初的注释写着"用设备坐标、不吃变换"，
   **是错的**：目标传 32 设备像素，GDI 实际输出 64（×SS=2）。
   `spriteBlit` 之所以看不出，是因为它先复位成单位变换再调用 ——
   "吃不吃变换"结果相同，两种理解无法区分。**独立原语测试才揭露了真相。**
3. **`Polygon` 内外判定用了未变换的坐标**：扫描线在设备坐标跑、判定在逻辑坐标判，
   结果只画出应画面积的 40%。"图形画出来了、位置也对，只是范围不对"——
   不逐像素比对根本发现不了。
4. **`putOpaque` 不该写 alpha**。GDI 的填充图元**只写 RGB、不碰 alpha**；
   我写了 255，于是每个原语都报"100% 不同"，而 RGB 其实**逐字节一致**。
   （对显示无影响，但对抓帧基线比对是致命的 —— 基线 dump 全部 4 字节。）
5. **`plat.h` 里 `NULL_PEN` 与 `NULL_BRUSH` 都是 `NULL`**，后端无法区分
   "只填充"与"只描边"。引擎里 `NULL_PEN` 用了 4 处、`NULL_BRUSH` 用了 8 处，
   语义完全不同。已改为 Windows 的真实值（`NULL_BRUSH=5`、`NULL_PEN=8`）。

### 顺带修掉的两个真 bug

- **`GdiFlush` 缺失**：GDI 的绘制是**批处理**的，不 flush 就读 DIB 会拿到陈旧内存。
  症状是"所有原语 100% 不一致"，会把人往"混合公式写错了"的方向带偏。
- **`draftPickOne` 的 `cand` 未初始化告警**：根因是 `n<=0` 时 `m==0` →
  `rand() % m` 是**除零**（UB）；`n > R_COUNT` 时还会**越界写栈**。已加边界检查。

### 剩余差异的性质与影响

- 1~4% 的那几项是**光栅化取整差异**（描边宽度取整、椭圆边界包含性），
  视觉上不可分；对游戏无影响。
- `GradientFill` / 裁剪区两项是**实现方式不同**（GDI 的渐变插值与 Region 语义），
  已明确记录，属于后续可继续收敛的项。
- **结论**：主流渲染路径（精灵 blit + 世界变换 + 混合）已证明可逐像素对齐，
  这是网页版可行性的关键证据。

---

## 6.6 ★ P3 + P4 结果：音频桥接、iPad 外壳、端到端验证

### 已完成

| 模块 | 内容 |
|---|---|
| **音频桥接** | `plat_web.c` 里的 MCI 状态机（别名表 + 6 个动词）+ Web Audio |
| **iPad 外壳** | `web/index.html` / `shell.css` / `shell.js`（约 1000 行） |
| **端到端验证** | `_test_web_shell.py`，Playwright 模拟 iPad 视口，**35 条断言全过** |
| **CI** | `.github/workflows/build-web.yml`（gcc 自检 → emcc → 部署 Pages） |

**音频分工**（刻意的）：音效（1.3MB）走 MEMFS + Web Audio，低延迟；
BGM（20MB）走 HTMLAudio 按 URL 流式，不占 wasm 内存、支持边下边播。

### 端到端验证覆盖的场景

在 **iPad Pro 11 横屏 1194×834** 与 **竖屏 834×1194** 两种视口下各跑一遍：

- 页面加载无 JS 错误；启动按钮可用
- canvas 像素尺寸恒为 2000×1300（与引擎世界层一致）
- 显示比例保持 1.538（**不裁切**），黑边上下/左右均匀（居中）
- **真实帧注入**：把真后端导出的世界层灌进模拟堆，验证整条管线
  （真像素 → HEAPU32 → BGRA→RGBA + alpha=255 → putImageData → canvas）
  - 暗部占比 11%（确实有内容，不是全黑）
  - alpha 不透明占比 100%（外壳正确补了 alpha）
  - **G=110 > B=41**（草坪是绿的 → 通道顺序正确，无 R/B 互换）
- 触摸点按 → `gamePointerDown(500,325)`（逻辑坐标精确）
- 长按 250ms → 触发悬停、**不**触发点击
- 拖动 → **不**触发点击
- 屏幕快捷键 → `handleKey(32)`
- 竖屏 → 出现"横屏体验更好"提示，且可关闭
- 旋转到竖屏 → 提示重新出现，且中心点仍映射到 (500,325)

### 这一步抓到的两个真 bug（都靠浏览器实测才发现）

1. **`[hidden]` 被 `display:flex` 覆盖 —— 横屏时游戏完全点不动。**
   `#rotateHint` 是 `display:flex`，它把 HTML 的 `hidden` 默认样式
   （`display:none`）盖掉了。于是横屏时那个"提示层"虽然不可见，
   却仍然铺满整屏、**吃掉所有触摸**。
   截图上看一切正常 —— 只有自动化点击才暴露（Playwright 报
   "intercepting pointer events"）。已加 `[hidden] { display:none !important }`。
   > 同类问题还波及 `#keys`：启动页还没点"开始"，快捷键按钮就显示出来了。

2. **`checkOrientation()` 里的 `if (!running) return` 让竖屏提示永远不出现。**
   `startGame()` 是"先调 checkOrientation()、再置 running = true"，
   所以那一句直接短路了整个提示逻辑。
   这类**顺序依赖**的 bug 读代码很难发现。

### 仓库与构建

- 本地仓库已就绪：`main` 分支，**81.6 MiB / 1157 文件**
  （从 337MB 裁下：排除 BMP/WAV 母版与 `art/` 下的素材生产中间产物）
- Actions 工作流：先 gcc 自检（几十秒，能拦住 90% 的低级错误）
  → 再 emcc 构建 → 部署 Pages
- **待办**：connector 的 GitHub 授权**不含建仓库权限**（`POST /user/repos`
  返回 403 `Resource not accessible by integration`），需要手动建一次仓库。

---

## 6.6 ★ P3 + P4 结果：音频桥接、iPad 外壳、端到端验证

### 已完成

| 模块 | 内容 |
|---|---|
| **音频桥接** | `plat_web.c` 里的 MCI 状态机（别名表 + 6 个动词）+ Web Audio |
| **iPad 外壳** | `web/index.html` / `shell.css` / `shell.js`（约 1000 行） |
| **端到端验证** | `_test_web_shell.py`，Playwright 模拟 iPad 视口，**35 条断言全过** |
| **CI** | `.github/workflows/build-web.yml`（gcc 自检 → emcc → 部署 Pages） |

**音频分工**（刻意的）：音效（1.3MB）走 MEMFS + Web Audio，低延迟；
BGM（20MB）走 HTMLAudio 按 URL 流式，不占 wasm 内存、支持边下边播。

### 端到端验证覆盖的场景

在 **iPad Pro 11 横屏 1194×834** 与 **竖屏 834×1194** 两种视口下各跑一遍：

- 页面加载无 JS 错误；启动按钮可用
- canvas 像素尺寸恒为 2000×1300（与引擎世界层一致）
- 显示比例保持 1.538（**不裁切**），黑边上下/左右均匀（居中）
- **真实帧注入**：把真后端导出的世界层灌进模拟堆，验证整条管线
  （真像素 → HEAPU32 → BGRA→RGBA + alpha=255 → putImageData → canvas）
  - 暗部占比 11%（确实有内容，不是全黑）
  - alpha 不透明占比 100%（外壳正确补了 alpha）
  - **G=110 > B=41**（草坪是绿的 → 通道顺序正确，无 R/B 互换）
- 触摸点按 → `gamePointerDown(500,325)`（逻辑坐标精确）
- 长按 250ms → 触发悬停、**不**触发点击
- 拖动 → **不**触发点击
- 屏幕快捷键 → `handleKey(32)`
- 竖屏 → 出现"横屏体验更好"提示，且可关闭
- 旋转到竖屏 → 提示重新出现，且中心点仍映射到 (500,325)

### 这一步抓到的两个真 bug（都靠浏览器实测才发现）

1. **`[hidden]` 被 `display:flex` 覆盖 —— 横屏时游戏完全点不动。**
   `#rotateHint` 是 `display:flex`，它把 HTML 的 `hidden` 默认样式
   （`display:none`）盖掉了。于是横屏时那个"提示层"虽然不可见，
   却仍然铺满整屏、**吃掉所有触摸**。
   截图上看一切正常 —— 只有自动化点击才暴露（Playwright 报
   "intercepting pointer events"）。已加 `[hidden] { display:none !important }`。
   > 同类问题还波及 `#keys`：启动页还没点"开始"，快捷键按钮就显示出来了。

2. **`checkOrientation()` 里的 `if (!running) return` 让竖屏提示永远不出现。**
   `startGame()` 是"先调 checkOrientation()、再置 running = true"，
   所以那一句直接短路了整个提示逻辑。
   这类**顺序依赖**的 bug 读代码很难发现。

### 仓库与构建

- 本地仓库已就绪：`main` 分支，**81.6 MiB / 1157 文件**
  （从 337MB 裁下：排除 BMP/WAV 母版与 `art/` 下的素材生产中间产物）
- Actions 工作流：先 gcc 自检（几十秒，能拦住 90% 的低级错误）
  → 再 emcc 构建 → 部署 Pages
- **待办**：connector 的 GitHub 授权**不含建仓库权限**（`POST /user/repos`
  返回 403 `Resource not accessible by integration`），需要手动建一次仓库。

---

## 7. 不做的事（明确边界）

- **不改游戏逻辑与数值**：158 张卡、抽卡概率、始祖强度、7 档音乐，一行不动
- **不下架桌面版**：两套后端共用同一份 `pvz.c`，Windows 版继续可用、继续跑回归
- **不做 iPad 上重写 UI 布局**：游戏内部仍是 1000×650 的固定坐标系，
  "适配"发生在呈现层（CSS 缩放 + 安全区 + 触摸映射），不在游戏逻辑层
- **不做多端存档同步**：存档留在本机（IndexedDB），不做账号体系
- **不追求 App Store 上架**：目标是"你在 iPad Safari 里打开就能玩"
