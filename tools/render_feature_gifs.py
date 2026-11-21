#!/usr/bin/env python3
"""
Render photorealistic GamaKay TK75 HE V2 ISO + pixel-accurate live TFT feature GIFs.

Hardware layout matches the physical TK75 HE V2 ISO PCB & top case:
  - Sculpted matte-black PBT ISO 75% keycaps (82 keys) with 3D dish shading,
    translucent shine-through legends, and switch-plate RGB bleed.
  - Vertical milky-white 8-LED diffused accent light bar (LEDs 82..89) recessed
    vertically between the main block (Del/Bksp/Enter) and the right nav column
    (Knob/Home/PgUp/PgDn), terminating just above the top-left of the TFT bezel.
  - Knurled CNC aluminum rotary knob in the top-right corner.
  - Recessed 0.85" 128x128 TFT screen at (row 4, col 14) directly below PgDn,
    above Right Arrow, and right of Up Arrow, plus a crisp 2x (256x256) callout.

Every TFT pixel and all 90 LEDs are produced directly by the real AT32F405 C
firmware modules (pixel_hud.c, rgb_effects.c, hall_engine.c, hall_cal.c,
macro_recorder.c, macro_player.c) and the authentic U24 ROM 6x12 bitmap font.
"""

from __future__ import annotations

import ctypes
import math
import pathlib
import random
import subprocess
import tempfile
from dataclasses import dataclass
from typing import Callable

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[1]
ASSETS_DIR = ROOT / "docs" / "assets"
U24_BIN = ROOT / "backups" / "stock_u24_lcd_v102_x5.bin"

CANVAS_W = 1060
CANVAS_H = 420

# Exact physical TK75 HE V2 ISO geometry (units = 1u).
# Note the real ~0.46u gap between the main block (ends at 15.0u) and the right
# nav column (starts at 15.46u), which houses the vertical 8-LED diffuser strip!
NAV_X = 15.48
KEY_DEFS: list[tuple[int, int, float, float, float, float, str, bool]] = [
    # Row 0 (F-row + Del; Knob sits at x=NAV_X, y=0)
    (0, 0, 0.00, 0.00, 1.0, 1.0, "ESC", False),
    (0, 1, 1.25, 0.00, 1.0, 1.0, "F1", False),
    (0, 2, 2.25, 0.00, 1.0, 1.0, "F2", False),
    (0, 3, 3.25, 0.00, 1.0, 1.0, "F3", False),
    (0, 4, 4.25, 0.00, 1.0, 1.0, "F4", False),
    (0, 5, 5.50, 0.00, 1.0, 1.0, "F5", False),
    (0, 6, 6.50, 0.00, 1.0, 1.0, "F6", False),
    (0, 7, 7.50, 0.00, 1.0, 1.0, "F7", False),
    (0, 8, 8.50, 0.00, 1.0, 1.0, "F8", False),
    (0, 9, 9.75, 0.00, 1.0, 1.0, "F9", False),
    (0, 10, 10.75, 0.00, 1.0, 1.0, "F10", False),
    (0, 11, 11.75, 0.00, 1.0, 1.0, "F11", False),
    (0, 12, 12.75, 0.00, 1.0, 1.0, "F12", False),
    (0, 13, 14.00, 0.00, 1.0, 1.0, "DEL", False),
    # Row 1 (Number row + Home)
    (1, 0, 0.00, 1.24, 1.0, 1.0, "`", False),
    (1, 1, 1.00, 1.24, 1.0, 1.0, "1", False),
    (1, 2, 2.00, 1.24, 1.0, 1.0, "2", False),
    (1, 3, 3.00, 1.24, 1.0, 1.0, "3", False),
    (1, 4, 4.00, 1.24, 1.0, 1.0, "4", False),
    (1, 5, 5.00, 1.24, 1.0, 1.0, "5", False),
    (1, 6, 6.00, 1.24, 1.0, 1.0, "6", False),
    (1, 7, 7.00, 1.24, 1.0, 1.0, "7", False),
    (1, 8, 8.00, 1.24, 1.0, 1.0, "8", False),
    (1, 9, 9.00, 1.24, 1.0, 1.0, "9", False),
    (1, 10, 10.00, 1.24, 1.0, 1.0, "0", False),
    (1, 11, 11.00, 1.24, 1.0, 1.0, "-", False),
    (1, 12, 12.00, 1.24, 1.0, 1.0, "=", False),
    (1, 13, 13.00, 1.24, 2.0, 1.0, "BKSP", False),
    (1, 14, NAV_X, 1.24, 1.0, 1.0, "HOM", False),
    # Row 2 (QWERTY + PgUp)
    (2, 0, 0.00, 2.24, 1.5, 1.0, "TAB", False),
    (2, 1, 1.50, 2.24, 1.0, 1.0, "Q", False),
    (2, 2, 2.50, 2.24, 1.0, 1.0, "W", False),
    (2, 3, 3.50, 2.24, 1.0, 1.0, "E", False),
    (2, 4, 4.50, 2.24, 1.0, 1.0, "R", False),
    (2, 5, 5.50, 2.24, 1.0, 1.0, "T", False),
    (2, 6, 6.50, 2.24, 1.0, 1.0, "Y", False),
    (2, 7, 7.50, 2.24, 1.0, 1.0, "U", False),
    (2, 8, 8.50, 2.24, 1.0, 1.0, "I", False),
    (2, 9, 9.50, 2.24, 1.0, 1.0, "O", False),
    (2, 10, 10.50, 2.24, 1.0, 1.0, "P", False),
    (2, 11, 11.50, 2.24, 1.0, 1.0, "[", False),
    (2, 12, 12.50, 2.24, 1.0, 1.0, "]", False),
    (2, 14, NAV_X, 2.24, 1.0, 1.0, "PGU", False),
    # Row 3 (ASDF + ISO # + ISO Enter + PgDn)
    (3, 0, 0.00, 3.24, 1.75, 1.0, "CAPS", False),
    (3, 1, 1.75, 3.24, 1.0, 1.0, "A", False),
    (3, 2, 2.75, 3.24, 1.0, 1.0, "S", False),
    (3, 3, 3.75, 3.24, 1.0, 1.0, "D", False),
    (3, 4, 4.75, 3.24, 1.0, 1.0, "F", False),
    (3, 5, 5.75, 3.24, 1.0, 1.0, "G", False),
    (3, 6, 6.75, 3.24, 1.0, 1.0, "H", False),
    (3, 7, 7.75, 3.24, 1.0, 1.0, "J", False),
    (3, 8, 8.75, 3.24, 1.0, 1.0, "K", False),
    (3, 9, 9.75, 3.24, 1.0, 1.0, "L", False),
    (3, 10, 10.75, 3.24, 1.0, 1.0, ";", False),
    (3, 11, 11.75, 3.24, 1.0, 1.0, "'", False),
    (3, 12, 12.75, 3.24, 1.0, 1.0, "#", False),
    (3, 13, 13.75, 2.24, 1.25, 2.0, "ENT", True),
    (3, 14, NAV_X, 3.24, 1.0, 1.0, "PGD", False),
    # Row 4 (ISO Shift + <> + ZXCV + Up; TFT sits right of Up at x=NAV_X, y=4.24!)
    (4, 0, 0.00, 4.24, 1.25, 1.0, "SHF", False),
    (4, 1, 1.25, 4.24, 1.0, 1.0, "<>", False),
    (4, 2, 2.25, 4.24, 1.0, 1.0, "Z", False),
    (4, 3, 3.25, 4.24, 1.0, 1.0, "X", False),
    (4, 4, 4.25, 4.24, 1.0, 1.0, "C", False),
    (4, 5, 5.25, 4.24, 1.0, 1.0, "V", False),
    (4, 6, 6.25, 4.24, 1.0, 1.0, "B", False),
    (4, 7, 7.25, 4.24, 1.0, 1.0, "N", False),
    (4, 8, 8.25, 4.24, 1.0, 1.0, "M", False),
    (4, 9, 9.25, 4.24, 1.0, 1.0, ",", False),
    (4, 10, 10.25, 4.24, 1.0, 1.0, ".", False),
    (4, 11, 11.25, 4.24, 1.0, 1.0, "/", False),
    (4, 12, 12.25, 4.24, 1.75, 1.0, "SHIFT", False),
    (4, 13, 14.32, 4.24, 1.0, 1.0, "^", False),
    # Row 5 (Modifiers + Space + Fn + Arrows; > sits right under TFT at x=NAV_X)
    (5, 0, 0.00, 5.24, 1.25, 1.0, "CTL", False),
    (5, 1, 1.25, 5.24, 1.25, 1.0, "WIN", False),
    (5, 2, 2.50, 5.24, 1.25, 1.0, "ALT", False),
    (5, 6, 3.75, 5.24, 6.25, 1.0, "", False),
    (5, 9, 10.00, 5.24, 1.0, 1.0, "ALT", False),
    (5, 10, 11.00, 5.24, 1.0, 1.0, "FN", False),
    (5, 11, 12.00, 5.24, 1.0, 1.0, "CTL", False),
    (5, 12, 13.32, 5.24, 1.0, 1.0, "<", False),
    (5, 13, 14.32, 5.24, 1.0, 1.0, "v", False),
    (5, 14, NAV_X, 5.24, 1.0, 1.0, ">", False),
]


