#!/usr/bin/env python3
"""
GamaKay TK75 HE V2 Smart TFT & Telemetry CLI (Cross-Platform via VIA 0xFF60).

Features:
  - `sync-clock`   : Push host local date/time to keyboard TFT RTC (VIA ch=8 val_id=2)
  - `set-page`     : Switch active TFT HUD screen page (0..5)
  - `upload-image` : Resize image to TFT size (default 128x128 RGB565) & stream via VIA ch=8 val_id=3
"""

from __future__ import annotations

import argparse
import datetime
import pathlib
import struct
import sys
import time
from typing import Iterable

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import tk75_hid  # noqa: E402

VID_GAMAKAY = 0x3151
PID_VIA_FW = 0x502B
USAGE_PAGE_VIA = tk75_hid.USAGE_PAGE_VIA

VIA_ID_CUSTOM_SET = 0x07
VIA_CH_TFT_ENC = 7
VIA_CH_EXT = 8
MAX_CHUNK_BYTES = 24


def build_rtc_packet(dt: datetime.datetime) -> bytes:
    """Build 32-byte VIA command setting keyboard TFT RTC time (ch=8, val_id=2)."""
    pkt = bytearray(32)
    pkt[0] = VIA_ID_CUSTOM_SET
    pkt[1] = VIA_CH_EXT
    pkt[2] = 2
    pkt[3] = (dt.year >> 8) & 0xFF
    pkt[4] = dt.year & 0xFF
    pkt[5] = dt.month
    pkt[6] = dt.day
    pkt[7] = dt.hour
    pkt[8] = dt.minute
    pkt[9] = dt.second
    return bytes(pkt)


def build_tft_page_packet(page: int) -> bytes:
    """Build 32-byte VIA command switching TFT HUD page (ch=7, val_id=2)."""
    if not (0 <= page <= 5):
        raise ValueError("TFT page must be 0..5")
    pkt = bytearray(32)
    pkt[0] = VIA_ID_CUSTOM_SET
    pkt[1] = VIA_CH_TFT_ENC
    pkt[2] = 2
    pkt[3] = page
    return bytes(pkt)


def image_to_rgb565_bytes(
    img_path: pathlib.Path,
    width: int = 128,
    height: int = 128,
    big_endian: bool = True,
    column_major: bool = True,
) -> bytes:
    """Load PNG/JPG/BMP/GIF frame and convert to RGB565 matching stock TFT order (column-major, BE)."""
    from PIL import Image

    with Image.open(img_path) as im:
        rgb = im.convert("RGB").resize((width, height), Image.Resampling.LANCZOS)
        fmt = ">H" if big_endian else "<H"
        out = bytearray(width * height * 2)
        idx = 0
        raw_pixels = list(rgb.get_flattened_data() if hasattr(rgb, "get_flattened_data") else rgb.getdata())
        if column_major:
            coords = ((x, y) for x in range(width) for y in range(height))
            for x, y in coords:
                r, g, b = raw_pixels[y * width + x]
                val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                struct.pack_into(fmt, out, idx, val)
                idx += 2
        else:
            for r, g, b in raw_pixels:
                val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                struct.pack_into(fmt, out, idx, val)
                idx += 2
        return bytes(out)


def build_media_prep_packet(
    slot: int = 0,
    total_bytes: int = 32768,
    width: int = 128,
    height: int = 128,
    total_frames: int = 1,
    delay_units: int = 0,
) -> bytes:
    """Build VIA Ext `val_id=3` `seq=0xFFFF` metadata PREP packet matching stock UART7 cmd 0x03."""
    if not (0 <= slot <= 15):
        raise ValueError("slot must be 0..15")
    pkt = bytearray(32)
    pkt[0] = VIA_ID_CUSTOM_SET
    pkt[1] = VIA_CH_EXT
    pkt[2] = 3
    pkt[3] = ((slot & 0x0F) << 4) | 0x01
    pkt[4] = 0xFF
    pkt[5] = 0xFF
    pkt[6] = 10
    pkt[7] = slot & 0xFF
    pkt[8] = max(1, min(255, total_frames))
    pkt[9] = delay_units & 0xFF
    pkt[10] = total_bytes & 0xFF
    pkt[11] = (total_bytes >> 8) & 0xFF
    pkt[12] = (total_bytes >> 16) & 0xFF
    pkt[13] = (total_bytes >> 24) & 0xFF
    pkt[14] = width & 0xFF
    pkt[15] = (width >> 8) & 0xFF
    pkt[16] = height & 0xFF
    return bytes(pkt)


def build_media_erase_packet() -> bytes:
    """Build VIA Ext `val_id=3` erase packet triggering UART7 cmd 0x05."""
    pkt = bytearray(32)
    pkt[0] = VIA_ID_CUSTOM_SET
    pkt[1] = VIA_CH_EXT
    pkt[2] = 3
    pkt[3] = 0x04
    return bytes(pkt)


