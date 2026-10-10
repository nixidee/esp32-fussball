"""Build baseline-JPEG stock images at the selected hardware profile size."""
from pathlib import Path
import argparse
import math
import re
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]


def stock(name):
    side = 1024
    image = Image.new("RGB", (side, side), (16, 24, 32))
    draw = ImageDraw.Draw(image)
    for y in range(side):
        draw.line((0, y, side, y), fill=(12, 19 + y * 8 // side, 27 + y * 10 // side))
    if name == "crest" or name.startswith("slide"):
        centre, radius = side / 2, side * 0.29
        draw.ellipse((centre-radius, centre-radius, centre+radius, centre+radius), fill=(218, 235, 229), outline=(73, 203, 160), width=side//64)
        for angle, distance in [(0, 0), (-90, 0.7), (-18, 0.7), (54, 0.7), (126, 0.7), (198, 0.7)]:
            x = centre + radius * distance * math.cos(math.radians(angle))
            y = centre + radius * distance * math.sin(math.radians(angle))
            r = radius * (0.29 if distance == 0 else 0.20)
            points = [(x+r*math.cos(math.radians(-90+i*72)), y+r*math.sin(math.radians(-90+i*72))) for i in range(5)]
            draw.polygon(points, fill=(16, 24, 32))
    else:
        colour = (22, 42, 44)
        inset = side // 9
        draw.rectangle((inset, inset, side-inset, side-inset), outline=colour, width=side//150)
        draw.line((inset, side//2, side-inset, side//2), fill=colour, width=side//150)
        draw.ellipse((side*.35, side*.35, side*.65, side*.65), outline=colour, width=side//150)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, help="hardware target/environment name")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9_]+", args.target):
        parser.error("invalid target name")
    target = (ROOT / f"targets/{args.target}.h").read_text()
    match = re.search(r'#include "(displays/[^\"]+)"', target)
    if not match:
        parser.error("target has no display profile")
    profile = (ROOT / match[1]).read_text()
    size = tuple(int(re.search(rf"\.{axis}\s*=\s*(\d+)", profile)[1]) for axis in ("width", "height"))
    config = (ROOT / "include/app_config.h").read_text()
    names = list(dict.fromkeys(re.findall(r'"([a-z0-9_]+)\.jpg"', re.search(r"kDefaultImageFiles\[\]\[32\]\s*=\s*\{([^}]+)\}", config)[1])))
    maximum = int(re.search(r"kImageMaxBytes\s*=\s*(\d+)", config)[1])
    (ROOT / "data").mkdir(exist_ok=True)
    total = 0
    for name in names:
        source = ROOT / f"assets/src/{name}.png"
        if not source.exists():
            stock(name.removeprefix("default_")).save(source)
        image = Image.open(source).convert("RGB").resize(size, Image.Resampling.LANCZOS)
        destination = ROOT / f"data/{name}.jpg"
        image.save(destination, quality=86, optimize=True, progressive=False, subsampling=2)
        count = destination.stat().st_size
        if count > maximum:
            raise SystemExit(f"{destination.name}: {count} exceeds {maximum}; revise source before deployment")
        total += count
        print(f"{destination.name}: {size[0]}x{size[1]}, {count} B, baseline JPEG")
    print(f"Total default image bytes: {total}; no device was written")


if __name__ == "__main__":
    main()
