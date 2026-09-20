# -*- coding: utf-8 -*-
"""批量生成 50 株肉鸽植物 / 50 种肉鸽僵尸的专属贴图（mmx CLI）。

为什么要给每个肉鸽单位单独出图：
  这些单位现在都借基础植物的图（比如 7 株"豌豆系"肉鸽植物全用 peashooter.bmp），
  玩家根本分不清自己抽到的是哪一张 —— 肉鸽的乐趣建立在"一眼认出这张卡"上。

提示词结构（三段拼）：
    1. STYLE   统一画风锚点（和地形贴图共用同一套，保证整包素材是一伙的）
    2. BASE    基座原型（沙发射手 / 向日葵 / 坚果 …），来自 rgPlants[].base
    3. HOOK    这个单位独有的视觉钩子（量子光晕 / 冻霜 / 像素块 …）
  再叠一层品质氛围（TIER_FLAIR），让高稀有度单位天然更"贵"。

用法：
    python art/_gen_units.py                 # 全部 100 张
    python art/_gen_units.py P49 Z49         # 只生成指定的
    python art/_gen_units.py --plant         # 只植物
    python art/_gen_units.py --jobs 4        # 并发数（默认 4）
"""
import argparse
import os
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, "gen2")

# ------------------------------------------------------------------ 统一画风
# ⚠️ 长度预算：服务端硬限制 prompt < 1500 字符。
# STYLE(精简后 400) + TIER_FLAIR(最长 168) + GLOW_GUARD(240) + STYLE_STRICT 增量(200)
# ≈ 1010，余下 ~470 给 core（角色本体描述）。改动任何一段前先跑：
#   python -c "import sys;sys.path.insert(0,'art');import _gen_units as G;\
#              print(len(G.STYLE),len(G.STYLE_STRICT),len(G.GLOW_GUARD))"
# 曾经把 STYLE_STRICT 写成"完整 STYLE + 一大段否定项"，两段加起来 1396 字符，
# _fit() 的 room 变成**负数**，负索引切片失效 → 提示词 1769 字符被服务端拒收。
STYLE = ("cartoon 2D game asset, Plants vs Zombies art style, hand painted, "
         "vibrant saturated colors, bold clean dark outlines, rich shading, "
         "glossy highlights, crisp readable silhouette, "
         "single character centered, three-quarter front view facing left, "
         "full body, feet at the bottom of the frame, "
         "on a completely flat solid pure magenta #FF00FF background, "
         "uniform magenta with no gradient, "
         "no ground shadow, no drop shadow, no ellipse under the character, "
         "no text, no watermark, no border")

# 重生成用的加强版背景契约。
# 为什么需要：当提示词带「酸蚀 / 诅咒 / 死灵 / 虚空」这类暗色主题时，
# 模型会自作主张在角色背后补一圈同色系辉光或暗角（离散度 14~25），
# 抠图键控没法把"洋红辉光"和"洋红背景"分开，结果留一圈粉雾。
# 这里把背景约束从"陈述句"改成"显式否定项 + 画布描述"，实测能压住。
STYLE_STRICT = STYLE + (
    ", background is one flat magenta fill RGB(255,0,255), "
    "no glow, no aura, no vignette, no radial gradient, no color bleed, "
    "no particles, no scenery, no backdrop, no spotlight behind the character, "
    "the magenta background stays completely empty and untouched")

# 各品质档的"华丽度"。⚠️ 措辞有个硬约束：**能量必须长在角色身上，不能弥散到背后**。
# 原因（实测踩过）：
#   原来 tier4 写的是 "cosmic mythic aura, swirling energy particles, floating glowing shards"，
#   tier5 写 "layerered overlapping auras"，都是"环绕角色的能量场"这种**环境描述**。
#   模型会照实画一大片粉色/紫色辉光铺在角色背后 —— 抠图时这片辉光
#   与背景洋红同色系、且是柔和渐变的软边，无论单色键控还是泛洪都分不开，
#   成品干净不了（P26 的蘑菇、P43 的卡牌、P14 的时间漩涡都是这么来的）。
#   改成"能量收在角色体内/表面"（contained / inside / on the character）之后，
#   辉光成了角色的一部分，抠图时天然跟着主体保留，就不再是脏边。
TIER_FLAIR = {
    0: "simple clean design, modest detail",
    1: "a few subtle magical accents painted directly on the character, slightly upgraded look",
    2: "intricate glowing runes etched on the character's own body, energy accents inside its silhouette, epic upgraded design",
    # ⚠️ tier3 原写 "dramatic rim lighting hugging its outline"，
    # 结果模型画出**一大片环绕角色的粉紫色能量光**（Z00 时间悖论僵尸、
    # Z03 黑客僵尸实测），而且它和贴地椭圆连成一片洋红区域，
    # 任何后处理都分不开（见 art/_diag_srcvis.py 的叠加图）。
    # 改成"光只长在角色自身的边缘和装饰上"，并把规模压到 edge 一级。
    3: "ornate golden trim painted on the character itself, subtle warm highlights along its own edges, "
       "legendary hero design",
    4: "cosmic mythic energy contained inside the character's own form, glowing patterns across its body, "
       "small floating shards held tightly against it, mythic godlike design",
    5: "transcendent prismatic rainbow energy burning within the character's body, layered auras painted "
       "on its own surface, reality-bending ultimate final form, maximum detail",
    # 殿堂（第 7 档，索引 100~104）。
    # 这一档是「传奇伤害的 5 倍」，视觉上必须和 tier5 拉开层级 ——
    # 用的是**白金/炽白**而不是继续加彩虹：再叠一层彩色能量会和 tier5 糊成同一档。
    # 同样遵守 GLOW_GUARD：光长在角色身上，不许在背后铺场。
    #
    # ⚠️ 这里额外带了「没有地面 / 地面不许发光」的显式否定，且只挂在殿堂这一档。
    # 实测第一版（P100 玉米炮等 5 张）有三个共同缺陷：
    #   ① 模型自作主张在脚下补一块**棕色泥土台** —— 既不是 STYLE 否定的
    #      shadow/ellipse，也不是洋红族，fix_ground_shadow 根本清不掉；
    #   ② 更普遍的是**脚下一圈白色发光底盘 / 光池**（5 张里 4 张中招）——
    #      GLOW_GUARD 否掉的是"背后铺辉光"，管不住"地面上的光"；
    #   ③ 白色不是洋红族，键控时全被保留 → 进游戏就是植物踩着一块亮盘子。
    # 所以这里必须把"光只留在体内"和"地面什么都没有"写成两条独立的显式否定。
    # 不并进 STYLE 的原因：STYLE 是全部 175 个单位共用的契约段，
    # 改它等于让所有素材"看起来像重出过"，而且它已经 479 字符，塞不下。
    # 结论：**不要在提示词里写"发光/光辉"**。前两轮的失败都源于此 ——
    # "radiance / glowing from inside" 会被模型翻译成"角色脚下有一块发光的圆盘"。
    # 第三轮改成**纯材质**描述（白金装甲 + 鎏金纹样 + 宝石镶嵌），一个 light/glow
    # 的褒义词都不给，只留否定项，地面才干净。殿堂的"高级感"靠材质密度做，
    # 不靠发光 —— 这一点和 tier5（彩虹能量）刚好相反。
    6: "hallowed platinum-white armour plating with molten gold filigree inlaid on the character's own "
       "surface, gemstone accents set into its own body, throne-tier final form, maximum detail, "
       "rendered as solid polished materials with no light emission and no glowing parts, "
       "standing directly on the empty background: no dirt mound, no soil patch, no ground disc, "
       "no terrain, no pedestal, no glow, no light pool, no bright disc under its feet, "
       "no shadow cast on the ground",
    # 始祖（第 8 档，索引 105~107）。比殿堂更高一档，但**依然不能写发光/光辉** ——
    # 上一批殿堂就是被 "radiance" 引出了脚下的发光底盘（见 PIPELINE.md §6）。
    # 这一档改用"太初材质"表达最高级：黑曜石 + 熔金纹 + 内部炽白核，全是材质名词，
    # 没有一个发光动词，并继续把"地面什么都没有"写成独立否定项。
    7: "primordial obsidian and molten-gold materials, fossilised bark plates with white-hot "
       "filament veins set inside its own body, ancient runes carved into its own surface, "
       "creation-era final form, the most detailed asset in the game, "
       "rendered as solid polished materials with no light emission and no glowing parts, "
       "standing directly on the empty background: no dirt mound, no soil patch, no ground disc, "
       "no terrain, no pedestal, no glow, no light pool, no bright disc under its feet, "
       "no shadow cast on the ground",
}

