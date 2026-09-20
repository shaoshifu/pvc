# 50 creative plants batch generation
$plants = @(
'Pixel Farmer','Starshot','Mushroom Scout','Fire Imp','Frost Pitcher',
'Coin Sprout','Wooden Guardian','Cactus Dart','Dual Shooter','Heal Petal',
'Thunder Mage','Chrono Distorter','Dragon Breath Vine','Shadow Assassin','Summoner Shaman',
'Bounce Thorn','Flash Berry','Miner Radish','Hologram Projector','Gravity Controller',
'Void Devourer','Thunder Overlord','Phoenix Flame','Frost Queen','Time Hunter',
'Silver Moon Assassin','Demolition Expert','Moonlight Priestess','Lucky Roulette','Speedy Pea',
'Baby Dragon Grass','Mage Pumpkin','Engineer Walnut','Boomerang Pea','Thor Flower',
'Mirror Sunflower','Crystal Shooter','Zombie Slayer','Seduction Queen','Pixel Thrower',
'Eagle Eye Shooter','Spore Tank','Star Diviner','Poison Vine','Lock-On Pea',
'Absolute Zero','Kaleidoscope Flower','Werewolf Grass','Genesis Flower'
)
$i=11
foreach($name in $plants[10..49]){
    $prompt = "Plants vs Zombies style 2D game asset, hand-painted cartoon illustration, bold clean dark outline, rich saturated colors, $name plant character, front view, standing pose, bottom-center anchor, transparent background, no text, no watermark"
    mmx image generate --prompt $prompt --aspect-ratio 1:1 --n 1 --out-dir 'C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\art\raw' --out-prefix "plant_$i" --quiet
    Write-Host "Generated $name"
    $i++
}
