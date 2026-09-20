$plants = @(
'Moonlight Priestess','Lucky Roulette','Speedy Pea','Baby Dragon Grass','Mage Pumpkin',
'Engineer Walnut','Boomerang Pea','Thor Flower','Mirror Sunflower','Crystal Shooter',
'Zombie Slayer','Seduction Queen','Pixel Thrower','Eagle Eye Shooter','Spore Tank',
'Star Diviner','Poison Vine','Lock-On Pea','Absolute Zero','Kaleidoscope Flower',
'Werewolf Grass','Genesis Flower'
)
$i=28
foreach($name in $plants){
    $prompt = "Plants vs Zombies style 2D game asset, hand-painted cartoon illustration, bold clean dark outline, rich saturated colors, $name plant character, front view, standing pose, bottom-center anchor, transparent background, no text, no watermark"
    mmx image generate --prompt $prompt --aspect-ratio 1:1 --n 1 --out-dir 'C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\art\raw' --out-prefix "plant_$i" --quiet
    Write-Host "Generated $name"
    $i++
}
