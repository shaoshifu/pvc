# 美术资源管线（加资源必读）

本文件是 **「怎么加一张新图、怎么加一株新植物」** 的操作清单。
风格规范见 [ART_BIBLE.md](ART_BIBLE.md)，抠图算法细节见 skill `ai-sprite-asset-pipeline`。

---

## 0. 硬性规则（违反即视为没做完）

1. **每加一株会开火的植物，必须同步生成配套子弹。**
   没有子弹的植物会退回默认绿圆弹，和本体风格对不上。
2. **神级植物只在本局有效。** 禁止写 `gSave.plantOwned[pid] = 1`，
   禁止把 `PT_COUNT` 当抽卡池上限（池子只用 `PT_HERO_FLAME`）。
3. **子弹禁止走角色 `grade()`。** 火焰橙 / 末日紫被拉向灰绿基准后就不可读。
4. **每个绘制入口必须「有资源走精灵 / 无资源走程序化」。** 删掉 `assets/` 游戏仍能跑。

---

## 1. 出图

**通道 A：mmx CLI（子弹 / 小物件首选）**

```
export PATH="/c/nvm4w/nodejs:$PATH"
mmx image generate --prompt "<prompt>" --aspect-ratio 1:1 --n 3 \
    --out-dir art/raw --out-prefix <资源名> --quiet
```

出的是 **JPG**，命名成 `art/raw/<资源名>.jpg` 即可直接进管线（`find_raw` 支持多扩展名）。
一次出 `--n 3/4` 挑一张，比一张一张试快得多。

**通道 B：tupian.py（植物 / 僵尸等大图）**

```
C:\Python314\python.exe E:\pdf\tupian.py "<prompt>" --model gpt-image-2 --size 1024x1024 -n 1 --name <id> -o pvz-c/art/raw
```

- 凭据：`C:\Users\Administrator\.workbuddy\tupian.json`（换 key 只改这个文件）
- 必须带浏览器 UA（`tupian.py` 已内置）
- 约 30~90 秒/张；失败就重试，不要换通道

**风格锚点句**（每一张都原样带上，只替换对象描述）：

```
Plants vs Zombies style 2D game asset, hand-painted cartoon illustration,
bold clean dark outline, rich saturated colors, simple bold shape readable
when scaled down to 16 pixels, centered, isolated on transparent background,
no text, no watermark, no border, no ground, no shadow.
```

| 类型 | 对象描述要点 |
| --- | --- |
| 植物 | `facing right, front three-quarter view, standing upright in neutral idle pose, small dirt patch at the base` |
| 僵尸 | `facing left, side three-quarter view, both arms outstretched forward, tattered grey-blue clothes, neutral idle pose` |
| **子弹** | `A single <形状>, horizontal orientation pointing right, square composition.` 形状必须简单到缩到 16px 仍可辨 |

发光类（阳光、末日能量球若走辉光）用**纯黑底**，不要棋盘格。

### 1.1 子弹提示词模板（踩过坑后的稳定写法）

子弹最容易翻车的三件事：**画了光晕**、**多画了一个东西**、**太暗/太细**。
提示词里要显式点名禁止，模型才会收敛：

```
Simple 2D game bullet sprite icon. EXACTLY ONE SINGLE OBJECT in the whole image:
a sharp <材质> <形状> pointing diagonally toward the upper right.
<主体配色，要指明是亮色> , small bright <强调色> tail fin.
Very BRIGHT and high-key so it reads clearly on a dark green lawn.
Cute flat vector cartoon style, crisp chunky shapes, only a very thin dark edge line.
THE IMAGE MUST CONTAIN ONLY THIS ONE OBJECT AND PURE EMPTY FLAT WHITE ELSEWHERE.
Strictly no second object, no duplicate, no reflection, no mirror copy,
no ground line, no horizon, no table surface, no floor bar, no drop shadow,
no motion blur streak, no speed lines, no sparkles, no glow, no aura,
no light rays, no vignette, no gradient in the background.
Pure uniform flat white background. Centered, occupying about the middle 40 percent,
wide empty margin on every side; nothing may touch the image border.
Plants vs Zombies art style.
```

四条实测结论：

1. **不要写 `bold clean dark outline` 给它**（那是给植物的）。
   子弹只有 27px，重黑描边会把中位亮度压到 70 上下，在草坪上糊成一块。
   改成 `very thin dark edge line` + 明确要求亮色主体。
