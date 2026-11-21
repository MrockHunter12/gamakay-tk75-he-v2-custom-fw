#!/usr/bin/env python3
"""
GamaKay TK75 HE V2 Live TFT Framebuffer Controller (requires patched U24 firmware).

Commands (0x20..0x25 over AT32 VIA Ext ch=8 val_id=7 -> UART7):
  - `demo`       : Render live custom HUD demo on the 128x128 TFT (zero flash wear)
  - `text`       : Draw ASCII text at (x, y) with 1x/2x/3x scale & RGB colors
  - `fill`       : Fill rectangle (x, y, w, h) with RGB color
  - `image`      : Stream PNG/JPG directly into 128x128 RAM framebuffer (instant, 0 flash wear)
  - `brightness` : Set TFT backlight level (0..4)
  - `stock`      : Exit manual mode and restore stock GamaKay clock/menu UI
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import time

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from tk75_tft_flash import TftUartBridge  # noqa: E402


def rgb888_to_rgb565(r: int, g: int, b: int) -> tuple[int, int]:
    val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)
    return (val >> 8) & 0xFF, val & 0xFF


def parse_hex_color(color: str) -> tuple[int, int]:
    c = color.strip().lstrip("#")
    if len(c) != 6:
        raise ValueError(f"Expected #RRGGBB color, got {color!r}")
    return rgb888_to_rgb565(int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16))


class TftCanvas:
    """High-level live drawing client for patched U24 (YC3123-Q) firmware."""

    def __init__(self, bridge: TftUartBridge) -> None:
        self.bridge = bridge

    def patch_version(self) -> tuple[int, int]:
        rx_cmd, pay = self.bridge.exchange(0x25, b"", timeout_s=0.5)
        if rx_cmd != 0xA5 or len(pay) < 3 or pay[0] != 0x00:
            raise RuntimeError(f"Patched U24 not responding to 0x25: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")
        return pay[1], pay[2]

    def set_manual_mode(self, enabled: bool) -> None:
        rx_cmd, pay = self.bridge.exchange(0x20, bytes([1 if enabled else 0]), timeout_s=0.5)
        if rx_cmd != 0xA0 or not pay or pay[0] != 0x00:
            raise RuntimeError(f"0x20 failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")

    def fill_rect(self, x: int, y: int, w: int, h: int, color: tuple[int, int], flush: bool = False) -> None:
        payload = bytes([x & 0xFF, y & 0xFF, w & 0xFF, h & 0xFF, color[0], color[1], 1 if flush else 0])
        rx_cmd, pay = self.bridge.exchange(0x21, payload, timeout_s=0.5)
        if rx_cmd != 0xA1 or not pay or pay[0] != 0x00:
            raise RuntimeError(f"0x21 failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")

    def draw_text(
        self,
        x: int,
        y: int,
        text: str,
        fg: tuple[int, int],
        bg: tuple[int, int] = (0, 0),
        scale: int = 1,
        transparent: bool = False,
        flush: bool = False,
    ) -> None:
        raw = text.encode("ascii", errors="replace")[:64]
        flags = (max(1, min(3, scale)) & 0x03) | (0x40 if transparent else 0) | (0x80 if flush else 0)
        payload = bytes([x & 0xFF, y & 0xFF, fg[0], fg[1], bg[0], bg[1], flags]) + raw
        rx_cmd, pay = self.bridge.exchange(0x23, payload, timeout_s=0.5)
        if rx_cmd != 0xA3 or not pay or pay[0] != 0x00:
            raise RuntimeError(f"0x23 failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")

    def blit_rgb565(
        self,
        x: int,
        y: int,
        w: int,
        h: int,
        rgb565_be: bytes,
        flush: bool = True,
    ) -> None:
        """Stream row-major BE RGB565 rectangle into U24 RAM framebuffer (44 px max per UART frame)."""
        max_px = 44
        for row in range(h):
            row_bytes = rgb565_be[row * w * 2 : (row + 1) * w * 2]
            col = 0
            while col < w:
                n_px = min(max_px, w - col)
                chunk = row_bytes[col * 2 : (col + n_px) * 2]
                is_last = flush and (row == h - 1) and (col + n_px == w)
                flags = 0x02 if is_last else 0x00
                payload = bytes([(x + col) & 0xFF, (y + row) & 0xFF, n_px & 0xFF, 1, flags]) + chunk
                rx_cmd, pay = self.bridge.exchange(0x22, payload, timeout_s=0.5)
                if rx_cmd != 0xA2 or not pay or pay[0] != 0x00:
                    raise RuntimeError(f"0x22 failed at ({x+col},{y+row}): cmd=0x{rx_cmd:02X}")
                col += n_px

    def flush(self, brightness: int = 0xFF) -> None:
        rx_cmd, pay = self.bridge.exchange(0x24, bytes([brightness & 0xFF]), timeout_s=0.5)
        if rx_cmd != 0xA4 or not pay or pay[0] != 0x00:
            raise RuntimeError(f"0x24 failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")


def run_demo(canvas: TftCanvas) -> None:
    major, minor = canvas.patch_version()
    print(f"Patched U24 alive! custom patch v{major}.{minor}")

    bg = rgb888_to_rgb565(10, 14, 24)
    cyan = rgb888_to_rgb565(0, 230, 255)
    white = rgb888_to_rgb565(255, 255, 255)
    green = rgb888_to_rgb565(50, 255, 130)
    amber = rgb888_to_rgb565(255, 190, 40)
    bar_bg = rgb888_to_rgb565(30, 38, 56)

    canvas.set_manual_mode(True)
    canvas.fill_rect(0, 0, 128, 128, bg)
    canvas.fill_rect(0, 0, 128, 22, rgb888_to_rgb565(18, 45, 78))
    canvas.draw_text(8, 5, "TK75 HE V2", cyan, bg, scale=1, transparent=True)
    canvas.draw_text(84, 5, "CUSTOM", green, bg, scale=1, transparent=True)

    canvas.draw_text(10, 30, "FULL", white, bg, scale=2, transparent=True)
    canvas.draw_text(10, 56, "CTRL!", amber, bg, scale=2, transparent=True)

    canvas.draw_text(10, 88, "ACT: 0.4mm RT:ON", white, bg, scale=1, transparent=True)
    canvas.fill_rect(10, 104, 108, 12, bar_bg)
    canvas.fill_rect(12, 106, 78, 8, green, flush=True)
    print("Demo rendered to 128x128 RAM framebuffer (0 flash wear)!")


def stream_image(canvas: TftCanvas, path: pathlib.Path, x: int, y: int, w: int, h: int) -> None:
    from PIL import Image

    with Image.open(path) as im:
        rgb = im.convert("RGB").resize((w, h), Image.Resampling.LANCZOS)
        raw_px = list(rgb.get_flattened_data() if hasattr(rgb, "get_flattened_data") else rgb.getdata())
        buf = bytearray(w * h * 2)
        for i, (r, g, b) in enumerate(raw_px):
            val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            struct.pack_into(">H", buf, i * 2, val)
    canvas.set_manual_mode(True)
    t0 = time.monotonic()
    canvas.blit_rgb565(x, y, w, h, bytes(buf), flush=True)
    print(f"Streamed {path.name} ({w}x{h}) to RAM framebuffer in {time.monotonic() - t0:.2f}s (0 flash wear)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Live 128x128 TFT Framebuffer Controller")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("demo", help="Draw live custom HUD demo")
    sub.add_parser("stock", help="Exit manual mode and resume stock clock/menu")

    p_txt = sub.add_parser("text", help="Draw ASCII text")
    p_txt.add_argument("x", type=int)
    p_txt.add_argument("y", type=int)
    p_txt.add_argument("text")
    p_txt.add_argument("--fg", default="#FFFFFF")
    p_txt.add_argument("--bg", default="#000000")
    p_txt.add_argument("--scale", type=int, default=1)
    p_txt.add_argument("--transparent", action="store_true")

    p_fill = sub.add_parser("fill", help="Fill rectangle")
    p_fill.add_argument("x", type=int)
    p_fill.add_argument("y", type=int)
    p_fill.add_argument("w", type=int)
    p_fill.add_argument("h", type=int)
    p_fill.add_argument("color", help="#RRGGBB")

    p_img = sub.add_parser("image", help="Stream image into live RAM framebuffer")
    p_img.add_argument("path", type=pathlib.Path)
    p_img.add_argument("--x", type=int, default=0)
    p_img.add_argument("--y", type=int, default=0)
    p_img.add_argument("--width", type=int, default=128)
    p_img.add_argument("--height", type=int, default=128)

    p_brt = sub.add_parser("brightness", help="Set TFT backlight level (0..4)")
    p_brt.add_argument("level", type=int, choices=range(5))

    args = parser.parse_args(argv)
    bridge = TftUartBridge()
    try:
        bridge.set_hud_paused(True)
        time.sleep(0.02)
        canvas = TftCanvas(bridge)
        if args.cmd == "demo":
            run_demo(canvas)
        elif args.cmd == "stock":
            canvas.set_manual_mode(False)
            print("Restored stock GamaKay clock/menu UI.")
        elif args.cmd == "text":
            canvas.set_manual_mode(True)
            canvas.draw_text(
                args.x,
                args.y,
                args.text,
                parse_hex_color(args.fg),
                parse_hex_color(args.bg),
                scale=args.scale,
                transparent=args.transparent,
                flush=True,
            )
        elif args.cmd == "fill":
            canvas.set_manual_mode(True)
            canvas.fill_rect(args.x, args.y, args.w, args.h, parse_hex_color(args.color), flush=True)
        elif args.cmd == "image":
            stream_image(canvas, args.path, args.x, args.y, args.width, args.height)
        elif args.cmd == "brightness":
            canvas.flush(brightness=args.level)
            print(f"Set TFT backlight level to {args.level}/4")
    finally:
        try:
            bridge.set_hud_paused(False)
        except Exception:
            pass
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