def load_font(size: int, bold: bool = True) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = (
        [
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/truetype/roboto/unhinted/RobotoCondensed-Medium.ttf",
        ]
        if bold
        else [
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        ]
    )
    for p in candidates:
        if pathlib.Path(p).exists():
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


FONT_KEY = load_font(10, bold=True)
FONT_KEY_SM = load_font(8, bold=True)
FONT_TITLE = load_font(15, bold=True)
FONT_SUB = load_font(12, bold=False)
FONT_BADGE = load_font(11, bold=True)
FONT_TINY = load_font(9, bold=True)

# Pre-bake subtle desk & matte-black PBT chassis backdrop once for authentic texture + fast rendering
U_PX = 40.0
KB_X0 = 32
KB_Y0 = 66
KB_W = int((NAV_X + 1.0) * U_PX)
KB_H = int(6.24 * U_PX)
CASE_X0 = KB_X0 - 16
CASE_Y0 = KB_Y0 - 15
CASE_X1 = KB_X0 + KB_W + 16
CASE_Y1 = KB_Y0 + KB_H + 15
TFT_CARD_X = CASE_X1 + 36
TFT_CARD_Y = CASE_Y0 + 3
TFT_VIEW_SZ = 256


def build_base_backdrop() -> Image.Image:
    """Create photorealistic dark studio desk + textured matte-black TK75 chassis base."""
    img = Image.new("RGB", (CANVAS_W, CANVAS_H), (12, 14, 19))
    draw = ImageDraw.Draw(img)

    # Subtle studio overhead vignette desk gradient
    for y in range(CANVAS_H):
        t = y / CANVAS_H
        r = int(17 - 7 * t)
        g = int(19 - 7 * t)
        b = int(25 - 9 * t)
        draw.line([(0, y), (CANVAS_W, y)], fill=(r, g, b))

    # Soft multi-layer ambient drop shadow under the keyboard chassis
    sh = Image.new("RGB", (CANVAS_W, CANVAS_H), (0, 0, 0))
    sdraw = ImageDraw.Draw(sh)
    sdraw.rounded_rectangle([CASE_X0 + 6, CASE_Y0 + 12, CASE_X1 + 8, CASE_Y1 + 16], radius=18, fill=(24, 24, 28))
    sh = sh.filter(ImageFilter.GaussianBlur(radius=10))
    img = ImageChops.subtract(img, sh)
    draw = ImageDraw.Draw(img)

    # Matte textured black polycarbonate/aluminum TK75 HE V2 top case with chamfered bezel
    draw.rounded_rectangle([CASE_X0, CASE_Y0, CASE_X1, CASE_Y1], radius=14, fill=(22, 23, 27), outline=(56, 60, 68), width=2)
    # Top-edge studio light specular bevel
    draw.line([(CASE_X0 + 14, CASE_Y0 + 1), (CASE_X1 - 14, CASE_Y0 + 1)], fill=(86, 90, 100), width=1)
    # Bottom lip shadow bevel
    draw.line([(CASE_X0 + 14, CASE_Y1 - 1), (CASE_X1 - 14, CASE_Y1 - 1)], fill=(10, 11, 13), width=2)

    # Subtle anodized micro-grain texture on case deck (deterministic seed for zero flicker!)
    rng = random.Random(752025)
    px = img.load()
    for _ in range(9000):
        gx = rng.randint(CASE_X0 + 3, CASE_X1 - 3)
        gy = rng.randint(CASE_Y0 + 3, CASE_Y1 - 3)
        cr, cg, cb = px[gx, gy]
        d = rng.choice((-2, -1, 1, 2))
        px[gx, gy] = (max(0, min(255, cr + d)), max(0, min(255, cg + d)), max(0, min(255, cb + d)))

    # Deep recessed switch-plate cutouts (matching physical TK75 matte-black top frame)
    # Main block + nav cluster cavity
    draw.rounded_rectangle([KB_X0 - 4, KB_Y0 - 4, KB_X0 + int(15.0 * U_PX) + 4, KB_Y0 + KB_H + 4], radius=6, fill=(7, 8, 11), outline=(34, 36, 42), width=1)
    # Right nav column cavity (Home / PgUp / PgDn / TFT / >)
    nav_px = KB_X0 + int(NAV_X * U_PX)
    draw.rounded_rectangle([nav_px - 4, KB_Y0 + int(1.24 * U_PX) - 4, nav_px + int(U_PX) + 4, KB_Y0 + KB_H + 4], radius=6, fill=(7, 8, 11), outline=(34, 36, 42), width=1)
    # Small bridge between Up/Down/Right arrows
    draw.rectangle([KB_X0 + int(13.3 * U_PX), KB_Y0 + int(4.24 * U_PX) - 2, nav_px + int(U_PX), KB_Y0 + KB_H + 3], fill=(7, 8, 11))

    # Right-hand 2x TFT studio housing
    draw.rounded_rectangle(
        [TFT_CARD_X - 10, TFT_CARD_Y - 10, TFT_CARD_X + TFT_VIEW_SZ + 10, TFT_CARD_Y + TFT_VIEW_SZ + 10],
        radius=14,
        fill=(18, 20, 25),
        outline=(56, 64, 78),
        width=2,
    )
    draw.line([(TFT_CARD_X, TFT_CARD_Y - 9), (TFT_CARD_X + TFT_VIEW_SZ, TFT_CARD_Y - 9)], fill=(86, 94, 110), width=1)

    return img


BASE_BACKDROP = build_base_backdrop()