# 紧随品质档之后再加一道"辉光归属"约束 —— 挨着那句容易引起误解的话说，
# 比放在 STYLE 里更有效（STYLE 离得太远，模型往往只顾着执行前面那句生动描述）。
GLOW_GUARD = ("all glowing energy belongs to the character itself and is painted on its own body, "
              "nothing glowing floats behind it, no aura, no energy field, no magic circle on the "
              "ground, no haze or wisps of light around the body, keep the space around the "
              "character clean magenta, no glowing platform under its feet")

# ------------------------------------------------------------------ 植物基座原型
P_BASE = {
    0:  "a cheerful sunflower with a round golden-yellow petal head, big friendly eyes, a smiling face and a green stalk with two leaves",
    1:  "a peashooter plant: a plump green bulbous body with a short wide green barrel snout for a mouth, two large expressive eyes and a green stem with leaves",
    2:  "a chubby brown wallnut: a big rounded tan seed with a hard shell texture, two big worried eyes and a small mouth",
    3:  "a potato mine: a lumpy brown potato lying low to the ground with a small red antenna light on top and a mischievous face",
    4:  "a snow pea plant: like a peashooter but icy light blue with frozen frost coating, a blue barrel snout and cool blue eyes",
    5:  "a double-barrel repeater peashooter with TWO stacked barrel snouts and a green body",
    6:  "a pair of round glossy red cherry bombs joined by a green stem, angry cartoon faces, one wearing a fuse",
    7:  "a long curved bright red jalapeno pepper with an angry face and a green stem",
    8:  "a three-headed peashooter plant with one body and three separate green barrel snouts stacked vertically",
    9:  "a spiky weed: a low flat patch of dark green thorny vines and sharp spines creeping along the ground",
    10: "a magnet mushroom: a blue-grey mushroom with a horseshoe magnet shape growing out of its cap and a small face",
    11: "a kernel-pult: a plant with a corn cob body and a bent green arm that flings kernels, wearing a small husk hat",
    12: "a starfruit: a bright yellow five-pointed star shaped fruit with a small face and tiny green leaves",
    13: "a cactus: a tall rounded green cactus with white spikes all over, a hollow snout mouth and a friendly face",
    14: "a split pea: a green peashooter with two barrels pointing in opposite directions",
    15: "a lightning reed: a tall slim green reed with a bulbous top that crackles with electricity",
    16: "a bloomerang: a green plant with a bent boomerang shaped body and a curved throwing arm",
    17: "a fume-shroom: a purple-grey mushroom with a wide open funnel shaped mouth puffing green gas",
    18: "a laser bean: a dark green bean plant with a long straight tube and a glowing red laser emitter at the tip",
    19: "a melon-pult: a green plant with a catapult arm holding a striped green watermelon",
    20: "a winter melon-pult: a pale blue-white plant with a catapult arm holding a frosted light blue melon",
    21: "a gloom-shroom: a dark purple mushroom with a wide funnel mouth surrounded by a ring of spores",
    22: "a gold magnet: a golden magnet mushroom with a shiny gold horseshoe magnet and sparkles",
    23: "a twin sunflower: one sunflower plant with two separate golden flower heads on forked stalks",
    24: "a marrow plant: a pale bone-white squash with a hollow bone texture and a small face",
    25: "a pumpkin: a big round orange pumpkin shell with a carved ribbed surface and a green stem",
    26: "a garlic: a plump white garlic bulb with clove segments and a small sour face",
    27: "a hypnotic mushroom: a purple mushroom with a swirling spiral cap pattern and glowing eyes",
    28: "an ice-shroom: a pale cyan mushroom covered in frost and icicles",
    29: "a tangle kelp: a dark green seaweed plant with long slimy fronds and a mouth full of tendrils",
    30: "a cob cannon: a heavy green corn cannon on a wooden carriage with a big corn cob barrel",
    31: "a cattail: a brown cattail reed plant with a fluffy tail tip and a spiky face",
    # 神级植物（已在游戏里，这批只补肉鸽单位，但原型描述保留以便复用）
    32: "a fiery hero shooter plant with a burning flame mane and an orange glowing barrel",
    33: "a heavy armoured iron wallnut with riveted steel plates and glowing bolts",
    34: "a crystalline shooter with faceted gem shards growing from its body",
    35: "a thorn vine hero with a barbed whip of vines and purple thorns",
    36: "a radiant sun-god flower with a blazing halo of light and golden petals",
    37: "a frost hero plant covered in eternal ice, snowflakes and frozen blades",
    38: "a colossal world tree with a woody trunk, wide canopy and glowing ancient runes",
    39: "a doom flower: a huge dark carnivorous flower with a fanged maw and violet void energy",
}

# ------------------------------------------------------------------ 僵尸基座原型
Z0 = "a shambling cartoon zombie: pale grey-green sickly skin, slack jaw with teeth, dull white eyes, tattered torn shirt and trousers, arms stretched out in front, bare feet"
Z_BASE = {
    0:  Z0,
    1:  Z0 + ", also wearing a bright orange traffic cone on its head",
    2:  Z0 + ", also wearing a dented grey metal bucket on its head",
    3:  Z0 + ", also holding a small red flag on a pole",
    4:  Z0 + ", wearing a purple disco vest and sunglasses, frozen mid disco pose",
    5:  Z0 + ", riding a small rusty ice-resurfacing zamboni machine",
    6:  Z0 + ", bloated and inflated, lifting off the ground like a balloon",
    7:  Z0 + ", carrying a long red vaulting pole in its hands",
    8:  Z0 + ", pushing a rusted metal screen door as a shield in front of itself",
    9:  Z0 + ", wearing full red-and-white american football armour and helmet",
    10: Z0 + ", wearing a rumpled grey suit and reading an open newspaper",
    11: Z0 + ", wearing a mining hard hat with a lamp and holding a pickaxe",
    12: Z0 + ", a gigantic hulking zombie twice as tall as a normal one, wearing overalls, "
          "with a broken telephone pole across its shoulders",
}

