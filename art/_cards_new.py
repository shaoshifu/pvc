# -*- coding: utf-8 -*-
"""新增 50 张殿堂/始祖卡牌的**单一数据源**。

改这里一处，_patch_cards.py（写进 pvz.c）与 art/_gen_units.py（出图 P_HOOK）
都从这里取，避免两份清单手工同步走样。

设计依据（调研过的成熟机制）：
  · 杀戮尖塔 / 小丑牌：卡牌要有「流派核心」，单卡强不如成套强 → 每张殿堂卡
    围绕一个流派给 8~12 条能力位，抽到就能起一套；
  · 吸血鬼幸存者：武器随击杀成长 → 始祖的 GROWEAT / CONVERT 常驻；
  · PvZ 原作：功能位划分（输出/坦克/控制/经济）→ 每张卡都锚定一个功能位；
  · 平衡原则：殿堂 = 流派核心（100× 伤害基准），始祖 = 流派终局（4000× 基准，
    即殿堂的 40 倍，满足"至少 5 倍"的硬要求）。

始祖的差异化方式（它们都拿 RG_ALL_TRAITS，能力完全一样）：
  ① **基座不同** → 在本引擎里基座决定攻击形态（齐射 / 穿刺 / 溅射 / 脉冲 /
     经济 / 磁力 / 召唤…），30 张各不相同，实际打法差异很大；
  ② **aspect（本卡专属强度系数）** → 新加的字段，只对新卡生效，
     默认 100 = 基准。始祖取 100~150，于是 30 张内部也有强弱梯度，
     而且**最低一档仍是 4000×，是殿堂的 40 倍**，不破坏硬要求。
"""

# 殿堂级：流派核心。cost 统一 500（与现有殿堂五尊一致）。
# 始祖级：流派终局。cost 统一 10000（始祖是唯一不免费的档位）。
HALL_COST = 500
PRIM_COST = 10000

