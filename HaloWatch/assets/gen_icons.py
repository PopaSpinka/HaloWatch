"""Weather icons for HaloWatch: a sun partly hidden behind a cloud, as on the Pixel Watch weather tile.

Run from the repo root:  python3 HaloWatch/assets/gen_icons.py
Writes assets/wx_big.png (the current-weather icon) and assets/wx_small.png (hourly row) for the Weather screen, the
other kinds in the same style (assets/wx_sun*.png, wx_cloud*.png, wx_rain*.png), and assets/face_*.png (partly cloudy,
sunny, cloudy, rain) for the watch face; then
`python3 tools/halo_assets.py HaloWatch` turns them into Halo images.

Geometry is measured from design/weather_mockup.webp and converted to board pixels (circle 1254 px -> 466 px,
scale 0.3722): the sun is a circle; the cloud is two circles (the left one is cut by the flat bottom) joined by a
base; a thin black gap separates the cloud from the sun. Shapes are drawn 8x larger and scaled down, so the edges are smooth.
The Weather screen now follows design/weather2_mockup.webp, keeping these icons: they are scaled so that the drawing is
as tall as the mockup's icons (BIG_SCALE, SMALL_SCALE).
"""
import os
import re
import shlex
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../tools"))
import halo_assets as ha  # noqa: E402 — the same font rendering as the firmware's assets

HERE = os.path.dirname(os.path.abspath(__file__))
SS = 8  # supersampling
SCALE = 1.2  # the first weather layout: 1.2x the old mockup
# design/weather2_mockup.webp: the current-weather icon is 69.8 px tall, the hourly ones 45.9 px; the drawing of
# "sun behind a cloud" is 54 px tall at scale 1
WX2_SCALE = 1.2  # the Weather screen is drawn 1.2x the mockup (WX_SCALE in weather.h)
BIG_SCALE = 69.8 / 54 * WX2_SCALE
SMALL_SCALE = 45.9 / 54 * WX2_SCALE

SUN = 0xFCC425
CLOUD = 0xF9F4F8

# Big icon in board pixels: the cloud circles are fitted to the mockup's cloud mask (94.5 % overlap)
SUN_C, SUN_R = (125.8, 179.0), 22.7
BIG_C, BIG_R = (138.1, 195.8), 15.3      # right bump of the cloud
SMALL_C, SMALL_R = (119.8, 205.5), 9.7   # left bump, cut by the flat bottom
BASE_Y1 = 210.3                          # flat bottom of the cloud
GAP = 1.0                                # black gap between the cloud and the sun


def rgb(c):
    return (c >> 16 & 255, c >> 8 & 255, c & 255)


def cloud(size, ox, oy, s, grow):
    """Cloud mask: two circles joined by a base, cut flat at the bottom; grow > 0 makes it fatter (for the gap)."""
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    for c, r in ((BIG_C, BIG_R), (SMALL_C, SMALL_R)):
        x, y, rr = (c[0] - ox) * s * SS, (c[1] - oy) * s * SS, (r + grow) * s * SS
        d.ellipse((x - rr, y - rr, x + rr, y + rr), fill=255)
    x0, x1 = (SMALL_C[0] - ox) * s * SS, (BIG_C[0] - ox) * s * SS
    y0, y1 = (SMALL_C[1] - oy) * s * SS, (BASE_Y1 + grow - oy) * s * SS
    d.rectangle((x0, y0, x1, y1), fill=255)
    d.rectangle((0, y1, size[0], size[1]), fill=0)  # flat bottom
    return m


def icon(scale, path):
    save(icon_img(scale), path)


def save(img, path):
    img.save(path)
    print(f"{os.path.relpath(path)}: {img.width}x{img.height}, drawing {ink_h(img)} px tall")


def ink_h(img):
    """Height of what Halo will draw: rows with alpha above 0 after the 4-bit cut (as the image's ext rows)."""
    rows = np.nonzero((ha.q4(np.array(img)[..., 3]) > 0).any(1))[0]
    return int(rows.max() - rows.min() + 1) if len(rows) else 0


def digit_band(name, ref="14"):
    """How tall the digits of font NAME (from halo.conf) are on the board, by the flat ones ("1", "4": round digits
    overshoot a row up and down, as in every font): rows that survive the 4-bit cut — the big temperature's digits
    set the two lines the icon fits in (weather.h, wx_digit_rows — the same reference)."""
    conf = open(os.path.join(HERE, "halo.conf"), encoding="utf-8").read()
    m = re.search(rf"^font\s+{name}\s+(.+)$", conf, re.M)
    parts = shlex.split(m.group(1).split("#")[0])
    path, size = ha.font_path(parts[0]), int(parts[1])
    axes = next((p[1:] for p in parts if p.startswith("@")), "")
    font = ImageFont.truetype(path, size)
    ha.set_axes(font, size, axes)
    top, bottom = 1e9, -1e9
    for ch in ref:
        a, x0, y0 = ha.render_glyph(font, ch)
        rows = np.nonzero((ha.q4(a) > 0).any(1))[0]
        top, bottom = min(top, y0 + rows.min()), max(bottom, y0 + rows.max() + 1)
    return bottom - top


