# PvZ-C 视觉升级 - 进度报告

> ⚠️ **本报告已过期，仅作历史存档（2026-09-17 标注）。**
> - 下面的「完成度百分比 / 待生成 28 种 / 里程碑日期」**均不再有效**：`assets/` 实际已有 **165 张 BMP**
>   （40 株植物 + 13 种僵尸含 eat/倒地帧 + 23 种子弹 + 5 种武器 + 6 个模式图标），P0 美术资源**已全部产出**。
> - **粒子系统的独立模块路线已废弃**：`particle.h` / `particle.c` / `_test_particle.c` /
>   `art/generate_all_plants.sh` / `art/test_plants/`（12 张 JPG）现均位于 `_scratch/`，**不参与构建**。
>   粒子最终以 `Particle` 结构体（`pvz.c:1137`）+ `addParticle()`（`pvz.c:1602`）的形式**内联进 pvz.c**，共 30+ 发射点。
> - 构建只有一条路径：`build.bat`，单文件 `pvz.c`，不链接 `particle.c`。
> - 视觉升级的实际落地清单见 `VISUAL_UPGRADE_PLAN.md` 顶部说明与 `GAPS.md` 的 B 节。

**启动时间**：2026-09-16 23:01  
**当前状态**：⛔ 已废弃（作废于 2026-09-17，见上方横幅）

---

## 📊 总体进度

| 阶段 | 任务 | 状态 | 完成度 |
|------|------|------|--------|
| **P0** | 美术资源补全 | 🔄 进行中 | 15% |
| P1 | 特效与质感 | ⏳ 等待 | 0% |
| P2 | 动画与polish | ⏳ 等待 | 0% |

**总体完成度**: 5% (1/20天)

---

## ✅ 已完成项目

### 1. 规划与设计 ✅
- [x] 完整视觉升级方案（`VISUAL_UPGRADE_PLAN.md`）
- [x] 分阶段实施计划
- [x] 工作量估算（17.5-20.5天）
- [x] 美术资源规范定义

### 2. 工具链建设 ✅（⚠️ `generate_all_plants.sh` 已挪入 `_scratch/` 停用；`art/convert_to_png.py` 仍在用）
- [x] `art/generate_all_plants.sh` - 批量生成脚本
- [x] `art/convert_to_png.py` - PNG转换+去背景工具
- [x] mmx CLI集成（已验证可用）

### 3. 第一批植物测试 ✅
- [x] 豌豆射手 × 3候选
- [x] 坚果墙 × 3候选
- [x] 寒冰射手 × 3候选
- [x] 樱桃炸弹 × 3候选
- [x] **共12张JPG图片**（art/test_plants/）

### 4. 粒子系统框架 ✅（⚠️ 该路线已废弃：`particle.c` / `particle.h` / `_test_particle.*` 已挪入 `_scratch/`，不参与构建）
- [x] `particle.h` / `particle.c` 完整实现
- [x] 6种粒子类型（DOT/SPARK/SMOKE/STAR/ICE/LEAF）
- [x] 6个预设发射器（爆炸/击中/烟雾/阳光/冰冻/火焰）
- [x] 独立测试程序 `_test_particle.c`
- [x] 编译通过（零错误，4个unused variable警告）

---

## 🔄 进行中任务

### P0-1: 植物精灵生成（剩余28种）

**当前进度**: 4/33 (12%)

**已生成**:
- ✅ 豌豆射手 (peashooter)
- ✅ 坚果墙 (wallnut)
- ✅ 寒冰射手 (snowpea)
- ✅ 樱桃炸弹 (cherrybomb)

**待生成** (28种):
- 向日葵、土豆地雷、双发射手、火爆辣椒
- 三线射手、地刺、磁力菇、玉米投手
- 杨桃、仙人掌、裂荚射手、闪电芦苇
- 回旋镖花、大喷菇、激光豆、西瓜投手
- 冰冻西瓜、忧郁菇、金盏花、双子向日葵
- 榴莲投手、南瓜护甲、大蒜、魅惑菇
- 寒冰菇、缠绕海草、玉米加农炮、香蒲
- 火焰英雄、铁壁坚果、水晶英雄、荆棘藤蔓
- 太阳神、霜之哀伤、世界树、末日花

