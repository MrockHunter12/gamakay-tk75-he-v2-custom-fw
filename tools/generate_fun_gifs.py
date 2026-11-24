#!/usr/bin/env python3
"""
Generate 5 fun 128x128 animated GIFs for the GamaKay TK75 HE V2 TFT screensaver:
  1. assets/gifs/funny_eyes.gif   - Pitch-black bg, glossy cartoon eyes looking around & making funny faces
  2. assets/gifs/bongo_cat.gif    - Cute desk cat slapping a glowing RGB mechanical keyboard + music notes
  3. assets/gifs/party_parrot.gif - Rainbow head-banging party parrot on black bg
  4. assets/gifs/nyan_space.gif   - Retro rainbow space cat flying through twinkling stars
  5. assets/gifs/coffee_afk.gif   - Cozy neon espresso mug with animated rising steam & AFK glow

All GIFs are strictly <= 30 frames so they fit natively in U9 @ 0x6C0000..0x7BFFFF with zero dropped frames!
"""

from __future__ import annotations

import math
import pathlib
from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "assets" / "gifs"


def _save_gif(frames: list[Image.Image], path: pathlib.Path, duration_ms: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pal_frames = [f.convert("P", palette=Image.Palette.ADAPTIVE, colors=255) for f in frames]
    pal_frames[0].save(
        path,
        save_all=True,
        append_images=pal_frames[1:],
        duration=duration_ms,
        loop=0,
        disposal=2,
        optimize=False,
    )
    print(f"Generated {path.relative_to(ROOT)} ({len(frames)} frames, {duration_ms}ms, {path.stat().st_size} B)")


def _draw_simple_eye(
    draw: ImageDraw.ImageDraw,
    cx: int,
    cy: int,
    px_off: float,
    py_off: float,
    rx: int = 23,
    ry: int = 31,
    top_lid: int = 0,
    bot_lid: int = 0,
    brow_tilt: int = 0,
) -> None:
    """Clean minimal white cartoon eye with glossy black pupil & white catchlight on pitch-black bg."""
    draw.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=(252, 253, 255))

    px = int(round(cx + px_off))
    py = int(round(cy + py_off))
    draw.ellipse((px - 10, py - 12, px + 10, py + 12), fill=(0, 0, 0))
    draw.ellipse((px - 6, py - 8, px - 1, py - 3), fill=(255, 255, 255))
    draw.ellipse((px + 2, py + 3, px + 5, py + 6), fill=(235, 240, 248))

    if top_lid > 0:
        if brow_tilt == 0:
            draw.rectangle((cx - rx - 2, cy - ry - 2, cx + rx + 2, cy - ry + top_lid), fill=(0, 0, 0))
        else:
            y_l = cy - ry + top_lid + brow_tilt
            y_r = cy - ry + top_lid - brow_tilt
            draw.polygon(
                [(cx - rx - 3, cy - ry - 4), (cx + rx + 3, cy - ry - 4), (cx + rx + 3, y_r), (cx - rx - 3, y_l)],
                fill=(0, 0, 0),
            )
    if bot_lid > 0:
        draw.rectangle((cx - rx - 2, cy + ry - bot_lid, cx + rx + 2, cy + ry + 2), fill=(0, 0, 0))