2. **`no shadow / no reflection / no ground` 必须写全。**
   只写 `no shadow` 时模型仍会画一条横贯画面的"地面反光条"，
   抠图后是一条色带（用 `_diag_objects.py` 抓）。
   实测模型的三种默认加戏（同一类问题，都要点名禁止）：
   **灰色椭圆投影**、**横向反光条/地面线**、**绿色草地圆盘（还带草叶）**。
   最后一种尤其容易被漏掉 —— 它自己"补"了一块草坪，
   抠图后底部会多出一片绿色，进游戏看着像拖了个小板子。
   负向清单至少要覆盖：`no grass patch, no ground ellipse, no floating fragments`。
3. **不要追求"全不贴边"。** 抠图会裁到包围盒，斜向子弹必然顶到 2 个角。
   真正要避免的是**光晕**（主体平均饱和度异常）和**多主体**。
4. **一次出 3 张，肉眼挑。** 同一条提示词里，模型给的构图差异很大
   （同一批三张可能是"干净箭头 / 带地面圆盘 / 带草地"）。
   先拼成一张对照图再看，比逐张看省事，也更容易比出高下。

---

## 2. 导出（`art/build_assets.py`）

1. `MAPPING` 加 `('<raw 文件名关键词>', '<输出名>')`
2. `TARGET_H` 加目标高度（**2× 逻辑像素**）：

| 类型 | 高度经验值 |
| --- | --- |
| 普通植物 | 200 |
| 神级植物 | 215–340（稀有度越高越「重」） |
| 僵尸 | 200（巨人 330，气球 260） |
| **子弹** | **44–52**（逻辑 22–26px）。再小缩下去糊成点，再大会盖住目标 |

3. 跑：

```
cd pvz-c/art
python build_assets.py
```

输出到 `assets/<name>.bmp`（32 位 BGRA、预乘、底部居中）。

脚本已自动处理：

- 子弹走 `cutout(..., small=True)`（腐蚀核 5px，口袋阈值 400）
- 子弹 / 阳光 / 爆发特效跳过 `grade()`
- 预乘后断言 `rgb <= alpha`

---

## 3. 接入引擎（`pvz.c`）—— 漏一处就是 bug

### 3.1 加一株植物（清单，按顺序）

| # | 位置 | 做什么 |
| --- | --- | --- |
| 1 | `enum { PT_... }` | 加枚举。普通植物插在 `PT_HERO_FLAME` **之前**；神级植物插在其后 |
| 2 | `plantDefs[PT_COUNT]` | 名称 / 费用 / 冷却 / 生命 / 稀有度 / `hero` 标志 / `desc` |
| 3 | `plantFile[PT_COUNT]` | 资源文件名，**与枚举顺序一一对应** |
| 4 | 开火逻辑（`updatePlants` 的 `switch`） | 射击间隔、弹道、伤害、特殊效果 |
| 5 | `drawPlantShapeProc` | 无资源时的程序化回退剪影 |
| 6 | 悬停说明 | 神级用 `plantDefs.desc`；普通植物若要更细的介绍也写在这里 |
| 7 | **子弹（开火植物必做）** | 见 3.2 |

**不要动这些**（除非你知道自己在干什么）：

- 抽卡池、金币抽卡、编组界面的循环上限是 `PT_HERO_FLAME`，不是 `PT_COUNT`
- `applyHeroPlant()` 只写 `gBonusPlant[]`，**绝不**写 `gSave.plantOwned` / `saveFlush`
- `resetGame()` 必须清 `gBonusN` / `gBonusPlant[]` / `gSinceHero`

### 3.2 加一颗子弹（开火植物必做）

| # | 位置 | 做什么 |
| --- | --- | --- |
| 1 | `enum { BUL_... }` | 加枚举 |
| 2 | `bulletFile[BUL_COUNT]` | 资源文件名 |
| 3 | `bulletSpriteOf()` | 按「武器模组优先，其次 `pe->src` 植物」选图 |
| 4 | 绘制处的回退色 `gc` | 资源缺失时拖尾 / 圆弹的颜色，要和子弹主色一致 |

选图优先级（已经写死，不要改）：

```
武器模组（穿透/溅射/反弹/冰冻/灼烧） > 发射它的植物 > 默认绿豌豆
```

现有映射（**一株植物一张图 —— 机制辨识度就靠这张图**，所以新子弹优先"替换对应植物的图"，
而不是新增枚举。只有确实没有任何植物承载新机制时才加 `BUL_*`）：