# ------------------------------------------------------------------ 专属视觉钩子
P_HOOK = {
    0:  "quantum theme: glowing cyan energy sphere around its head, three ghostly semi-transparent duplicate images of itself overlapping behind, sparkling blue particles",
    1:  "time theme: a translucent clock face behind its petals with slowly rotating hands, golden time-freeze sparks, clock gears floating around",
    2:  "acid theme: dripping corrosive acid, a bubbling green liquid trail under it, holes burned through its leaves, toxic green fumes",
    3:  "black hole theme: a small swirling black hole disk above its cap sucking in a stream of white bullets, deep purple accretion glow",
    4:  "mirror theme: polished mirror-metal shell, three faint mirrored duplicate wallnuts floating behind it, reflected light glints",
    5:  "gluttony theme: an oversized gaping toothed maw, bloated swollen body, food scraps and bone bits stuck in its mouth",
    6:  "blockchain theme: a chain of glowing hexagonal blockchain links wrapping its body, digital cubes orbiting, one big blinking red digital fuse",
    7:  "gambling theme: a pair of giant red dice, playing cards fanned out, casino chip particles, question mark sparkles",
    8:  "virus theme: tiny cartoon virus cells with spikes crawling over its stalk and infecting the neighbouring soil, sickly green glow",
    9:  "gravity theme: concentric gravity ripples warping the air, small rocks and debris orbiting in a ring, a purple gravity core",
    10: "gacha theme: floating tarot-like blank cards with golden backs flying out of the explosion, rainbow lottery sparkles, a spinning slot reel",
    11: "loan theme: a two-times-too-thick inflatable shell padded like a money bag, gold coins spilling out of a tear, a glowing contract scroll",
    12: "spider theme: thick white spider silk cords wrapped around it, a large spider web spread on the ground under it, small spiders",
    13: "roof-climbing theme: brick roof tiles growing under its roots like little boots, a red brick chimney sprouting from the top, mortar and trowel",
    14: "time-rewind theme: thin blue time ribbons wrapping around its own body, a small glowing clock face "
        "on its chest, a faint afterimage hugging its back",
    15: "quantum entanglement theme: two identical plants joined by a glowing energy beam and a linked bluish cord, sparks along the link",
    16: "noise theme: visible sound wave rings blasting from its mouth, musical static symbols, cracked shapes in the air",
    17: "ice theme: encased in crystalline ice armour with frozen zombie silhouettes inside the ice, sharp ice shards and blue frost mist",
    18: "self-destruct theme: cracks of glowing orange magma over its body, a ticking bomb inside its cap, sparks shooting out",
    19: "meme theme: small floating emoji-like glowing happy faces orbiting it, a spiral of glowing glyphs, smug smiling sunflower face",
    20: "laser theme: an alien sci-fi cannon replacing its snout, glowing green laser beam and targeting reticle, metal tech plating",
    21: "gambler theme: a spinning roulette wheel behind it, chips and dice on the ground, dramatic red-black lighting",
    22: "grid-crawling theme: pixel grid squares unfolding under its roots, it is stepping one tile forward with glowing green arrows",
    23: "code theme: a curtain of falling green matrix code raining in front of it, digital square glitch shards, blue wireframe overlay",
    24: "rhythm theme: floating musical notes and a pulsing equalizer bar behind it, glowing neon beat rings",
    25: "pixel-glitch theme: part of its body exploded into huge chunky pixels and voxels scattering, a glitch screen tear, RGB split",
    26: "hacking theme: a tiny laptop held in its own hands, a small blue holographic code panel resting "
        "flat against its cap, a USB cable plugging into its own head",
    27: "fusion theme: it is absorbing and merging a smaller plant into its body, alchemical fusion circle with glowing runes",
    28: "teleport theme: three afterimages in mid-teleport, a ring of blue portal energy around it, star sparkles",
    29: "petal-barrage theme: a dense storm of rainbow flower petals blasting out in many directions, spinning floral mandala",
    30: "life-drain theme: glowing red life-energy veins running through its own body, a small red orb glowing in its mouth, greedy licking lips",
    31: "curse theme: dark violet hex runes branded across its own shell, small skull charms hanging on its body, a smoky black tint on its own surface",
    32: "mirror-glutton theme: it has swallowed a zombie and a translucent copy of that zombie is growing out of its side, mirror shards",
    33: "virtual-reality theme: a wireframe holographic duplicate of itself layered directly over its own body,"
        " blue hologram scanlines on its surface, glitchy fake-health bar attached to its own head",
    34: "dance-king theme: a disco dance pose, a tight ring of orange-gold flame hugging its own silhouette, glowing sequins on its body, music notes gripped in its hands",
    35: "quantum-cherry theme: the two cherries teleporting apart with blue portal rings hugging each cherry, a chain of pixel explosions along its stem, reassembling fragments",
    36: "bioweapon theme: sickly pulsing green infection sacs growing on its own body, a small beam of contagion spores from its own mouth, a biohazard symbol glowing on its body",
    37: "reverse-time theme: an inverted clock face mounted on its own body spinning backwards, green restorative light pulses along its own vines, healing leaves",
    38: "probability theme: half of its body is solid and the other half is a translucent flickering ghost outline, percent symbols engraved on its own body",
    39: "gluttony-sun theme: it has grown a second greedy flower head, absorbing beams of sunlight from the sky into its petals",
    40: "bouncing theme: a bouncing rubbery yellow corn kernel with motion arcs, springy coil under it, ricochet trail lines",
    41: "maze theme: it is growing a small hedge maze wall out of its body, blocks and a tiny labyrinth diagram glowing above",
    42: "weather theme: a small personal storm cloud above it raining, a snowflake and a lightning bolt and a wind swirl orbiting",
    43: "card-draw theme: an arm of glowing playing cards extending from its own body like a hand of five cards, cards clipped to its head, glowing card symbols printed on its own skin",
    44: "assassin theme: cloaked in shadow, holding a curved dagger, only its glowing yellow eyes visible, a fading smoke afterimage",
    45: "music-box theme: a giant wind-up music box key in its back, floating lullaby music notes with Z z z sleeping symbols, soft pink glow",
    46: "kung-fu theme: martial arts pose, a glowing golden qi energy aura shield around it, floating calligraphy brush stroke of light",
    47: "pathogen theme: bubbling toxic pustules spreading over its stalk, a splatter trail of purple plague goo, floating disease cells",
    48: "blackhole-sun theme: a mini black hole at its center pulling a spiral of sunlight and petals inward, deep violet accretion disk",
    49: "ultimate everything plant: an insane overloaded chimera covered in ALL of these effects at once - quantum spheres, clock faces, acid drips, black hole, mirror shards, blockchain links, dice, virus cells, gravity rings, tarot cards, spider webs, lasers, musical notes, matrix code, pixels, portals, flames, ice, petals, hooks, wings, gears and rainbow energy - a magnificent chaotic final boss plant, still readable as one character",

    # ---- 第二批 50 株（索引 50~99）----
    50: "spring theme: a coiled steel spring body compressed under pressure, shown mid-bounce with motion arcs, a trampoline pad at its base",
    51: "grapple theme: a heavy iron claw on a chain shooting out and hooking forward, taut chain links, sparks at the hook tip",
    52: "magnet-winch theme: a big horseshoe magnet drum with copper coil windings, metal debris and armour plates flying toward it along field lines",
    53: "piston theme: a hydraulic piston ram with a pressure gauge, steam venting from valves, sparks where the ram strikes",
    54: "acid-nozzle theme: a brass spray nozzle dripping corrosive green liquid, a bubbling acid puddle under it, toxic fumes rising",
    55: "hunter-sight theme: a sniper scope mounted on its stalk with a red laser dot, a targeting crosshair projected onto the ground",
    56: "judgement theme: a golden balance scale with a glowing verdict scroll, brass weights floating around, a beam of judgement light",
    57: "peeling-blade theme: twin curved blades shaving layers off an armour plate, metal flakes peeling away in the air, sharp sparks",
    58: "lucky-clover theme: a four-leaf clover crown, spinning gold coins and lucky stars around it, bright emerald shine",
    59: "executioner theme: a heavy guillotine axe head on a dark wooden frame, a red judgement mark stamped on the ground, ominous red glow",
    60: "guillotine theme: a full guillotine tower with a chained falling blade, blood-red runes running up the frame, cold steel shine",
    61: "resonance theme: concentric sound-wave rings pulsing outward, floating musical notes, a warm golden resonance halo",
    62: "ward theme: a translucent blue shield dome projected over its allies, thorned vines forming a fence, soft barrier light",
    63: "sanctuary-bell theme: a polished golden temple bell on a wooden frame, holy light rays, ringing sound ripples spreading out",
    64: "cornucopia theme: a woven horn of plenty spilling golden grain and coins, wheat sheaves, a warm abundance glow",
    65: "golden-law theme: a golden codex tablet with glowing laws engraved on it, a sun-coin halo behind it, drifting gold particles",
    66: "purification theme: a tiered spring of pure water, a floating white lotus, cleansing light washing outward in waves",
    67: "frost-lichen theme: crystalline frost growing in lichen patches on the ground, cold vapour curling, pale blue rime",
    68: "swamp-moss theme: a sticky mossy bog patch with rising bubbles and tar streaks, reeds, humid green murk",
    69: "polar-tundra theme: jagged ice pillars and frozen ground cracks, aurora ribbons above, frozen shards embedded in the soil",
    70: "stasis-field theme: a humming spherical stasis bubble, suspended particles frozen mid-air, violet field lattice",
    71: "clockwork theme: brass gears meshing across its body, a visible wind-up mainspring, oiled mechanical joints",
    72: "hummingbird theme: translucent hummingbird wings blurred with motion, speed afterimages, a nectar droplet trail",
    73: "overclock theme: an exposed reactor core with glowing coolant pipes, warning lamps flashing red, heat distortion above the vents",
    74: "beehive theme: a hanging paper beehive dripping honey, small cartoon bees flying out in a line, honeycomb cells",
    75: "ant-colony theme: a mound nest with tunnels, a marching line of small cartoon ants carrying leaves, a crumb trail",
    76: "spider-queen theme: an ornate spider queen with a crown of eyes, a large egg sac, thick webs stretching out around it",
    77: "necro-garden theme: leaning gravestones with glowing green wisps rising, small ghostly undead sprouts, eerie green fog",
    78: "star-beacon theme: a tall lighthouse beacon projecting a targeting beam into the sky, an orbital ring above, satellite dishes",
    79: "meteor theme: a burning meteor trailing fire and smoke angling down, a glowing impact crater, molten debris",
    80: "sky-eye theme: a colossal eye-shaped satellite above it scanning with a laser line, mechanical iris blades, starlight",
    81: "blood-rose theme: deep crimson roses dripping blood droplets, sharp thorns, a red vitality glow pulsing",
    82: "maneater-garden theme: a flower bed of gaping toothed blooms, scattered bones and a gnawed boot, fleshy reddish petals",
    83: "immortal-vine theme: glowing life-thread vines looping back into itself, floating green vitality orbs, unbroken regrowth buds",
    84: "sun-furnace theme: a clay furnace with molten sunlight pouring out of its mouth, glowing coals, heat shimmer",
    85: "light-convergence theme: a faceted crystal lens concentrating many light beams into one bright core, prismatic flares",
    86: "sun-bank theme: a heavy iron vault with a sun emblem on the door, stacked coin bags and a ledger, tarnished gold",
    87: "spike-trap theme: rows of rusty metal spikes jutting from the soil, an armour plate skewered on them, dry scratches",
    88: "tar-pit theme: a black bubbling tar pool with sticky strands, bones sinking in, thick dark smoke",
    89: "lava-fissure theme: a glowing crack in the ground with molten lava seeping out, red-hot pebbles, heat waves distorting the air",
    90: "scatter-bloom theme: several small flower barrels angled in different directions, petals spraying outward in a fan",
    91: "boomerang theme: curved wooden boomerang blades with a flight-path arc, spin motion rings, a return trail",
    92: "marble-bounce theme: glossy glass marbles ricocheting with dotted bounce paths, small impact stars at each contact",
    93: "prism theme: a triangular crystal prism splitting a laser into rainbow beams, refracted light bands, glass sparkle",
    94: "phase theme: a half-transparent phasing body with a glitching edge, a mirrored afterimage offset beside it, phase shimmer",
    95: "thorn-crown theme: a barbed crown of thorns worn on top, a spiked bark armour shell, blood-tipped thorns",
    96: "last-stand theme: a battle-worn battered body with cracked plates, a fierce red berserk aura flaring, glowing battle scars",
    97: "time-lord theme: a floating throne of clock faces and suspended gears, frozen falling sand and stopped clock hands, an hourglass crown",
    98: "genesis-tree theme: a colossal world tree crown bearing glowing life fruit, roots coiling around other plants, creation light",
    99: "ultimate second-generation plant: an overloaded chimera wreathed in ALL of the second batch effects at once - springs, chains, magnets, pistons, acid, scopes, scales, blades, clovers, guillotines, bells, horns, lotuses, frost lichen, swamp moss, ice pillars, stasis bubbles, gears, hummingbird wings, reactor cores, beehives, ant trails, spider webs, gravestones, beacons, meteors, satellite eyes, blood roses, glow vines, furnaces, prisms, vaults, spikes, tar, lava, marbles, thorn crowns and rainbow berserk energy - a magnificent chaotic final form, still readable as one character",

    # ---- 殿堂五尊（索引 100~104）：rgPlants[] 里 RGQ_HALL 那一档 ----
    # 五尊的基座刻意各不相同（玉米炮 / 冰瓜 / 杨桃 / 激光豆 / 猫尾草），
    # 好让"五张专属贴图 + 行为手感"的差异真的看得出来。
    # 每条的钩子都对应它在 pvz.c 里的机制描述，不写没接线的效果。
    100: "final judgement theme: an executioner cannon with a platinum guillotine blade folded into its "
         "barrel, white-hot plasma seams along its own corn body, a chained verdict seal on its carriage",
    101: "eternal frost prison theme: a sphere of absolute-zero air around its own frozen melon body, "
         "icicle chains growing along its own rind, web-silk frost on its own skin, one eye frozen shut",
    102: "celestial constellation theme: five star-points of its own body firing a rotating petal barrage, "
         "tiny constellations inlaid on its own yellow rind, a star servant circling tight against it",
    103: "dawnlight verdict theme: a spear of white light running lengthwise through its own body and out "
         "its muzzle, a luminous targeting mark on its own forehead, filament glow tracing its pod",
    104: "heaven-net subjugation theme: a crown of light-net threads coiled around its own cattail body, "
         "homing seeker bolts drawn tight against its own fluff, shield and sunlight bands on its surface",

    # ---- 始祖三尊（索引 105~107）：rgPlants[] 里 RGQ_PRIMORDIAL 那一档 ----
    # 三个基座分别是 地刺 / 坚果 / 豌豆炮（用户指定），钩子对应它们在 pvz.c 里的机制。
    # 同样受"不许出现地面物件"约束，所以每条都以自身材质收尾。
    105: "primordial bramble field theme: an ocean of obsidian thorns spreading outward flat across "
         "the ground from its own root mass, molten gold sap glowing inside every thorn, fossil "
         "bark plates armouring its own base, ancient runes carved along its own vines",
    106: "primordial bulwark theme: a wall of fossilised bark and obsidian slabs standing since the "
         "beginning of time, white-hot filament seams running across its own face, gold inlay "
         "rivets along its own shell, a single calm ancient eye set into its own centre",
    107: "primordial chaos volley theme: five obsidian seed-barrels ringing its own body firing in "
         "every direction at once, molten gold veins along each of its own barrels, cracks of "
         "white-hot creation light splitting its own stem, ancient runes on its own stalk",
}

