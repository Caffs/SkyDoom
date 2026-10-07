#!/usr/bin/env python3
"""
banner.py — Generate docs/assets/banner.png.

Requires: Pillow  (python-pillow / pip install Pillow)
Font:      Cinzel Regular — downloaded automatically from Google Fonts on first
           run and cached in $XDG_CACHE_HOME/<project>/ (default: ~/.cache/<project>/).

Usage:
    python3 scripts/banner.py [options]

Options:
    --title TEXT        Main title  (default: "ExampleMod")
    --subtitle TEXT     Subtitle    (default: "A brief description of what the plugin does")
    --tags TEXT         Tags line   (default: "Skyrim SE / AE  ·  SKSE")
    --font PATH         Path to a .ttf font file (skips auto-download)
    --output PATH       Output file (default: docs/assets/banner.png)
    --width INT         Canvas width  in pixels (default: 1280)
    --height INT        Canvas height in pixels (default: 638)
    --font-size INT     Base font size in pt (default: 180, minimum: 30)
    --no-vignette       Disable the radial vignette effect
"""

import argparse
import hashlib
import os
import sys
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

DEFAULT_TITLE    = "ExampleMod"
DEFAULT_SUBTITLE = "A brief description of what the plugin does"
DEFAULT_TAGS     = "Skyrim SE / AE  ·  SKSE"
DEFAULT_WIDTH    = 1280
DEFAULT_HEIGHT   = 638
DEFAULT_OUTPUT   = Path(__file__).parents[1] / "docs" / "assets" / "banner.png"

BASE_FONT_SIZE = 180

DIVIDER_X_PAD = 140

# Colours
BG_CENTRE = (18, 16,  9)
BG_DARK   = (10,  9,  5)
GOLD      = (184, 149, 62)
GOLD_DIM  = (100,  83, 38)

# Font — pinned to a specific commit for reproducibility
FONT_URL    = "https://github.com/google/fonts/raw/45071f07c63e863a539442ef3562b71ab1f147a6/ofl/cinzel/Cinzel%5Bwght%5D.ttf"
FONT_SHA256 = "f4d83d34d1f6c741193e4acf4b3dff9531e5a67b6aa65228d00a7db72a4e0f34"
_project    = Path(__file__).parents[1].name.lower()
_xdg_cache  = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
FONT_CACHE  = _xdg_cache / _project / "Cinzel-Regular.ttf"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def ensure_font(custom: str | None) -> Path:
    if custom:
        path = Path(custom)
        if not path.exists():
            raise FileNotFoundError(f"Font not found: {path}")
        return path
    if not FONT_CACHE.exists():
        print(f"Downloading Cinzel → {FONT_CACHE}")
        FONT_CACHE.parent.mkdir(parents=True, exist_ok=True)
        tmp = FONT_CACHE.with_suffix(".tmp")
        try:
            urllib.request.urlretrieve(FONT_URL, tmp)
            digest = hashlib.sha256(tmp.read_bytes()).hexdigest()
            if digest != FONT_SHA256:
                raise ValueError(f"Font checksum mismatch: expected {FONT_SHA256}, got {digest}")
            tmp.rename(FONT_CACHE)
        except Exception:
            tmp.unlink(missing_ok=True)
            raise
    return FONT_CACHE


