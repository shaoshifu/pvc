"""Build generated UI masters into the premultiplied BGRA BMPs used by Win32 GDI."""

from pathlib import Path
import struct

import numpy as np
from PIL import Image, ImageEnhance, ImageFilter, ImageOps


ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "art" / "raw_ui"
OUT = ROOT / "assets"
PROCESSED = ROOT / "art" / "processed_ui"


def premultiply(rgba: np.ndarray) -> np.ndarray:
    out = rgba.astype(np.float32)
    out[:, :, :3] *= out[:, :, 3:4] / 255.0
    out[:, :, :3] = np.minimum(out[:, :, :3], out[:, :, 3:4])
    return np.clip(out, 0, 255).astype(np.uint8)


def save_bmp32(path: Path, rgba: np.ndarray) -> None:
    h, w = rgba.shape[:2]
    bgra = rgba[:, :, [2, 1, 0, 3]].astype(np.uint8)
    data = bgra[::-1].tobytes()
    header = b"BM" + struct.pack("<IHHI", 14 + 40 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(data),
                       2835, 2835, 0, 0)
    path.write_bytes(header + info + data)


def cover(im: Image.Image, size: tuple[int, int]) -> Image.Image:
    tw, th = size
    scale = max(tw / im.width, th / im.height)
    nw, nh = round(im.width * scale), round(im.height * scale)
    im = im.resize((nw, nh), Image.Resampling.LANCZOS)
    left = (nw - tw) // 2
    top = (nh - th) // 2
    return im.crop((left, top, left + tw, top + th))


def build_opaque(source: str, target: str) -> None:
    im = Image.open(RAW / source).convert("RGB")
    im = cover(im, (2000, 1300))
    # Slightly restrain contrast behind UI while keeping the generated brushwork.
    im = ImageEnhance.Contrast(im).enhance(0.94)
    rgba = np.dstack((np.asarray(im, dtype=np.uint8),
                      np.full((1300, 2000), 255, dtype=np.uint8)))
    save_bmp32(OUT / target, rgba)