# ---------------------------------------------------------------------------
# 20 张殿堂级（索引 108~127）
# 字段：名称、基座、流派、能力位列表、描述、视觉钩子（给出图用）
# ---------------------------------------------------------------------------
HALL = [
    dict(name=u"裂空弹幕", base="PT_STARSHOOTER", aspect=116, school=u"弹幕",
         traits=["MULTILANE", "PETALBARRAGE", "BOUNCE5", "CRIT", "MARK",
                 "OVERLOAD", "RHYTHM", "KNOCKBACK"],
         desc=u"九向弹幕铺满整行，命中不断叠加攻速；暴击与标记让后续每一发都更痛",
         hook="bullet-hell theme: nine thin seed-barrels fanned out around its head as a "
              "circular volley, a visible spiral of glowing seed projectiles orbiting it, "
              "each projectile leaving a bright streak"),

    dict(name=u"万钧重奏", base="PT_QUARTZKERNEL", aspect=116, school=u"重火力",
         traits=["MULTILANE", "CHAINBOOM", "ARMORBREAK", "CRIT", "GRAVITY",
                 "KNOCKBACK", "LASTSTAND", "AURABUFF"],
         desc=u"超重石英弹齐射，落点二段连锁引爆；正面破甲并把整列僵尸持续向后压",
         hook="heavy-artillery theme: an oversized faceted crystal shell braced by steel "
              "trunnions and a thick recoil piston, glowing crack lines across the crystal, "
              "muzzle shockwave rings"),

    dict(name=u"荒芜毒沼", base="PT_POISNOVINE", aspect=118, school=u"毒蚀",
         traits=["SLOWFIELD", "CORRODE", "PLAGUE", "CURSE", "INFECTTURN",
                 "LIFESTEAL", "AURABUFF", "MARK"],
         desc=u"脚下自动铺开腐蚀泥沼，中毒会传染并持续叠伤；伤者掉的血补给自己与队友",
         hook="plague-swamp theme: a bubbling toxic bog spreading around its roots with "
              "skull-shaped gas bubbles, sickly yellow-green pustules on its vines, "
              "contagion spores drifting outward"),

    dict(name=u"寂灭寒渊", base="PT_ABSOLUTEZERO", aspect=116, school=u"冻结封锁",
         traits=["SLOWFIELD", "TIMEFREEZE", "ICERESOURCE", "WEBSLOW", "GRAVITY",
                 "LULLABY", "CONFUSE", "AURABUFF"],
         desc=u"半径内一切冻结并陷入沉睡，被冻住的僵尸不断碎裂成阳光与金气",
         hook="absolute-zero theme: a core of blue-white ice crystals radiating cold fog, "
              "a thin sheet of spreading permafrost under it, frozen air shards hanging "
              "in a ring, deep turquoise glow"),

    dict(name=u"熔心裂爆", base="PT_FIREIMP", aspect=118, school=u"爆炸",
         traits=["CHAINBOOM", "SELFDESTRUCT", "CURSE", "CRIT", "GAMBLEDMG",
                 "LASTSTAND", "AURABUFF", "KNOCKBACK"],
         desc=u"每次攻击都是爆炸判定，炸死的会连锁再爆；血量越低引爆半径与伤害越夸张",
         hook="detonation-core theme: a cracked molten heart-shaped core exposed in its "
              "chest, orange-white blast light leaking through the cracks, ash and ember "
              "particles, scorched black shell plates"),

    dict(name=u"噬魂血祭", base="PT_SOULREAPER", aspect=118, school=u"吸血流",
         traits=["LIFESTEAL", "EXECUTE", "LASTSTAND", "TIMEHEAL", "CONVERT",
                 "AURABUFF", "MARK", "CRIT"],
         desc=u"斩杀残血僵尸并把灵魂转为己用：濒死队友被拉回，全队持续回血",
         hook="soul-harvest theme: a curved obsidian reaping blade grown from its stalk, "
              "thin crimson soul threads flowing from the blade into its own body, "
              "small floating will-o-wisp souls around it"),

    dict(name=u"千机蜂巢", base="PT_SUMMONERSHAMAN", aspect=108, school=u"召唤",
         traits=["SUMMON", "MULTILANE", "PETALBARRAGE", "AURABUFF", "SHIELDALLY",
                 "ORBITAL", "MARK", "CRIT"],
         desc=u"不断召出可独立索敌的蜂群，蜂群与本体共享光环与护盾，越打越多",
         hook="hive-swarm theme: a honeycomb lattice fused into its body with warm amber "
              "glow in every hexagon cell, a swirling swarm of small cartoon bees around "
              "it, dripping honey, broken wax seals"),

    dict(name=u"圣辉共鸣", base="PT_BELLFLOWER", aspect=108, school=u"光环支援",
         traits=["AURABUFF", "SHIELDALLY", "TIMEHEAL", "ENERGYGAIN", "SHARESUN",
                 "CHI", "PURGE", "MARK"],
         desc=u"自身不还手，但全队增伤、减伤、回血、产阳光与金气；并定期净化负面状态",
         hook="divine-resonance theme: a cluster of polished golden bells as petals, "
              "concentric holy light rings pulsing outward, thin choirs of glowing sound "
              "waves, a soft white halo above it"),

    dict(name=u"荆棘王铠", base="PT_BOUNCETHORN", aspect=112, school=u"反击",
         traits=["MIRROR", "FAKEHP", "LASTSTAND", "DODGE", "SHIELDALLY",
                 "PURGE", "TIMEHEAL", "AURABUFF"],
         desc=u"承受的伤害按比例原样奉还，荆棘外壳会自行修复；濒死时反弹效果翻倍",
         hook="thorn-mail theme: layered interlocking thorn armour plates over a heavy "
              "round body, sharp steel-blue spikes bristling outward, a few bent broken "
              "spikes stuck in the armour showing past blows"),

    dict(name=u"时砂回廊", base="PT_CHRONODISTORTER", aspect=112, school=u"时间",
         traits=["TIMEFREEZE", "REWIND", "SLOWFIELD", "STEPMOVE", "DODGE",
                 "CONVERT", "LULLABY", "AURABUFF"],
         desc=u"每格推进一次时间：命中的僵尸被倒回原位并冻结，己方则被加速",
         hook="chrono-corridor theme: a segmented hourglass spine through its middle with "
              "sand streaming upward, broken clock hands orbiting it, faint afterimages "
              "trailing behind its own motion"),

    dict(name=u"命运赌局", base="PT_LUCKYROULETTE", aspect=102, school=u"概率",
         traits=["GAMBLEDMG", "GAMBLERISK", "DROPCARD", "DRAWCARD", "CRIT",
                 "PIXELCRUSH", "ENERGYGAIN", "LASTSTAND"],
         desc=u"每发都是赌注：要么十倍暴击要么空枪；但命中就有概率直接掉出一张卡",
         hook="casino theme: three rotating dice set into its body, a fanned hand of blank "
              "gold-backed playing cards, casino chips and star sparkles, a slot-machine "
              "lever growing from its stalk"),

    dict(name=u"逐星猎标", base="PT_EAGLEEYESHOOTER", aspect=114, school=u"标记集火",
         traits=["MARK", "EXECUTE", "ARMORBREAK", "CRIT", "LASERCUT",
                 "ORBITAL", "OVERLOAD", "AURABUFF"],
         desc=u"先标记最痛的目标，随后全队对它破甲、穿盾并追加处决伤害",
         hook="hunter-mark theme: a long rifled barrel with an engraved scope and brass "
              "sights, a glowing red targeting reticle projected in the air ahead of it, "
              "small crosshair runes"),

    dict(name=u"不动碉堡", base="PT_ENGINEERWALNUT", aspect=112, school=u"护盾坦克",
         traits=["MAZEWALL", "FAKEHP", "MIRROR", "SHIELDALLY", "DIGITRAIN",
                 "LASTSTAND", "TIMEHEAL", "PURGE"],
         desc=u"把自己变成一堵带护盾力场的工事，替整行吸收伤害并持续自我修复",
         hook="bunker-fortress theme: thick riveted armour plating bolted over a round "
              "nut body with a hexagonal energy shield dome flickering above it, sandbags "
              "and steel reinforcement bars around its base"),

    dict(name=u"万象磁枢", base="PT_GRAVITYCONTROLLER", aspect=114, school=u"磁力控制",
         traits=["PULL", "KNOCKBACK", "GRAVITY", "CONFUSE", "HIJACK",
                 "ICERESOURCE", "SLOWFIELD", "AURABUFF"],
         desc=u"把全场僵尸来回拽动、击退与致眩；重力井把它们叠成一堆再一起碾碎",
         hook="magnet-core theme: a large horseshoe magnet fused into its head with "
              "copper coil windings, arcing blue-white electricity between the poles, "
              "iron filings and bolts orbiting it in rings"),

    dict(name=u"蜕变之种", base="PT_GENESISFLOWER", aspect=100, school=u"成长",
         traits=["GROWEAT", "COPYEAT", "FUSEPLANT", "CONVERT", "LIFESTEAL",
                 "ENERGYGAIN", "AURABUFF", "TIMEHEAL"],
         desc=u"每吃掉一只僵尸就永久变大变强，还会吞掉旁边植物学走它的本事",
         hook="evolution-seed theme: a translucent egg-like seed pod on its stalk with a "
              "faint embryonic plant silhouette glowing inside, shedding cracked smaller "
              "seed shells around its base, spiral growth rings"),

    dict(name=u"镜裂分身", base="PT_MIRRORFERN", aspect=114, school=u"分裂镜像",
         traits=["MIRROR", "DOUBLEBODY", "RECOMBINE", "BLINK", "DODGE",
                 "FAKEHP", "SUMMON", "CRIT"],
         desc=u"分出多个可独立承受伤害的镜像；被打破时会在别处重新聚合再炸一次",
         hook="mirror-clone theme: polished mirror-chrome fronds, three translucent "
              "duplicate copies of itself offset behind, sharp glass shards floating "
              "around, prismatic light refractions"),

    dict(name=u"虚影之息", base="PT_DREAMCAP", aspect=106, school=u"相位闪避",
         traits=["DODGE", "FAKEHP", "BLINK", "MEMESPREAD", "PURGE",
                 "TIMEHEAL", "LASTSTAND", "CONFUSE"],
         desc=u"本体在相位之间闪烁，绝大多数攻击穿过它打空；被命中反而扩散减速孢子",
         hook="phase-shift theme: its body semi-transparent with part of it dissolved into "
              "drifting purple mist, a dreamlike double exposure effect, soft lavender "
              "spores shedding from its cap"),

    dict(name=u"孤注终章", base="PT_PHENIXFLAME", aspect=120, school=u"绝境爆发",
         traits=["LASTSTAND", "ASSASSIN", "EXECUTE", "SELFDESTRUCT", "CRIT",
                 "GAMBLEDMG", "LIFESTEAL", "AURABUFF"],
         desc=u"越是濒死越强，血量见底时全身燃起白焰、伤害与斩杀线同时拉满",
         hook="last-stand phoenix theme: ragged phoenix wing-feathers of living white-gold "
              "flame, its own body cracking apart and burning from the inside, glowing "
              "ember trail rising upward"),

    dict(name=u"深空观测", base="PT_HOLOGRAMPROJECTOR", aspect=116, school=u"轨道打击",
         traits=["ORBITAL", "LASERCUT", "MULTILANE", "MARK", "GRAVITY",
                 "ARMORBREAK", "CRIT", "OVERLOAD"],
         desc=u"锁定僵尸最密集处，从天上连续投下贯穿光束；护甲在它面前不存在",
         hook="orbital-strike theme: a holographic targeting grid and globe projector lens "
              "on its head, thin vertical light columns descending onto the ground ahead, "
              "scanning rings and telemetry sparkles"),

    dict(name=u"千面棱彩", base="PT_KALEIDOSCOPEFLOWER", aspect=120, school=u"全能随机",
         traits=["GAMBLEDMG", "DROPCARD", "DRAWCARD", "COPYEAT", "FUSEPLANT",
                 "SUMMON", "BLINK", "MEMESPREAD"],
         desc=u"每回合随机复制全场任意一株的能力，样样都会一点，但什么都做不精",
         hook="kaleidoscope theme: a faceted prismatic flower head with mirrored inner "
              "petals showing fragments of other plants inside it, rainbow refracted light "
              "beams, rotating geometric shards"),
]

