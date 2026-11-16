# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the application icon (an original mark: a film frame holding a
spectrum) and writes apps/spectra-app/resources/app.png and app.ico.

    python tools/assets/make_icon.py
"""
import colorsys
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parents[2] / "apps" / "spectra-app" / "resources"


def draw(size: int) -> Image.Image:
    s = size * 4  # supersample
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = s * 0.2
    d.rounded_rectangle([s * 0.04, s * 0.04, s * 0.96, s * 0.96], radius=r, fill=(34, 34, 38, 255))
    # perforations, top and bottom
    n = 5
    for i in range(n):
        x = s * (0.16 + i * 0.17)
        for y in (0.13, 0.80):
            d.rounded_rectangle([x, s * y, x + s * 0.09, s * (y + 0.07)], radius=s * 0.015, fill=(214, 214, 214, 255))
    # the frame: a spectrum, violet to red
    x0, x1, y0, y1 = s * 0.14, s * 0.86, s * 0.27, s * 0.73
    for x in range(int(x0), int(x1)):
        t = (x - x0) / (x1 - x0)
        hue = 0.76 * (1.0 - t)
        rr, gg, bb = colorsys.hsv_to_rgb(hue, 0.78, 0.96)
        d.line([(x, y0), (x, y1)], fill=(int(rr * 255), int(gg * 255), int(bb * 255), 255))
    d.rounded_rectangle([x0, y0, x1, y1], radius=s * 0.03, outline=(34, 34, 38, 255), width=int(s * 0.012))
    return img.resize((size, size), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    big = draw(256)
    big.save(OUT / "app.png")
    big.save(OUT / "app.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("wrote", OUT / "app.png", OUT / "app.ico")


if __name__ == "__main__":
    main()
