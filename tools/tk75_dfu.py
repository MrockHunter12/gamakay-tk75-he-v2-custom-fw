#!/usr/bin/env python3
"""
GamaKay TK75 HE V2 (AT32F405) Cross-Platform DFU Flash & Stock Restore Tool.

Matches stock RY/GamaKay HID bootloader @ 0x08000000 (VID 0x3151, PID 0x502A):
  - Flash start:     [0xBA, 0xC0, blocks_le16, total_bytes_le24, 0...]
  - Data packets:    64 B chunks (last chunk 0xFF-padded to 64 B)
  - Flash complete:  [0xBA, 0xC2, blocks_le16, sum24_le, 0...] -> ACK reply[4] == 0x55
  - Bootloader exit: [0xBA, 0xFF, 0...]

Supports Linux (`/dev/hidraw*` native ioctl) and Windows/macOS (`hidapi`).
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import struct
import sys
import time
from collections.abc import Callable
from typing import Any, Protocol

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import tk75_hid  # noqa: E402

VID_GAMAKAY = 0x3151
PID_BOOTLOADER = 0x502A
PID_VIA_FW = 0x502B
PID_STOCK_FW = 0x5030

USAGE_PAGE_BOOT = tk75_hid.USAGE_PAGE_BOOT
USAGE_PAGE_VIA = tk75_hid.USAGE_PAGE_VIA
USAGE_PAGE_STOCK = tk75_hid.USAGE_PAGE_STOCK_VENDOR

BOOT_HEADER_MAGIC = b"AT32F405 8KMKB  "
MAX_CUSTOM_APP_BYTES = 135168  # 0x08005000 .. 0x08025FFF (EEPROM at 0x0803C000..0x0803FFFF)
MAX_STOCK_APP_BYTES = 143360   # 70 * 2048 B = 0x23000 B (0x08005000 .. 0x08027FFF)
CHUNK_SIZE = 64
FLASH_ACK_OK = 0x55
DEFAULT_BACKUP_PATH = SCRIPT_DIR.parent / "backups" / "last_via_config.json"

VIA_ID_GET_KEYBOARD_VALUE = 0x02
VIA_ID_SET_KEYBOARD_VALUE = 0x03
VIA_KBD_LAYOUT_OPTIONS = 0x02
VIA_ID_CUSTOM_SET_VALUE = 0x07
VIA_ID_CUSTOM_GET_VALUE = 0x08
VIA_ID_CUSTOM_SAVE = 0x09
VIA_ID_MACRO_GET_SIZE = 0x0D
VIA_ID_MACRO_GET_BUF = 0x0E
VIA_ID_MACRO_SET_BUF = 0x0F
VIA_ID_KEYMAP_GET_BUF = 0x12
VIA_ID_KEYMAP_SET_BUF = 0x13
VIA_ID_GET_ENCODER = 0x14
VIA_ID_SET_ENCODER = 0x15

VIA_LAYERS = 4
VIA_ROWS = 6
VIA_COLS = 16
VIA_HALL_COLS = 15
VIA_KEYMAP_BYTES = VIA_LAYERS * VIA_ROWS * VIA_COLS * 2  # 768
VIA_MACRO_BYTES = 2048
VIA_BUF_CHUNK = 28

# Custom channel descriptors: (channel, val_id, byte_len, key_name)
VIA_CUSTOM_FIELDS: tuple[tuple[int, int, int, str], ...] = (
    # Channel 3: RGB Matrix
    (3, 1, 1, "rgb_brightness"),
    (3, 2, 1, "rgb_effect"),
    (3, 3, 1, "rgb_speed"),
    (3, 4, 2, "rgb_color"),
    # Channel 5: Hall / RT / SOCD
    (5, 1, 1, "actuation_step"),
    (5, 2, 1, "hyst_step"),
    (5, 3, 1, "rt_enabled"),
    (5, 4, 1, "rt_continuous"),
    (5, 5, 1, "rt_down_step"),
    (5, 6, 1, "rt_up_step"),
    (5, 7, 1, "socd_mode"),
    (5, 8, 1, "socd_preset"),
    # Channel 6: Sidelight & master RGB enable (rgb_enabled restored AFTER rgb_effect)
    (6, 1, 1, "side_enabled"),
    (6, 2, 1, "side_brightness"),
    (6, 3, 1, "side_effect"),
    (6, 4, 1, "side_speed"),
    (6, 5, 2, "side_color"),
    (6, 6, 1, "caps_ind_enabled"),
    (6, 7, 1, "rgb_enabled"),
    (6, 8, 1, "caps_side_enabled"),
    (6, 9, 1, "caps_side_effect"),
    # Channel 7: TFT & Encoder
    (7, 1, 1, "tft_enabled"),
    (7, 2, 1, "tft_page"),
    (7, 3, 1, "tft_brightness"),
    (7, 4, 1, "tft_timeout_idx"),
    (7, 5, 1, "tft_clock_24h"),
    (7, 6, 1, "tft_wpm_enabled"),
    (7, 7, 1, "tft_he_bar_enabled"),
    (7, 8, 1, "encoder_mode"),
    (7, 9, 1, "tft_screensaver"),
    # Channel 9: Wireless / System
    (9, 1, 1, "rf_bt_slot"),
    (9, 2, 1, "rf_poll_rate_idx"),
    (9, 3, 1, "mac_mode"),
    (9, 4, 1, "sleep_timeout_min"),
)


class BootTransport(Protocol):
    def send_feature(self, payload64: bytes) -> None: ...
    def recv_feature(self) -> bytes: ...
    def close(self) -> None: ...


def validate_firmware_blob(data: bytes, allow_stock_size: bool = False) -> tuple[bool, str]:
    """Validate TK75 HE V2 binary layout before touching bootloader flash."""
    max_len = MAX_STOCK_APP_BYTES if allow_stock_size else MAX_CUSTOM_APP_BYTES
    if len(data) < 0x210:
        return False, f"Image too small ({len(data)} bytes)"
    if len(data) > max_len:
        return False, f"Image exceeds flash ceiling ({len(data)} > {max_len} bytes)"
    if data[:16] != BOOT_HEADER_MAGIC:
        return False, f"Missing stock header magic {BOOT_HEADER_MAGIC!r} at 0x000"

    sp, reset_vec = struct.unpack_from("<II", data, 0x200)
    if not (0x20001000 <= sp <= 0x20020000):
        return False, f"Invalid initial SP 0x{sp:08X} at vector table +0x200"
    if not (0x08005200 <= reset_vec < 0x08028000) or (reset_vec & 1) == 0:
        return False, f"Invalid Thumb Reset_Handler 0x{reset_vec:08X} at +0x204"
    return True, f"Valid image ({len(data)} B, SP=0x{sp:08X}, Reset=0x{reset_vec:08X})"


def pad_firmware_64(data: bytes) -> bytes:
    """Pad firmware image with 0xFF to an exact 64-byte boundary (matching bootloader DMA sum)."""
    rem = len(data) % CHUNK_SIZE
    if rem == 0:
        return data
    return data + (b"\xFF" * (CHUNK_SIZE - rem))


def calculate_bootloader_checksum24(padded_data: bytes) -> int:
    """Compute bootloader running 24-bit byte sum over 64-byte-padded chunks."""
    return sum(padded_data) & 0xFFFFFF


def build_dfu_start_packet(total_bytes: int) -> bytes:
    """Build 64-byte `0xBA 0xC0` DFU start feature packet."""
    blocks = math.ceil(total_bytes / CHUNK_SIZE)
    pkt = bytearray(CHUNK_SIZE)
    pkt[0] = 0xBA
    pkt[1] = 0xC0
    pkt[2] = blocks & 0xFF
    pkt[3] = (blocks >> 8) & 0xFF
    pkt[4] = total_bytes & 0xFF
    pkt[5] = (total_bytes >> 8) & 0xFF
    pkt[6] = (total_bytes >> 16) & 0xFF
    return bytes(pkt)


def build_dfu_complete_packet(total_bytes: int, checksum24: int) -> bytes:
    """Build 64-byte `0xBA 0xC2` DFU verify/commit feature packet."""
    blocks = math.ceil(total_bytes / CHUNK_SIZE)
    pkt = bytearray(CHUNK_SIZE)
    pkt[0] = 0xBA
    pkt[1] = 0xC2
    pkt[2] = blocks & 0xFF
    pkt[3] = (blocks >> 8) & 0xFF
    pkt[4] = checksum24 & 0xFF
    pkt[5] = (checksum24 >> 8) & 0xFF
    pkt[6] = (checksum24 >> 16) & 0xFF
    return bytes(pkt)


def build_dfu_reboot_packet() -> bytes:
    """Build 64-byte `0xBA 0xFF` reboot command packet."""
    pkt = bytearray(CHUNK_SIZE)
    pkt[0] = 0xBA
    pkt[1] = 0xFF
    return bytes(pkt)


def parse_bootloader_reply(reply: bytes) -> tuple[int, int, int, int]:
    """Return (cmd0, cmd1, blocks, status_or_zero) from 64-byte GET_REPORT response."""
    if len(reply) >= 65 and reply[0] == 0x00:
        reply = reply[1:]
    if len(reply) < 8:
        raise ValueError(f"Short bootloader reply ({len(reply)} B)")
    blocks = reply[2] | (reply[3] << 8)
    return reply[0], reply[1], blocks, reply[4]


def flash_via_transport(transport: BootTransport, firmware: bytes, progress_cb=None) -> int:
    """Execute complete 0xBA 0xC0 -> chunks -> 0xBA 0xC2 sequence against BootTransport."""
    padded = pad_firmware_64(firmware)
    checksum24 = calculate_bootloader_checksum24(padded)
    total_blocks = len(padded) // CHUNK_SIZE

    transport.send_feature(build_dfu_start_packet(len(padded)))
    time.sleep(0.05)
    c0, c1, ack_blocks, _ = parse_bootloader_reply(transport.recv_feature())
    if (c0, c1) != (0xAB, 0xC0) or ack_blocks != total_blocks:
        raise RuntimeError(f"Unexpected START ACK: {c0:#04x} {c1:#04x} blocks={ack_blocks}")

    for idx in range(total_blocks):
        chunk = padded[idx * CHUNK_SIZE : (idx + 1) * CHUNK_SIZE]
        transport.send_feature(chunk)
        if progress_cb is not None:
            progress_cb(idx + 1, total_blocks)
        time.sleep(0.002)

    transport.send_feature(build_dfu_complete_packet(len(padded), checksum24))
    time.sleep(0.05)
    v0, v1, _, status = parse_bootloader_reply(transport.recv_feature())
    if (v0, v1) != (0xAB, 0xC2) or status != FLASH_ACK_OK:
        raise RuntimeError(
            f"Flash verification NACK: reply=({v0:#04x},{v1:#04x}) status={status:#04x} "
            "(device remains safely in bootloader)"
        )
    return checksum24


def build_stock_feature_packet(payload: bytes) -> bytes:
    """Build 64-byte stock FW (`3151:5030`) feature report payload with checksum `0xFF - sum[:7]` at byte 7."""
    pkt = bytearray(CHUNK_SIZE)
    n = min(len(payload), 7)
    pkt[0:n] = payload[:n]
    pkt[7] = (0xFF - (sum(pkt[0:7]) & 0xFF)) & 0xFF
    return bytes(pkt)


HidapiBootTransport = tk75_hid.HidapiFeatureTransport


def trigger_bootloader_if_running() -> bool:
    """If keyboard is active (0x502B VIA or 0x5030 Stock), ask it to enter 0x502A bootloader."""
    if tk75_hid.find_device_path(VID_GAMAKAY, PID_BOOTLOADER) is not None:
        return True

    via_path = tk75_hid.find_device_path(VID_GAMAKAY, PID_VIA_FW, USAGE_PAGE_VIA)
    if via_path is not None:
        via = tk75_hid.open_via_transport(VID_GAMAKAY, PID_VIA_FW, USAGE_PAGE_VIA)
        pkt = bytearray(32)
        pkt[0] = 0x0B  # VIA ID_BOOTLOADER_JUMP
        try:
            via.write_only(bytes(pkt))
        except OSError:
            pass
        finally:
            via.close()
        return True

    stock_path = tk75_hid.find_device_path(
        VID_GAMAKAY, PID_STOCK_FW, USAGE_PAGE_STOCK
    ) or tk75_hid.find_device_path(VID_GAMAKAY, PID_STOCK_FW)
    if stock_path is not None:
        feat = tk75_hid.open_feature_transport(stock_path)
        try:
            feat.send_feature(build_stock_feature_packet(bytes([0xC5, 0x3A])))
            time.sleep(0.05)
            feat.send_feature(build_stock_feature_packet(bytes([0x7F, 0x55, 0xAA, 0x55, 0xAA])))
        except OSError:
            pass
        finally:
            feat.close()
        return True

    return False


def wait_for_bootloader(timeout_s: float = 12.0) -> str | bytes:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        path = tk75_hid.find_device_path(
            VID_GAMAKAY, PID_BOOTLOADER, USAGE_PAGE_BOOT
        ) or tk75_hid.find_device_path(VID_GAMAKAY, PID_BOOTLOADER)
        if path is not None:
            return path
        time.sleep(0.25)
    raise TimeoutError(
        "Timed out waiting for GamaKay TK75 HE V2 Bootloader (3151:502A). "
        "Tip: hold Esc while plugging in USB to enter unbrickable bootloader."
    )


def open_bootloader_with_retry(
    wait_fn: Callable[[], str | bytes] = wait_for_bootloader,
    open_fn: Callable[[str | bytes], BootTransport] = tk75_hid.open_feature_transport,
    timeout_s: float = 2.0,
) -> BootTransport:
    """Open enumerated bootloader device, retrying briefly while udev sets permissions."""
    boot_path = wait_fn()
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            return open_fn(boot_path)
        except (PermissionError, FileNotFoundError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def _via_pkt(*prefix: int) -> bytes:
    pkt = bytearray(32)
    pkt[: len(prefix)] = bytes(prefix)
    return bytes(pkt)


def backup_via_config(via: tk75_hid.ViaInterruptTransport) -> dict[str, Any]:
    """Read full user configuration (keymap, encoders, macros, custom Hall/RGB/TFT/RF) over VIA."""
    # 1. Layout options
    rep = via.exchange(_via_pkt(VIA_ID_GET_KEYBOARD_VALUE, VIA_KBD_LAYOUT_OPTIONS))
    layout_options = int.from_bytes(rep[2:6], "big")

    # 2. Keymap buffer (768 B -> 4x6x16 u16 keycodes)
    km_raw = bytearray()
    for off in range(0, VIA_KEYMAP_BYTES, VIA_BUF_CHUNK):
        sz = min(VIA_BUF_CHUNK, VIA_KEYMAP_BYTES - off)
        rep = via.exchange(_via_pkt(VIA_ID_KEYMAP_GET_BUF, (off >> 8) & 0xFF, off & 0xFF, sz))
        km_raw.extend(rep[4 : 4 + sz])

    keymap: list[list[list[int]]] = []
    idx = 0
    for _layer in range(VIA_LAYERS):
        rows: list[list[int]] = []
        for _row in range(VIA_ROWS):
            cols: list[int] = []
            for _col in range(VIA_COLS):
                cols.append((km_raw[idx] << 8) | km_raw[idx + 1])
                idx += 2
            rows.append(cols)
        keymap.append(rows)

    # 3. Encoders (4 layers x [CCW, CW])
    encoders: list[list[int]] = []
    for layer in range(VIA_LAYERS):
        pair: list[int] = []
        for cw in (0, 1):
            rep = via.exchange(_via_pkt(VIA_ID_GET_ENCODER, layer, 0, cw))
            pair.append((rep[4] << 8) | rep[5])
        encoders.append(pair)

    # 4. Macros buffer (dynamically queried via VIA_ID_MACRO_GET_SIZE)
    sz_rep = via.exchange(_via_pkt(VIA_ID_MACRO_GET_SIZE))
    macro_size = ((sz_rep[1] << 8) | sz_rep[2]) if sz_rep[0] == VIA_ID_MACRO_GET_SIZE else VIA_MACRO_BYTES
    if macro_size == 0 or macro_size > 16384:
        macro_size = VIA_MACRO_BYTES
    macro_raw = bytearray()
    for off in range(0, macro_size, VIA_BUF_CHUNK):
        sz = min(VIA_BUF_CHUNK, macro_size - off)
        rep = via.exchange(_via_pkt(VIA_ID_MACRO_GET_BUF, (off >> 8) & 0xFF, off & 0xFF, sz))
        macro_raw.extend(rep[4 : 4 + sz])

    # 5. Custom channels (RGB, Hall, Sidelight, TFT, Wireless)
    custom: dict[str, int | list[int]] = {}
    for ch, val_id, byte_len, name in VIA_CUSTOM_FIELDS:
        rep = via.exchange(_via_pkt(VIA_ID_CUSTOM_GET_VALUE, ch, val_id))
        if byte_len == 1:
            custom[name] = rep[3]
        else:
            custom[name] = list(rep[3 : 3 + byte_len])

    # 6. Per-key Hall overrides (6 rows x 15 cols)
    hall_per_key: list[dict[str, int]] = []
    for row in range(VIA_ROWS):
        for col in range(VIA_HALL_COLS):
            rep = via.exchange(_via_pkt(VIA_ID_CUSTOM_GET_VALUE, 8, 4, row, col))
            act_step = rep[5]
            rt_byte = rep[6]
            if act_step != 0 or rt_byte != 0:
                hall_per_key.append(
                    {"row": row, "col": col, "act_step": act_step, "rt_byte": rt_byte}
                )

    return {
        "format": "tk75_he_v2_via_backup_v1",
        "layout_options": layout_options,
        "custom": custom,
        "encoders": encoders,
        "keymap": keymap,
        "macros_hex": bytes(macro_raw).hex(),
        "hall_per_key": hall_per_key,
    }


def restore_via_config(via: tk75_hid.ViaInterruptTransport, data: dict[str, Any]) -> None:
    """Restore full user configuration over VIA and commit to flash EEPROM."""
    if data.get("format") != "tk75_he_v2_via_backup_v1":
        raise ValueError(f"Unsupported backup format: {data.get('format')!r}")

    # 1. Layout options
    if "layout_options" in data:
        opt = int(data["layout_options"]) & 0xFFFFFFFF
        via.exchange(
            _via_pkt(
                VIA_ID_SET_KEYBOARD_VALUE,
                VIA_KBD_LAYOUT_OPTIONS,
                (opt >> 24) & 0xFF,
                (opt >> 16) & 0xFF,
                (opt >> 8) & 0xFF,
                opt & 0xFF,
            )
        )

    # 2. Keymap buffer
    if "keymap" in data:
        km_raw = bytearray()
        for layer in data["keymap"]:
            for row in layer:
                for kc in row:
                    val = int(kc) & 0xFFFF
                    km_raw.append((val >> 8) & 0xFF)
                    km_raw.append(val & 0xFF)
        if len(km_raw) != VIA_KEYMAP_BYTES:
            raise ValueError(f"Invalid keymap byte length: {len(km_raw)}")
        for off in range(0, VIA_KEYMAP_BYTES, VIA_BUF_CHUNK):
            chunk = km_raw[off : off + VIA_BUF_CHUNK]
            pkt = bytearray(32)
            pkt[0] = VIA_ID_KEYMAP_SET_BUF
            pkt[1] = (off >> 8) & 0xFF
            pkt[2] = off & 0xFF
            pkt[3] = len(chunk)
            pkt[4 : 4 + len(chunk)] = chunk
            via.exchange(bytes(pkt))

    # 3. Encoders
    if "encoders" in data:
        for layer, pair in enumerate(data["encoders"][:VIA_LAYERS]):
            for cw, kc in enumerate(pair[:2]):
                val = int(kc) & 0xFFFF
                via.exchange(
                    _via_pkt(VIA_ID_SET_ENCODER, layer, 0, cw, (val >> 8) & 0xFF, val & 0xFF)
                )

    # 4. Macros buffer (supports seamless upgrade from older 384 B backups to 2048 B firmware)
    if "macros_hex" in data:
        sz_rep = via.exchange(_via_pkt(VIA_ID_MACRO_GET_SIZE))
        target_macro_bytes = (
            ((sz_rep[1] << 8) | sz_rep[2]) if sz_rep[0] == VIA_ID_MACRO_GET_SIZE else VIA_MACRO_BYTES
        )
        if target_macro_bytes == 0 or target_macro_bytes > 16384:
            target_macro_bytes = VIA_MACRO_BYTES
        macro_raw = bytes.fromhex(data["macros_hex"])
        if len(macro_raw) < target_macro_bytes:
            macro_raw = macro_raw + bytes(target_macro_bytes - len(macro_raw))
        elif len(macro_raw) > target_macro_bytes:
            if any(macro_raw[target_macro_bytes:]):
                raise ValueError(
                    f"Backup macro payload ({len(macro_raw)} B) exceeds firmware macro buffer ({target_macro_bytes} B)"
                )
            macro_raw = macro_raw[:target_macro_bytes]
        for off in range(0, target_macro_bytes, VIA_BUF_CHUNK):
            chunk = macro_raw[off : off + VIA_BUF_CHUNK]
            pkt = bytearray(32)
            pkt[0] = VIA_ID_MACRO_SET_BUF
            pkt[1] = (off >> 8) & 0xFF
            pkt[2] = off & 0xFF
            pkt[3] = len(chunk)
            pkt[4 : 4 + len(chunk)] = chunk
            via.exchange(bytes(pkt))

    # 5. Custom channels (VIA_CUSTOM_FIELDS already orders rgb_enabled after rgb_effect)
    custom = data.get("custom", {})
    for ch, val_id, byte_len, name in VIA_CUSTOM_FIELDS:
        if name not in custom:
            continue
        val = custom[name]
        if byte_len == 1:
            via.exchange(_via_pkt(VIA_ID_CUSTOM_SET_VALUE, ch, val_id, int(val) & 0xFF))
        else:
            seq = [int(x) & 0xFF for x in val[:byte_len]]
            via.exchange(_via_pkt(VIA_ID_CUSTOM_SET_VALUE, ch, val_id, *seq))

    # 6. Per-key Hall overrides (reset all or apply sparse overrides)
    if "hall_per_key" in data:
        overrides: dict[tuple[int, int], tuple[int, int]] = {}
        for item in data["hall_per_key"]:
            r = int(item["row"])
            c = int(item["col"])
            overrides[(r, c)] = (int(item["act_step"]) & 0xFF, int(item["rt_byte"]) & 0xFF)
        for row in range(VIA_ROWS):
            for col in range(VIA_HALL_COLS):
                act_s, rt_b = overrides.get((row, col), (0, 0))
                rep = via.exchange(_via_pkt(VIA_ID_CUSTOM_GET_VALUE, 8, 4, row, col))
                if (rep[5], rep[6]) != (act_s, rt_b):
                    via.exchange(_via_pkt(VIA_ID_CUSTOM_SET_VALUE, 8, 4, row, col, act_s, rt_b))

    # 7. Commit to flash EEPROM
    via.exchange(_via_pkt(VIA_ID_CUSTOM_SAVE))


def wait_for_via_transport(
    timeout_s: float = 10.0,
    open_fn: Callable[[int, int, int], tk75_hid.ViaInterruptTransport] = tk75_hid.open_via_transport,
) -> tk75_hid.ViaInterruptTransport:
    """Wait for custom VIA firmware (3151:502B) to enumerate and open transport."""
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            return open_fn(VID_GAMAKAY, PID_VIA_FW, USAGE_PAGE_VIA)
        except (RuntimeError, PermissionError, FileNotFoundError, OSError):
            if time.monotonic() >= deadline:
                raise TimeoutError("Timed out waiting for VIA firmware (3151:502B) after flash.")
            time.sleep(0.2)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="GamaKay TK75 HE V2 DFU Flasher & Stock Restorer")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--flash", type=pathlib.Path, help="Flash custom VIA firmware .bin")
    group.add_argument("--restore", type=pathlib.Path, help="Restore decrypted stock firmware .bin")
    group.add_argument("--verify-only", type=pathlib.Path, help="Validate .bin header/vectors without USB access")
    group.add_argument("--enter-bootloader", action="store_true", help="Reboot connected keyboard into DFU 3151:502A")
    group.add_argument("--backup-config", type=pathlib.Path, help="Save VIA settings & keymap to JSON file")
    group.add_argument("--restore-config", type=pathlib.Path, help="Restore VIA settings & keymap from JSON file")
    parser.add_argument("--dry-run", action="store_true", help="Validate image & packet counts without touching USB")
    parser.add_argument(
        "--no-preserve-config",
        action="store_true",
        help="Skip automatic VIA config backup/restore across --flash",
    )
    args = parser.parse_args(argv)

    if args.backup_config is not None:
        via = tk75_hid.open_via_transport(VID_GAMAKAY, PID_VIA_FW, USAGE_PAGE_VIA)
        try:
            cfg = backup_via_config(via)
        finally:
            via.close()
        args.backup_config.parent.mkdir(parents=True, exist_ok=True)
        args.backup_config.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
        print(f"Saved VIA config backup to {args.backup_config}")
        return 0

    if args.restore_config is not None:
        cfg = json.loads(args.restore_config.read_text(encoding="utf-8"))
        via = tk75_hid.open_via_transport(VID_GAMAKAY, PID_VIA_FW, USAGE_PAGE_VIA)
        try:
            restore_via_config(via, cfg)
        finally:
            via.close()
        print(f"Restored VIA config from {args.restore_config}")
        return 0

    if args.enter_bootloader:
        if args.dry_run:
            print("Dry-run: would send bootloader jump command.")
            return 0
        if not trigger_bootloader_if_running():
            print("ERROR: No GamaKay TK75 device found on USB.", file=sys.stderr)
            return 1
        path = wait_for_bootloader()
        print(f"Bootloader ready at {path!r}")
        return 0

    bin_path: pathlib.Path = args.flash or args.restore or args.verify_only
    allow_stock = args.restore is not None
    data = bin_path.read_bytes()
    ok, msg = validate_firmware_blob(data, allow_stock_size=allow_stock)
    if not ok:
        print(f"ERROR: {msg}", file=sys.stderr)
        return 2
    print(msg)

    if args.verify_only or args.dry_run:
        padded = pad_firmware_64(data)
        csum24 = calculate_bootloader_checksum24(padded)
        blocks = len(padded) // CHUNK_SIZE
        print(f"Plan: {blocks} x 64B chunks ({len(padded)} B padded), sum24=0x{csum24:06X}")
        return 0

    saved_cfg: dict[str, Any] | None = None
    if args.flash is not None and not args.no_preserve_config:
        try:
            via = tk75_hid.open_via_transport(VID_GAMAKAY, PID_VIA_FW, USAGE_PAGE_VIA)
        except Exception:
            via = None
        if via is not None:
            try:
                print("Backing up current VIA settings before flash...")
                saved_cfg = backup_via_config(via)
                DEFAULT_BACKUP_PATH.parent.mkdir(parents=True, exist_ok=True)
                DEFAULT_BACKUP_PATH.write_text(json.dumps(saved_cfg, indent=2) + "\n", encoding="utf-8")
                print(f"Backed up live VIA settings to {DEFAULT_BACKUP_PATH}")
            except Exception as exc:
                print(f"WARNING: Pre-flash VIA backup skipped ({exc})", file=sys.stderr)
            finally:
                via.close()

    print("Checking for keyboard / switching to bootloader...")
    trigger_bootloader_if_running()
    transport = open_bootloader_with_retry()
    try:
        def _prog(done: int, total: int) -> None:
            if done == total or (done % 64) == 0:
                pct = (done * 100) // total
                print(f"\rFlashing: {done}/{total} blocks ({pct}%)", end="", flush=True)

        csum = flash_via_transport(transport, data, progress_cb=_prog)
        print(f"\nDONE: Verified sum24=0x{csum:06X}; keyboard rebooting into firmware.")
    finally:
        transport.close()

    if saved_cfg is not None:
        try:
            via_post = wait_for_via_transport()
            try:
                restore_via_config(via_post, saved_cfg)
                print("Restored VIA settings to keyboard EEPROM.")
            finally:
                via_post.close()
        except Exception as exc:
            print(
                f"WARNING: Automatic post-flash config restore failed ({exc}). "
                f"Restore manually with: --restore-config {DEFAULT_BACKUP_PATH}",
                file=sys.stderr,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