# ---------------------------------------------------------------------------
# 30 张始祖级（索引 128~157）
# 命名规则：创世意象词（2 字）+ 功能词（2 字），与「始源棘原 / 太初坚壁 / 混沌齐射」同构。
# aspect：本卡专属强度系数（100 = 基准）。范围 100~150，做成明显的内部梯度。
# ---------------------------------------------------------------------------
PRIM = [
    dict(name=u"鸿蒙母树", base="PT_ECHOBAMBOO", aspect=150, school=u"成长·终局",
         desc=u"一切生命的起点。每击杀一次就永久加粗一圈，最终长成全屏不可摧毁的母株",
         hook="primordial mother-tree theme: a colossal world-tree trunk growing through "
              "its own body with spiral creation-age bark grooves, glowing green sap "
              "channels, tiny new saplings sprouting from its roots"),

    dict(name=u"无极星渊", base="PT_NEBULABEET", aspect=148, school=u"范围·终局",
         desc=u"自身即一片星渊。攻击范围覆盖全屏，落入其中的僵尸同时被吸扯与灼烧",
         hook="infinite star-abyss theme: a swirling purple nebula disk embedded in its "
              "body with pinprick stars, a dark accretion core, thin light jets shooting "
              "from both poles"),

    dict(name=u"归墟之喉", base="PT_VOIDDEVOURER", aspect=147, school=u"吞噬·终局",
         desc=u"张开就是深渊。吞掉进入范围的一切——僵尸、子弹、护甲，转化成自身与阳光",
         hook="void-maw theme: an enormous circular void mouth with a ring of inward-"
              "pointing teeth, everything drawn toward the dark centre, faint swallowed "
              "silhouettes inside"),

    dict(name=u"万象轮盘", base="PT_VORTEXTURNIP", aspect=145, school=u"随机·终局",
         desc=u"每回合重掷全部能力，掷出什么就打什么；期望值高得离谱，下限也低得离谱",
         hook="cosmic-wheel theme: a huge rotating wheel of glowing runes embedded around "
              "its body, spinning vortex rings, betting-chip shapes orbiting it"),

    dict(name=u"烛照晶簇", base="PT_CRYSTALSHOOTER", aspect=144, school=u"破甲·终局",
         desc=u"创世之光凝成晶体。射出的每一发都是穿甲贯穿，无视一切护具与减伤",
         hook="primeval-light crystal theme: a cluster of giant faceted crystals growing "
              "from its base, pure white-gold light radiating from inside the largest "
              "crystal, prismatic rainbow caustics"),

    dict(name=u"天枢雷霆", base="PT_THUNDEROVERLORD", aspect=143, school=u"雷电·终局",
         desc=u"天枢垂落的雷柱不断砸落，命中即麻痹；雷击会在僵尸之间无限跳跃",
         hook="celestial-thunder theme: a towering storm-god silhouette with a crown of "
              "levitating thunder stones, thick branching lightning bolts chained to the "
              "ground, dark storm clouds around its head"),

    dict(name=u"地脉根须", base="PT_THORFLOWER", aspect=142, school=u"地面·终局",
         desc=u"根系扎穿整个地面。全屏处处是根刺，任何移动都会触发一次撕裂",
         hook="earth-root-network theme: thick gnarled roots erupting from the ground in "
              "every direction, glowing amber earth-vein seams across its own body, "
              "sharp thorns on every root"),

    dict(name=u"星枢坠幕", base="PT_STARDIVINER", aspect=141, school=u"轨道·终局",
         desc=u"把整片星空拽到地面：持续不断的星陨覆盖全屏，敌我皆伤唯独它自己无恙",
         hook="star-fall-curtain theme: a curtain of falling glowing meteors streaming down "
              "around it, a star-chart constellation etched in gold on its own body, "
              "orbital rings with tiny planets"),

    dict(name=u"岁渊之钟", base="PT_CLOCKSPROUT", aspect=140, school=u"时间·终局",
         desc=u"时间在它手里是资源。全场僵尸被反复倒回与冻结，己方攻速被拉到极限",
         hook="time-abyss clock theme: an ancient bronze clock face growing as its head, "
              "gears and escapement springs exposed, hands that spin through ages, "
              "sand swirling in a vortex"),

    dict(name=u"墟核裂解", base="PT_LANTERNMELON", aspect=139, school=u"轰炸·终局",
         desc=u"每发都是创世级的裂解弹。落点整屏连锁引爆，被炸死的会继续炸",
         hook="core-fission theme: a split-open molten reactor core in its chest with "
              "white-hot plasma leaking out, chained blast waves rippling the air, "
              "blackened ceramic plating"),

    dict(name=u"玄黄母岩", base="PT_CORALGUARD", aspect=138, school=u"坦克·终局",
         desc=u"天地初开的原石。血量高到不可摧毁，并把承受的伤害加倍奉还给攻击者",
         hook="primordial-bedrock theme: a mountain-like layered rock body with gold ore "
              "veins, moss and ancient fossil imprints on its surface, a soft glowing "
              "earth-mother aura"),

    dict(name=u"太一独尊", base="PT_MARROW", aspect=137, school=u"单体·终局",
         desc=u"所有力量凝于一株。它的每一击都自动打出最高档伤害，从不失手也从无浮动",
         hook="supreme-one theme: a statue-like impeccable white marble body with seamless "
              "gold inlay seams, a single crown-like halo, absolutely symmetrical armour "
              "plates, nothing superfluous"),

    dict(name=u"阴阳双生", base="PT_MIRRORSUNFLOWER", aspect=136, school=u"双形态·终局",
         desc=u"同时存在两株：一株专司爆发、一株专司封锁，被打掉一株另一株立刻补上",
         hook="yin-yang-twins theme: two mirrored halves fused into one plant, one side "
              "blazing white-hot and the other deep frozen blue, a thin boundary line "
              "between them, both halves visibly independent"),

    dict(name=u"轮回祭坛", base="PT_MOONLIGHTPRIESTESS", aspect=135, school=u"复生·终局",
         desc=u"被摧毁的植物会从祭坛上重新长出；它自己倒下时会带着半场僵尸一起走",
         hook="reincarnation-altar theme: a circular moon-lit stone altar built around its "
              "base with engraved prayer rings, silver lunar glyphs floating, a faint "
              "ghostly second copy of itself standing inside the altar"),

    dict(name=u"天罡战鼓", base="PT_SPORETANK", aspect=134, school=u"光环·终局",
         desc=u"战鼓一响全队攻速与伤害同时拉满，鼓点越密加成越高，直到把整条线推平",
         hook="war-drum theme: a massive taut war drum fused into its torso with hide "
              "lashings and bronze studs, visible shockwave rings beating outward, "
              "war-banner tassels"),

    dict(name=u"紫微帝庭", base="PT_SEDUCTIONQUEEN", aspect=133, school=u"统御·终局",
         desc=u"在场即统御。全场僵尸被强制拉到它面前排队，任何反抗都被立刻压制",
         hook="celestial-court theme: an imperial throne-like crown of purple crystal on "
              "its head, a long regal cape of woven light, kneeling ghost figures drawn "
              "toward it, gold throne ornaments"),

    dict(name=u"沧溟潮涌", base="PT_MONSOONREED", aspect=132, school=u"洪流·终局",
         desc=u"掀起吞没一切的海啸，把整行僵尸往后推并淹死；潮水还会冲刷掉负面状态",
         hook="primordial-flood theme: a cresting wave of glowing deep-blue water forming "
              "behind it, white foam and driftwood debris, its own stalks like reeds "
              "bending in the surge, water spray"),

    dict(name=u"乾元鼎炉", base="PT_BUBBLECANNON", aspect=131, school=u"灼烧·终局",
         desc=u"一座创世熔炉。喷出的每一团都是可以持续燃烧整屏的白焰",
         hook="creation-furnace theme: a three-legged bronze cauldron forged into its body "
              "with white flame pouring out of the mouth, molten metal runoff, pressed "
              "eight-trigram seal patterns"),

    dict(name=u"涅槃红莲", base="PT_FLASHBERRY", aspect=130, school=u"自爆·终局",
         desc=u"一次性点燃整屏，随后从灰烬里满血重生；每次重生都比上一次更猛烈",
         hook="nirvana-lotus theme: a fully bloomed crimson lotus sitting in a bed of "
              "white-hot ash, petals of living flame, a half-reborn bud rising from the "
              "ashes under it"),

    dict(name=u"烛龙吐息", base="PT_DRAGONBREATHVINE", aspect=129, school=u"范围灼烧·终局",
         desc=u"一口吐息烧穿整条战线，火焰会沿地面蔓延并长期停留",
         hook="primordial-dragon theme: a sinuous dragon head grown as its flower with "
              "glowing whiskers and antler-horns, a long jet of white-gold fire from its "
              "mouth, molten ember drifts"),

    dict(name=u"羲和耀斑", base="PT_DEMOLITIONEXPERT", aspect=128, school=u"太阳·终局",
         desc=u"自身就是太阳。全屏被持续照射灼烧，僵尸的护具在它面前直接汽化",
         hook="sun-flare theme: an actual miniature sun corona blazing above its head with "
              "prominence loops, blinding white-gold core, solar wind particles streaming "
              "outward, heat haze"),

    dict(name=u"望舒冰轮", base="PT_FROSTQUEEN", aspect=127, school=u"冰封·终局",
         desc=u"月轮悬空，全场进入永冬。被冻住的僵尸直接变成冰雕资源",
         hook="moon-ice theme: a large silver moon disc hovering over its head, glacier "
              "ice shelves growing under it, sharp ice spears radiating outward, "
              "deep blue-white mist, frozen breath"),

    dict(name=u"共工怒涛", base="PT_BUBBLELOTUS", aspect=126, school=u"水系·终局",
         desc=u"把水压成炮。每一击都在僵尸之间水锤传导，护甲被水压从内部顶开",
         hook="rage-tide theme: high-pressure water jets blasting from every petal, a "
              "swirling column of water around its body, cracked stone pillars toppled "
              "beside it, dense mist"),

    dict(name=u"盘古开天", base="PT_STEAMPEPPER", aspect=125, school=u"开局·终局",
         desc=u"开局直接改写战场：背景与音乐永久转为始祖，我方全体起始强度翻倍",
         hook="world-cleaving theme: a colossal double-edged stone axe embedded beside it, "
              "a vertical seam of blinding light splitting the ground beneath, dense "
              "creation-age mist and starfield above"),

    dict(name=u"女娲补天", base="PT_HEALPETAL", aspect=124, school=u"治愈·终局",
         desc=u"把所有损伤都补回来。全队持续满血、免疫负面状态，被摧毁的植物原地复原",
         hook="mending-heaven theme: five glowing differently-coloured sacred stones "
              "orbiting it, a web of luminous repair threads stitching cracks in the air "
              "around it, soft pink-white radiance"),

    dict(name=u"后土承载", base="PT_WOODGUARDIAN", aspect=123, school=u"守护·终局",
         desc=u"大地本身替你挡伤害。整条线路获得护盾力场，任何攻击先由它承受",
         hook="earth-bearer theme: a massive terracotta guardian shell with ancient "
              "earthenware cracks filled with gold, a translucent dome of earth-light "
              "over it, floating soil and stone rings"),

    dict(name=u"祝融焚天", base="PT_CANDLESPROUT", aspect=122, school=u"烈焰·终局",
         desc=u"火焰不再熄灭。全场持续燃烧，火焰会永久留在地面并不断扩散",
         hook="fire-god theme: an ancestral fire-god mask burning as its flower with "
              "molten gold cracks, eternal unquenchable flames along the whole stem, "
              "fire tornado wisps"),

    dict(name=u"玄冥幽狱", base="PT_DUSKORCHID", aspect=121, school=u"黑暗·终局",
         desc=u"把战线拖入永夜。全场视野被压缩，僵尸在其中持续失血并失去方向",
         hook="nether-prison theme: a black orchid blooming in absolute darkness with "
              "faint violet edge-glow, glowing chained spirit hands reaching up from the "
              "ground, cold fog rolling out"),

    dict(name=u"句芒春律", base="PT_COMETCLOVER", aspect=120, school=u"生机·终局",
         desc=u"生命自己会赢。全队攻击频率与阳光产出同时被拉到极值，负面状态每秒净化",
         hook="spring-deity theme: a verdant antlered bird-spirit crown on its head, "
              "vines and blossoms erupting in every direction, warm sunlight beams from "
              "above, a swirling green growth aura"),

    dict(name=u"蓐收金秋", base="PT_COINSPROUT", aspect=100, school=u"经济·终局",
         desc=u"开局即是终局经济：每击杀产出阳光与金气，越打越有钱，用金币直接碾过去",
         hook="autumn-harvest theme: a gilded harvest crown of wheat sheaves and hanging "
              "gold coins, an overflowing cornucopia tied to its stalk, golden leaf "
              "drifts, warm amber light"),
]


