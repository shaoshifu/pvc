# 批量生成 50 种创意植物精灵图
$plants = @(
'像素农夫','星之射手','蘑菇侦察兵','火焰小鬼','寒冰投手',
'小金币','木质守护','仙人掌飞镖','双发射手','治愈花瓣',
'雷电法师','时空扭曲者','龙息藤蔓','暗影刺客','召唤师萨满',
'反弹荆棘','闪光草莓','矿工萝卜','全息投影','重力控制',
'虚空吞噬者','雷电领主','凤凰火焰','寒冰女王','时间猎手',
'银月刺客','爆破专家','月光祭司','幸运轮盘','极速豌豆',
'幼龙草','魔导师南瓜','工程兵坚果','回旋豌豆','雷神花',
'镜像向日葵','水晶射手','丧尸克星','魅惑女王','像素投手',
'鹰眼射手','孢子坦克','星辰占卜师','毒藤蔓','锁定豌豆',
'绝对零度','万花筒花','狼人草','创世之花'
)
$i=1
foreach($name in $plants){
    $prompt = "Plants vs Zombies style 2D game asset, hand-painted cartoon illustration, bold clean dark outline, rich saturated colors, $name plant character, front view, standing pose, bottom-center anchor, transparent background, no text, no watermark"
    mmx image generate --prompt $prompt --aspect-ratio 1:1 --n 1 --out-dir 'C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\art\raw' --out-prefix "plant_$i" --quiet
    Write-Host "Generated $name"
    $i++
}