def fit_height(make, target, scale):
    """Scale so that the drawing is exactly `target` px tall (each kind has its own shape and height): a first guess by
    the ratio, then halving between a scale that is too small and one that is too large."""
    img = make(scale)
    h = ink_h(img)
    if h == target:
        return img
    scale *= target / h
    lo, hi = scale * 0.97, scale * 1.03
    best = None
    for _ in range(24):
        mid = (lo + hi) / 2
        img = make(mid)
        h = ink_h(img)
        if h == target:
            best = img
            hi = mid  # the smallest scale that is tall enough: no row of the drawing goes past the line
        elif h < target:
            lo = mid
        else:
            hi = mid
    return best or img


def icon_img(scale):
    # Canvas around the icon with a margin, in board pixels relative to its top-left corner
    left, top = SUN_C[0] - SUN_R - 2, SUN_C[1] - SUN_R - 2
    right, bottom = BIG_C[0] + BIG_R + 2, BASE_Y1 + 2
    w, h = int((right - left) * scale + 1), int((bottom - top) * scale + 1)
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x, y, r = (SUN_C[0] - left) * scale * SS, (SUN_C[1] - top) * scale * SS, SUN_R * scale * SS
    d.ellipse((x - r, y - r, x + r, y + r), fill=rgb(SUN) + (255,))
    clear = Image.new("RGBA", img.size, (0, 0, 0, 0))
    img.paste(clear, (0, 0), cloud(img.size, left, top, scale, GAP))  # cut the gap out of the sun
    img.paste(Image.new("RGBA", img.size, rgb(CLOUD) + (255,)), (0, 0), cloud(img.size, left, top, scale, 0))
    return img.resize((w, h), Image.LANCZOS)


def weather_icon(kind, scale, path):
    save(weather_icon_img(kind, scale), path)


def weather_icon_img(kind, scale):
    """Sunny, cloudy and rain for the Weather screen: the canvas and the cloud of its "sun behind a cloud" (icon())."""
    left, top = SUN_C[0] - SUN_R - 2, SUN_C[1] - SUN_R - 2
    right, bottom = BIG_C[0] + BIG_R + 2, BASE_Y1 + 2
    w, h = int((right - left) * scale + 1), int((bottom - top) * scale + 1)
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    mx, my = (left + right) / 2, (top + bottom) / 2  # canvas center, mockup px
    ccx = (SMALL_C[0] - SMALL_R + BIG_C[0] + BIG_R) / 2  # cloud bbox center, mockup px
    ccy = (BIG_C[1] - BIG_R + BASE_Y1) / 2

    def cloud_mask(dx, dy, k, grow):
        """The cloud scaled by k around its center, its center at the canvas center + (dx, dy)."""
        ox, oy = ccx - (mx + dx - left) / k, ccy - (my + dy - top) / k
        return cloud(img.size, ox, oy, scale * k, grow)

    def fill(color, mask):
        img.paste(Image.new("RGBA", img.size, rgb(color) + (255,)), (0, 0), mask)

    def cut(mask):
        img.paste(Image.new("RGBA", img.size, (0, 0, 0, 0)), (0, 0), mask)

    if kind == "sun":
        x, y, r = (mx - left) * scale * SS, (my - top) * scale * SS, 24 * scale * SS
        d.ellipse((x - r, y - r, x + r, y + r), fill=rgb(SUN) + (255,))
    elif kind == "cloud":  # a gray cloud behind a white one
        fill(FACE_CLOUD_BACK, cloud_mask(-7, -8, 0.82, 0))
        cut(cloud_mask(3, 5, 1, GAP))
        fill(CLOUD, cloud_mask(3, 5, 1, 0))
    elif kind == "rain":  # the cloud higher up, three slanted drops under it
        fill(CLOUD, cloud_mask(0, -7, 1, 0))
        for x in (-10, 0, 10):
            x0, y0 = (mx + x + 2.4 - left) * scale, (my + 13 - top) * scale
            x1, y1 = (mx + x - 1.8 - left) * scale, (my + 22 - top) * scale
            wd = 4.6 * scale
            d.line(((x0 * SS, y0 * SS), (x1 * SS, y1 * SS)), fill=rgb(FACE_RAIN) + (255,), width=int(wd * SS))
            for xx, yy in ((x0, y0), (x1, y1)):  # round caps
                d.ellipse(((xx - wd / 2) * SS, (yy - wd / 2) * SS, (xx + wd / 2) * SS, (yy + wd / 2) * SS),
                          fill=rgb(FACE_RAIN) + (255,))
    return img.resize((w, h), Image.LANCZOS)