# ---- 第三批（108~157）：视觉钩子直接取自 art/_cards_new.py ----
# 为什么不在这里硬编码 50 条：那会和 pvz.c 的行、卡牌清单形成三份手工同步的列表。
# 现在 _cards_new.py 是唯一真源，改卡只改那一处。
try:
    import _cards_new as _CN
    for _idx, _card, _prim in _CN.all_cards():
        if _card.get("hook"):
            P_HOOK[_idx] = _card["hook"]
except Exception as _e:            # noqa: BLE001
    print("[warn] 未能从 _cards_new 载入第三批视觉钩子：%s" % _e)

Z_HOOK = {
    0:  "time paradox theme: several ghostly afterimages of itself standing apart in time, a cracked floating clock face, rewinding clock hands",
    1:  "quantum superposition theme: three semi-transparent copies of itself standing side by side, each flickering with a different colored aura",
    2:  "virus theme: bright green cartoon virus spikes erupting from its skin, contagious green cloud, small virus cells flying to other zombies",
    3:  "hacker theme: a laptop under one arm, a floating holographic screen full of code, glowing cable plugged into a plant, smug grin",
    4:  "gluttony theme: hugely bloated belly, stuffing a plant into its mouth, food scraps and bones all over, drooling",
    5:  "chef theme: a white chef hat and apron, a bandolier of cleavers and kitchen knives, throwing a spinning cleaver, flying utensils",
    6:  "maze-builder theme: pushing a rolling wall of stone bricks and hedge maze blocks, a blueprint in its hand",
    7:  "gambling theme: holding a giant spinning roulette wheel, casino chips and dice flying, glowing red eyes, wild grin",
    8:  "loan theme: inflated and overstuffed, wearing a torn business suit, gold coins spilling from its pockets, a glowing red debt contract",
    9:  "spider theme: shooting white web strands from its mouth, spider webs draped over its body, small spiders crawling on it",
    10: "virtual reality theme: a wireframe hologram of a normal zombie overlaid on it, a fake floating health bar above its head, glitch scanlines",
    11: "code compilation theme: a wall of glowing green code blocks being generated from its hands, a laptop fused into its chest",
    12: "noise theme: a giant boombox speaker replacing its head, sound wave rings blasting out, cracked speaker cone, static",
    13: "gravity well theme: a swirling dark purple gravity vortex in front of its chest, other zombies and debris being dragged in, distorted air",
    14: "rhythm theme: wearing headphones, a glowing equalizer across its chest, neon dance floor tiles under its feet, music notes",
    15: "teleport theme: mid-teleport with three flickering afterimages, blue portal rings around it, dizzy stars",
    16: "mirror clone theme: three identical copies of itself standing in a row with a mirror shard between them, shimmering",
    17: "sun-devouring theme: a giant vacuum funnel mouth sucking a beam of sunlight into itself, drained sunflowers wilting nearby",
    18: "blockchain fork theme: its body splitting into two smaller zombie copies like a blockchain fork, glowing chain links and cubes",
    19: "biochemical mutation theme: bubbling mutant growths, extra arms and eyes bulging, a broken glass lab flask pouring green liquid",
    20: "alien laser theme: a chrome alien blaster replacing its arm, glowing purple laser beam and targeting reticle, sci-fi tech armour",
    21: "gambler theme: holding a giant pair of dice, its body visibly flickering between huge and tiny, a roulette wheel behind",
    22: "slow grid-crawler theme: a heavy blocky pixel-grid body, one giant glowing green arrow marking its next single step, snail beside it",
    23: "pixel-glitch theme: parts of its body have crumbled into large chunky pixels and voxel cubes, RGB split glitch seams, screen tear",
    24: "fusion boss theme: a colossal zombie made of several smaller zombies fused together, extra limbs and heads sticking out, glowing seams, tower-shaped",
    25: "bouncing reflector theme: a shiny chrome mirror finish body, bullets bouncing off its chest in glowing ricochet arcs, spring under its feet",
    26: "assassin theme: cloaked in dark shadow with only glowing red eyes visible, holding a curved blade, leaving smoke afterimages",
    27: "greedy sun theme: its outstretched hands sucking in glowing sun coins, a bag of sun collected, shiny greedy grin",
    28: "curse theme: black cursed aura, floating skull wisps, purple hex runes burning on the ground, a torn deck of cards in its hand",
    29: "kung-fu theme: martial arts stance, a glowing golden qi energy shield bubble around its body, floating calligraphy brush stroke of light",
    30: "quantum entanglement theme: a glowing energy chain linking its chest to another zombie offscreen, shared damage sparks along the chain",
    31: "time reversal theme: an hourglass in its chest with sand flowing upward, green healing light mending its wounds, rewinding clock hands",
    32: "sun-hacker theme: wearing a visor and typing on a floating holographic panel full of code, glowing sun symbols being crossed out",
    33: "pathogen theme: bubbling toxic purple pustules all over, spraying plague goo, floating disease cells, sickly smoke",
    34: "reverse-time theme: a giant clock hand pushing a plant backwards, glowing blue rewinding trails, an hourglass spinning backwards",
    35: "probability shield theme: a translucent flickering hexagonal shield of light in front of it, floating percentage symbols",
    36: "maze-back builder theme: stone maze walls rising behind it as it walks forward, a blueprint scroll, brick dust",
    37: "storm-caller theme: a personal dark storm cloud above it shooting lightning, swirling snow and rain, howling wind arcs",
    38: "card-dealer theme: an arm of glowing negative playing cards fanned out, cards orbiting its head, a magic circle of purple runes",
    39: "kung-fu combo theme: mid flying kick with multiple glowing golden leg afterimages, impact shockwave rings, martial arts belt",
    40: "blackhole devourer theme: a colossal zombie with a swirling black hole in its open chest, plants and bullets being sucked in and stretched, deep violet accretion glow",
    41: "bombardier chef theme: a chef hat and apron loaded with cartoon bombs, throwing a lit bomb with a trailing fuse, explosion behind",
    42: "quantum overlay theme: three stacked overlapping copies of itself with mirrored duplicates, layered translucent auras in different colours",
    43: "virus swarm theme: a cloud of green virus cells and toxic spores swirling around it, infecting everything nearby, glowing contagion veins",
    44: "infinite gluttony theme: an endlessly expanding bloated zombie that has swallowed several zombies which bulge out of its body, extra mouths",
    45: "virtual-reflect theme: a holographic fake body flickering over its real body, a translucent mirror bubble shield reflecting bullets",
    46: "code-maze theme: glowing green code walls rising both in front of and behind it, trapping plants in a digital grid cage",
    47: "noise-blackout theme: a huge amplifier stack and speakers on its back, blasting sound waves that crack a sunflower, dead radio static",
    48: "gravity dash theme: a swirling gravity vortex dragging smaller zombies behind it, sprint speed lines and crushed ground",
    49: "ultimate everything zombie: an insane colossal chimera boss covered in ALL of these effects at once - afterimages, quantum clones, "
        "virus clouds, holographic code walls, cleavers, maze bricks, dice, webs, gravity vortex, headphones, portals, mirror shards, "
        "sun funnels, blockchain cubes, mutant growths, alien lasers, chunky pixels, fused zombie bodies, hourglass, plague pustules, "
        "storm clouds, playing cards, black hole chest, cartoon bombs, blades and rainbow energy - a magnificent chaotic final boss, "
        "still readable as one character",

    # ---- 第三批 20 只（索引 50~69）：会飞 / 会跳 / 会召唤 / 会闪现 ----
    50: "glider theme: a tattered cloth hang-glider strapped to its back, feet dangling clear off the ground, faint updraft swirls beneath it, goggles",
    51: "flea theme: oversized coiled spring legs, compressed mid-jump with motion arcs, tattered shorts, dust puffing off its heels",
    52: "slinger theme: a heavy leather pouch of jagged rocks on its back, one arm wound up mid-throw, a rock already airborne ahead of it",
    53: "necromancer theme: a ragged hooded robe with glowing green runes, a bone staff topped with a skull, small bony hands clawing up out of the ground around it",
    54: "phase theme: body rendered half-transparent and split into offset glitch slices, a violet after-image trailing behind, reality cracks at its edges",
    55: "charger theme: a battered leather helmet with a bent face-guard, shoulder pads, lowered head-first charge pose, speed streaks behind",
    56: "bulwark theme: a huge riveted iron tower shield held forward, overlapping plate armour, faint hexagonal barrier shimmer on the shield face",
    57: "mirror theme: body clad in polished mirror-metal plating, a mirror-bubble dome around its torso bouncing light, reflected glints",
    58: "splitter theme: a body visibly cracking down the middle with glowing green seams, two half-formed smaller torsos budding out of its sides",
    59: "regenerator theme: exposed muscle and sinew that is knitting itself back together, wet pink regeneration glow, medical stitch marks",
    60: "berserker theme: bloodshot bulging eyes, torn-off shirt, steam venting from its shoulders, deep red rage aura, clenched fists",
    61: "taunt theme: a dented brass megaphone held to its mouth, a loudspeaker backpack, exaggerated jeering posture, mockery sound-wave rings",
    62: "toxic-cloud theme: a bloated gas sac on its back leaking green vapour, a hissing valve, sickly yellow-green haze pooling around its feet",
    63: "frostbite theme: jagged ice-crystal teeth, frozen drool icicles hanging from its jaw, rime coating its arms, cold mist",
    64: "sun-thief theme: bulging pockets and a sack stuffed with glowing sun orbs, sticky grasping fingers, a sneaky hunched posture",
    65: "glutton theme: an impossibly wide unhinged jaw with rows of teeth, distended belly, food scraps and plant stems dangling from its mouth",
    66: "undead theme: skeletal face showing through torn skin, a cracked rib cage with faint red light inside, dead-but-standing posture",
    67: "magnet theme: copper coil windings wrapped around its torso, a humming electromagnet core in its chest, scrap metal and tools orbiting it",
    68: "burrower theme: a whirring conical drill mounted on its head, clumps of dirt and torn roots flying up, half-submerged into the soil",
    69: "zombie-king theme: a crooked golden crown, a tattered royal cape, an aura of command, smaller zombies kneeling in its shadow, regal but rotting",
}