def centered_x(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.FreeTypeFont, width: int) -> int:
    bbox = draw.textbbox((0, 0), text, font=font)
    return int((width - (bbox[2] - bbox[0])) // 2)


def radial_vignette(img: Image.Image) -> Image.Image:
    """Blend a dark radial vignette over the image (Pillow-only, no numpy)."""
    cx, cy    = img.width / 2, img.height / 2
    mask      = Image.new("L", img.size, 0)
    mask_draw = ImageDraw.Draw(mask)
    steps = 80
    for i in range(steps, 0, -1):
        t     = i / steps
        alpha = int((1 - t ** 1.4) * 220)
        rx    = int(cx * t * 1.6)
        ry    = int(cy * t * 1.6)
        box   = [int(cx - rx), int(cy - ry), int(cx + rx), int(cy + ry)]
        mask_draw.ellipse(box, fill=alpha)
    vignette = Image.new("RGB", img.size, BG_DARK)
    return Image.composite(img, vignette, mask)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the Nexus Mods banner image.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--title",       default=DEFAULT_TITLE,    help="Main title text")
    parser.add_argument("--subtitle",    default=DEFAULT_SUBTITLE, help="Subtitle text")
    parser.add_argument("--tags",        default=DEFAULT_TAGS,     help="Tags line text")
    parser.add_argument("--font",        default=None,             metavar="PATH", help="Path to a .ttf font file (skips auto-download)")
    parser.add_argument("--output", "-o", default=str(DEFAULT_OUTPUT), metavar="PATH", help="Output .png path")
    def positive_int(v: str) -> int:
        n = int(v)
        if n <= 0:
            raise argparse.ArgumentTypeError("must be a positive integer")
        return n
    def font_size_int(v: str) -> int:
        n = int(v)
        if n < 30:
            raise argparse.ArgumentTypeError("must be >= 30 (tags font is base // 5, minimum 6pt)")
        return n
    parser.add_argument("--width",       default=DEFAULT_WIDTH,     type=positive_int,  help="Canvas width in pixels")
    parser.add_argument("--height",      default=DEFAULT_HEIGHT,    type=positive_int,  help="Canvas height in pixels")
    parser.add_argument("--font-size",   default=BASE_FONT_SIZE,    type=font_size_int, help="Base font size in points (subtitle and tags scale proportionally, min 30)")
    parser.add_argument("--no-vignette", action="store_true",       help="Disable radial vignette")
    return parser.parse_args()


def main() -> None:
    args      = parse_args()
    font_path = ensure_font(args.font)

    W, H = args.width, args.height
    base = args.font_size

    # Shrink font until title fits within the canvas (leaving DIVIDER_X_PAD on each side).
    # Stops at 30pt minimum; very long titles may still overflow at that size.
    max_title_w = W - DIVIDER_X_PAD * 2
    _tmp_draw   = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    title_font  = ImageFont.truetype(str(font_path), base)
    while base > 30:
        _bb = _tmp_draw.textbbox((0, 0), args.title, font=title_font)
        if (_bb[2] - _bb[0]) <= max_title_w:
            break
        base       = max(base - 2, 30)
        title_font = ImageFont.truetype(str(font_path), base)

    _bb = _tmp_draw.textbbox((0, 0), args.title, font=title_font)
    if (_bb[2] - _bb[0]) > max_title_w:
        print(
            f"warning: title still exceeds canvas width at minimum font size {base}pt — it will be clipped.",
            file=sys.stderr,
        )

    subtitle_font = ImageFont.truetype(str(font_path), base // 3)
    tags_font     = ImageFont.truetype(str(font_path), base // 5)

    img  = Image.new("RGB", (W, H), BG_CENTRE)
    draw = ImageDraw.Draw(img)

    # Measure font metrics for precise vertical placement.
    # "Q" is the only Cinzel glyph whose tail extends below the cap baseline.
    # _probe_cap measures the cap body; _probe_q finds how far Q's tail extends.
    _probe_cap  = draw.textbbox((0, 0), "ABCDEFHIKLMNOPRSTUVWXYZ", font=title_font)
    _probe_q    = draw.textbbox((0, 0), "Q",                       font=title_font)
    _probe_sub  = draw.textbbox((0, 0), args.subtitle,             font=subtitle_font)
    _probe_tags = draw.textbbox((0, 0), args.tags,                 font=tags_font)

    title_h     = int(_probe_cap[3]  - _probe_cap[1])   # cap body height
    tail_extra  = int(_probe_q[3]    - _probe_cap[3])   # extra pixels Q's tail adds
    sub_h       = int(_probe_sub[3]  - _probe_sub[1])
    tags_h      = int(_probe_tags[3] - _probe_tags[1])

    DIVIDER_PAD = 4   # gap between cap baseline and divider line
    SUB_PAD     = 8   # gap between divider (below Q tail) and subtitle
    TAGS_PAD    = 20  # gap between subtitle and tags

    # Total block height (title top → tags bottom)
    block_h = (title_h + DIVIDER_PAD + tail_extra + SUB_PAD + sub_h + TAGS_PAD + tags_h)

    # Vertically centre the block
    title_y    = (H - block_h) // 2 - int(_probe_cap[1])  # account for top bearing
    divider_y  = title_y + int(_probe_cap[1]) + title_h + DIVIDER_PAD
    subtitle_y = divider_y + tail_extra + SUB_PAD
    tags_y     = subtitle_y + sub_h + TAGS_PAD

    # Vignette applied to background before text
    if not args.no_vignette:
        img = radial_vignette(img)

    draw = ImageDraw.Draw(img)

    # Title
    x = centered_x(draw, args.title, title_font, W)
    draw.text((x, title_y), args.title, font=title_font, fill=GOLD)

    # Divider
    draw.line(
        [(DIVIDER_X_PAD, divider_y), (W - DIVIDER_X_PAD, divider_y)],
        fill=GOLD_DIM,
        width=1,
    )

    # Subtitle
    x = centered_x(draw, args.subtitle, subtitle_font, W)
    draw.text((x, subtitle_y), args.subtitle, font=subtitle_font, fill=GOLD)

    # Tags
    x = centered_x(draw, args.tags, tags_font, W)
    draw.text((x, tags_y), args.tags, font=tags_font, fill=GOLD_DIM)

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out, format="PNG", optimize=True)
    print(f"Saved → {out}")


if __name__ == "__main__":
    main()