def build_sim_lib(build_dir: pathlib.Path) -> ctypes.CDLL:
    so_path = build_dir / "libtk75_sim.so"
    c_files = [
        ROOT / "tools/sim_fb_bridge.c",
        ROOT / "src/hall/hall_engine.c",
        ROOT / "src/hall/hall_cal.c",
        ROOT / "src/storage/flash_eeprom.c",
        ROOT / "src/keymap/keymap.c",
        ROOT / "src/keymap/macro_player.c",
        ROOT / "src/keymap/macro_recorder.c",
        ROOT / "src/rgb/rgb_effects.c",
        ROOT / "src/tft/pixel_hud.c",
    ]
    inc_dirs = [
        ROOT / "src/hall",
        ROOT / "src/storage",
        ROOT / "src/keymap",
        ROOT / "src/via",
        ROOT / "src/rgb",
        ROOT / "src/tft",
    ]
    cmd = [
        "gcc", "-std=c11", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra",
        *(f"-I{d}" for d in inc_dirs),
        *(str(f) for f in c_files),
        "-o", str(so_path),
    ]
    subprocess.run(cmd, check=True)
    lib = ctypes.CDLL(str(so_path))
    lib.sim_init.argtypes = [ctypes.c_char_p]
    lib.sim_set_rgb.argtypes = [ctypes.c_uint8] * 6
    lib.sim_set_side.argtypes = [ctypes.c_uint8] * 6
    lib.sim_set_caps_side.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
    lib.sim_set_tft_cfg.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]
    lib.sim_set_hall_cfg.argtypes = [ctypes.c_uint16, ctypes.c_uint8, ctypes.c_uint8]
    lib.sim_set_power_state.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]
    lib.sim_set_macro_edit.argtypes = [ctypes.c_uint8, ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
    lib.sim_set_macro_slot_used.argtypes = [ctypes.c_uint8]
    lib.sim_set_key_travel.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16]
    lib.sim_hud_open_menu.argtypes = [ctypes.c_uint32]
    lib.sim_hud_input.argtypes = [ctypes.c_uint8, ctypes.c_uint32]
    lib.sim_cal_mark_done.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
    lib.sim_step.argtypes = [
        ctypes.c_uint32, ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint32,
        ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint32,
        ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint8, ctypes.c_uint8,
        ctypes.c_uint32, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8,
        ctypes.c_void_p,
    ]
    lib.sim_get_tft_rgb.argtypes = [ctypes.c_void_p]
    lib.sim_get_tft_pwm.restype = ctypes.c_uint8
    lib.sim_get_leds_rgb.argtypes = [ctypes.c_void_p]
    lib.sim_led_idx.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
    lib.sim_led_idx.restype = ctypes.c_uint8
    return lib


@dataclass
class FrameMeta:
    title: str
    action: str
    pressed_keys: dict[tuple[int, int], float]
    knob_angle: float = 0.0
    knob_pressed: bool = False
    tft_powered: bool = True
    callout_label: str = "LIVE 128x128 TFT (2x)"


def apply_tft_brightness(tft_img: Image.Image, pwm_pct: int, powered: bool) -> Image.Image:
    if not powered:
        return Image.new("RGB", tft_img.size, (6, 7, 10))
    scale = max(0.08, min(1.0, 0.14 + 0.86 * (pwm_pct / 100.0)))
    if scale >= 0.99:
        return tft_img
    return tft_img.point(lambda p: int(p * scale))


def draw_knurled_knob(draw: ImageDraw.ImageDraw, cx: int, cy: int, rad: int, angle: float, pressed: bool) -> None:
    """Draw a photorealistic CNC anodized dark-gunmetal fluted rotary knob."""
    # Recessed knob collar
    collar_col = (56, 232, 255) if pressed else (68, 72, 82)
    draw.ellipse([cx - rad - 3, cy - rad - 3, cx + rad + 3, cy + rad + 3], fill=(9, 10, 13), outline=collar_col, width=2)
    # Knurled fluted outer teeth (18 ridges)
    for k in range(18):
        a = angle + (k * 2.0 * math.pi / 18.0)
        ox = cx + math.cos(a) * rad
        oy = cy + math.sin(a) * rad
        ix = cx + math.cos(a) * (rad - 3.5)
        iy = cy + math.sin(a) * (rad - 3.5)
        shade = 96 if (k & 1) == 0 else 44
        draw.line([(ix, iy), (ox, oy)], fill=(shade, shade + 4, shade + 10), width=2)
    # Brushed dark aluminum top cap with concentric chamfer highlight
    draw.ellipse([cx - rad + 3, cy - rad + 3, cx + rad - 3, cy + rad - 3], fill=(42, 46, 54), outline=(126, 134, 148), width=1)
    draw.ellipse([cx - rad + 6, cy - rad + 6, cx + rad - 6, cy + rad - 6], fill=(32, 35, 42), outline=(66, 72, 84), width=1)
    # Machined index mark showing real rotation
    nx0 = cx + int(math.cos(angle) * 4)
    ny0 = cy + int(math.sin(angle) * 4)
    nx1 = cx + int(math.cos(angle) * (rad - 5))
    ny1 = cy + int(math.sin(angle) * (rad - 5))
    draw.line([(nx0, ny0), (nx1, ny1)], fill=(56, 232, 255) if pressed else (210, 220, 232), width=2)