| 植物 / 模组 | 子弹 | 外观要表达的机制 |
| --- | --- | --- |
| 豌豆 / 双发 / 三线 / 默认 | `bullet_pea` | 基础直线弹 |
| 寒冰射手 / 冰冻模组 | `bullet_ice` | **冰晶**（冰冻 / 减速） |
| 玉米投手 | `bullet_kernel` | **冲击波**（击退 knockback=8） |
| 烈焰射手 | `bullet_flame` | 灼烧 |
| 晶簇射手 | `bullet_crystal` | 定身（root=450） |
| 霜之哀伤 | `bullet_frost` | 冰冻 + 定身（freeze=900 / root=600） |
| 末日花 | `bullet_doom` | **追踪弹**（homing=1） |
| 穿透模组 | `bullet_pierce` | **穿刺针**（pierceLeft） |
| 溅射 / 反弹 / 灼烧模组 | `bullet_splash` / `_bounce` / `_burn` | — |
| 闪电芦苇 | `bullet_arc` | 电弧（穿刺 3 层 + 击退 15） |
| 仙人掌 | `bullet_spine` | 针刺（穿刺 2 层） |
| 西瓜投手 / 玉米加农炮 | `bullet_melon` / `bullet_cob` | **爆炸**（威力巨大 70 / 520） |

**不开火的植物**（向日葵、坚果、土豆雷、地刺、磁力菇、世界树、荆棘藤蔓……）不需要子弹。

> **mmx 出图直接可用**：`find_raw()` 已支持 `.png / .jpg / .jpeg / .webp`，
> mmx CLI 的 JPG 产物（`--out-dir art/raw`）命名成 `<资源名>.jpg` 即可，
> 同名时 png 优先。旧图挪到 `art/raw/_old/` 留档，别直接删。

### 3.3 加一种僵尸

行走图必做；啃食图没有就退化成行走图（代码已处理）。倒地帧由管线从行走图绕脚底旋转派生，**不要单独生成**。

---

## 4. 验收（每次加资源都跑）

一条命令跑完前 5 项：

```
cd pvz-c/art
python _verify_bmp.py                 # 全部 bullet_*.bmp
python _verify_bmp.py bullet_pierce.bmp
```

它手写解析 32 位 BMP（**不能用 PIL**，见 4.5 第 3 条），检查：
位数 32 / 自顶向下 / alpha 没被写丢 / 预乘 `rgb <= alpha` / 覆盖率区间 / 四角残留。

- [ ] **覆盖率在 6%~75%**（子弹经验区间 15%~45%）。低于 8% 说明主体太细，缩到 27px 看不见
- [ ] **主体亮度够**：`alpha>200` 的像素中位亮度 ≳ 100，且亮度 <60 的占比 < 25%。
      纯深灰/重黑描边的图在草坪上会糊成一块（实测一次：中位 74、37% 像素 <60，不可用）
- [ ] **原图只有 1 个主体**：`python _diag_objects.py --raw`。
      生成模型常偷偷多画一个东西（倒影 / 地面横条 / 第二根针），
      抠完会变成一条横贯画面的色带，进游戏就是异常像素
- [ ] **角像素 alpha 不适用**：`trim()` 会把包围盒裁到内容，
      所以【斜向】的子弹（穿刺针等）必然有 2 个角 alpha > 0。这不是缺陷。
      只有「四角都很亮（>200）」才说明有光晕残留。
      判定用覆盖率 + 连通域 + 亮度，不要只看角像素
- [ ] 预乘：`rgb <= alpha`（脚本已断言）
- [ ] 放大 400% 看边缘：无白边 / 灰描边光晕
- [ ] **10px 缩略仍能认出是哪种植物 / 哪种子弹**
- [ ] 开火植物：对局里打出的弹外观和植物匹配，不是绿圆
- [ ] `gcc -O2 -mwindows -static-libgcc -o pvz.exe pvz.c -lgdi32 -luser32 -lmsimg32 -lm -Wall -Wextra` 零警告
- [ ] 删掉刚加的 bmp 再开游戏：该对象回退程序化，不崩

### 4.1 出图后的辅助诊断脚本

**先纠正一条旧结论：本模型其实是能"看"图的。**
用 Read 工具直接打开 PNG / JPG（包括 `art/raw/*.jpg` 和渲染出的预览图），
图像会真的以视觉形式呈现出来。之前文档里写的"只能靠数字验收"是错的，
**出图后第一步就该直接看图**，下面这些脚本是用来在你怀疑伪影、
或需要给出可复现的量化判据时才用的补充手段。