def gen_funny_eyes() -> None:
    """30-frame clean minimal moving/side-eye/blinking cartoon eyes on pitch-black background."""
    frames: list[Image.Image] = []
    lcx, rcx, cy = 36, 92, 64

    # 30 unique frames @ 145 ms (4.35s seamless loop): pure moving eyes, side-eye & blinking
    #  0-1: calm center
    #  2-4: look right -> far right -> up-right
    #  5-6: blink shut -> open
    #  7-9: look left -> far left -> down-left
    # 10-11: quick blink -> open
    # 12-17: suspicious narrow side-eye (left -> pause -> slide right -> hold right)
    # 18-20: look straight up -> top-right -> bottom-left
    # 21-23: quick funny cross-eye glance -> blink
    # 24-26: look right-down -> look left-up -> glance right
    # 27-29: double blink -> settle back to center
    keyframes = [
        # (ldx, ldy, rdx, rdy, top_lid, bot_lid, l_tilt, r_tilt, blink_bar)
        (0,   0,   0,   0,   0,  0,  0,  0, False),  # 0: center
        (1,  -1,   1,  -1,   0,  0,  0,  0, False),  # 1: subtle settle
        (8,  -1,   8,  -1,   0,  0,  0,  0, False),  # 2: glide right
        (12, -2,  12,  -2,   0,  0,  0,  0, False),  # 3: look right
        (11, -6,  11,  -6,   0,  0,  0,  0, False),  # 4: look up-right
        (0,   0,   0,   0,   0,  0,  0,  0, True),   # 5: blink
        (0,   0,   0,   0,  14, 10,  0,  0, False),  # 6: blink open
        (-8, -1,  -8,  -1,   0,  0,  0,  0, False),  # 7: glide left
        (-12,-2, -12,  -2,   0,  0,  0,  0, False),  # 8: look left
        (-11, 5, -11,   5,   0,  0,  0,  0, False),  # 9: look down-left
        (0,   0,   0,   0,   0,  0,  0,  0, True),   # 10: blink
        (-2,  0,  -2,   0,  12,  8,  0,  0, False),  # 11: blink open
        (-10, 2, -10,   2,  21, 15, -3,  3, False),  # 12: side-eye left
        (-12, 2, -12,   2,  23, 15, -3,  3, False),  # 13: deeper side-eye left
        (-9,  2,  -9,   2,  22, 16, -3,  3, False),  # 14: hold side-eye left
        (4,   2,   4,   2,  22, 15, -3,  3, False),  # 15: slide across
        (11,  2,  11,   2,  21, 15, -3,  3, False),  # 16: side-eye right
        (12,  2,  12,   2,  23, 16, -3,  3, False),  # 17: hold side-eye right
        (0, -11,   0, -11,   0,  0,  0,  0, False),  # 18: look up
        (9,  -9,   9,  -9,   0,  0,  0,  0, False),  # 19: look top-right
        (-9,  9,  -9,   9,   0,  0,  0,  0, False),  # 20: look bottom-left
        (10,  2, -10,   2,   0,  0,  0,  0, False),  # 21: quick cross-eye
        (11,  3, -11,   3,   0,  0,  0,  0, False),  # 22: cross-eye hold
        (0,   0,   0,   0,   0,  0,  0,  0, True),   # 23: blink reset
        (9,   6,   9,   6,   0,  0,  0,  0, False),  # 24: look down-right
        (-9, -6,  -9,  -6,   0,  0,  0,  0, False),  # 25: look up-left
        (6,   0,   6,   0,   0,  0,  0,  0, False),  # 26: quick right check
        (0,   0,   0,   0,   0,  0,  0,  0, True),   # 27: double blink 1
        (0,   0,   0,   0,  16, 12,  0,  0, False),  # 28: half-open
        (-1,  0,  -1,   0,   0,  0,  0,  0, False),  # 29: calm center
    ]

    for ldx, ldy, rdx, rdy, tl, bl, lt, rt, blink_bar in keyframes:
        img = Image.new("RGB", (128, 128), (0, 0, 0))
        d = ImageDraw.Draw(img)
        if blink_bar:
            d.rounded_rectangle((14, 62, 58, 67), radius=2, fill=(252, 253, 255))
            d.rounded_rectangle((70, 62, 114, 67), radius=2, fill=(252, 253, 255))
        else:
            _draw_simple_eye(d, lcx, cy, ldx, ldy, top_lid=tl, bot_lid=bl, brow_tilt=lt)
            _draw_simple_eye(d, rcx, cy, rdx, rdy, top_lid=tl, bot_lid=bl, brow_tilt=rt)
        frames.append(img)

    _save_gif(frames, OUT_DIR / "funny_eyes.gif", duration_ms=145)


