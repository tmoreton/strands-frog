# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow==13.0.0", "resvg-py==0.5.0"]
# ///
"""Bake editable SVG poses and accessories into native LVGL image data."""

import io
import re
import struct
from pathlib import Path
from xml.etree import ElementTree as ET

from PIL import Image
from resvg_py import svg_to_bytes

ASSETS = Path(__file__).resolve().parent
ET.register_namespace("", "http://www.w3.org/2000/svg")
BAYER_4 = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))

# Set only the parts that move relative to the neutral artwork.
POSES = {
    "IDLE": {},
    "BLINK": {"left": "closed", "right": "closed"},
    "CROUCH": {"body": "translate(-6 10) scale(1.06 .94)", "left": "wink", "right": "wink"},
    "HOP": {"body": "translate(4 -5) scale(.96 1.03)", "mouth": "happy"},
    "LISTEN": {"face": "rotate(-3 108 96)", "pupils": (0, -2), "mouth": "small"},
    "THINK_LEFT": {"face": "rotate(-4 108 96)", "pupils": (-5, -3), "mouth": "think"},
    "THINK_RIGHT": {"face": "rotate(4 108 96)", "pupils": (4, -3), "mouth": "think"},
    "HAPPY": {"mouth": "happy"},
    "WINK": {"right": "wink", "mouth": "happy"},
    "WORRIED": {"pupils": (0, 2), "mouth": "worry"},
    "TALK": {"mouth": "wide"},
    "WAVE_LIFT": {"arm": -49, "mouth": "happy"},
    "WAVE_IN": {"arm": -99, "mouth": "happy"},
    "WAVE_MID": {"arm": -117, "mouth": "happy"},
    "WAVE_OUT": {"arm": -137, "mouth": "happy"},
}
THEME_OVERLAYS = ("wizard-hat.svg", "garden-crown.svg")


def draw_pose(art, width, height, *, left="open", right="open", pupils=(0, 0),
              mouth="smile", arm=8, body="", face=""):
    svg = ET.fromstring(art)
    parts = {node.get("id"): node for node in svg.iter() if "id" in node.attrib}
    for side, eye in (("left", left), ("right", right)):
        parts[f"{side}-eye"].set("href", f"#eye-{eye}")
        pupil = parts[f"{side}-pupil"]
        pupil.set("visibility", "visible" if eye == "open" else "hidden")
        pupil.set("transform", f"translate({pupils[0]} {pupils[1]})")
    parts["frog"].set("transform", body)
    parts["face"].set("transform", face)
    parts["mouth"].set("href", f"#mouth-{mouth}")
    parts["wave-arm"].set("transform", f"rotate({arm})")
    png = svg_to_bytes(svg_string=ET.tostring(svg, encoding="unicode"),
                       background="#000000", zoom=2, skip_system_fonts=True)
    return Image.open(io.BytesIO(png)).convert("RGB").resize((width, height), Image.Resampling.LANCZOS)


def pack_rgb565(image):
    # Dither gradients at native resolution while keeping the background black.
    pixels = []
    for index, (red, green, blue) in enumerate(image.get_flattened_data()):
        y, x = divmod(index, image.width)
        threshold = BAYER_4[y % 4][x % 4] * 16 + 8
        r5 = (red * 31 + threshold) // 255
        g6 = (green * 63 + threshold) // 255
        b5 = (blue * 31 + threshold) // 255
        pixels.append(r5 << 11 | g6 << 5 | b5)
    return struct.pack(f"<{len(pixels)}H", *pixels)


def draw_overlay(path, width, height):
    art = path.read_text()
    svg = ET.fromstring(art)
    if (int(svg.attrib["width"]), int(svg.attrib["height"])) != (width, height):
        raise ValueError(f"{path.name} dimensions must match frog_sprites.h")
    png = svg_to_bytes(svg_string=art, zoom=2, skip_system_fonts=True)
    return Image.open(io.BytesIO(png)).convert("RGBA").resize((width, height), Image.Resampling.LANCZOS)


def main():
    art = (ASSETS / "charm-frog.svg").read_text()
    svg = ET.fromstring(art)
    width, height = int(svg.attrib["width"]), int(svg.attrib["height"])
    header = (ASSETS.parent / "main" / "frog_sprites.h").read_text()
    poses = re.findall(r"^ +FROG_(\w+),$", header, re.MULTILINE)
    poses.remove("SPRITE_COUNT")
    if set(poses) != POSES.keys() or any(
        f"#define FROG_SPRITE_{dimension} {size}" not in header
        for dimension, size in (("WIDTH", width), ("HEIGHT", height))
    ):
        raise ValueError("SVG dimensions and pose names must match frog_sprites.h")
    frames = (pack_rgb565(draw_pose(art, width, height, **POSES[pose])) for pose in poses)
    (ASSETS / "frog_frames.rgb565").write_bytes(b"".join(frames))

    width, height = (
        int(re.search(rf"^#define FROG_THEME_OVERLAY_{dimension} (\d+)$", header, re.MULTILINE)[1])
        for dimension in ("WIDTH", "HEIGHT")
    )
    overlays = (draw_overlay(ASSETS / name, width, height).tobytes("raw", "BGRA") for name in THEME_OVERLAYS)
    (ASSETS / "frog_theme_overlays.argb8888").write_bytes(b"".join(overlays))


if __name__ == "__main__":
    main()
