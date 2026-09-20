#!/bin/bash
# 批量生成33种植物精灵（每种3个候选）
# 用法：./generate_all_plants.sh

export PATH="/c/nvm4w/nodejs:$PATH"
cd "$(dirname "$0")"

# 植物定义：名称|中文名|prompt关键描述
PLANTS=(
    "sunflower|向日葵|vibrant yellow petals, bright smiling face, green stem with leaves"
    "peashooter|豌豆射手|green pea pod head with large mouth cannon barrel, bright green color, leafy stem"
    "wallnut|坚果墙|large brown walnut shell with cute sleepy face, tough defensive appearance"
    "potatomine|土豆地雷|brown potato character half buried in ground, grumpy face, explosive appearance"
    "snowpea|寒冰射手|ice blue pea pod head with frost cannon, icy cold appearance with snowflakes"
    "repeater|双发射手|green pea plant with double cannon barrels, aggressive look, muscular stem"
    "cherrybomb|樱桃炸弹|two red cherries with angry explosive faces, fuse burning on top, fire energy"
    "jalapeno|火爆辣椒|red hot chili pepper character, flames coming off body, angry spicy face"
    "threepeater|三线射手|green plant with three pea shooter heads in a row, tri-barrel design"
    "spikeweed|地刺|brown spiky plant on ground level, sharp thorns pointing up, menacing look"
    "magnetshroom|磁力菇|purple mushroom with magnetic horseshoe symbol, glowing energy field"
    "kernelpult|玉米投手|corn cob plant with catapult arm, yellow kernels, launching pose"
    "starfruit|杨桃|yellow star-shaped plant with five points, each point shooting, cosmic theme"
    "cactus|仙人掌|green cactus with spiky arms, desert plant, sharp needles, tough appearance"
    "splitpea|裂荚射手|green plant shooting both front and back, split personality, dual cannons"
    "lightningreed|闪电芦苇|blue electric reed plant, lightning bolts, energy crackling, tall thin"
    "bloomerang|回旋镖花|orange boomerang-shaped flower, curved petals, spinning motion blur"
    "fumeshroom|大喷菇|purple mushroom with poison gas clouds, toxic fumes, dark ominous"
    "laserbean|激光豆|high-tech green bean plant, laser beam eye, sci-fi cyberpunk style"
    "melonpult|西瓜投手|green plant with catapult launching watermelons, heavy artillery feel"
    "wintermelon|冰冻西瓜|blue icy watermelon plant catapult, frozen fruit, frost aura"
    "gloomshroom|忧郁菇|dark purple mushroom, shadowy appearance, multiple spore shooting heads"
    "goldmagnet|金盏花|golden sunflower with magnetic powers, shiny metallic petals, treasure hunter"
    "twinflower|双子向日葵|two sunflowers merged together, twin faces, double sun production"
    "marrow|榴莲投手|brown marrow plant with spiky projectile launcher, tough armored look"
    "pumpkin|南瓜护甲|orange pumpkin shell armor piece, protective barrier, hollow interior"
    "garlic|大蒜|white garlic bulb character, stinky aura lines, repelling appearance"
    "hypnoshroom|魅惑菇|pink hypnotic mushroom, swirling spiral eyes, charm magic aura"
    "iceshroom|寒冰菇|bright blue frozen mushroom, icicles, cold mist, freezing power"
    "tanglekelp|缠绕海草|green underwater seaweed, tentacle-like leaves, aquatic plant"
    "cobcannon|玉米加农炮|heavy artillery corn cannon, military camouflage, explosive power"
    "cattail|香蒲|aquatic plant with cattail spike, water plant, dart shooter"
    "hero_flame|火焰英雄|epic flame elemental plant, fire aura, glowing embers, legendary appearance"
)

mkdir -p plants

for entry in "${PLANTS[@]}"; do
    IFS='|' read -r name cname desc <<< "$entry"
    
    echo "========================================="
    echo "正在生成: $cname ($name)"
    echo "========================================="
    
    mmx image generate \
        --prompt "A cute cartoon $name plant character for tower defense game, $desc, hand-painted style, clear black outlines, front view standing pose, bottom-center anchor point, transparent background, 2D game sprite art, high detail, Plants vs Zombies style" \
        --aspect-ratio 1:1 \
        --n 3 \
        --out-dir plants/
    
    sleep 2
    
    # 重命名输出文件
    if [ -f plants/image_001.jpg ]; then
        mv plants/image_001.jpg "plants/${name}_1.jpg"
        mv plants/image_002.jpg "plants/${name}_2.jpg"
        mv plants/image_003.jpg "plants/${name}_3.jpg"
        echo "✓ 已生成 ${name} 的3个候选"
    else
        echo "✗ 生成失败: $name"
    fi
    
    echo ""
done

echo "========================================="
echo "全部完成！共生成 $(ls plants/*.jpg 2>/dev/null | wc -l) 张图片"
echo "========================================="