def build_plant(i, base, tier):
    hook = P_HOOK.get(i)
    if not hook:
        return None
    return "%s, %s" % (P_BASE[base], hook)


def _zbase_key(base):
    # ZT_* -> Z_BASE 的映射：只有 13 个基础僵尸，其余都回退到普通僵尸
    return base if base in Z_BASE else 0


# mmx 服务端硬限制：prompt 必须 < 1500 字符，否则直接返回
# "invalid params, prompt length must be less than 1500"。
# STYLE 那一段（背景/构图契约）是绝对不能动的，超长只能从 core 里砍。
MAX_PROMPT = 1480


def _fit(core, tier, style):
    """把 core 压到「core + TIER_FLAIR + GLOW_GUARD + style」不超 MAX_PROMPT。

    砍的是 core 的尾段（视觉 hook 的后半），在逗号边界上断，
    免得留下半个词组让模型乱补。style 与 GLOW_GUARD 是出图的契约，必须完整保留。
    """
    room = MAX_PROMPT - len(TIER_FLAIR[tier]) - len(GLOW_GUARD) - len(style) - 8
    if room <= 0:
        # 契约段自己就把预算吃光了 —— 这时**必须返回空 core 并让调用方报错**，
        # 不能继续切片：负 room 会让 core[:room] 变成"去掉末尾若干字符"，
        # 长度不减反增，拼出来的 prompt 直接超服务端 1500 硬限被拒。
        raise ValueError("提示词预算不足：TIER_FLAIR(%d)+GLOW_GUARD(%d)+style(%d) 已超 %d，"
                         "请先精简契约段"
                         % (len(TIER_FLAIR[tier]), len(GLOW_GUARD), len(style), MAX_PROMPT))
    if len(core) <= room:
        return core
    cut = core[:room]
    if "," in cut:
        cut = cut[:cut.rfind(",")]          # 退到最近的逗号，不要半个短语
    return cut