def chunk_media_packets(
    raw_bytes: bytes,
    slot: int = 0,
    width: int = 128,
    height: int = 128,
    prepend_prep: bool = True,
) -> list[bytes]:
    """Slice raw RGB565/GIF blob into `PREP` (optional) + <=24B VIA Ext `val_id=3` packets."""
    if not (0 <= slot <= 15):
        raise ValueError("slot must be 0..15")
    total_chunks = max(1, (len(raw_bytes) + MAX_CHUNK_BYTES - 1) // MAX_CHUNK_BYTES)
    packets: list[bytes] = []
    if prepend_prep:
        packets.append(build_media_prep_packet(slot=slot, total_bytes=len(raw_bytes), width=width, height=height))
    for seq in range(total_chunks):
        chunk = raw_bytes[seq * MAX_CHUNK_BYTES : (seq + 1) * MAX_CHUNK_BYTES]
        flags = (slot & 0x0F) << 4
        if not prepend_prep and seq == 0:
            flags |= 0x01
        if seq == total_chunks - 1:
            flags |= 0x02
        pkt = bytearray(32)
        pkt[0] = VIA_ID_CUSTOM_SET
        pkt[1] = VIA_CH_EXT
        pkt[2] = 3
        pkt[3] = flags
        pkt[4] = (seq >> 8) & 0xFF
        pkt[5] = seq & 0xFF
        pkt[6] = len(chunk)
        pkt[7 : 7 + len(chunk)] = chunk
        packets.append(bytes(pkt))
    return packets


def _send_via_packets(packets: Iterable[bytes], delay_s: float = 0.002) -> int:
    via = tk75_hid.open_via_transport(VID_GAMAKAY, PID_VIA_FW, USAGE_PAGE_VIA)
    count = 0
    try:
        for pkt in packets:
            via.exchange(pkt, timeout_s=0.25)
            count += 1
            if delay_s > 0:
                time.sleep(delay_s)
    finally:
        via.close()
    return count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="GamaKay TK75 HE V2 TFT CLI")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_clk = sub.add_parser("sync-clock", help="Sync PC date & time to keyboard TFT RTC")
    p_clk.add_argument("--dry-run", action="store_true")

    p_page = sub.add_parser("set-page", help="Switch active TFT HUD page (0..5)")
    p_page.add_argument("page", type=int)
    p_page.add_argument("--dry-run", action="store_true")

    p_img = sub.add_parser("upload-image", help="Upload custom image to TFT screen")
    p_img.add_argument("image", type=pathlib.Path)
    p_img.add_argument("--width", type=int, default=128)
    p_img.add_argument("--height", type=int, default=128)
    p_img.add_argument("--slot", type=int, default=0)
    p_img.add_argument("--little-endian", action="store_true", help="Override default big-endian RGB565")
    p_img.add_argument("--row-major", action="store_true", help="Override default column-major scan order")
    p_img.add_argument("--big-endian", action="store_true", help=argparse.SUPPRESS)
    p_img.add_argument("--dry-run", action="store_true")

    p_erase = sub.add_parser("erase-screen", help="Erase custom images stored in TFT flash (cmd 0x05)")
    p_erase.add_argument("--dry-run", action="store_true")

    args = parser.parse_args(argv)
    if args.cmd == "sync-clock":
        now = datetime.datetime.now()
        pkt = build_rtc_packet(now)
        if args.dry_run:
            print(f"Dry-run RTC packet: {now.isoformat()} -> {pkt[:10].hex(' ')}")
            return 0
        _send_via_packets([pkt])
        print(f"Synced TFT clock to {now:%Y-%m-%d %H:%M:%S}")
        return 0

    if args.cmd == "set-page":
        pkt = build_tft_page_packet(args.page)
        if args.dry_run:
            print(f"Dry-run page {args.page}: {pkt[:4].hex(' ')}")
            return 0
        _send_via_packets([pkt])
        print(f"Switched TFT HUD to page {args.page}")
        return 0

    if args.cmd == "erase-screen":
        pkt = build_media_erase_packet()
        if args.dry_run:
            print(f"Dry-run erase screen: {pkt[:4].hex(' ')}")
            return 0
        _send_via_packets([pkt])
        print("Sent custom TFT screen erase command (0x05)")
        return 0

    raw = image_to_rgb565_bytes(
        args.image,
        width=args.width,
        height=args.height,
        big_endian=not args.little_endian,
        column_major=not args.row_major,
    )
    packets = chunk_media_packets(raw, slot=args.slot, width=args.width, height=args.height)
    if args.dry_run:
        print(f"Dry-run upload {args.image.name}: {len(raw)} B RGB565 in {len(packets)} VIA packets")
        return 0
    _send_via_packets(packets)
    print(f"Uploaded {args.image.name} ({len(raw)} B, {len(packets)} packets) to TFT slot {args.slot}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