**执行方式**:
```bash
cd art
chmod +x generate_all_plants.sh
./generate_all_plants.sh
# 预计时间：~2小时（33种×3候选×30秒/张）
```

---

## 📝 下一步计划

### 今晚/明天早上
1. **运行粒子系统测试** `_test_particle.exe`
   - 验证视觉效果
   - 检查性能（目标50fps+）
   
2. **批量生成剩余植物**
   - 运行 `generate_all_plants.sh`
   - 人工筛选最佳候选
   - 转换为PNG（去背景）

### 本周剩余时间
3. **僵尸精灵生成**（10种×6变体=60张）
4. **场景草坪图**（5张）
5. **粒子系统集成进主游戏**
   - 在 `pvz.c` 中添加粒子更新/绘制
   - 关键特效点调用（爆炸、击中、死亡）

---

## 🎯 关键里程碑

| 里程碑 | 目标日期 | 状态 |
|--------|---------|------|
| P0完成（美术资源齐全） | Week 2 | 🔄 |
| P1完成（特效+材质） | Week 3 | ⏳ |
| P2完成（动画+polish） | Week 4 | ⏳ |

---

## 📦 文件清单

### 新增文件（⚠️ 下列文件现均位于 `_scratch/`，`pvz-c/` 根目录下已不存在 particle.*）
```
pvz-c/
├── VISUAL_UPGRADE_PLAN.md          # 完整方案文档
├── particle.h                       # 粒子系统头文件
├── particle.c                       # 粒子系统实现
├── _test_particle.c                 # 粒子系统测试程序
├── _test_particle.exe               # 编译后的测试程序
├── art/
│   ├── generate_all_plants.sh       # 批量生成脚本
│   ├── convert_to_png.py            # PNG转换工具
│   └── test_plants/                 # 测试植物目录
│       ├── peashooter_1.jpg
│       ├── peashooter_2.jpg
│       ├── peashooter_3.jpg
│       ├── wallnut_1.jpg
│       ├── wallnut_2.jpg
│       ├── wallnut_3.jpg
│       ├── snowpea_1.jpg
│       ├── snowpea_2.jpg
│       ├── snowpea_3.jpg
│       ├── cherrybomb_1.jpg
│       ├── cherrybomb_2.jpg
│       └── cherrybomb_3.jpg
```

### 编译命令
```bash
# 粒子系统测试
gcc -O2 -o _test_particle.exe _test_particle.c particle.c \
    -lgdi32 -luser32 -lm -Wall -Wextra

# 主游戏（集成粒子系统后）
gcc -O2 -mwindows -static-libgcc -o pvz.exe pvz.c particle.c \
    -lgdi32 -luser32 -lmsimg32 -lm -Wall -Wextra
```

---

## 🐛 已知问题

1. **测试植物缺少向日葵** - 第一次生成被豌豆射手覆盖
   - 解决方案：在批量脚本中已修复命名逻辑
   
2. **PNG转换依赖rembg** - 需要安装Python库
   - 解决方案：`pip install pillow rembg`
   
3. **粒子渲染有4个unused variable警告**
   - 影响：无（纯警告，不影响功能）
   - 原因：预留了AlphaBlend透明混合的变量，当前简化版未使用

---

## 💡 技术亮点

1. **粒子系统设计**
   - 轻量级实现（512粒子上限）
   - 数组压缩回收（高效内存管理）
   - 6种类型覆盖所有特效需求

2. **美术生成流程**
   - mmx CLI自动化
   - 风格统一的prompt模板
   - 批量处理+人工筛选结合

3. **渐进式改进**
   - 先测试（4种植物验证风格）
   - 再批量（剩余28种）
   - 避免全部推倒重来

---

## 📈 预期效果

完成后游戏视觉将有质的提升：

**P0完成后**:
- ✨ 所有植物/僵尸有精美精灵图（不再是程序化几何）
- ✨ 场景有完整美术资源（5种草坪变体）

**P1完成后**:
- ✨ 爆炸/击中/死亡有粒子特效
- ✨ UI有材质和深度感（木纹、渐变、3D）

**P2完成后**:
- ✨ 植物有丰富动画（攻击、受伤、待机）
- ✨ 僵尸有逐帧行走动画
- ✨ UI有流畅过渡动画

---

**最后更新**: 2026-09-16 23:08  
**负责人**: Kiro AI Agent  
**项目仓库**: `pvz-c/`