def build_cardback() -> None:
    im = Image.open(RAW / "ui_cardback_v2.png").convert("RGBA")
    arr = np.asarray(im)
    alpha = arr[:, :, 3]
    ys, xs = np.where(alpha > 6)
    if len(xs):
        im = im.crop((int(xs.min()), int(ys.min()), int(xs.max()) + 1,
                      int(ys.max()) + 1))
    target = (400, 536)
    pad = 14
    scale = min((target[0] - pad * 2) / im.width,
                (target[1] - pad * 2) / im.height)
    im = im.resize((max(1, round(im.width * scale)),
                    max(1, round(im.height * scale))), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", target, (0, 0, 0, 0))
    canvas.alpha_composite(im, ((target[0] - im.width) // 2,
                                target[1] - pad - im.height))
    save_bmp32(OUT / "ui_cardback_v2.bmp",
               premultiply(np.asarray(canvas, dtype=np.uint8)))


def cut_white_jpeg(im: Image.Image) -> Image.Image:
    """Extract an mmx JPEG object from its near-white generation backdrop."""
    rgb = np.asarray(im.convert("RGB"), dtype=np.float32)
    corners = np.concatenate((rgb[:24, :24].reshape(-1, 3),
                              rgb[:24, -24:].reshape(-1, 3),
                              rgb[-24:, :24].reshape(-1, 3),
                              rgb[-24:, -24:].reshape(-1, 3)), axis=0)
    bg = np.median(corners, axis=0)
    dist = np.sqrt(np.sum((rgb - bg) ** 2, axis=2))
    alpha = np.clip((dist - 9.0) * (255.0 / 42.0), 0, 255).astype(np.uint8)
    alpha = np.asarray(Image.fromarray(alpha).filter(ImageFilter.GaussianBlur(0.7)))
    rgba = np.dstack((rgb.astype(np.uint8), alpha))
    ys, xs = np.where(alpha > 5)
    if len(xs):
        rgba = rgba[max(0, ys.min() - 3):min(rgba.shape[0], ys.max() + 4),
                    max(0, xs.min() - 3):min(rgba.shape[1], xs.max() + 4)]
    return Image.fromarray(rgba, "RGBA")


def tint_button(base: Image.Image, color: tuple[int, int, int], strength: float) -> Image.Image:
    rgba = np.asarray(base, dtype=np.uint8)
    rgb = rgba[:, :, :3].astype(np.float32)
    # The green-painted center is where G dominates; keep the walnut/brass rim intact.
    gmask = np.clip((rgb[:, :, 1] - 0.72 * rgb[:, :, 0] - 5.0) / 55.0, 0, 1)
    gmask *= rgba[:, :, 3].astype(np.float32) / 255.0
    target = np.array(color, dtype=np.float32)[None, None, :]
    mix = (gmask * strength)[:, :, None]
    out = rgb * (1.0 - mix) + target * mix
    return Image.fromarray(np.dstack((np.clip(out, 0, 255).astype(np.uint8),
                                      rgba[:, :, 3])), "RGBA")


def button_state(im: Image.Image, state: str) -> Image.Image:
    if state == "hot":
        im = ImageEnhance.Brightness(im).enhance(1.13)
        im = ImageEnhance.Contrast(im).enhance(1.04)
    elif state == "down":
        im = ImageEnhance.Brightness(im).enhance(0.78)
        # A one-pixel logical press is baked into the art so labels can follow it.
        shifted = Image.new("RGBA", im.size, (0, 0, 0, 0))
        shifted.alpha_composite(im, (0, 3))
        im = shifted
    return im


def build_buttons() -> None:
    src = Image.open(RAW / "mmx_buttons" / "button_compact_001.jpg")
    cut = cut_white_jpeg(src)
    target = (640, 220)
    scale = min((target[0] - 4) / cut.width, (target[1] - 4) / cut.height)
    cut = cut.resize((max(1, round(cut.width * scale)),
                      max(1, round(cut.height * scale))), Image.Resampling.LANCZOS)
    base = Image.new("RGBA", target, (0, 0, 0, 0))
    base.alpha_composite(cut, ((target[0] - cut.width) // 2,
                               (target[1] - cut.height) // 2))
    styles = {
        "green":  ((48, 118, 68), 0.34),
        "dark":   ((28, 54, 40), 0.55),
        "gold":   ((166, 112, 38), 0.58),
        "purple": ((100, 58, 142), 0.72),
        "red":    ((136, 54, 42), 0.70),
        "blue":   ((46, 92, 142), 0.68),
    }
    for name, (color, strength) in styles.items():
        colored = tint_button(base, color, strength)
        for state in ("normal", "hot", "down"):
            frame = button_state(colored, state)
            save_bmp32(OUT / f"ui_btn_{name}_{state}.bmp",
                       premultiply(np.asarray(frame, dtype=np.uint8)))
    disabled = ImageOps.grayscale(base.convert("RGB")).convert("RGBA")
    disabled.putalpha(base.getchannel("A"))
    disabled = ImageEnhance.Brightness(disabled).enhance(0.58)
    save_bmp32(OUT / "ui_btn_disabled.bmp",
               premultiply(np.asarray(disabled, dtype=np.uint8)))


def build_icons() -> None:
    """Split the generated 4x4 transparent master into consistent HUD icons."""
    names = (
        "sun", "coin", "star", "xp",
        "kill", "wave", "heart", "damage",
        "cooldown", "ice", "fire", "poison",
        "shield", "pause", "fullscreen", "back",
    )
    sheet = Image.open(RAW / "ui_icon_sheet_v1.png").convert("RGBA")
    PROCESSED.mkdir(parents=True, exist_ok=True)
    target = (160, 160)                  # 80 logical px at SS=2
    for index, name in enumerate(names):
        row, col = divmod(index, 4)
        x0 = round(sheet.width * col / 4)
        x1 = round(sheet.width * (col + 1) / 4)
        y0 = round(sheet.height * row / 4)
        y1 = round(sheet.height * (row + 1) / 4)
        icon = sheet.crop((x0, y0, x1, y1))
        arr = np.asarray(icon)
        ys, xs = np.where(arr[:, :, 3] > 5)
        if len(xs):
            icon = icon.crop((max(0, int(xs.min()) - 2), max(0, int(ys.min()) - 2),
                              min(icon.width, int(xs.max()) + 3),
                              min(icon.height, int(ys.max()) + 3)))
        scale = min(150 / icon.width, 150 / icon.height)
        icon = icon.resize((max(1, round(icon.width * scale)),
                            max(1, round(icon.height * scale))), Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", target, (0, 0, 0, 0))
        canvas.alpha_composite(icon, ((target[0] - icon.width) // 2,
                                      (target[1] - icon.height) // 2))
        canvas.save(PROCESSED / f"ui_icon_{name}_v1.png")
        save_bmp32(OUT / f"ui_icon_{name}_v1.bmp",
                   premultiply(np.asarray(canvas, dtype=np.uint8)))


def build_growth_cards() -> None:
    """Split the illustrated 4x2 growth-card master into seven card artworks."""
    names = ("damage", "rate", "hp", "boom", "frost", "cooldown", "pierce")
    sheet = Image.open(RAW / "growth_card_sheet_v1.png").convert("RGBA")
    target = (320, 260)                 # 160x130 logical px at SS=2
    for index, name in enumerate(names):
        row, col = divmod(index, 4)
        x0 = round(sheet.width * col / 4)
        x1 = round(sheet.width * (col + 1) / 4)
        y0 = round(sheet.height * row / 2)
        y1 = round(sheet.height * (row + 1) / 2)
        art = sheet.crop((x0, y0, x1, y1))
        arr = np.asarray(art).copy()
        # The rate cell inherited a few orange pixels from its left neighbour's
        # projectile at the sheet boundary. They are disconnected generation
        # spill, not part of the emblem.
        if name == "rate":
            arr[:, :round(arr.shape[1] * 0.045), 3] = 0
            art = Image.fromarray(arr, "RGBA")
        ys, xs = np.where(arr[:, :, 3] > 5)
        if len(xs):
            art = art.crop((max(0, int(xs.min()) - 3), max(0, int(ys.min()) - 3),
                            min(art.width, int(xs.max()) + 4),
                            min(art.height, int(ys.max()) + 4)))
        scale = min(304 / art.width, 244 / art.height)
        art = art.resize((max(1, round(art.width * scale)),
                          max(1, round(art.height * scale))), Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", target, (0, 0, 0, 0))
        canvas.alpha_composite(art, ((target[0] - art.width) // 2,
                                     target[1] - 5 - art.height))
        canvas.save(PROCESSED / f"growth_{name}_v1.png")
        save_bmp32(OUT / f"growth_{name}_v1.bmp",
                   premultiply(np.asarray(canvas, dtype=np.uint8)))


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    build_opaque("ui_menu_bg_v1.png", "ui_menu_bg_v1.bmp")
    build_opaque("ui_meta_bg_v1.png", "ui_meta_bg_v1.bmp")
    build_cardback()
    build_buttons()
    build_icons()
    build_growth_cards()
    print("built ui_menu_bg_v1.bmp 2000x1300")
    print("built ui_meta_bg_v1.bmp 2000x1300")
    print("built ui_cardback_v2.bmp 400x536 premultiplied-alpha")
    print("built 19 mmx-derived button skins 640x220 premultiplied-alpha")
    print("built 16 hand-painted HUD icons 160x160 premultiplied-alpha")
    print("built 7 illustrated growth-card artworks 320x260 premultiplied-alpha")


if __name__ == "__main__":
    main()