把「好不好看」也拆成可断言的数字指标：

| 脚本 | 看什么 | 什么时候用 |
| --- | --- | --- |
| `_contact_bullets.py` | 把若干 BMP 合成到草坪色上出对照图（真实尺寸 + 4x 放大 + 数值） | **首选**：一眼看全部机制子弹 |
| `_eval_bullet.py <目标高度> <图...>` | 覆盖率 + 四角 alpha + 形状 ASCII 热力图 | 候选图初筛 |
| `_diag_objects.py --raw` | 连通域个数（**必须有且只有 1 个主体**） | 排除"多画了一个东西" |
| `_diag_lum.py <图...>` | 亮度 p10/p50/p90、暗部/亮部占比 | 判断缩到 27px 还看不看得清 |
| `_diag_color.py <图...>` | 主体平均 RGB、过渡带平均饱和度 | **抓光晕**（v1 穿刺针主体是 RGB(163,230,146) 的绿色，一眼假） |
| `_diag_rawmap.py <图...>` | 把原图渲染成 ASCII，直接"看"画了什么 | 定位横条/倒影从哪来 |
| `_diag_alpha_rows.py <图>` | 抠图后逐行 alpha | 追"底部横条"这类伪影 |

> 一句话经验：**"抠不掉的光晕"用饱和度抓，"多画的东西"用连通域抓，"太暗/太细"用亮度+覆盖率抓。**
> 这三类问题肉眼看原图往往一眼就发现，数字只是用来复核。


---

## 4.5 五条容易踩的规矩

1. **生成图右下角有「AI 生成 / WORKBUDDY」水印，必须抹掉。**
   位置固定在右下角约 22% × 17%，做法是**用正上方同宽的一块覆盖**
   （`strip_watermark()`），不要裁掉那块 —— 会破坏构图。
   验收：右下角区域内不应再出现 >150 的亮色文字。

2. **无窗口测试程序必须设置 `PVZ_SAVE_FILE`，否则会覆盖玩家的真实存档。**
   `saveFlush()` / `saveLoad()` 走 `savePath()`，该函数优先读环境变量：

   ```c
   _wputenv(L"PVZ_SAVE_FILE=_test_save.dat");   /* 测试程序开头必加 */
   ```

   真实教训：曾因测试程序调用 `applyRelic()` → `saveFlush()`，
   把玩家存档写成了测试用的空档，进度不可恢复。

3. **核对产物 BMP 时不能用 PIL 读 alpha。**
   `Image.open(p).convert('RGBA')` 对 32 位 BMP 会把 alpha 全返回 255，
   于是"透明背景"看起来完美，实际游戏里是一个黑方块 —— 被这个骗过一次。
   必须手写解析：

   ```python
   off = struct.unpack_from('<I', b, 10)[0]
   w, h = struct.unpack_from('<ii', b, 18)
   px = np.frombuffer(b, np.uint8, count=w*abs(h)*4, offset=off).reshape(abs(h), w, 4)  # BGRA
   ```

   已封装成 `_verify_bmp.py`。

4. **构建日志必须用 `-u` 且显式记退出码。**
   `python build_assets.py > ba.log` 在崩溃时输出被缓冲，`ba.log` 是 **0 字节**，
   看起来像"什么都没发生"，实际是半路抛异常：

   ```
   python -u build_assets.py > ba.log 2>&1; echo "EXIT=$?" >> ba.log
   ```

   跑完先看 `EXIT=`，再看有没有 `skip` 行 —— 别只看最后几行。

5. **把 BMP 渲染成预览图时必须把 BGRA 换回 RGB。**
   BMP 里前三个字节是 **B,G,R**。手写解析后如果直接把
   `px[:, :, :3]` 当成 RGB 喂给 `Image.fromarray(..., 'RGBA')`，
   画面里**红蓝互换**，会把青色的冰晶看成金色、橘色的爆炸看成蓝色 ——
   曾经据此误判"美术资源出错了"，白查一轮。

   ```python
   rgb = bgr[:, :, ::-1]      # 必须这一步
   ```

   本文件收尾的 `art/_contact_bullets.py` 是正确写法（同时也做反预乘，
   因为 BMP 存的是**预乘 alpha**：`原值 = 存储值 * 255 / alpha`）。