def build(key, strict=False):
    """key 形如 'P07' / 'Z31'。返回 (key, prompt, out_path)。

    三段拼接顺序有讲究：core（角色描述） → TIER_FLAIR（华丽度）
    → GLOW_GUARD（辉光归属） → style（背景契约）。
    GLOW_GUARD 必须**紧贴** TIER_FLAIR 之后，因为 tier4/5 那句最容易被
    模型理解成"在背后铺一片能量场"，紧接着否定它才压得住；放进 style
    里离得太远，实测无效。
    """
    kind, idx = key[0], int(key[1:])
    if kind == "P":
        tier = P_TIER[idx]
        core = build_plant(idx, P_BASEIDX[idx], tier)
    else:
        tier = Z_TIER[idx]
        core = "%s, %s" % (Z_BASE[_zbase_key(Z_BASEIDX[idx])], Z_HOOK[idx])
    style = STYLE_STRICT if strict else STYLE
    prompt = "%s, %s, %s, %s" % (_fit(core, tier, style), TIER_FLAIR[tier],
                                 GLOW_GUARD, style)
    return key, prompt, os.path.join(OUT, "u_%s.jpg" % key.lower())


# 服务端按 RPM 限流，并发一高就整批返回
# code 10 "Input content flagged by sensitivity filter (rate limit exceeded(RPM))"
# —— 名字唬人，其实就是「请求太快」。所以这里：串行 + 每次之间留间隔，
# 真撞上限就退避重试，而不是把这张图记为失败。
RPM_GAP  = 2.0      # 每次请求之间的最小间隔（秒），全局生效
_gen_lock = threading.Lock()
_last_t = [0.0]
_gap    = [RPM_GAP]


