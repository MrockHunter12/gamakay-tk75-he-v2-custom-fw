#!/usr/bin/env python3
"""
Compile tools/tft_patch/u24_patch.c and patch backups/stock_u24_lcd_v102_x5.bin ->
backups/patched_u24_lcd_v102_custom.bin with verified Thumb BL hooks.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PATCH_DIR = pathlib.Path(__file__).resolve().parent


def find_arm_tool(name: str) -> str:
    """Locate ARM GNU toolchain binary via ARM_TOOLCHAIN_DIR, PATH, or local fallback."""
    env_dir = os.environ.get("ARM_TOOLCHAIN_DIR")
    if env_dir:
        cand = pathlib.Path(env_dir) / name
        if cand.is_file():
            return str(cand)
    found = shutil.which(name)
    if found:
        return found
    local_cand = pathlib.Path.home() / ".local/toolchains/arm-none-eabi/usr/bin" / name
    if local_cand.is_file():
        return str(local_cand)
    raise FileNotFoundError(
        f"ARM toolchain binary {name!r} not found. Install arm-none-eabi-gcc or set ARM_TOOLCHAIN_DIR."
    )

XIP_BASE = 0x01000200
PATCH_FILE_OFF = 0x55500
PATCH_VA = XIP_BASE + PATCH_FILE_OFF  # 0x01055700

HOOK_CMD_FILE_OFF = 0x12860
HOOK_CMD_VA = XIP_BASE + HOOK_CMD_FILE_OFF  # 0x01012A60
HOOK_CMD_ORIG = bytes.fromhex("2a488471")   # ldr r0, [pc,#168]; strb r4, [r0,#6]

HOOK_UI_FILE_OFF = 0x16AA2
HOOK_UI_VA = XIP_BASE + HOOK_UI_FILE_OFF    # 0x01016CA2
HOOK_UI_ORIG = bytes.fromhex("fef767fbfcf719f9")  # bl 0x1015374; bl 0x1012edc

# FUN_010169b8 (ST7735S flush): center 128x128 window at RASET 1..128 (0x01..0x80) to eliminate
# both left (at 3..130) and right (at 0..127) edge offset bars on ST7735S 128x128 panels.
RASET_START_FILE_OFF = 0x169C6  # VA 0x01016BC6: movs r0, #3   (03 20) -> movs r0, #1   (01 20)
RASET_END_FILE_OFF = 0x169D2    # VA 0x01016BD2: movs r0, #130 (82 20) -> movs r0, #128 (80 20)
RASET_START_ORIG = bytes.fromhex("0320")
RASET_END_ORIG = bytes.fromhex("8220")


def encode_thumb_bl(src_va: int, dst_va: int) -> bytes:
    """Encode 32-bit Thumb BL instruction from src_va to dst_va."""
    offset = dst_va - (src_va + 4)
    if not (-0x1000000 <= offset <= 0xFFFFFE) or (offset & 1) != 0:
        raise ValueError(f"BL out of range or unaligned: {hex(src_va)} -> {hex(dst_va)} ({hex(offset)})")
    s = (offset >> 24) & 1
    i1 = (offset >> 23) & 1
    i2 = (offset >> 22) & 1
    imm10 = (offset >> 12) & 0x3FF
    imm11 = (offset >> 1) & 0x7FF
    j1 = (1 - i1) ^ s
    j2 = (1 - i2) ^ s
    hw1 = 0xF000 | (s << 10) | imm10
    hw2 = 0xD000 | (j1 << 13) | (1 << 12) | (j2 << 11) | imm11
    return struct.pack("<HH", hw1, hw2)


def main() -> int:
    gcc = find_arm_tool("arm-none-eabi-gcc")
    objcopy = find_arm_tool("arm-none-eabi-objcopy")
    objdump = find_arm_tool("arm-none-eabi-objdump")

    elf_out = PATCH_DIR / "u24_patch.elf"
    bin_out = PATCH_DIR / "u24_patch.bin"

    subprocess.run(
        [
            str(gcc),
            "-mcpu=cortex-m0",
            "-mthumb",
            "-Os",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffreestanding",
            "-nostdlib",
            "-fno-builtin",
            "-fno-pie",
            "-T",
            str(PATCH_DIR / "u24_patch.ld"),
            str(PATCH_DIR / "u24_patch.c"),
            "-o",
            str(elf_out),
        ],
        check=True,
    )
    subprocess.run([str(objcopy), "-O", "binary", str(elf_out), str(bin_out)], check=True)
    patch_bytes = bin_out.read_bytes()
    print(f"Compiled u24_patch.bin: {len(patch_bytes)} B @ VA 0x{PATCH_VA:08X}")

    stock_path = ROOT / "backups/stock_u24_lcd_v102_x5.bin"
    patched_path = ROOT / "backups/patched_u24_lcd_v102_custom.bin"
    img = bytearray(stock_path.read_bytes())

    if bytes(img[HOOK_CMD_FILE_OFF : HOOK_CMD_FILE_OFF + 4]) != HOOK_CMD_ORIG:
        raise RuntimeError("Unexpected bytes at HOOK_CMD_FILE_OFF!")
    if bytes(img[HOOK_UI_FILE_OFF : HOOK_UI_FILE_OFF + 8]) != HOOK_UI_ORIG:
        raise RuntimeError("Unexpected bytes at HOOK_UI_FILE_OFF!")
    if bytes(img[RASET_START_FILE_OFF : RASET_START_FILE_OFF + 2]) != RASET_START_ORIG:
        raise RuntimeError("Unexpected bytes at RASET_START_FILE_OFF!")
    if bytes(img[RASET_END_FILE_OFF : RASET_END_FILE_OFF + 2]) != RASET_END_ORIG:
        raise RuntimeError("Unexpected bytes at RASET_END_FILE_OFF!")

    # Pad stock image with 0xFF up to PATCH_FILE_OFF, then append patch payload
    if len(img) > PATCH_FILE_OFF:
        raise RuntimeError(f"Stock image overlaps patch offset: {hex(len(img))} > {hex(PATCH_FILE_OFF)}")
    img.extend(b"\xFF" * (PATCH_FILE_OFF - len(img)))
    img.extend(patch_bytes)

    # Apply 2 verified hooks + RASET 1..128 panel alignment fix
    img[HOOK_CMD_FILE_OFF : HOOK_CMD_FILE_OFF + 4] = encode_thumb_bl(HOOK_CMD_VA, PATCH_VA)
    img[HOOK_UI_FILE_OFF : HOOK_UI_FILE_OFF + 8] = (
        encode_thumb_bl(HOOK_UI_VA, PATCH_VA + 4) + struct.pack("<HH", 0xBF00, 0xBF00)
    )
    img[RASET_START_FILE_OFF : RASET_START_FILE_OFF + 2] = bytes.fromhex("0120")
    img[RASET_END_FILE_OFF : RASET_END_FILE_OFF + 2] = bytes.fromhex("8020")

    patched_path.write_bytes(img)
    print(f"Wrote {patched_path} ({len(img)} B, app slice={len(img) - 0x10000} B)")

    # Disassemble both patched hooks + entry table for visual verification
    for label, start, stop in [
        ("HOOK_CMD", HOOK_CMD_VA - 4, HOOK_CMD_VA + 8),
        ("HOOK_UI", HOOK_UI_VA - 4, HOOK_UI_VA + 12),
        ("PATCH_ENTRY", PATCH_VA, PATCH_VA + 16),
    ]:
        print(f"=== {label} ===")
        out = subprocess.check_output(
            [
                str(objdump),
                "-D",
                "-b",
                "binary",
                "-m",
                "arm",
                "-M",
                "force-thumb",
                f"--adjust-vma=0x{XIP_BASE:x}",
                f"--start-address=0x{start:x}",
                f"--stop-address=0x{stop:x}",
                str(patched_path),
            ],
            text=True,
        )
        print("\n".join(out.splitlines()[6:]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