## 5. 常见翻车


| 症状 | 原因 | 修法 |
| --- | --- | --- |
| 子弹是灰绿色 | 走了角色 `grade()` | 确认名字以 `bullet_` 开头（进 `NO_GRADE`） |
| 子弹抠完是空的 | `MinFilter(13)` 把核心吃光 | 走 `small=True`（脚本已按前缀自动开） |
| 新植物永远绿圆弹 | 忘了改 `bulletSpriteOf` | 3.2 第 3 步 |
| 神级植物下一局还在 | 写了存档或没清 `gBonusN` | 规则 2 |
| 编组界面出现神级植物 | 循环用了 `PT_COUNT` | 改 `PT_HERO_FLAME` |
| 精灵叠在一起巨大 | blit 忘了 `/ SS` | `spriteBlit` 已经除过，别另写一套 |
| 卡片图标跑出画面 | 画在了 1× 文字层 | 精灵一律画在世界层 |
| `inside=100%` 没抠掉 | 灰底被浅色阈值漏判 | `_detect_bg` 已按边框亮度开窗 |

---

## 6. 批次记录：殿堂五尊（P100~P104，`RGQ_HALL`）

`rgPlants[]` 从 100 扩到 105，新增最高档「殿堂」（第 7 档）。加这一批时踩到的坑：

1. **`_gen_units.py` 的行数断言是写死的 100。**
   只改 `pvz.c` 的 `RG_PLANT_N`，`load_tables()` 会直接报「表行数不对」退出。
   现在改成常量 `RG_PLANTS_N`（=105）/ `RG_ZOMBIES_N`（=70），改一处即可。
   同时 `main()` 的默认键表也改成按真实行数生成 —— 原来写死 `range(50)`，
   「裸跑一遍补缺图」会静默漏掉 50 之后的所有批次。

2. **`_TIER` 和 `TIER_FLAIR` 都要加第 7 档。** 漏了会 `KeyError`。

3. ⚠️ **"最高档"的提示词写"发光/光辉"，模型必在脚下画一块发光底盘 / 光环。**
   三代提示词的实测结果：

   | 版本 | 写法 | 结果 |
   | --- | --- | --- |
   | 1 | "radiance emanating from within its own body" | 脚下补**棕色泥土台**（P100） |
   | 2 | 加 `no dirt mound, no ground disc` | 泥土没了，改成**脚下一圈白色发光底盘** |
   | 3 | 去掉所有发光褒义词，改纯材质（白金装甲 + 鎏金纹样） | 光环仍在，还多一层**洋红泛光** |

   模型对"最高档角色"就是会补光环。**继续赌提示词是在烧配额**，
   最终用确定性后处理收尾（见下条）。

4. **`art/_strip_pinkhalo.py`：清「贴轮廓的粉雾」。**
   `_unitlib` 原本把粉雾归为「源图缺陷，靠重生成解决」，
   但殿堂这一档重生成三轮都压不住，所以补了这个后处理。判据两条：

   - 粉雾 = 色相属洋红/粉族：`g < r-20 且 g < b-20 且 (mx-mn) > 50`
     （即「G 是最低通道」）。**前提是角色本体不是粉紫系** ——
     殿堂五尊是 绿/青/黄白/暗绿/棕金，都安全；
   - 粉雾的 alpha 落在中间段（本体是 255），
     所以加 `al < 250` 保护角色自身的粉紫装饰；下半部（>0.45H）的粉族即使
     255 也清。

   用法：`python art/_strip_pinkhalo.py P100 P101 ...`，`--report` 只看命中量。

5. **残留（已知、已接受）**：判据放宽到 `margin=6/chroma=26` 命中率只涨几个点，
   说明残余粉斑是**不透明且位于上半身**，被 `al<250` 保护挡下了。
   再放宽会开始伤到角色（P103 的红色光矛满足粉族判据）。
   当前 5 张在草坪纹理上已看不出粉斑，故不再迭代。
   如需彻底干净：换基座描述重出，或手工修图。

6. **实机验收入口**：抓帧版新增 `D` 键（`_mk_framedump.py`），
   一次摆出五尊 + 一只巨人 + 一只普通僵尸，tag **23**；
   自动序列 `dumpAuto()` 在 230 帧处也会走这套动作。
   它内部会先 `gState = ST_PLAY` —— 不这么做的话，
   上一幕遗留的三选一面板会把草坪整个盖住，截图看起来像「贴图没加载」。
   证据图：`art/panels_hall_onlawn.png`（草坪合成）、
   `art/panels_hall_ingame.png`（真实抓帧）。