# ---------- Watch face (design/face_mockup.webp, screen circle 1215 px -> 466 px) ----------
# Fitted to the mockup's masks: the cloud is a stadium (two caps of radius 7.19) plus one big circle on top (97 %
# overlap); the sun is a circle (97.6 %). Coordinates are relative to the icon's box center (193.43, 337.03) on the
# board; every face icon is drawn on the same canvas around that center, so they swap in place.
FACE_BOX = 54                      # canvas, px (the icon itself is about 48 x 48)
FACE_SCALE = 1.2                   # the face is drawn 1.2x the mockup (FACE_SCALE in face.h): icons are made that size
FACE_SUN = 0xFCC62E
FACE_CLOUD = 0xF8F7F9
FACE_CLOUD_BACK = 0xA9AEB5         # the far cloud of "cloudy"
FACE_RAIN = 0x7CC0FF
# Cloud relative to its own bbox center: stadium caps at x -14.63 .. 10.19, y 7.94, r 7.19; big circle (6.36, 0.32) r 15.46
CLOUD_CAPS = (-14.63, 10.19, 7.94, 7.19)
CLOUD_BIG = (6.36, 0.32, 15.46)
FACE_GAP = 1.0                     # transparent gap where one shape overlaps another


def face_cloud(size, cx, cy, s, grow):
    """Cloud mask centered at (cx, cy) canvas px, scaled by s; grow > 0 makes it fatter (for the gap)."""
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    xl, xr, yc, r = CLOUD_CAPS
    bx, by, br = CLOUD_BIG
    k = s * SS

    def disc(x, y, rr):
        d.ellipse(((cx + x * s) * SS - rr * k, (cy + y * s) * SS - rr * k,
                   (cx + x * s) * SS + rr * k, (cy + y * s) * SS + rr * k), fill=255)

    disc(xl, yc, r + grow / s)
    disc(xr, yc, r + grow / s)
    disc(bx, by, br + grow / s)
    d.rectangle(((cx + xl * s) * SS, (cy + (yc - r) * s - grow) * SS, (cx + xr * s) * SS, (cy + (yc + r) * s + grow) * SS),
                fill=255)
    return m


def face_icon(kind, path):
    n = FACE_BOX * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = FACE_BOX / 2

    def fill(color, mask):
        img.paste(Image.new("RGBA", img.size, rgb(color) + (255,)), (0, 0), mask)

    def cut(mask):
        img.paste(Image.new("RGBA", img.size, (0, 0, 0, 0)), (0, 0), mask)

    def sun(x, y, r):
        d.ellipse(((c + x - r) * SS, (c + y - r) * SS, (c + x + r) * SS, (c + y + r) * SS), fill=rgb(FACE_SUN) + (255,))

    if kind == "partly":  # the mockup: the sun upper left, the cloud in front of it lower right
        sun(-3.45, -3.68, 20.41)
        cut(face_cloud(img.size, c + 2.04, c + 8.95, 1, FACE_GAP))
        fill(FACE_CLOUD, face_cloud(img.size, c + 2.04, c + 8.95, 1, 0))
    elif kind == "sun":
        sun(0, 0, 21.5)
    elif kind == "cloud":  # a gray cloud behind a white one
        fill(FACE_CLOUD_BACK, face_cloud(img.size, c - 5.5, c - 9, 0.85, 0))
        cut(face_cloud(img.size, c + 2.2, c + 8.5, 1, FACE_GAP))
        fill(FACE_CLOUD, face_cloud(img.size, c + 2.2, c + 8.5, 1, 0))
    elif kind == "rain":  # the cloud higher up, three slanted drops under it
        fill(FACE_CLOUD, face_cloud(img.size, c, c - 6, 1, 0))
        for x in (-10, 0, 10):
            x0, y0, x1, y1 = c + x + 2.2, c + 13.5, c + x - 1.8, c + 21.5
            d.line(((x0 * SS, y0 * SS), (x1 * SS, y1 * SS)), fill=rgb(FACE_RAIN) + (255,), width=int(4.4 * SS))
            for x_, y_ in ((x0, y0), (x1, y1)):  # round caps
                d.ellipse(((x_ - 2.2) * SS, (y_ - 2.2) * SS, (x_ + 2.2) * SS, (y_ + 2.2) * SS), fill=rgb(FACE_RAIN) + (255,))
    out = round(FACE_BOX * FACE_SCALE)
    img = img.resize((out, out), Image.LANCZOS)
    img.save(path)
    print(f"{os.path.relpath(path)}: {out}x{out}")


if __name__ == "__main__":
    # The Weather screen's current icon: every kind exactly as tall as the big temperature's digits (w_now in
    # halo.conf) — it fits between the lines along their top and bottom (weather.h)
    band = digit_band("w_now")
    print(f"w_now digits: {band} px")
    save(fit_height(icon_img, band, BIG_SCALE), os.path.join(HERE, "wx_big.png"))
    icon(SMALL_SCALE, os.path.join(HERE, "wx_small.png"))
    for kind in ("sun", "cloud", "rain"):
        save(fit_height(lambda sc: weather_icon_img(kind, sc), band, BIG_SCALE), os.path.join(HERE, f"wx_{kind}.png"))
        weather_icon(kind, SMALL_SCALE, os.path.join(HERE, f"wx_{kind}_sm.png"))
    for kind in ("partly", "sun", "cloud", "rain"):
        face_icon(kind, os.path.join(HERE, f"face_{kind}.png"))