def _throttle():
    with _gen_lock:
        gap = time.time() - _last_t[0]
        if gap < _gap[0]:
            time.sleep(_gap[0] - gap)
        _last_t[0] = time.time()


def _slow_down():
    """撞到限流就把全局间隔加倍（上限 20s）——与其整批失败，不如自己降速。"""
    with _gen_lock:
        _gap[0] = min(_gap[0] * 2.0, 20.0)
        return _gap[0]


def _run_once(prompt, out):
    cmd = ["mmx", "image", "generate", "--prompt", prompt,
           "--aspect-ratio", "1:1", "--out", out]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, shell=True)
    ok = os.path.exists(out) and os.path.getsize(out) > 20000
    return ok, ((r.stdout or "") + (r.stderr or ""))


def _bg_spread(path):
    """量一张出图的背景纯度：取边框一圈的中位色与平均偏差。

    这是整条流水线里唯一能客观判断"这张图能不能用"的指标 ——
    背景不纯（模型自作主张画了渐变/辉光/场景）就没法键控抠图，
    留着就是一圈粉雾。返回 None 表示图不可读。
    """
    try:
        import numpy as np
        from PIL import Image
        a = np.array(Image.open(path).convert("RGB")).astype(np.float32)
    except Exception:                                # noqa: BLE001
        return None
    ring = 8
    px = np.concatenate([a[:ring].reshape(-1, 3), a[-ring:].reshape(-1, 3),
                         a[:, :ring].reshape(-1, 3), a[:, -ring:].reshape(-1, 3)])
    c = np.median(px, axis=0)
    return float(np.mean(np.abs(px - c)))


def _run_bestof(prompt, out, n):
    """一次生成 n 张候选，挑背景最纯的那张留用。

    为什么要 best-of-N 而不是"失败就重试"：
      背景渐变不是随机失败，是提示词主题决定的（量子/能量/酸蚀这类设定下
      模型几乎每次都会补一层辉光）。单纯重试等于赌运气；
      一次多出几张、按客观指标挑最好的，命中率是线性的。
    """
    import glob
    import shutil
    import tempfile
    d = tempfile.mkdtemp(prefix="rgcand_")
    try:
        cmd = ["mmx", "image", "generate", "--prompt", prompt,
               "--aspect-ratio", "1:1", "--n", str(n),
               "--out-dir", d, "--out-prefix", "c"]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, shell=True)
        cands = sorted(glob.glob(os.path.join(d, "c_*.jpg")))
        if not cands:
            return False, ((r.stdout or "") + (r.stderr or ""))
        scored = []
        for c in cands:
            s = _bg_spread(c)
            if s is not None:
                scored.append((s, c))
        if not scored:
            return False, "候选图都无法读取"
        scored.sort()
        best_s, best = scored[0]
        shutil.copyfile(best, out)
        return True, "best-of-%d 选中（背景离散 %.1f，候选 %d 张）" % (
            n, best_s, len(cands))
    finally:
        shutil.rmtree(d, ignore_errors=True)


def gen(key, tries=3, strict=False, force=False, bestof=1):
    key, prompt, out = build(key, strict)
    if not force and os.path.exists(out) and os.path.getsize(out) > 20000:
        return key, True, "已存在，跳过"
    msg = ""
    for attempt in range(tries):
        _throttle()
        if bestof > 1:
            ok, msg = _run_bestof(prompt, out, bestof)
        else:
            ok, msg = _run_once(prompt, out)
        if ok:
            return key, True, msg if bestof > 1 else "%.0f KB" % (os.path.getsize(out) / 1024.0)
        if "sensitivity filter" in msg or "RPM" in msg:
            wait = _slow_down()                 # 限流：全局限速减半 + 退避
            time.sleep(wait)
            continue
        break                                    # 参数类错误重试没意义
    return key, False, msg[-260:].replace("\n", " ")


# ------------------------------------------------------------------ 表：从 pvz.c 抽取
_PT_ORDER = ["PT_SUNFLOWER", "PT_PEASHOOTER", "PT_WALLNUT", "PT_POTATOMINE",
             "PT_SNOWPEA", "PT_REPEATER", "PT_CHERRY", "PT_JALAPENO",
             "PT_THREEPEATER", "PT_SPIKEWEED", "PT_MAGNET", "PT_KERNELPULT",
             "PT_STARFRUIT", "PT_CACTUS", "PT_SPLITPEA", "PT_REED", "PT_BLOOMERANG",
             "PT_FUME", "PT_LASERBEAN", "PT_MELONPULT", "PT_WINTERMELON", "PT_GLOOM",
             "PT_GOLDMAGNET", "PT_TWINFLOWER", "PT_MARROW", "PT_PUMPKIN", "PT_GARLIC",
             "PT_HYPNOSHROOM", "PT_ICESHROOM", "PT_TANGLEKELP", "PT_COBCANNON",
             "PT_CATTAIL", "PT_HERO_FLAME", "PT_HERO_IRONNUT", "PT_HERO_CRYSTAL",
             "PT_HERO_THORNVINE", "PT_HERO_SUNGOD", "PT_HERO_FROST",
             "PT_HERO_WORLDTREE", "PT_HERO_DOOM"]