---

## 7. 批次记录：始祖三尊（P105~P107，`RGQ_PRIMORDIAL`）

档位表扩到 8 档（`RGQ_COUNT` 7→8），`rgPlants[]` 扩到 108，新增最高档「始祖」。
这一批的坑按重要性排序：

1. ⚠️⚠️ **必须同步扩 9 个数组**（`RGQ_NAME/COL/DMG/RATE/RANGE/TRAITN/W/DRAFT_W/COST`）。
   漏一个就是越界读，而且**编译器不报、也 assert 不了**。
   `_test_hall_tier.c` 里有 `SA_LEN` 编译期断言守着这一族数组，加档位时它会当场拦住。

2. ⚠️ **`RG_PLANT_N` 一改，出图脚本的"最后几条"逻辑会静默错位。**
   `_test_hall_tier.c` 里原来用 `RG_PLANT_N - 5` 取"最后五条"来检查殿堂 ——
   始祖三尊接在后面之后，那个范围变成了别的档位，8 条断言静默失效。
   **规则：批次相关的断言一律写显式索引区间（如 100~104），不要用 `N-k`。**

3. ⚠️ **价格类字段有两层夹取，两层都会静默改数**：
   `RgPlantDef.cost` 是 `unsigned short`（上限 65535，10000 放得下），
   但 `rgPlantCost()` 里还有 `CLAMP(cost, 50, 500)` —— 不改上界的话，
   填 10000 会**悄悄变成 500**。已把上界抬到 10000 并在注释里写明因果关系。

4. **"售价"要先确认这个游戏会不会真的收钱。** 本项目三选一是"每波免费"，
   只写售价等于这个数字没有任何游戏效果。始祖被设为**唯一不免费的档位**，
   走 `rgGrant(idx, freebie=0)` 的既有收费通道（阳光不足会 `gBuyFailed`，
   不扣款也不发卡，两条失败路径都是安全的）。

5. **总权重继续凑 2 的幂**（2048 → 8192）。见 §6 第 2 条：
   `rand() % tot` 在 tot 不能整除 32768 时偏低段取值，实测能把 5% 压成 4.73%。
   校验式：`24wR+26wE+18wL+13wM+5wU+5wH+3wP == 8192`。

6. **"最高档"的美术别写发光词**（§6 第 3 条的老问题）。
   始祖档改用"太初材质"（黑曜石 + 熔金纹 + 内部炽白核），全是材质名词；
   结果**脚下光环依旧**。所以这一条现在是明确结论：
   **靠提示词解决不了，只能靠后处理或接受。**
   后处理能力边界也实测清楚了：
   `_strip_pinkhalo.py` 能清粉雾（命中 2.3%/9.3%/0.2%）；
   但**光环与主体是连通的**（主干域占 99.5%+），连通域剔除那招对它无效。

7. **新资源接入时优先走既有通路，让"缺图"不致命**：
   始祖背景复用了 `gLawnVariant` 变体机制，`drawBackground` 本来就有
   "贴图缺失 → 退回程序化草坪"的兜底，所以可以先合代码、后补美术。
   BGM 同理（`bgmPlayFile` 找不到文件就 return）。**先能玩，再好看。**

8. **音乐可以程序化合成，不必依赖外部模型。** `art/_gen_bgm.py` 用 numpy
   合成了 7 层的太初主题曲（次低音 + 失谐 drone + pad + 高频微光 + FM 钟 +
   心跳脉冲 + 噪声涨落），再过 Schroeder 混响，最后 ffmpeg 编码。
   两个实测要点：
   · **必须出 mp3 不能出 wav** —— MCI 播 WAV 时 `play ... repeat` 返回错误 259
     （打开成功但不出声），靠 `bgmTick` 续播会有循环缝；mp3 走 mpegvideo 正常。
   · 两端各留 1.5 秒淡入淡出，循环时才不会爆音。

9. **抓帧诊断要支持"多套序列"**：`_mk_framedump.py` 的 `dumpAuto()` 里，
   `PVZ_AUTOSEQ=1` 是常规序列（`_rg_verify.py` 用），
   `PVZ_AUTOSEQ=2` 是始祖专项序列。把新场景塞进序列 1 会让**每次**验证都多等十几秒，
   分开之后互不拖累。