def gen_bongo_cat() -> None:
    """16-frame Bongo Cat slapping a glowing RGB mechanical keyboard on black bg."""
    frames: list[Image.Image] = []
    rgb_cols = [
        (56, 232, 255),
        (54, 249, 152),
        (255, 184, 48),
        (255, 70, 140),
        (178, 102, 255),
        (255, 95, 75),
    ]
    for f in range(16):
        img = Image.new("RGB", (128, 128), (0, 0, 0))
        d = ImageDraw.Draw(img)
        beat = f % 4
        left_dn = beat in (0, 3)
        right_dn = beat in (2, 3)
        blink = f == 14
        bob = -2 if beat == 1 else 0

        # Wagging tail on right
        tx = 114 if (f % 4 < 2) else 107
        d.line((96, 86, tx, 64), fill=(245, 248, 255), width=7)
        d.ellipse((tx - 4, 60, tx + 4, 68), fill=(245, 248, 255))

        # Cat ears
        d.polygon([(29, 44 + bob), (35, 18 + bob), (50, 38 + bob)], fill=(250, 252, 255))
        d.polygon([(33, 40 + bob), (37, 25 + bob), (46, 38 + bob)], fill=(255, 155, 195))
        d.polygon([(99, 44 + bob), (93, 18 + bob), (78, 38 + bob)], fill=(250, 252, 255))
        d.polygon([(95, 40 + bob), (91, 25 + bob), (82, 38 + bob)], fill=(255, 155, 195))

        # Cat round head + body
        d.ellipse((24, 30 + bob, 104, 104 + bob), fill=(250, 252, 255))
        d.rectangle((24, 66 + bob, 104, 96), fill=(250, 252, 255))

        # Eyes
        if blink:
            d.line((41, 56 + bob, 52, 56 + bob), fill=(20, 22, 30), width=3)
            d.line((76, 56 + bob, 87, 56 + bob), fill=(20, 22, 30), width=3)
        else:
            d.ellipse((42, 50 + bob, 52, 62 + bob), fill=(16, 18, 26))
            d.ellipse((44, 52 + bob, 47, 55 + bob), fill=(255, 255, 255))
            d.ellipse((76, 50 + bob, 86, 62 + bob), fill=(16, 18, 26))
            d.ellipse((78, 52 + bob, 81, 55 + bob), fill=(255, 255, 255))

        # Rosy cheeks & cute 'w' mouth
        d.ellipse((32, 63 + bob, 43, 69 + bob), fill=(255, 140, 185))
        d.ellipse((85, 63 + bob, 96, 69 + bob), fill=(255, 140, 185))
        d.arc((55, 60 + bob, 65, 70 + bob), start=0, end=180, fill=(25, 25, 35), width=2)
        d.arc((63, 60 + bob, 73, 70 + bob), start=0, end=180, fill=(25, 25, 35), width=2)

        # Desk & RGB mechanical keyboard
        d.rectangle((0, 96, 128, 98), fill=(230, 240, 255))
        d.rectangle((0, 98, 128, 128), fill=(18, 22, 34))
        d.rounded_rectangle((28, 84, 100, 98), radius=3, fill=(36, 42, 60), outline=(95, 115, 150))
        for k in range(6):
            lit = (left_dn and k < 3) or (right_dn and k >= 3)
            col = rgb_cols[(k + f) % len(rgb_cols)] if lit else (65, 75, 100)
            kx = 33 + k * 11
            d.rounded_rectangle((kx, 87, kx + 8, 94), radius=2, fill=col)

        # Left & right paws
        if left_dn:
            d.ellipse((28, 76, 50, 92), fill=(255, 255, 255), outline=(200, 210, 225))
            d.arc((22, 74, 56, 94), start=190, end=350, fill=(56, 232, 255), width=2)
        else:
            d.ellipse((10, 62, 30, 79), fill=(255, 255, 255))
            d.ellipse((15, 67, 24, 74), fill=(255, 155, 195))

        if right_dn:
            d.ellipse((78, 76, 100, 92), fill=(255, 255, 255), outline=(200, 210, 225))
            d.arc((72, 74, 106, 94), start=190, end=350, fill=(255, 70, 140), width=2)
        else:
            d.ellipse((98, 62, 118, 79), fill=(255, 255, 255))
            d.ellipse((104, 67, 113, 74), fill=(255, 155, 195))

        # Floating colorful musical notes
        ny = 22 - (f % 4) * 4
        if left_dn:
            d.ellipse((8, ny + 6, 15, ny + 12), fill=(56, 232, 255))
            d.line((14, ny + 9, 14, ny - 2, 20, ny), fill=(56, 232, 255), width=2)
        if right_dn:
            d.ellipse((108, ny + 6, 115, ny + 12), fill=(255, 95, 165))
            d.line((114, ny + 9, 114, ny - 2, 120, ny), fill=(255, 95, 165), width=2)

        frames.append(img)

    _save_gif(frames, OUT_DIR / "bongo_cat.gif", duration_ms=110)