_ZT_ORDER = ["ZT_NORMAL", "ZT_CONE", "ZT_BUCKET", "ZT_FLAG", "ZT_DANCER",
             "ZT_ZAMBONI", "ZT_BALLOON", "ZT_VAULTER", "ZT_SCREENDOOR",
             "ZT_FOOTBALL", "ZT_NEWSPAPER", "ZT_DIGGER", "ZT_GIANT"]
_TIER = {"RGQ_COMMON": 0, "RGQ_RARE": 1, "RGQ_EPIC": 2,
         "RGQ_LEGEND": 3, "RGQ_MYTH": 4, "RGQ_ULTRA": 5, "RGQ_HALL": 6,
         "RGQ_PRIMORDIAL": 7}

# ---- 第三批基座（108~157 用的那 50 个）：**追加到末尾** ----
# 为什么必须追加而不是插入：P_BASE 是按**索引**取身形描述的，
# 插到中间会让所有旧基座的索引整体后移 → 已生成的 108 张旧图对应的提示词
# 会全部指向错误的身形描述。追加则旧索引 0..39 逐字不变，旧提示词一个字节不差。
try:
    import _cards_new as _CN_BASE
    for _idx, _card, _prim in _CN_BASE.all_cards():
        _b = _card["base"]
        if _b not in _PT_ORDER:
            _PT_ORDER.append(_b)
            P_BASE[len(_PT_ORDER) - 1] = _CN_BASE.base_proto(_b)
    del _idx, _card, _prim, _b
except Exception as _e:                       # noqa: BLE001
    print("[warn] 未能合并第三批基座原型：%s" % _e)

# rgPlants[] 的期望行数。改它必须同时确认三处：
#   ① pvz.c 的 RG_PLANT_N；② 这里的行数断言；③ _build_units.py 的 MAX_PX 够不够装。
# ⚠️ 100 -> 105 是「殿堂五尊」上线时改的。只改 pvz.c 不改这里，
#    症状是 P100~P104 拿不到 tier/base，build() 直接 None 崩掉。
# ⚠️ 158 = 108 + 20 张殿堂 + 30 张始祖（2026-09-21 第三批）。
#    这个数字必须和 pvz.c 的 RG_PLANT_N 一致，否则 load_tables() 直接退出。
RG_PLANTS_N = 158
RG_ZOMBIES_N = 70


def load_tables():
    """从 pvz.c 的 rgPlants[] / rgZombies[] 抽 tier 与 base，避免两份清单手工同步。

    注意 PT_* / ZT_* 是 enum 不是 #define，值本身取不到；
    但既然基座是「行为 + 贴图」的键，只要名字能映射到序号即可，
    所以这里直接按枚举声明顺序建索引表。
    """
    import re
    src = open(os.path.join(ROOT, "pvz.c"), encoding="utf-8", errors="replace").read()

    def grab(tbl_name, n, tier_d, base_d, order):
        m = re.search(re.escape(tbl_name) + r"\[.*?\]\s*=\s*\{(.*?)\n\};", src, re.S)
        if not m:
            raise SystemExit("找不到表 " + tbl_name)
        rows = re.findall(r"\{\s*L\"[^\"]*\",\s*(RGQ_\w+),\s*(\w+),", m.group(1))
        for i, (t, b) in enumerate(rows):
            if t not in _TIER:
                raise SystemExit("未知品质 %s（行 %d）" % (t, i))
            if b not in order:
                raise SystemExit("未知基座 %s（行 %d）" % (b, i))
            tier_d[i] = _TIER[t]
            base_d[i] = order.index(b)
        return len(rows)

    pt, pb = {}, {}
    zt, zb = {}, {}
    np_ = grab("rgPlants", RG_PLANTS_N, pt, pb, _PT_ORDER)
    nz = grab("rgZombies", RG_ZOMBIES_N, zt, zb, _ZT_ORDER)
    if np_ != RG_PLANTS_N or nz != RG_ZOMBIES_N:
        raise SystemExit("表行数不对：plants=%d（应 %d，殿堂五尊上线后从 100 扩容）"
                         " zombies=%d（应 %d，第三批上线后从 50 扩容）"
                         % (np_, RG_PLANTS_N, nz, RG_ZOMBIES_N))
    return pt, pb, zt, zb


P_TIER, P_BASEIDX, Z_TIER, Z_BASEIDX = load_tables()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keys", nargs="*", help="指定单位，如 P07 Z31；不给则全部")
    ap.add_argument("--plant", action="store_true", help="只生成植物")
    ap.add_argument("--zombie", action="store_true", help="只生成僵尸")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--list", action="store_true", help="只打印提示词，不出图")
    ap.add_argument("--strict", action="store_true",
                    help="加强背景约束（重生成背景不纯的图时用）")
    ap.add_argument("--force", action="store_true",
                    help="已存在也重新生成（配合 --strict 用）")
    ap.add_argument("--bestof", type=int, default=1,
                    help="一次出 N 张候选、按背景纯度挑最好的（难搞的图用 3~4）")
    args = ap.parse_args()

    # 默认全量：已存在的图会被 gen() 跳过，所以裸跑只会补缺的。
    # 这里用真实行数而不是写死的 50，免得新增批次后"裸跑一遍"漏掉它们。
    keys = args.keys or (["P%02d" % i for i in range(RG_PLANTS_N)] +
                         ["Z%02d" % i for i in range(RG_ZOMBIES_N)])
    if args.plant:
        keys = [k for k in keys if k[0] == "P"]
    if args.zombie:
        keys = [k for k in keys if k[0] == "Z"]

    if args.list:
        for k in keys:
            _, p, _ = build(k, args.strict)
            print("== %s ==\n%s\n" % (k, p))
        return

    os.makedirs(OUT, exist_ok=True)
    lock = threading.Lock()
    done = [0]
    bad = []
    print("生成 %d 张，并发 %d%s%s ..." % (len(keys), args.jobs,
                                        "，严格背景" if args.strict else "",
                                        "，best-of-%d" % args.bestof if args.bestof > 1 else ""))
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(gen, k, 3, args.strict, args.force, args.bestof): k
                for k in keys}
        for f in as_completed(futs):
            try:
                k, ok, msg = f.result()
            except Exception as e:                    # noqa: BLE001
                k, ok, msg = futs[f], False, repr(e)[:180]
            with lock:
                done[0] += 1
                print("  [%3d/%3d] %s %s %s" % (done[0], len(keys), k,
                                                "OK " if ok else "X  ", msg))
                if not ok:
                    bad.append((k, msg))
    print("\n完成 %d / %d，失败 %d" % (len(keys) - len(bad), len(keys), len(bad)))
    for k, m in bad:
        print("  %s : %s" % (k, m))


if __name__ == "__main__":
    main()