def draw_realistic_keycap(
    draw: ImageDraw.ImageDraw,
    x0: int, y0: int, x1: int, y1: int,
    lbl: str,
    kr: int, kg: int, kb: int,
    depth: float,
    is_iso: bool,
) -> None:
    """Draw one sculpted matte-black PBT keycap with 3D dish shading & shine-through legend."""
    dy = int(round(depth * 2.8))
    y0 += dy
    y1 += dy

    # Subtle bottom-skirt specular tint picked up from neighboring LED bleed
    edge_r = min(255, 36 + (kr * 85) // 255)
    edge_g = min(255, 38 + (kg * 85) // 255)
    edge_b = min(255, 44 + (kb * 85) // 255)

    skirt_col = (16, 17, 21) if depth < 0.1 else (22, 25, 31)
    dish_col = (26, 28, 34) if depth < 0.1 else (34, 39, 48)
    top_hi = (52, 55, 64) if depth < 0.1 else (66, 78, 94)
    if depth >= 0.35:
        edge_r = min(255, edge_r + 55)
        edge_g = min(255, edge_g + 85)
        edge_b = min(255, edge_b + 90)

    if is_iso:
        tx0 = x0 - int(0.25 * U_PX)
        mid_y = y0 + int(U_PX) - 2
        pts = [(tx0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, mid_y), (tx0, mid_y)]
        draw.polygon(pts, fill=skirt_col, outline=(edge_r, edge_g, edge_b))
        # Sculpted top dish
        draw.rectangle([tx0 + 4, y0 + 3, x1 - 4, mid_y - 3], fill=dish_col)
        draw.rectangle([x0 + 4, mid_y - 4, x1 - 4, y1 - 6], fill=dish_col)
        draw.line([(tx0 + 5, y0 + 3), (x1 - 5, y0 + 3)], fill=top_hi, width=1)
    else:
        # Outer tapered PBT skirt
        draw.rounded_rectangle([x0, y0, x1, y1], radius=5, fill=skirt_col, outline=(edge_r, edge_g, edge_b), width=1)
        # Inner sculpted concave keycap top face (shifted up 2px for genuine perspective!)
        draw.rounded_rectangle([x0 + 3, y0 + 2, x1 - 3, y1 - 6], radius=4, fill=dish_col)
        # Subtle top chamfer highlight & bottom inner dish bevel
        draw.line([(x0 + 6, y0 + 2), (x1 - 6, y0 + 2)], fill=top_hi, width=1)
        draw.line([(x0 + 6, y1 - 6), (x1 - 6, y1 - 6)], fill=(18, 20, 25), width=1)

    # Translucent shine-through PBT legend illuminated directly by the key's real LED!
    lum = max(kr, kg, kb)
    if lum > 14:
        # Boost core color saturation like real backlit double-shot PBT legends
        leg_col = (
            min(255, 36 + (kr * 219) // 255),
            min(255, 36 + (kg * 219) // 255),
            min(255, 36 + (kb * 219) // 255),
        )
    else:
        # Unlit dark translucent legend (barely visible graphite on real black PBT)
        leg_col = (72, 76, 86)

    if lbl:
        fnt = FONT_KEY if len(lbl) <= 2 else FONT_KEY_SM
        cx = (x0 + x1) // 2
        cy = (y0 + y1 - 5) // 2
        bbox = draw.textbbox((0, 0), lbl, font=fnt)
        tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        draw.text((cx - tw // 2, cy - th // 2 - 1), lbl, fill=leg_col, font=fnt)


def render_frame(
    lib: ctypes.CDLL,
    led_map: dict[tuple[int, int], int],
    meta: FrameMeta,
) -> Image.Image:
    tft_buf = (ctypes.c_uint8 * (128 * 128 * 3))()
    led_buf = (ctypes.c_uint8 * (90 * 3))()
    lib.sim_get_tft_rgb(tft_buf)
    lib.sim_get_leds_rgb(led_buf)
    tft_pwm = int(lib.sim_get_tft_pwm()) if meta.tft_powered else 0

    tft_raw = Image.frombytes("RGB", (128, 128), bytes(tft_buf))
    tft_lit = apply_tft_brightness(tft_raw, tft_pwm, meta.tft_powered)

    img = BASE_BACKDROP.copy()

    # Exact physical vertical 8-LED milky-white diffuser bar position!
    # Sits right in the gap between main block (15.00u) and nav column (NAV_X=15.48u),
    # vertically spanning from Row 0 (Del/Knob) down to Row 3 (Enter/PgDn) right above the TFT!
    bar_cx = KB_X0 + int(15.24 * U_PX)
    bar_x0 = bar_cx - 4
    bar_x1 = bar_cx + 4
    bar_y0 = KB_Y0 + int(0.32 * U_PX)
    bar_y1 = KB_Y0 + int(4.12 * U_PX)
    seg_h = (bar_y1 - bar_y0) / 8.0

    # Optical bloom layer: switch-plate per-key underglow + vertical 8-LED accent bar + TFT halo
    glow = Image.new("RGB", (CANVAS_W, CANVAS_H), (0, 0, 0))
    gdraw = ImageDraw.Draw(glow)

    # 1. Per-key switch-plate RGB glow (bleeds through gaps around every matte-black keycap)
    for r, c, xu, yu, wu, hu, _lbl, is_iso in KEY_DEFS:
        li = led_map.get((r, c), 255)
        if li >= 82:
            continue
        kr, kg, kb = led_buf[li * 3 : li * 3 + 3]
        if not (kr or kg or kb):
            continue
        x0 = KB_X0 + int(xu * U_PX)
        y0 = KB_Y0 + int(yu * U_PX)
        x1 = KB_X0 + int((xu + wu) * U_PX)
        y1 = KB_Y0 + int((yu + hu) * U_PX)
        if is_iso:
            gdraw.rectangle([x0 - int(0.25 * U_PX), y0, x1, y0 + int(U_PX)], fill=(kr, kg, kb))
            gdraw.rectangle([x0, y0 + int(U_PX), x1, y1], fill=(kr, kg, kb))
        else:
            gdraw.rounded_rectangle([x0, y0, x1, y1], radius=5, fill=(kr, kg, kb))

    # 2. Vertical 8-LED internal accent light bar (LEDs 82..89) soft diffuser bloom
    for i in range(8):
        sr, sg, sb = led_buf[(82 + i) * 3 : (82 + i) * 3 + 3]
        if sr or sg or sb:
            sy0 = int(bar_y0 + i * seg_h) - 2
            sy1 = int(bar_y0 + (i + 1) * seg_h) + 2
            gdraw.rounded_rectangle([bar_x0 - 5, sy0, bar_x1 + 5, sy1], radius=5, fill=(sr, sg, sb))

    # 3. Zoomed TFT backlight ambient spill
    halo_v = int(26 * (tft_pwm / 100.0)) if meta.tft_powered else 0
    if halo_v > 0:
        gdraw.rounded_rectangle(
            [TFT_CARD_X - 6, TFT_CARD_Y - 6, TFT_CARD_X + TFT_VIEW_SZ + 6, TFT_CARD_Y + TFT_VIEW_SZ + 6],
            radius=14,
            fill=(halo_v, int(halo_v * 1.35), int(halo_v * 1.75)),
        )

    # Composite realistic two-radius optical bloom (wide soft spill + tight bright plate glow)
    wide_blur = glow.filter(ImageFilter.GaussianBlur(radius=9))
    tight_blur = glow.filter(ImageFilter.GaussianBlur(radius=3.5))
    img = ImageChops.add(img, wide_blur, scale=1.8)
    img = ImageChops.add(img, tight_blur, scale=1.35)
    draw = ImageDraw.Draw(img)

    # Title bar
    draw.text((28, 12), "GAMAKAY TK75 HE V2 ISO  ·  OPEN VIA FIRMWARE", fill=(115, 130, 152), font=FONT_TINY)
    draw.text((28, 25), meta.title, fill=(241, 245, 249), font=FONT_TITLE)

    # Draw the recessed milky-white vertical 8-LED accent diffuser bar (EXACT real hardware!)
    draw.rounded_rectangle([bar_x0 - 2, bar_y0 - 2, bar_x1 + 2, bar_y1 + 2], radius=5, fill=(9, 10, 13), outline=(44, 48, 56), width=1)
    # Render smooth 1px vertical interpolation across the 8 LEDs inside the milky diffuser
    bar_strip = Image.new("RGB", (bar_x1 - bar_x0, bar_y1 - bar_y0), (156, 160, 166))
    bdraw = ImageDraw.Draw(bar_strip)
    bar_h = bar_y1 - bar_y0
    for y in range(bar_h):
        pos = (y / max(1, bar_h - 1)) * 7.0
        i0 = min(7, int(pos))
        i1 = min(7, i0 + 1)
        frac = pos - i0
        r0, g0, b0 = led_buf[(82 + i0) * 3 : (82 + i0) * 3 + 3]
        r1, g1, b1 = led_buf[(82 + i1) * 3 : (82 + i1) * 3 + 3]
        lr = int(r0 * (1.0 - frac) + r1 * frac)
        lg = int(g0 * (1.0 - frac) + g1 * frac)
        lb = int(b0 * (1.0 - frac) + b1 * frac)
        # Unlit milky-white polycarbonate base (matches top-left of real photo!) + bright LED core
        lum = max(lr, lg, lb)
        if lum > 8:
            # Bright diffused neon core with subtle frosted-bar highlight
            cr = min(255, int(lr * 0.96 + 42 * (lum / 255.0)))
            cg = min(255, int(lg * 0.96 + 42 * (lum / 255.0)))
            cb = min(255, int(lb * 0.96 + 42 * (lum / 255.0)))
        else:
            cr, cg, cb = (56, 60, 68)  # Frosted milky diffuser in dark studio lighting
        bdraw.line([(0, y), (bar_x1 - bar_x0 - 1, y)], fill=(cr, cg, cb))
    # Mask diffuser bar with crisp rounded pill ends
    mask = Image.new("L", bar_strip.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, bar_strip.size[0] - 1, bar_strip.size[1] - 1], radius=4, fill=255)
    img.paste(bar_strip, (bar_x0, bar_y0), mask)
    draw = ImageDraw.Draw(img)

    # Draw all 82 photorealistic sculpted matte-black PBT keycaps
    for r, c, xu, yu, wu, hu, lbl, is_iso in KEY_DEFS:
        li = led_map.get((r, c), 255)
        kr, kg, kb = led_buf[li * 3 : li * 3 + 3] if li < 82 else (0, 0, 0)
        x0 = KB_X0 + int(xu * U_PX) + 2
        y0 = KB_Y0 + int(yu * U_PX) + 2
        x1 = KB_X0 + int((xu + wu) * U_PX) - 2
        y1 = KB_Y0 + int((yu + hu) * U_PX) - 2
        draw_realistic_keycap(draw, x0, y0, x1, y1, lbl, kr, kg, kb, meta.pressed_keys.get((r, c), 0.0), is_iso)

    # Top-right knurled CNC rotary knob at (NAV_X, 0)
    kx = KB_X0 + int((NAV_X + 0.50) * U_PX)
    ky = KB_Y0 + int(0.50 * U_PX)
    draw_knurled_knob(draw, kx, ky, 18, meta.knob_angle, meta.knob_pressed)

    # Embedded recessed 0.85" square TFT display at exact real position (row 4, col 14)
    mini_x = KB_X0 + int(NAV_X * U_PX) + 1
    mini_y = KB_Y0 + int(4.24 * U_PX) + 1
    mini_sz = int(U_PX) - 2
    draw.rounded_rectangle([mini_x - 3, mini_y - 3, mini_x + mini_sz + 3, mini_y + mini_sz + 3], radius=4, fill=(8, 9, 12), outline=(62, 68, 80), width=1)
    img.paste(tft_lit.resize((mini_sz, mini_sz), Image.Resampling.NEAREST), (mini_x, mini_y))
    draw = ImageDraw.Draw(img)

    # Subtle callout connector from embedded TFT -> crisp 2x live TFT viewport
    draw.line(
        [(mini_x + mini_sz + 4, mini_y + mini_sz // 2), (TFT_CARD_X - 11, TFT_CARD_Y + TFT_VIEW_SZ // 2)],
        fill=(48, 136, 166),
        width=2,
    )

    # 2x Live TFT viewport with recessed dark bezel
    draw.rectangle(
        [TFT_CARD_X - 2, TFT_CARD_Y - 2, TFT_CARD_X + TFT_VIEW_SZ + 1, TFT_CARD_Y + TFT_VIEW_SZ + 1],
        outline=(8, 10, 14),
        width=2,
    )
    img.paste(tft_lit.resize((TFT_VIEW_SZ, TFT_VIEW_SZ), Image.Resampling.NEAREST), (TFT_CARD_X, TFT_CARD_Y))
    draw = ImageDraw.Draw(img)

    # Subtle glass reflection sheen corner on 2x TFT
    draw.line([(TFT_CARD_X + 1, TFT_CARD_Y + 1), (TFT_CARD_X + TFT_VIEW_SZ - 1, TFT_CARD_Y + 1)], fill=(45, 58, 76), width=1)

    # TFT footer caption + live PWM indicator
    draw.text((TFT_CARD_X, TFT_CARD_Y + TFT_VIEW_SZ + 15), meta.callout_label, fill=(148, 163, 184), font=FONT_TINY)
    pwm_txt = f"PWM {tft_pwm:3d}%" if meta.tft_powered else "TFT OFF"
    pwm_col = (56, 232, 255) if meta.tft_powered else (255, 92, 108)
    draw.text((TFT_CARD_X + TFT_VIEW_SZ - 56, TFT_CARD_Y + TFT_VIEW_SZ + 15), pwm_txt, fill=pwm_col, font=FONT_TINY)

    # Bottom action telemetry bar
    bar_b_y0 = CASE_Y1 + 18
    draw.rounded_rectangle([CASE_X0, bar_b_y0, TFT_CARD_X + TFT_VIEW_SZ + 10, CANVAS_H - 12], radius=10, fill=(16, 20, 30), outline=(42, 52, 74), width=1)
    draw.rounded_rectangle([CASE_X0 + 10, bar_b_y0 + 7, CASE_X0 + 70, bar_b_y0 + 27], radius=6, fill=(18, 72, 92))
    draw.text((CASE_X0 + 18, bar_b_y0 + 11), "LIVE", fill=(56, 232, 255), font=FONT_BADGE)
    draw.text((CASE_X0 + 82, bar_b_y0 + 10), meta.action, fill=(226, 232, 240), font=FONT_SUB)

    return img


def save_gif(frames: list[Image.Image], path: pathlib.Path, duration_ms: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sample = Image.new("RGB", (CANVAS_W, CANVAS_H * min(6, len(frames))))
    step = max(1, len(frames) // 6)
    for idx, fi in enumerate(range(0, len(frames), step)[:6]):
        sample.paste(frames[fi], (0, idx * CANVAS_H))
    pal_img = sample.quantize(colors=144, method=Image.Quantize.MEDIANCUT)
    p_frames = [f.quantize(palette=pal_img, dither=Image.Dither.NONE) for f in frames]
    p_frames[0].save(
        path,
        save_all=True,
        append_images=p_frames[1:],
        optimize=True,
        duration=duration_ms,
        loop=0,
        disposal=2,
    )
    kb = path.stat().st_size / 1024.0
    print(f"Wrote {path.relative_to(ROOT)} ({len(frames)} frames, {kb:.1f} KB)")


def make_typed_bmp(keycodes: list[int]) -> bytes:
    bmp = bytearray(32)
    for kc in keycodes:
        if 0 <= kc < 256:
            bmp[kc >> 3] |= 1 << (kc & 7)
    return bytes(bmp)


def gen_01_live_home_hall(lib: ctypes.CDLL, led_map: dict[tuple[int, int], int]) -> None:
    lib.sim_reset()
    lib.sim_set_rgb(1, 225, 4, 140, 165, 255)  # HE_ANALOG_DEPTH cobalt->cyan->mint
    lib.sim_set_side(1, 190, 1, 130, 160, 255)  # Rainbow flow on vertical accent bar
    lib.sim_set_hall_cfg(1800, 1, 1)            # 1.8mm act, RT ON, SOCD Last
    lib.sim_set_tft_cfg(90, 0, 0)

    seq = [((2, 2), "W"), ((3, 1), "A"), ((3, 2), "S"), ((3, 3), "D"), ((5, 6), "SPACE")]
    frames: list[Image.Image] = []
    t = 1000
    wpm = 48
    for step in range(44):
        lib.sim_clear_all_keys()
        pressed: dict[tuple[int, int], float] = {}
        k_idx = (step // 8) % len(seq)
        sub = step % 8
        profile = [0.0, 0.30, 0.62, 0.95, 1.0, 0.65, 0.25, 0.0][sub]
        (kr, kc), kname = seq[k_idx]
        um = int(profile * 4000)
        if um > 0:
            lib.sim_set_key_travel(kr, kc, um)
            pressed[(kr, kc)] = profile
        if sub == 3:
            wpm = min(124, wpm + 14)
        lib.sim_step(t, wpm, 124, 420 + step, 0, 0, 1, 0, 0, 0, 0, 0, 0, 21, 45, step % 60, None)
        mm_s = f"{um / 1000.0:.1f} mm"
        act_tag = "ACTUATED!" if um >= 1800 else "tracking analog depth"
        frames.append(render_frame(lib, led_map, FrameMeta(
            title="Live 8 kHz Hall-Effect Switch HUD & Analog Travel",
            action=f"Key [{kname}] travel {mm_s} ({act_tag})  |  RGB: Hall Depth + Rapid Trigger ON",
            pressed_keys=pressed,
        )))
        t += 70
    save_gif(frames, ASSETS_DIR / "01_live_home_hall_switch.gif", duration_ms=95)


def gen_02_tft_menu_brightness_lighting(lib: ctypes.CDLL, led_map: dict[tuple[int, int], int]) -> None:
    lib.sim_reset()
    lib.sim_set_rgb(1, 225, 2, 140, 170, 255)  # Solid cobalt blue
    lib.sim_set_side(1, 200, 2, 140, 128, 255)  # Solid cyan vertical accent bar
    lib.sim_set_tft_cfg(100, 0, 0)

    frames: list[Image.Image] = []
    t = 2000
    angle = 0.0

    def snap(action: str, pressed: dict[tuple[int, int], float], knob_press: bool = False, repeats: int = 3):
        nonlocal t
        for _ in range(repeats):
            lib.sim_step(t, 0, 88, 120, 0, 0, 1, 0, 0, 0, 0, 0, 0, 21, 45, 10, None)
            frames.append(render_frame(lib, led_map, FrameMeta(
                title="TFT Menu: Live Brightness (PWM/RGB/SID) & Lighting (Colors + Effects)",
                action=action,
                pressed_keys=pressed,
                knob_angle=angle,
                knob_pressed=knob_press,
            )))
            t += 65

    snap("HOME Screen — Press Fn + Knob Push to open 6-page TFT Menu", {}, repeats=3)
    lib.sim_hud_open_menu(t)
    snap("Fn + Knob Push -> BRIGHTNESS (TFT / RGB / SID / CAP)", {(5, 10): 0.9}, knob_press=True, repeats=4)

    # Dim & brighten TFT PWM live
    lib.sim_hud_input(3, t)  # edit TFT
    for _ in range(4):
        angle -= 0.45
        lib.sim_hud_input(1, t)
        lib.sim_hud_input(1, t)
        snap("BRIGHTNESS -> TFT: Live smooth 0..100% PWM backlight dimming", {}, repeats=2)
    for _ in range(3):
        angle += 0.45
        lib.sim_hud_input(2, t)
        lib.sim_hud_input(2, t)
        snap("BRIGHTNESS -> TFT: Restoring bright TFT PWM backlight", {}, repeats=2)
    lib.sim_hud_input(3, t)

    # Switch to LIGHTING page with smooth 3-step cyan pixel wipe
    angle -= 0.4
    lib.sim_hud_input(1, t)  # row 0 tab bar
    angle += 0.5
    lib.sim_hud_input(2, t)  # BRIGHTNESS -> LIGHTING
    for _ in range(4):
        snap("Tab switch -> LIGHTING (FX / COL / SFX / SCOL + live color swatches)", {}, repeats=1)

    # Cycle FX (SOLID -> BREATH -> HE-MAP -> WAVE -> SOLID)
    lib.sim_hud_input(3, t)  # row 1 (FX)
    lib.sim_hud_input(3, t)  # edit FX
    for label in ["LIGHTING -> FX: Breathing", "LIGHTING -> FX: Hall Depth", "LIGHTING -> FX: Rainbow Wave", "LIGHTING -> FX: Solid"]:
        angle += 0.5
        lib.sim_hud_input(2, t)
        snap(label, {}, repeats=3)
    lib.sim_hud_input(3, t)

    # Row 2: COL (live RGB color + RGB565 swatch)
    angle += 0.4
    lib.sim_hud_input(2, t)  # row 2 (COL)
    lib.sim_hud_input(3, t)  # edit COL
    for cname in ["PURPLE", "PINK", "RED", "GREEN", "CYAN"]:
        angle += 0.5
        lib.sim_hud_input(2, t)
        snap(f"LIGHTING -> COL: {cname} (per-key LEDs + live TFT swatch)", {}, repeats=2)
    lib.sim_hud_input(3, t)

    # Row 4: SCOL (vertical accent bar color + swatch)
    lib.sim_hud_input(2, t)  # row 3 (SFX)
    lib.sim_hud_input(2, t)  # row 4 (SCOL)
    lib.sim_hud_input(3, t)  # edit SCOL
    for cname in ["BLUE", "PURPLE", "RED", "ORANGE", "GREEN"]:
        angle += 0.5
        lib.sim_hud_input(2, t)
        snap(f"LIGHTING -> SCOL: {cname} on vertical 8-LED accent bar", {}, repeats=2)

    save_gif(frames, ASSETS_DIR / "02_tft_menu_brightness_lighting.gif", duration_ms=115)


def gen_03_caps_lock_accent_bar(lib: ctypes.CDLL, led_map: dict[tuple[int, int], int]) -> None:
    lib.sim_reset()
    lib.sim_set_rgb(1, 180, 2, 128, 170, 255)  # Cobalt blue keys (so Caps fallback uses vibrant 180)
    lib.sim_set_side(0, 0, 1, 165, 0, 255)     # Normal vertical accent bar OFF (0%)! Vivid red hue
    lib.sim_set_caps_side(1, 1)                # Caps Lock trigger ON = BREATHING
    lib.sim_set_tft_cfg(90, 0, 0)

    frames: list[Image.Image] = []
    t = 3000
    angle = 0.0

    def step_frames(n: int, host_leds: int, action: str, pressed: dict[tuple[int, int], float], dt: int = 80):
        nonlocal t
        for _ in range(n):
            lib.sim_step(t, 0, 95, 310, host_leds, 0, 1, 0, 0, 0, 0, 0, 0, 21, 46, 5, None)
            frames.append(render_frame(lib, led_map, FrameMeta(
                title="Caps Lock -> Vertical 8-LED Accent Bar Trigger & Effects",
                action=action,
                pressed_keys=pressed,
                knob_angle=angle,
            )))
            t += dt

    step_frames(6, 0x00, "Vertical accent bar is OFF (SID=0%) — Press Caps Lock to trigger", {})
    step_frames(2, 0x02, "Caps Lock ON -> Vertical 8-LED bar lights up + CAP badge on TFT!", {(3, 0): 1.0})
    step_frames(8, 0x02, "Caps Lock ON -> Effect: BREATHING (works even with SID & RGB off)", {})

    lib.sim_hud_open_menu(t)
    lib.sim_hud_input(2, t)  # row 2
    lib.sim_hud_input(2, t)  # row 3
    lib.sim_hud_input(2, t)  # row 4 (CAP)
    lib.sim_hud_input(3, t)  # edit CAP

    for fx_name in ["WAVE (Vertical Rainbow Flow)", "BLINK (2 Hz Alert Flash)", "SCAN (Vertical KITT Scanner)"]:
        angle += 0.55
        lib.sim_hud_input(2, t)
        step_frames(10, 0x02, f"BRIGHTNESS -> CAP: {fx_name} on vertical accent bar", {}, dt=85)

    lib.sim_hud_close_menu()
    step_frames(2, 0x00, "Caps Lock OFF -> Vertical accent bar cleanly powers back OFF", {(3, 0): 0.9})
    step_frames(5, 0x00, "Caps Lock OFF -> Back to clean idle state", {})

    save_gif(frames, ASSETS_DIR / "03_caps_lock_sidelight.gif", duration_ms=105)


def gen_04_macro_record_playback(lib: ctypes.CDLL, led_map: dict[tuple[int, int], int]) -> None:
    lib.sim_reset()
    lib.sim_set_rgb(1, 185, 2, 128, 170, 255)  # Cobalt blue base
    lib.sim_set_side(1, 165, 2, 128, 128, 255)
    lib.sim_set_tft_cfg(90, 0, 0)

    frames: list[Image.Image] = []
    t = 4000

    # Phase 1: Hold Fn + C -> Fullscreen retro pixel-art cassette recorder + blinking red Fn/C keys!
    rec_seq = [
        (0, {(5, 10): 0.85, (4, 4): 0.9}, "Hold Fn + C (1.8s) -> Red blink + Fullscreen Cassette Recorder!"),
        (3, {(3, 6): 0.9}, "Recording M0 (C): typing [H] (host HID suppressed, reels spinning)"),
        (6, {(2, 3): 0.9}, "Recording M0 (C): typing [E]  |  Live EVTS count & buffer meter"),
        (9, {(3, 9): 0.9}, "Recording M0 (C): typing [L]  |  Native VIA v3 SS_TAP bytecode"),
        (12, {(3, 9): 0.9}, "Recording M0 (C): typing [L]"),
        (15, {(2, 9): 0.9}, "Recording M0 (C): typing [O]"),
    ]
    for b_len, pkeys, msg in rec_seq:
        for _ in range(3):
            lib.sim_step(t, 0, 90, 500, 0, 0, 1, 1, 0, b_len, 0, 0, 0, 21, 47, 1, None)
            frames.append(render_frame(lib, led_map, FrameMeta(
                title="Xtrfy K5-Style Onboard Macro Recorder, Speed & Live Glow",
                action=msg,
                pressed_keys=pkeys,
            )))
            t += 135

    # Phase 2: 1st Fn tap -> Opens Onboard TFT Macro Text Editor ("EDIT M0")!
    tok_hello = bytes([0x0B, 0x02, 0x08, 0x00, 0x0F, 0x00, 0x0F, 0x00, 0x12, 0x00])         # "Hello"
    tok_hello_excl = bytes([0x0B, 0x02, 0x08, 0x00, 0x0F, 0x00, 0x0F, 0x00, 0x12, 0x00, 0x1E, 0x02])  # "Hello!"
    edit_stages = [
        (tok_hello, 5, 5, {(5, 10): 0.9}, "1st Fn Tap -> Opens Onboard TFT Macro Text Editor (EDIT M0)!"),
        (tok_hello, 5, 4, {(5, 12): 0.9}, "Edit like Notepad: Left/Right/Knob moves cursor + #POS inspector"),
        (tok_hello_excl, 6, 6, {(4, 0): 0.85, (1, 1): 0.9}, "Type any key/combo -> Inserts [!] live at cursor (2 KB pool!)"),
    ]
    for tbytes, cnt, cur, pk, msg in edit_stages:
        lib.sim_set_macro_edit(1, ctypes.c_char_p(tbytes), cnt, cur)
        for _ in range(4):
            lib.sim_step(t, 0, 90, 500, 0, 0, 1, 1, 0, cnt * 3, 0, 0, 0, 21, 47, 2, None)
            frames.append(render_frame(lib, led_map, FrameMeta(
                title="Xtrfy K5 Macro Recorder, TFT Text Editor & Live Glow",
                action=msg,
                pressed_keys=pk,
            )))
            t += 130
    lib.sim_set_macro_edit(0, None, 0, 0)

    # Phase 3: 2nd Fn tap -> Confirms & saves edited macro to VIA M0 (mint flash!)
    lib.sim_set_macro_slot_used(0)
    for _ in range(4):
        lib.sim_step(t, 0, 90, 500, 0, 0, 1, 0, 0, 0, 1, 0, 0, 21, 47, 2, None)
        frames.append(render_frame(lib, led_map, FrameMeta(
            title="Xtrfy K5 Macro Recorder, TFT Text Editor & Live Glow",
            action="2nd Fn Tap -> Confirmed & Saved to VIA M0 (Knob Push cancels)",
            pressed_keys={(5, 10): 0.9},
        )))
        t += 100

    # Phase 3: Tap Fn+C -> Plays back with live complementary per-letter RGB ripple across H-E-L-L-O!
    play_letters = [(0x0B, "H"), (0x08, "E"), (0x0F, "L"), (0x0F, "L"), (0x12, "O")]
    for kc, ch in play_letters:
        bmp = make_typed_bmp([kc])
        for sub in range(3):
            ptr = ctypes.c_char_p(bmp) if sub == 0 else None
            lib.sim_step(t, 96, 96, 505, 0, 0, 1, 0, 0, 0, 0, 0, 0, 21, 47, 3, ptr)
            frames.append(render_frame(lib, led_map, FrameMeta(
                title="Xtrfy K5-Style Onboard Macro Recorder, Speed & Live Glow",
                action=f"Tap Fn + C -> Playing M0: live complementary RGB glow on [{ch}]!",
                pressed_keys={},
            )))
            t += 75

    # Phase 4: Speed shortcuts Fn+. (FAST) and Fn+, (SLOW) showing live TFT header toast
    for toast_id, lbl, pk in [
        (1, "Fn + . -> Macro Speed FAST (2ms tap / 0.25x delay) header toast", {(5, 10): 0.8, (4, 10): 0.9}),
        (4, "Fn + , -> Macro Speed SLOW (50ms tap / 4x delay) header toast", {(5, 10): 0.8, (4, 9): 0.9}),
    ]:
        for _ in range(5):
            lib.sim_step(t, 96, 96, 505, 0, 1, 3, 0, 0, 0, 0, toast_id, 0, 21, 47, 4, None)
            frames.append(render_frame(lib, led_map, FrameMeta(
                title="Xtrfy K5-Style Onboard Macro Recorder, Speed & Live Glow",
                action=lbl,
                pressed_keys=pk,
            )))
            t += 85

    save_gif(frames, ASSETS_DIR / "04_macro_record_playback.gif", duration_ms=115)


def gen_05_hall_calibration(lib: ctypes.CDLL, led_map: dict[tuple[int, int], int]) -> None:
    lib.sim_reset()
    lib.sim_set_rgb(1, 220, 2, 128, 170, 255)
    lib.sim_set_side(1, 150, 2, 128, 128, 255)
    lib.sim_set_tft_cfg(90, 0, 0)

    frames: list[Image.Image] = []
    t = 5000
    lib.sim_cal_start()

    cal_keys = [
        (3, 1, "A"), (3, 2, "S"), (3, 3, "D"), (3, 4, "F"), (2, 2, "W"), (3, 13, "ENT"), (5, 6, "SPC"),
    ]
    for _ in range(4):
        lib.sim_step(t, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 21, 48, 0, None)
        frames.append(render_frame(lib, led_map, FrameMeta(
            title="Interactive Onboard 82-Key Hall Calibration",
            action="HALL SWITCH -> CAL START: Uncalibrated keys glow BLUE (host HID paused)",
            pressed_keys={},
        )))
        t += 80

    for kr, kc, kname in cal_keys:
        for depth in [0.25, 0.65, 1.0, 1.0, 0.0]:
            lib.sim_clear_all_keys()
            pressed = {}
            if depth > 0:
                lib.sim_set_key_travel(kr, kc, int(depth * 4000))
                pressed[(kr, kc)] = depth
            lib.sim_step(t, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 21, 48, 5, None)
            state_txt = (
                f"Pressing [{kname}] -> CYAN-WHITE + live TFT keycap & FORCE bar ({int(depth * 100)}%)"
                if depth > 0
                else f"Released [{kname}] -> Turns MINT GREEN (per-key full-travel span locked!)"
            )
            frames.append(render_frame(lib, led_map, FrameMeta(
                title="Interactive Onboard 82-Key Hall Calibration",
                action=state_txt,
                pressed_keys=pressed,
            )))
            t += 85

    for r in range(6):
        for c in range(15):
            if (r, c) != (0, 0):
                lib.sim_cal_mark_done(r, c)
    for step_i, depth in enumerate([0.0, 1.0, 0.0]):
        lib.sim_clear_all_keys()
        pressed = {}
        if depth > 0:
            lib.sim_set_key_travel(0, 0, 4000)
            pressed[(0, 0)] = 1.0
        lib.sim_step(t, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 21, 48, 12, None)
        msg = (
            "82/82 complete -> Auto-exits & saves exact per-key spans to flash EEPROM!"
            if step_i == 2
            else "81/82 MINT GREEN -> Press final key [ESC] (or push knob anytime to abort)"
        )
        frames.append(render_frame(lib, led_map, FrameMeta(
            title="Interactive Onboard 82-Key Hall Calibration",
            action=msg,
            pressed_keys=pressed,
        )))
        t += 120

    save_gif(frames, ASSETS_DIR / "05_hall_calibration.gif", duration_ms=115)


def gen_06_screensaver_wake(lib: ctypes.CDLL, led_map: dict[tuple[int, int], int]) -> None:
    lib.sim_reset()
    lib.sim_set_rgb(1, 150, 3, 100, 128, 255)  # Calm cyan breathing
    lib.sim_set_side(1, 160, 3, 100, 170, 255)
    lib.sim_set_hall_cfg(1800, 1, 0)
    lib.sim_set_tft_cfg(85, 1, 0)

    frames: list[Image.Image] = []
    t = 6000

    for _ in range(5):
        lib.sim_step(t, 64, 118, 1420, 0, 0, 1, 0, 0, 0, 0, 0, 1000, 21, 45, 28, None)
        frames.append(render_frame(lib, led_map, FrameMeta(
            title="Zero-Software Pixel-Art Idle Screensaver & Instant Wake",
            action="HOME Dashboard — After 30s idle, pixel-art clock & system info starts",
            pressed_keys={},
        )))
        t += 120

    for s in range(18):
        sec = (29 + s // 2) % 60
        if s < 6:
            lib.sim_set_power_state(1, 1, 86)
            p_msg = "Idle Screensaver: Smear-free clock + USB Charging (bolt) + USB 8K + RT"
        elif s < 12:
            lib.sim_set_power_state(1, 0, 100)
            p_msg = "Idle Screensaver: USB Full 100% (cyan plug) + CV load-compensated gauge"
        else:
            lib.sim_set_power_state(0, 0, 94)
            p_msg = "Idle Screensaver: Battery Mode (clean icon, 2.4G 1K) + Peak WPM & keys"
        lib.sim_step(t, 0, 118, 1420, 0, 0, 1, 0, 0, 0, 0, 0, 31000 + s * 500, 21, 45, sec, None)
        frames.append(render_frame(lib, led_map, FrameMeta(
            title="Zero-Software Pixel-Art Idle Screensaver & Instant Wake",
            action=p_msg,
            pressed_keys={},
        )))
        t += 520
    lib.sim_set_power_state(1, 0, 100)

    lib.sim_set_key_travel(3, 1, 3200)
    for k in range(6):
        if k == 2:
            lib.sim_clear_all_keys()
        lib.sim_step(t, 78, 118, 1421, 0, 0, 1, 0, 0, 0, 0, 0, 0, 21, 45, 38, None)
        frames.append(render_frame(lib, led_map, FrameMeta(
            title="Zero-Software Pixel-Art Idle Screensaver & Instant Wake",
            action="Any key press wakes instantly back to live WPM & Hall switch HOME!",
            pressed_keys={(3, 1): 0.85} if k < 2 else {},
        )))
        t += 90

    save_gif(frames, ASSETS_DIR / "06_screensaver_wake.gif", duration_ms=145)


def gen_07_layers_rgb_effects(lib: ctypes.CDLL, led_map: dict[tuple[int, int], int]) -> None:
    lib.sim_reset()
    lib.sim_set_macro_slot_used(0)  # C
    lib.sim_set_macro_slot_used(1)  # V
    lib.sim_set_macro_slot_used(2)  # B
    lib.sim_set_rgb(1, 225, 2, 160, 170, 255)
    lib.sim_set_side(1, 195, 1, 160, 128, 255)
    lib.sim_set_tft_cfg(90, 0, 0)

    frames: list[Image.Image] = []
    t = 7000

    stages = [
        (0, 1, 0, 2, {}, "Layer 0 (Base): Solid Cobalt Blue + Rainbow vertical accent bar"),
        (1, 3, 0, 2, {(5, 10): 0.9}, "Layer 1 (Hold Fn): 25% dim base + 120° triad contrast Fn & Macros (C/V/B)"),
        (2, 5, 1, 2, {(5, 9): 0.8}, "Layer 2 (macOS): Exact 180° Complementary highlight + MAC badge on TFT"),
        (3, 9, 0, 2, {(5, 11): 0.8}, "Layer 3 (Custom): Triadic Complementary highlight on active binds"),
        (0, 1, 0, 1, {}, "Matrix Effect: Smooth 60 FPS Rainbow Wave across all 82 Hall keys"),
    ]
    for layer, lmask, mac, fx, pk, msg in stages:
        lib.sim_set_tft_cfg(90, 0, mac)
        lib.sim_set_rgb(1, 225, fx, 180, 170, 255)
        for _ in range(9):
            lib.sim_step(t, 0, 110, 880, 0, layer, lmask, 0, 0, 0, 0, 0, 0, 21, 50, 0, None)
            frames.append(render_frame(lib, led_map, FrameMeta(
                title="Per-Layer RGB Indicators & Matrix Effects",
                action=msg,
                pressed_keys=pk,
            )))
            t += 110

    save_gif(frames, ASSETS_DIR / "07_layers_rgb_effects.gif", duration_ms=115)


def gen_08_hall_macro_system_tft_power(lib: ctypes.CDLL, led_map: dict[tuple[int, int], int]) -> None:
    lib.sim_reset()
    lib.sim_set_macro_slot_used(0)  # C
    lib.sim_set_macro_slot_used(2)  # B
    lib.sim_set_rgb(1, 195, 2, 140, 128, 255)
    lib.sim_set_side(1, 180, 2, 140, 170, 255)
    lib.sim_set_hall_cfg(1800, 0, 0)
    lib.sim_set_tft_cfg(85, 1, 0)

    frames: list[Image.Image] = []
    t = 8000
    angle = 0.0

    def snap(action: str, pressed: dict[tuple[int, int], float], powered: bool = True, repeats: int = 3):
        nonlocal t
        for _ in range(repeats):
            lib.sim_step(t, 0, 102, 640, 0, 0, 1, 0, 0, 0, 0, 0, 0, 21, 52, 0, None)
            frames.append(render_frame(lib, led_map, FrameMeta(
                title="Onboard Hall Switch, Macro & System Menus + Fn+F4 TFT Power",
                action=action,
                pressed_keys=pressed,
                knob_angle=angle,
                tft_powered=powered,
            )))
            t += 70

    # Open menu -> jump to HALL SWITCH page (BRIGHTNESS -> LIGHTING -> HALL SWITCH)
    lib.sim_hud_open_menu(t)
    lib.sim_hud_input(1, t)  # row 0
    lib.sim_hud_input(2, t)  # LIGHTING
    lib.sim_hud_input(2, t)  # HALL SWITCH
    snap("HALL SWITCH: Tune Actuation (ACT), Rapid Trigger (RT), SOCD & CAL", {}, repeats=4)

    # Adjust ACT from 1.8mm -> 1.0mm
    lib.sim_hud_input(3, t)  # row 1 (ACT)
    lib.sim_hud_input(3, t)  # edit ACT
    for _ in range(4):
        angle -= 0.45
        lib.sim_hud_input(1, t)
        lib.sim_hud_input(1, t)
        snap("HALL SWITCH -> ACT: Live actuation tuning (0.10mm .. 3.90mm)", {}, repeats=2)
    lib.sim_hud_input(3, t)

    # Enable RT -> 0.4mm
    lib.sim_hud_input(2, t)  # row 2 (RT)
    lib.sim_hud_input(3, t)  # edit RT
    angle += 0.45
    lib.sim_hud_input(2, t)
    angle += 0.45
    lib.sim_hud_input(2, t)
    snap("HALL SWITCH -> RT: Rapid Trigger enabled at 0.4mm sensitivity", {}, repeats=4)
    lib.sim_hud_input(3, t)

    # Switch to MACRO page (complete 3-step wipe + hold clean MACRO page)
    lib.sim_hud_input(1, t)  # row 1
    lib.sim_hud_input(1, t)  # row 0
    angle += 0.5
    lib.sim_hud_input(2, t)  # MACRO
    snap("Smooth wipe -> MACRO Page (SPD / STEP / interactive KEYS)", {}, repeats=3)
    snap("MACRO Page: View speed (SPD/STEP) & trigger saved C/V/B/N/M macros", {}, repeats=7)

    # Switch to SYSTEM page (complete 3-step wipe + hold clean SYSTEM page)
    angle += 0.5
    lib.sim_hud_input(2, t)  # SYSTEM
    snap("Smooth wipe -> SYSTEM Page (SPD / OS / SAVR / PIXEL<->STOCK)", {}, repeats=3)
    snap("SYSTEM Page: RGB/SID Speed, WIN/MAC OS, Screensaver & PIXEL<->STOCK", {}, repeats=7)

    # Return to HOME & demonstrate Fn+F4 clean 0V TFT power off -> power on!
    lib.sim_hud_close_menu()
    snap("Returning to HOME dashboard...", {}, repeats=3)
    snap("HOME Screen — Press Fn + F4 to power-gate TFT rail cleanly OFF", {}, repeats=4)
    snap("Fn + F4 -> TFT rail + UART7 driven to 0V (clean power off)", {(5, 10): 0.85, (0, 4): 0.9}, powered=False, repeats=6)
    snap("Fn + F4 -> Cold-POR power-on restores full HUD & saved 85% PWM!", {(5, 10): 0.85, (0, 4): 0.9}, powered=True, repeats=6)

    save_gif(frames, ASSETS_DIR / "08_hall_macro_system_power.gif", duration_ms=115)


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        lib = build_sim_lib(pathlib.Path(tmp))
        u24_arg = str(U24_BIN).encode() if U24_BIN.exists() else None
        lib.sim_init(u24_arg)

        led_map = {(r, c): int(lib.sim_led_idx(r, c)) for r, c, *_ in KEY_DEFS}

        generators: list[Callable[[ctypes.CDLL, dict[tuple[int, int], int]], None]] = [
            gen_01_live_home_hall,
            gen_02_tft_menu_brightness_lighting,
            gen_03_caps_lock_accent_bar,
            gen_04_macro_record_playback,
            gen_05_hall_calibration,
            gen_06_screensaver_wake,
            gen_07_layers_rgb_effects,
            gen_08_hall_macro_system_tft_power,
        ]
        for fn in generators:
            fn(lib, led_map)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