# ---------------------------------------------------------------------------
# 新基座的**身形原型**（出图用）。
# 为什么单独写一份：art/_gen_units.py 的 P_BASE 是"基座序号 → 一段身形描述"，
# 序号来自它自己那份 _PT_ORDER。新卡用了 50 个新基座，必须补上对应描述，
# 否则出图管线直接 "未知基座" 退出。
# 这里按**基座名**给，避免依赖序号（序号是那个脚本的内部约定）。
# ---------------------------------------------------------------------------
BASE_PROTO = {
    # ---- 殿堂 20 张 ----
    "PT_STARSHOOTER": u"a star-shaped shooter plant with a pointed crystal-star head and a ring of small seed nozzles around it",
    "PT_QUARTZKERNEL": u"a chunk of faceted translucent quartz crystal mounted on a steel trunnion and recoil piston, small kernel pods on its sides",
    "PT_POISNOVINE": u"a creeping toxic vine with bloated pustule leaves dripping corrosive sap onto a bubbling swamp floor",
    "PT_ABSOLUTEZERO": u"a squat core of blue-white ice crystals with permafrost spreading under it and frozen air shards hovering in a ring",
    "PT_FIREIMP": u"a small imp-like plant with a cracked molten heart core exposed in its chest and scorched black shell plates",
    "PT_SOULREAPER": u"a hooded reaper-like plant with a curved obsidian reaping blade grown from its stalk and thin crimson soul threads",
    "PT_SUMMONERSHAMAN": u"a shaman plant wearing a bone-feather headdress with a honeycomb lattice fused into its torso, small cartoon bees swarming it",
    "PT_BELLFLOWER": u"a flower whose petals are polished golden bells on a slender stalk, with a soft halo above it",
    "PT_BOUNCETHORN": u"a heavy round body clad in layered interlocking thorn armour plates with steel-blue spikes bristling outward",
    "PT_CHRONODISTORTER": u"a plant built around a segmented hourglass spine, broken clock hands orbiting it and sand streaming upward",
    "PT_LUCKYROULETTE": u"a plant with three rotating dice set into its body, a fanned hand of gold-backed playing cards and a slot lever on its stalk",
    "PT_EAGLEEYESHOOTER": u"a long rifled shooter plant with an engraved scope, brass sights and a glowing red targeting reticle in the air ahead",
    "PT_ENGINEERWALNUT": u"a riveted armour-plated round nut with a hexagonal energy shield dome above it and sandbags at its base",
    "PT_GRAVITYCONTROLLER": u"a floating plant with a purple gravity core, concentric warped-air ripples and orbiting rocks in a ring",
    "PT_GENESISFLOWER": u"a translucent egg-like seed pod on a stalk with a glowing embryonic plant silhouette inside and cracked smaller seed shells at its base",
    "PT_MIRRORFERN": u"a fern of polished mirror-chrome fronds with glass shards floating around it and prismatic refractions",
    "PT_DREAMCAP": u"a soft lavender mushroom whose lower body dissolves into drifting purple mist, shedding glowing spores",
    "PT_PHENIXFLAME": u"a plant crowned with ragged phoenix wing-feathers of living white-gold flame, its shell cracking and burning from inside",
    "PT_HOLOGRAMPROJECTOR": u"a plant with a holographic targeting grid and globe lens on its head projecting thin light columns onto the ground",
    "PT_KALEIDOSCOPEFLOWER": u"a faceted prismatic flower head with mirrored inner petals showing fragments of other plants, rainbow light beams",
    # ---- 始祖 30 张 ----
    "PT_ECHOBAMBOO": u"a colossal bamboo world-tree trunk growing through its body with spiral bark grooves and glowing green sap channels",
    "PT_NEBULABEET": u"a beet-like body with a swirling purple nebula disk embedded in it, pinprick stars and a dark accretion core",
    "PT_VOIDDEVOURER": u"a plant that is mostly an enormous circular void maw with a ring of inward-pointing teeth and a dark centre",
    "PT_VORTEXTURNIP": u"a turnip with a huge rotating wheel of glowing runes embedded around its body and spinning vortex rings",
    "PT_CRYSTALSHOOTER": u"a cluster of giant faceted crystals growing from its base with pure white-gold light radiating from inside the largest",
    "PT_THUNDEROVERLORD": u"a towering storm-god plant with a crown of levitating thunder stones and thick branching lightning chained to the ground",
    "PT_THORFLOWER": u"a flower whose body is overgrown by thick gnarled roots erupting in every direction with glowing amber earth-vein seams",
    "PT_STARDIVINER": u"a plant draped in a curtain of falling glowing meteors with a gold constellation chart etched on its body",
    "PT_CLOCKSPROUT": u"a sprout whose head is an ancient bronze clock face with exposed gears and hands that spin through ages",
    "PT_LANTERNMELON": u"a split-open molten reactor melon with white-hot plasma leaking out and chained blast waves rippling the air",
    "PT_CORALGUARD": u"a mountain-like layered rock body with gold ore veins, moss and ancient fossil imprints on its surface",
    "PT_MARROW": u"a statue-like impeccable white marble squash with seamless gold inlay seams and a single crown-like halo",
    "PT_MIRRORSUNFLOWER": u"two mirrored halves fused into one sunflower, one side blazing white-hot and the other deep frozen blue",
    "PT_MOONLIGHTPRIESTESS": u"a moon-lit stone altar built around its base with engraved prayer rings and a faint ghostly copy of itself inside",
    "PT_SPORETANK": u"a massive taut war drum fused into its torso with hide lashings and bronze studs, shockwave rings beating outward",
    "PT_SEDUCTIONQUEEN": u"a regal plant with an imperial throne-like crown of purple crystal and a long cape of woven light",
    "PT_MONSOONREED": u"reeds bending inside a cresting wave of glowing deep-blue water with white foam and driftwood debris",
    "PT_BUBBLECANNON": u"a three-legged bronze cauldron forged into its body with white flame pouring out of its mouth and pressed eight-trigram seals",
    "PT_FLASHBERRY": u"a crimson lotus in full bloom sitting in a bed of white-hot ash with petals of living flame",
    "PT_DRAGONBREATHVINE": u"a sinuous dragon head grown as its flower with glowing whiskers and antler-horns breathing a long jet of white-gold fire",
    "PT_DEMOLITIONEXPERT": u"a plant crowned by an actual miniature sun with prominence loops, a blinding white-gold core and solar wind particles",
    "PT_FROSTQUEEN": u"a large silver moon disc hovering over its head, glacier ice shelves growing under it and ice spears radiating outward",
    "PT_BUBBLELOTUS": u"a lotus blasting high-pressure water jets from every petal inside a swirling column of water with cracked stone pillars beside it",
    "PT_STEAMPEPPER": u"a plant beside a colossal embedded double-edged stone axe with a vertical seam of blinding light splitting the ground beneath",
    "PT_HEALPETAL": u"a flower orbited by five glowing differently-coloured sacred stones and a web of luminous repair threads stitching the air",
    "PT_WOODGUARDIAN": u"a massive terracotta guardian shell with earthenware cracks filled with gold and a translucent dome of earth-light over it",
    "PT_CANDLESPROUT": u"a sprout whose flower is an ancestral fire-god mask burning with molten gold cracks and eternal flames along its stem",
    "PT_DUSKORCHID": u"a black orchid blooming in absolute darkness with faint violet edge-glow and glowing chained spirit hands reaching up from the ground",
    "PT_COMETCLOVER": u"a clover plant crowned with a verdant antlered bird-spirit head, vines and blossoms erupting in every direction",
    "PT_COINSPROUT": u"a sprout crowned with a gilded harvest wreath of wheat sheaves and hanging gold coins over an overflowing cornucopia",
}


def base_proto(name):
    u"""取新基座的身形原型；不在表里就返回 None（交给旧表处理）。"""
    return BASE_PROTO.get(name)


def all_cards():
    """返回 [(全局索引, 字典, 是否始祖)] —— 殿堂从 108 起，始祖从 128 起。"""
    out = []
    for i, c in enumerate(HALL):
        out.append((108 + i, c, False))
    for i, c in enumerate(PRIM):
        out.append((128 + i, c, True))
    return out


if __name__ == "__main__":
    cards = all_cards()
    print(u"殿堂 %d 张（108~%d） / 始祖 %d 张（128~%d） / 共 %d 张"
          % (len(HALL), 107 + len(HALL), len(PRIM), 127 + len(PRIM), len(cards)))
    asp = [c["aspect"] for c in PRIM]
    print(u"始祖 aspect 范围 %d~%d，均值 %.1f" % (min(asp), max(asp), sum(asp) / float(len(asp))))
