#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Generate 20 plant sprites + 13 bullet sprites via tupian.py."""
import os
import subprocess
import sys
import time

PY = r"C:\Python314\python.exe"
TUPIAN = r"E:\pdf\tupian.py"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "raw")
os.makedirs(OUT, exist_ok=True)

ANCHOR = (
    "Plants vs Zombies style 2D game asset, hand-painted cartoon illustration, "
    "bold clean dark outline, rich saturated colors, simple bold shape readable "
    "when scaled down to 16 pixels, centered, isolated on transparent background, "
    "no text, no watermark, no border, no ground, no shadow. "
)

QUALITY = (
    "Enhance material realism and micro-surface detail, stronger light-and-shadow "
    "hierarchy, richer color layering, crisp readable edges, better volume and depth, "
    "less flatness, less gray, less blur. Avoid oversharpening, overexposure, noise "
    "or style drift. Keep the original composition, subject, style and content. "
)

PLANT_POSE = (
    "a cute anthropomorphic plant character, facing right, front three-quarter view, "
    "standing upright in neutral idle pose, small dirt patch at the base. "
)

plants = [
    ("starfruit", "starfruit shooter: bright yellow five-pointed star fruit body with a cheerful face in the center, five short leaf-cannons at each point, waxy glossy rind, warm top-left highlight"),
    ("cactus", "saguaro cactus shooter: tall teal-green ribbed cactus column with dense white spines, a round cute face, a hollow cannon mouth near the top, dusty matte skin"),
    ("splitpea", "split-pea shooter: a plump yellow-green pea pod split into TWO cannons, one facing right and a smaller one facing left, two faces, smooth pea skin"),
    ("lightningreed", "lightning reed: a slender cattail-like reed with electric cyan leaf veins and tiny spark motes, a small face on the seed head, glassy wet highlights"),
    ("bloomerang", "bloomerang flower: an orange-pink boomerang-shaped blossom with layered petals, a round face in the hub, velvety petal texture"),
    ("fume", "fume-shroom: a stout purple-brown mushroom with a wide smoky nozzle mouth, pitted cap, earthy matte texture, faint purple haze at the mouth"),
    ("laserbean", "laser bean: a long dark-green bean pod standing upright, a glowing crimson crystal core in the belly, metallic sheen on the pod, determined face"),
    ("melonpult", "melon-pult: a chubby dark-green watermelon catapult plant with thick stripes, a wooden-looking leafy catapult arm, heavy rind texture"),
    ("wintermelon", "winter melon pult: a frosted pale-cyan watermelon catapult, ice crystals on the rind, cold white highlights, leafy catapult arm"),
    ("gloom", "gloom-shroom: a gloomy deep-purple mushroom with a heavily wrinkled umbrella cap, sad half-lidded eyes, velvety dark gills"),
    ("goldmagnet", "gold magnet-shroom: a mushroom with a metallic gold cap and horseshoe magnet arms, copper-gold filigree, wealthy cute face"),
    ("twinflower", "twin sunflower: TWO cheerful yellow sunflower heads on one forked stem, rich golden petals, fuzzy brown seed centers"),
    ("marrow", "marrow bloom: an ivory bone-white flower with a pink marrow core, petal edges like polished bone, gentle healer face"),
    ("pumpkin", "pumpkin shell plant: a thick orange pumpkin helmet with carved cute eye holes, heavy waxy rind, sitting low like a protective shell"),
    ("garlic", "garlic plant: a plump creamy-white garlic bulb with green spicy sprouts as hair, slightly teary cute face, papery skin layers"),
    ("hypnoshroom", "hypno-shroom: a pink-purple mushroom with a hypnotic spiral on the cap, dreamy half-closed eyes, soft velour texture"),
    ("iceshroom", "ice-shroom: a pale icy-blue mushroom capped with frost crystals and a ring of tiny snowflakes, cold glossy surface"),
    ("tanglekelp", "tangle kelp: dark olive ribbon-like seaweed with water droplets, two looping tendrils, a small aquatic face, wet translucent leaves"),
    ("cobcannon", "cob cannon: a massive yellow corn cob cannon bound with iron hoops, kernel texture, a determined face on the cob, heavy industrial plant"),
    ("cattail", "cattail plant: a brown fuzzy cattail spike on a long green stalk, cat-like cute face, velvet seed head, pond-plant feel"),
]

bullets = [
    ("bullet_star", "STAR PROJECTILE: a small five-pointed glossy bright yellow star with a warm highlight, thick dark outline"),
    ("bullet_spine", "SPINE PROJECTILE: a slender pale-green cactus needle dart with a sharp tip, tiny barbs, pointing right"),
    ("bullet_split", "SPLIT PEA PROJECTILE: a round glossy yellow-green pea, slightly smaller than a normal pea, bright highlight"),
    ("bullet_arc", "ARC PROJECTILE: a compact cyan electric spark ball with jagged lightning ticks, glowing core"),
    ("bullet_boomer", "BOOMERANG PROJECTILE: a small orange-pink boomerang crescent, two petal tips, glossy"),
    ("bullet_fume", "FUME PROJECTILE: a compact swirling purple smoke puff with a darker core, soft but still outlined"),
    ("bullet_beam", "BEAM PROJECTILE: a short crimson energy bolt, hexagonal crystal shard, glowing red"),
    ("bullet_melon", "MELON PROJECTILE: a tiny dark-green striped watermelon, round and heavy, glossy rind"),
    ("bullet_icemelon", "ICE MELON PROJECTILE: a tiny frosted pale-cyan watermelon with ice crystals, cold white highlight"),
    ("bullet_gloom", "GLOOM PROJECTILE: a small deep-purple spore cloud knot, wrinkled, ominous"),
    ("bullet_cob", "COB PROJECTILE: a stubby yellow corn cob rocket with iron-ring bands, pointing right"),
    ("bullet_dart", "DART PROJECTILE: a slim brown cattail seed dart with a fuzzy tuft at the back, sharp tip"),
    ("bullet_gold", "GOLD NUGGET PROJECTILE: a tiny metallic gold nugget, faceted, bright highlight"),
]


def already(name):
    hits = [f for f in os.listdir(OUT) if name in f and f.lower().endswith(".png")]
    return hits


def gen(name, prompt, retries=2):
    if already(name):
        print("skip", name)
        return True
    cmd = [PY, TUPIAN, prompt, "--model", "gpt-image-2", "--size", "1024x1024",
           "-n", "1", "--name", name, "-o", OUT]
    for attempt in range(retries + 1):
        print("gen", name, "try", attempt + 1)
        r = subprocess.run(cmd, cwd=r"E:\pdf")
        if r.returncode == 0 and already(name):
            return True
        time.sleep(3)
    print("FAIL", name)
    return False


ok = 0
fail = []
for name, desc in plants:
    prompt = ANCHOR + QUALITY + PLANT_POSE + desc + "."
    if gen(name, prompt):
        ok += 1
    else:
        fail.append(name)
    time.sleep(1)

for name, desc in bullets:
    prompt = (
        ANCHOR + QUALITY +
        "A single " + desc +
        ", horizontal orientation pointing right, square composition."
    )
    if gen(name, prompt):
        ok += 1
    else:
        fail.append(name)
    time.sleep(1)

print("done ok=%d fail=%s" % (ok, fail))
sys.exit(0 if not fail else 1)
