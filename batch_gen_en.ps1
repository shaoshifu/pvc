# English names for batch generation
$plants = @(
'Pixel Farmer','Starshot','Mushroom Scout','Fire Imp','Frost Pitcher',
'Coin Sprout','Wooden Guardian','Cactus Dart','Dual Shooter','Heal Petal'
)
$i=1
foreach($name in $plants){
    $prompt = "Plants vs Zombies style 2D game asset, hand-painted cartoon illustration, bold clean dark outline, rich saturated colors, $name plant character, front view, standing pose, bottom-center anchor, transparent background, no text, no watermark"
    mmx image generate --prompt $prompt --aspect-ratio 1:1 --n 1 --out-dir 'C:\Users\Administrator\WorkBuddy\2026-09-15-12-05-43\pvz-c\art\raw' --out-prefix "plant_$i" --quiet
    Write-Host "Generated $name"
    $i++
}
