#!/usr/bin/env python3
"""Export Play artwork from DataLux's existing native icon and shared brand colours.

Requires the project's ff-icon executable and Pillow (already bundled with Codex's
workspace runtime). Writes only this store-materials folder; no app assets change.
"""

from pathlib import Path
import argparse
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from PIL import Image, ImageCms, ImageDraw, ImageFont


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ASSETS = HERE / "assets"
PROFILE = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()


def save_png(image, path):
    image.save(path, "PNG", optimize=True, icc_profile=PROFILE)


def export_icon(exporter):
    # ff-icon owns the canonical grid geometry. Keep its rendered foreground,
    # while extending the same background gradient across Play's full square.
    with tempfile.TemporaryDirectory(prefix=".icon-export-", dir=ASSETS) as folder:
        temporary = Path(folder)
        subprocess.run(
            [str(exporter), str(temporary / "iconset"), str(temporary / "icon.ico"), str(temporary / "icon.png")],
            check=True,
        )
        foreground = Image.open(temporary / "icon.png").convert("RGBA")
        background = Image.new("RGBA", (512, 512))
        pixels = background.load()
        for y in range(512):
            # Matches desktop/src/icon_art.h's lit tile, with no outer mask/shadow.
            t = min(1.0, max(0.0, (y + 0.5 - 50.0) / 412.0))
            colour = tuple(round(255 * (a + (b - a) * t)) for a, b in
                           ((0.05, 0.04), (0.18, 0.10), (0.22, 0.12))) + (255,)
            for x in range(512):
                pixels[x, y] = colour
        icon = Image.alpha_composite(background, foreground)
        assert icon.getextrema()[3] == (255, 255)
        save_png(icon, ASSETS / "play-icon-512.png")

    # Retain an editable vector derivative with exactly the canonical paths.
    source = ET.parse(ROOT / "assets/fileflow-icon.svg")
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    square = source.getroot().find("{http://www.w3.org/2000/svg}rect")
    square.attrib.update(x="0", y="0", width="1024", height="1024")
    square.attrib.pop("rx", None)
    source.write(ASSETS / "play-icon-source.svg", encoding="utf-8", xml_declaration=True)


def export_feature():
    image = Image.new("RGB", (1024, 500))
    pixels = image.load()
    for y in range(500):
        for x in range(1024):
            t = (x / 1023 + y / 499) / 2
            # Deep teal is the existing icon/Android theme palette.
            pixels[x, y] = (round(9 + 3 * t), round(44 - 10 * t), round(54 - 9 * t))

    overlay = Image.new("RGBA", image.size)
    draw = ImageDraw.Draw(overlay)
    # An abstract path of light, rather than a mock device or duplicated app icon.
    draw.polygon([(606, 355), (944, 96), (944, 270), (606, 385)], fill=(125, 211, 240, 13))
    draw.polygon([(650, 370), (950, 161), (950, 221)], fill=(168, 230, 161, 13))
    draw.line([(643, 366), (909, 179)], fill=(125, 211, 240, 60), width=2)
    cells = [
        (727, 132, "#7DD3F0"), (791, 132, "#E1E6E8"),
        (727, 196, "#A8E6A1"), (791, 196, "#7DD3F0"), (855, 196, "#B6C9CF"),
        (791, 260, "#B6C9CF"), (855, 260, "#A8E6A1"),
    ]
    for x, y, colour in cells:
        draw.rounded_rectangle((x, y, x + 47, y + 47), radius=10, fill=colour)
    image = Image.alpha_composite(image.convert("RGBA"), overlay).convert("RGB")
    draw = ImageDraw.Draw(image)
    font = ROOT / "web/assets/InterVariable.ttf"
    title = ImageFont.truetype(str(font), 86)
    title.set_variation_by_name("SemiBold")
    caption = ImageFont.truetype(str(font), 37)
    detail = ImageFont.truetype(str(font), 25)
    draw.text((88, 113), "DataLux", font=title, fill="#E1E6E8", anchor="lt")
    draw.text((91, 235), "Files, carried by light.", font=caption, fill="#A8E6A1", anchor="lt")
    draw.text((92, 303), "Screen to camera. Offline.", font=detail, fill="#B6C9CF", anchor="lt")
    save_png(image, ASSETS / "feature-graphic-1024x500.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--icon-exporter", type=Path, default=ROOT / "build/desktop-app/desktop/ff-icon")
    args = parser.parse_args()
    ASSETS.mkdir(exist_ok=True)
    export_icon(args.icon_exporter.resolve())
    export_feature()
    for name, size, mode in (("play-icon-512.png", (512, 512), "RGBA"),
                              ("feature-graphic-1024x500.png", (1024, 500), "RGB")):
        path = ASSETS / name
        with Image.open(path) as image:
            assert image.size == size and image.mode == mode
            assert image.info.get("icc_profile")
        assert path.stat().st_size < 1024 * 1024
        print(f"{name}: {size[0]}x{size[1]} {mode}, {path.stat().st_size} bytes, sRGB")


if __name__ == "__main__":
    main()