def gen_party_parrot() -> None:
    """16-frame rainbow head-banging Party Parrot on pure black background."""
    frames: list[Image.Image] = []
    palette = [
        (255, 60, 80),
        (255, 150, 35),
        (255, 235, 50),
        (65, 245, 115),
        (45, 225, 255),
        (95, 120, 255),
        (205, 80, 255),
        (255, 75, 195),
    ]
    for f in range(16):
        img = Image.new("RGB", (128, 128), (0, 0, 0))
        d = ImageDraw.Draw(img)
        ang = (f / 16.0) * 2.0 * math.pi
        hx = int(round(math.cos(ang) * 11))
        hy = int(round(math.sin(ang) * 7))
        col = palette[f % len(palette)]
        col2 = palette[(f + 3) % len(palette)]

        # Disco sparkles around parrot
        for s in range(6):
            sa = ang + s * (math.pi / 3.0)
            sx = int(64 + math.cos(sa) * 52)
            sy = int(64 + math.sin(sa) * 52)
            sc = palette[(f + s) % len(palette)]
            d.line((sx - 3, sy, sx + 3, sy), fill=sc, width=2)
            d.line((sx, sy - 3, sx, sy + 3), fill=sc, width=2)

        # Parrot torso & head crest
        d.ellipse((32 + hx // 2, 58 + hy // 2, 96 + hx // 2, 124), fill=col)
        d.ellipse((36 + hx, 24 + hy, 92 + hx, 82 + hy), fill=col)
        # Cute top feather tuft
        d.polygon([(56 + hx, 26 + hy), (62 + hx, 10 + hy), (72 + hx, 26 + hy)], fill=col2)
        d.polygon([(66 + hx, 28 + hy), (78 + hx, 14 + hy), (80 + hx, 30 + hy)], fill=col2)
        # Wing accent
        d.ellipse((34 + hx // 2, 72 + hy // 2, 64 + hx // 2, 108), fill=col2)
        # Big expressive eyes
        d.ellipse((46 + hx, 40 + hy, 60 + hx, 54 + hy), fill=(255, 255, 255))
        d.ellipse((51 + hx, 44 + hy, 58 + hx, 51 + hy), fill=(10, 10, 15))
        d.ellipse((68 + hx, 40 + hy, 82 + hx, 54 + hy), fill=(255, 255, 255))
        d.ellipse((73 + hx, 44 + hy, 80 + hx, 51 + hy), fill=(10, 10, 15))
        # Yellow curved beak
        d.polygon(
            [(62 + hx, 52 + hy), (96 + hx, 58 + hy), (78 + hx, 70 + hy), (60 + hx, 64 + hy)],
            fill=(255, 215, 45),
        )
        d.polygon([(62 + hx, 62 + hy), (82 + hx, 66 + hy), (66 + hx, 72 + hy)], fill=(230, 150, 20))
        frames.append(img)

    _save_gif(frames, OUT_DIR / "party_parrot.gif", duration_ms=90)


def gen_nyan_space() -> None:
    """20-frame retro rainbow-trail space cat flying through twinkling stars."""
    frames: list[Image.Image] = []
    stripes = [
        (255, 40, 50),
        (255, 155, 25),
        (255, 240, 45),
        (55, 235, 95),
        (45, 175, 255),
        (165, 80, 255),
    ]
    stars = [(12, 18), (44, 12), (96, 16), (116, 30), (22, 106), (62, 114), (108, 102), (82, 24)]
    for f in range(20):
        img = Image.new("RGB", (128, 128), (2, 4, 14))
        d = ImageDraw.Draw(img)

        # Scrolling twinkling space stars
        for idx, (sx0, sy) in enumerate(stars):
            sx = (sx0 - f * 6) % 128
            r = 2 if ((f + idx) % 3 == 0) else 1
            col = (255, 255, 255) if (idx % 2 == 0) else (120, 220, 255)
            d.line((sx - r, sy, sx + r, sy), fill=col, width=1)
            d.line((sx, sy - r, sx, sy + r), fill=col, width=1)

        # Wavy 6-stripe rainbow trail
        for seg in range(7):
            x0 = seg * 10
            wave = 3 if ((seg + (f // 2)) % 2 == 0) else -3
            for si, scol in enumerate(stripes):
                y0 = 46 + si * 6 + wave
                d.rectangle((x0, y0, x0 + 10, y0 + 5), fill=scol)

        # Bouncing Pop-Tart cat body
        by = 44 + (2 if (f % 4 < 2) else -2)
        # Tail
        ty = by + 20 + (4 if (f % 2 == 0) else -4)
        d.line((52, by + 22, 42, ty), fill=(155, 160, 175), width=5)
        # Pastry body + pink frosting + sprinkles
        d.rounded_rectangle((52, by, 96, by + 36), radius=5, fill=(245, 200, 130), outline=(40, 30, 25))
        d.rounded_rectangle((56, by + 4, 92, by + 32), radius=4, fill=(255, 130, 205))
        for spx, spy in ((62, 10), (74, 14), (66, 24), (82, 22), (78, 9)):
            d.rectangle((spx, by + spy, spx + 2, by + spy + 2), fill=(255, 40, 120))
        # Animated little paws
        po = 3 if (f % 2 == 0) else -2
        for px in (56, 68, 84, 94):
            d.rectangle((px + po, by + 35, px + po + 5, by + 41), fill=(160, 165, 180))
        # Cute grey cat head on right
        hx = 86 + (1 if (f % 4 < 2) else 0)
        d.polygon([(hx + 2, by + 8), (hx + 5, by - 4), (hx + 12, by + 8)], fill=(160, 165, 180))
        d.polygon([(hx + 22, by + 8), (hx + 29, by - 4), (hx + 32, by + 8)], fill=(160, 165, 180))
        d.rounded_rectangle((hx, by + 6, hx + 34, by + 32), radius=7, fill=(165, 170, 185))
        d.ellipse((hx + 7, by + 13, hx + 12, by + 18), fill=(10, 10, 15))
        d.ellipse((hx + 22, by + 13, hx + 27, by + 18), fill=(10, 10, 15))
        d.rectangle((hx + 4, by + 20, hx + 9, by + 24), fill=(255, 120, 165))
        d.rectangle((hx + 25, by + 20, hx + 30, by + 24), fill=(255, 120, 165))
        d.line((hx + 13, by + 22, hx + 17, by + 25, hx + 21, by + 22), fill=(20, 20, 25), width=2)

        frames.append(img)

    _save_gif(frames, OUT_DIR / "nyan_space.gif", duration_ms=100)


def gen_coffee_afk() -> None:
    """20-frame neon cozy coffee mug with rising animated steam & pulsing AFK sign."""
    frames: list[Image.Image] = []
    for f in range(20):
        img = Image.new("RGB", (128, 128), (0, 0, 0))
        d = ImageDraw.Draw(img)
        phase = (f / 20.0) * 2.0 * math.pi

        # 3 curling animated steam wisps rising from mug
        for wi, wx0 in enumerate((42, 58, 74)):
            pts = []
            for seg in range(14):
                sy = 54 - seg * 3
                sx = wx0 + math.sin(phase + seg * 0.45 + wi * 1.6) * 5.0
                pts.append((sx, sy))
            alpha_boost = int(170 + 75 * math.sin(phase + wi))
            col = (60, min(255, alpha_boost), 255) if (wi == 1) else (190, 235, 255)
            d.line(pts, fill=col, width=3)

        # Mug handle
        d.arc((82, 62, 108, 96), start=270, end=90, fill=(255, 185, 65), width=6)
        # Mug body
        d.rounded_rectangle((28, 56, 90, 104), radius=9, fill=(38, 44, 62), outline=(255, 185, 65), width=3)
        # Rich coffee rim
        d.ellipse((32, 58, 86, 68), fill=(115, 58, 22))
        d.ellipse((36, 60, 74, 65), fill=(175, 98, 42))
        # Saucer glow
        d.rounded_rectangle((18, 102, 100, 108), radius=3, fill=(255, 185, 65))

        # Pulsing neon AFK / BRB badge on mug front
        glow = 0.5 + 0.5 * math.sin(phase)
        txt_col = (int(80 + 175 * glow), int(220 + 35 * glow), int(150 + 105 * glow))
        label = "AFK" if (f < 10) else "BRB"
        d.rounded_rectangle((41, 74, 77, 92), radius=4, fill=(14, 18, 28), outline=txt_col, width=2)
        d.text((49, 78), label, fill=txt_col)

        # Bottom subtle status bar
        d.text((22, 113), "COFFEE BREAK", fill=(110, 165, 210))
        frames.append(img)

    _save_gif(frames, OUT_DIR / "coffee_afk.gif", duration_ms=130)


def main() -> int:
    gen_funny_eyes()
    gen_bongo_cat()
    gen_party_parrot()
    gen_nyan_space()
    gen_coffee_afk()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
