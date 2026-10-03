#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Pre-render the WinRE-look boot menu of TopazOtgDxe (WinRe.c) into 8-bit grey BMPs.

UEFI has no font engine, so every text line is rendered here with PIL and stored as a
RAW section of one FREEFORM file (build_topaz_uefi.sh). The grey value of a pixel is its
coverage: WinRe.c blends white over the tile colour with it, so one bitmap serves both the
normal and the highlighted tile.

The look (layout, sizes, colours, the icons and the Selawik-based fonts) comes from
ntdevlabs/exynos9810-woa tools/twrp-winre (see third_party/exynos9810-woa and NOTICE.md).

    mkwinre.py OUTDIR      -> OUTDIR/winre_00.bmp .. winre_NN.bmp (order = WINRE_ASSET_* in WinRe.c)
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
NT = HERE / "third_party" / "exynos9810-woa"
FONT_LIGHT = str(NT / "fonts" / "winre-light.ttf")
FONT_SEMILIGHT = str(NT / "fonts" / "winre-semilight.ttf")
FONT_REGULAR = str(NT / "fonts" / "winre-regular.ttf")

# exynos9810-woa build.py layout (1080 wide theme space): tile 864 x 220, icon centre x 156,
# text x 264, title +54, description +122, single line +86; text colours #FFFFFF / #B4B4B4.
# The theme is 1920 high, tapas is 2400: rows keep their size, WinRe.c spreads them out.
TILE_W, TILE_H = 864, 220
ICON_CX = 100          # more left padding than exynos9810-woa (48): the focus outline needs room
TEXT_X = 204
DIM = 0xB4

ITEMS = [
    ("winre_ic_continue.png", "Continue", "Exit and continue to Windows"),
    ("gpu", "Windows without GPU", "Safe display, without the Adreno driver"),
    ("winre_ic_recovery.png", "Fastboot", "Restart to the bootloader for flashing"),
    ("winre_ic_poweroff.png", "Turn off your phone", None),
]


def text_img(text, font, size, grey=255, pad=4):
    f = ImageFont.truetype(font, size)
    x0, y0, x1, y1 = f.getbbox(text)
    im = Image.new("L", (x1 + 2 * pad, f.getmetrics()[0] + f.getmetrics()[1] + 2 * pad), 0)
    ImageDraw.Draw(im).text((pad, pad), text, font=f, fill=grey)
    return im


def gpu_icon(px=96, ss=4):
    """Our own drawing (not from exynos9810-woa): a graphics card (bracket, two fans, PCIe edge)."""
    n = px * ss
    im = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(im)
    w = 5 * ss                                   # stroke like the exynos9810-woa icons
    d.line([(0.10 * n, 0.22 * n), (0.10 * n, 0.84 * n)], fill=255, width=w)
    d.line([(0.10 * n, 0.24 * n), (0.18 * n, 0.24 * n)], fill=255, width=w)
    d.rounded_rectangle([0.18 * n, 0.28 * n, 0.92 * n, 0.70 * n], radius=0.05 * n, outline=255, width=w)
    d.line([(0.32 * n, 0.70 * n), (0.32 * n, 0.80 * n), (0.70 * n, 0.80 * n), (0.70 * n, 0.70 * n)],
           fill=255, width=w, joint="curve")
    cy, r = 0.49 * n, 0.11 * n
    for cx in (0.40 * n, 0.70 * n):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=w)
        d.ellipse([cx - 0.025 * n, cy - 0.025 * n, cx + 0.025 * n, cy + 0.025 * n], fill=255)
    return im.resize((px, px), Image.LANCZOS)


def tile(icon, title, desc):
    im = Image.new("L", (TILE_W, TILE_H), 0)
    ic = gpu_icon() if icon == "gpu" else Image.open(NT / "icons" / icon).convert("L")
    im.paste(ic, (ICON_CX - ic.width // 2, TILE_H // 2 - ic.height // 2))
    d = ImageDraw.Draw(im)
    ft = ImageFont.truetype(FONT_SEMILIGHT, 38)
    fb = ImageFont.truetype(FONT_REGULAR, 27)
    if desc:
        d.text((TEXT_X, 54), title, font=ft, fill=255)
        d.text((TEXT_X, 122), desc, font=fb, fill=DIM)
    else:
        d.text((TEXT_X, 86), title, font=ft, fill=255)
    return im


def save_bmp(im, path):
    p = Image.frombytes("P", im.size, im.tobytes())
    p.putpalette([v for i in range(256) for v in (i, i, i)])
    p.save(path)


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    assets = [text_img("Choose an option", FONT_LIGHT, 62)]                  # 0 header
    assets += [tile(*it) for it in ITEMS]                                     # 1..4 tiles
    assets.append(Image.open(NT / "icons" / "winre_ic_back.png").convert("L"))  # 5 back arrow
    assets.append(text_img("Tap an option, or use Volume up/down and Power", FONT_REGULAR, 27, DIM))  # 6 hint
    for i, im in enumerate(assets):
        save_bmp(im, out / f"winre_{i:02d}.bmp")
    print(f"{len(assets)} assets -> {out}")


if __name__ == "__main__":
    main()
