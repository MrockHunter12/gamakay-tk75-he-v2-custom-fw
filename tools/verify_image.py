#!/usr/bin/env python3
"""Verify built TK75 HE V2 custom firmware image against AT32F405 bootloader requirements."""

import argparse
import pathlib
import struct
import sys

BOOT_MAGIC = b"AT32F405 8KMKB  "
FW_TAG = b"TK75VIA2"
HEADER_SIZE = 512
FLASH_BASE = 0x08005000
VTOR_ADDR = 0x08005200
MAX_IMAGE_BYTES = 135168  # 0x08005000..0x08025FFF (reserves 0x08026000..0x08027FFF for EEPROM)


def verify_image_bytes(data: bytes) -> list[str]:
    """Validate header magic, VTOR vector table bounds, and DFU size limits."""
    errors: list[str] = []
    if len(data) < HEADER_SIZE + 16:
        return [f"Image too small: {len(data)} bytes"]
    if len(data) > MAX_IMAGE_BYTES:
        errors.append(
            f"Image exceeds max app size ({len(data)} > {MAX_IMAGE_BYTES} bytes); would overlap EEPROM @ 0x08026000"
        )
    if data[:16] != BOOT_MAGIC:
        errors.append(f"Invalid boot header magic at +0x000: {data[:16]!r} != {BOOT_MAGIC!r}")
    if data[16:24] != FW_TAG:
        errors.append(f"Missing firmware tag TK75VIA2 at +0x010: {data[16:24]!r}")

    vtor, = struct.unpack_from("<I", data, 24)
    if vtor != VTOR_ADDR:
        errors.append(f"Header VTOR field mismatch: {vtor:#x} != {VTOR_ADDR:#x}")

    sp_init, reset_vec = struct.unpack_from("<II", data, HEADER_SIZE)
    if not (0x20000400 <= sp_init <= 0x20019800):
        errors.append(f"Initial MSP out of AT32F405 SRAM bounds: {sp_init:#x}")
    if not (VTOR_ADDR < reset_vec < FLASH_BASE + len(data)) or (reset_vec & 1) == 0:
        errors.append(f"Invalid Thumb Reset_Handler address: {reset_vec:#x}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin_path", type=pathlib.Path, help="Path to firmware .bin built at 0x08005000")
    args = parser.parse_args()

    data = args.bin_path.read_bytes()
    errors = verify_image_bytes(data)
    if errors:
        for err in errors:
            print(f"FAIL: {err}", file=sys.stderr)
        return 1

    sp_init, reset_vec = struct.unpack_from("<II", data, HEADER_SIZE)
    print(
        f"OK: {args.bin_path} ({len(data)} / {MAX_IMAGE_BYTES} B, "
        f"{len(data) * 100.0 / MAX_IMAGE_BYTES:.1f}% app flash), "
        f"SP={sp_init:#010x}, Reset={reset_vec:#010x}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
