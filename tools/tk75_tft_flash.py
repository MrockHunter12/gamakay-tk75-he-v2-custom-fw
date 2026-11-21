#!/usr/bin/env python3
"""
GamaKay TK75 HE V2 TFT Co-Processor (U24 = YC3123-Q / RY5060) Flasher & Raw UART7 Tool.

Uses the AT32F405 VIA Ext channel (ch=8, val_id=7) raw UART7 bridge to:
  - Query U24 identity & mode (`--info`)
  - Flash app image (`x5_plain.bin` full image or pre-sliced app image starting with RY5060RY5060)
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

import tk75_hid  # noqa: E402

VID_GAMAKAY = 0x3151
PID_VIA_FW = 0x502B
USAGE_PAGE_VIA = tk75_hid.USAGE_PAGE_VIA

VIA_ID_CUSTOM_SET = 0x07
VIA_ID_CUSTOM_GET = 0x08
VIA_CH_EXT = 8
VIA_VAL_TFT_RAW = 7

OLED_APP_OFFSET = 0x10000
OLED_APP_MAX_BYTES = 11 * 32768  # 352 KB erased by bootloader (0x01010000..0x01067FFF)
OLED_MAGIC = b"RY5060RY5060"
CHUNK_SIZE = 64


class TftUartBridge:
    """Thin host wrapper over VIA Ext val_id=7 raw UART7 bridge."""

    def __init__(self) -> None:
        self._via = tk75_hid.open_via_transport(VID_GAMAKAY, PID_VIA_FW, USAGE_PAGE_VIA)

    def close(self) -> None:
        self._via.close()

    def set_hud_paused(self, paused: bool) -> None:
        pkt = bytearray(32)
        pkt[0] = VIA_ID_CUSTOM_SET
        pkt[1] = VIA_CH_EXT
        pkt[2] = VIA_VAL_TFT_RAW
        pkt[3] = 0  # op=0 pause/resume
        pkt[4] = 1 if paused else 0
        self._via.exchange(bytes(pkt), timeout_s=0.5)

    def send_frame(self, cmd: int, payload: bytes = b"") -> None:
        if len(payload) > 96:
            raise ValueError(f"UART7 payload too large ({len(payload)} > 96)")
        if len(payload) <= 24:
            pkt = bytearray(32)
            pkt[0] = VIA_ID_CUSTOM_SET
            pkt[1] = VIA_CH_EXT
            pkt[2] = VIA_VAL_TFT_RAW
            pkt[3] = 2  # op=2 inline send
            pkt[4] = cmd & 0xFF
            pkt[5] = len(payload)
            pkt[6 : 6 + len(payload)] = payload
            self._via.exchange(bytes(pkt), timeout_s=0.5)
            return

        # Stage chunks of up to 24 B, last chunk uses op=3 (stage+send)
        off = 0
        while off < len(payload):
            rem = len(payload) - off
            chunk = payload[off : off + min(24, rem)]
            is_last = (off + len(chunk)) == len(payload)
            pkt = bytearray(32)
            pkt[0] = VIA_ID_CUSTOM_SET
            pkt[1] = VIA_CH_EXT
            pkt[2] = VIA_VAL_TFT_RAW
            pkt[3] = 3 if is_last else 1
            pkt[4] = off
            pkt[5] = len(chunk)
            pkt[6 : 6 + len(chunk)] = chunk
            if is_last:
                pkt[31] = cmd & 0xFF
            self._via.exchange(bytes(pkt), timeout_s=0.5)
            off += len(chunk)

    def poll_rx(self, consume: bool = True) -> tuple[int, bytes] | None:
        pkt = bytearray(32)
        pkt[0] = VIA_ID_CUSTOM_GET
        pkt[1] = VIA_CH_EXT
        pkt[2] = VIA_VAL_TFT_RAW
        pkt[3] = 1 if consume else 0
        rep = self._via.exchange(bytes(pkt), timeout_s=0.5)
        if rep[4] == 0:
            return None
        rx_cmd = rep[5]
        rx_len = min(24, rep[6])
        return rx_cmd, bytes(rep[7 : 7 + rx_len])

    def exchange(self, cmd: int, payload: bytes = b"", timeout_s: float = 1.0) -> tuple[int, bytes]:
        self.send_frame(cmd, payload)
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            res = self.poll_rx(consume=True)
            if res is not None:
                return res
            time.sleep(0.002)
        raise TimeoutError(f"Timeout waiting for U24 reply to cmd=0x{cmd:02X}")


def parse_info_reply(payload: bytes) -> dict[str, object]:
    if len(payload) < 19:
        raise ValueError(f"Short cmd=0x08 reply ({len(payload)} B): {payload.hex(' ')}")
    status = payload[0]
    oled_ver, flash_ver, proto = struct.unpack_from("<HHH", payload, 1)
    ident = payload[7:19]
    mode = "BOOTLOADER" if proto == 0x0100 else ("APP" if proto == 0x0001 else f"UNKNOWN(0x{proto:04X})")
    return {
        "status": status,
        "oled_ver": oled_ver,
        "flash_ver": flash_ver,
        "proto": proto,
        "mode": mode,
        "id": ident,
    }


def load_tft_app_image(path: pathlib.Path) -> bytes:
    """Load full x5_plain.bin (slices from 0x10000) or pre-sliced app image; validate & pad to 64B."""
    raw = path.read_bytes()
    if raw[: len(OLED_MAGIC)] == OLED_MAGIC:
        app = raw
    elif len(raw) > OLED_APP_OFFSET and raw[OLED_APP_OFFSET : OLED_APP_OFFSET + len(OLED_MAGIC)] == OLED_MAGIC:
        app = raw[OLED_APP_OFFSET:]
    else:
        raise ValueError(f"{path} does not contain RY5060RY5060 magic at 0x0 or 0x10000")

    rem = len(app) % CHUNK_SIZE
    if rem != 0:
        app = app + (b"\xFF" * (CHUNK_SIZE - rem))
    if len(app) > OLED_APP_MAX_BYTES:
        raise ValueError(f"TFT app too large: {len(app)} B > {OLED_APP_MAX_BYTES} B")
    return app


def query_info(bridge: TftUartBridge, retries: int = 5, timeout_s: float = 0.5) -> dict[str, object]:
    last_err: Exception | None = None
    for _ in range(retries):
        try:
            rx_cmd, payload = bridge.exchange(0x08, b"", timeout_s=timeout_s)
            if rx_cmd == 0x88:
                return parse_info_reply(payload)
        except Exception as exc:
            last_err = exc
            time.sleep(0.1)
    raise RuntimeError(f"Failed to query U24 cmd=0x08: {last_err}")


def flash_tft(bridge: TftUartBridge, app: bytes) -> None:
    chunks = len(app) // CHUNK_SIZE
    sum32 = sum(app) & 0xFFFFFFFF
    print(f"Prepared U24 app image: {len(app)} B ({chunks} chunks x 64 B), sum32=0x{sum32:08X}")

    info = query_info(bridge)
    print(f"Current U24: mode={info['mode']} oled_ver=0x{info['oled_ver']:04X} flash_ver=0x{info['flash_ver']:04X} id={info['id']!r}")

    if info["mode"] != "BOOTLOADER":
        print("Entering U24 bootloader (cmd=0x3F)...")
        rx_cmd, rx_pay = bridge.exchange(0x3F, b"", timeout_s=1.0)
        if rx_cmd != 0xBF or not rx_pay or rx_pay[0] != 0x00:
            raise RuntimeError(f"Unexpected 0x3F reply: cmd=0x{rx_cmd:02X} payload={rx_pay.hex(' ')}")
        print("Waiting for U24 bootloader erase (11 x 32 KB)...")
        time.sleep(1.2)
        for _ in range(30):
            try:
                info = query_info(bridge, retries=1, timeout_s=0.3)
                if info["mode"] == "BOOTLOADER":
                    break
            except Exception:
                time.sleep(0.2)
        else:
            raise RuntimeError("U24 did not enter bootloader mode after cmd=0x3F")
        print(f"U24 in BOOTLOADER: oled_ver=0x{info['oled_ver']:04X} id={info['id']!r}")

    # 0x3D: start flash with chunk_count (LE16)
    rx_cmd, rx_pay = bridge.exchange(0x3D, struct.pack("<H", chunks), timeout_s=2.0)
    if rx_cmd != 0xBD or not rx_pay or rx_pay[0] != 0x00:
        raise RuntimeError(f"Unexpected 0x3D reply: cmd=0x{rx_cmd:02X} payload={rx_pay.hex(' ')}")

    # 0x3C: stream 64-byte chunks
    t0 = time.monotonic()
    for i in range(chunks):
        chunk = app[i * CHUNK_SIZE : (i + 1) * CHUNK_SIZE]
        rx_cmd, rx_pay = bridge.exchange(0x3C, chunk, timeout_s=1.0)
        if rx_cmd != 0xBC or not rx_pay or rx_pay[0] != 0x00:
            raise RuntimeError(f"Chunk {i}/{chunks} failed: cmd=0x{rx_cmd:02X} payload={rx_pay.hex(' ')}")
        if (i + 1) % 256 == 0 or (i + 1) == chunks:
            pct = (i + 1) * 100 // chunks
            elapsed = time.monotonic() - t0
            print(f"\rFlashing U24: {i + 1}/{chunks} ({pct}%) [{elapsed:.1f}s]", end="", flush=True)
    print()

    # 0x3B: commit with sum32 (LE32)
    rx_cmd, rx_pay = bridge.exchange(0x3B, struct.pack("<I", sum32), timeout_s=2.0)
    if rx_cmd != 0xBB or len(rx_pay) < 5:
        raise RuntimeError(f"Unexpected 0x3B reply: cmd=0x{rx_cmd:02X} payload={rx_pay.hex(' ')}")
    status = rx_pay[0]
    dev_sum = struct.unpack_from("<I", rx_pay, 1)[0]
    if status != 0x55:
        raise RuntimeError(f"U24 commit failed: status=0x{status:02X} dev_sum=0x{dev_sum:08X} expected=0x{sum32:08X}")
    print(f"U24 commit OK! status=0x{status:02X} sum32=0x{dev_sum:08X}. Rebooting into new TFT app...")

    time.sleep(1.5)
    info = query_info(bridge, retries=10, timeout_s=0.5)
    print(f"Post-flash U24: mode={info['mode']} oled_ver=0x{info['oled_ver']:04X} flash_ver=0x{info['flash_ver']:04X} id={info['id']!r}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="GamaKay TK75 HE V2 U24 (YC3123-Q) TFT Flasher")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--info", action="store_true", help="Query U24 version/mode over raw UART7 bridge")
    group.add_argument("--flash", type=pathlib.Path, help="Flash U24 app binary (x5_plain.bin or app slice)")
    parser.add_argument("--dry-run", action="store_true", help="Validate image without touching hardware")
    args = parser.parse_args(argv)

    if args.flash is not None:
        app = load_tft_app_image(args.flash)
        if args.dry_run:
            print(f"Dry-run OK: {args.flash} -> {len(app)} B ({len(app)//CHUNK_SIZE} chunks), sum32=0x{sum(app)&0xFFFFFFFF:08X}")
            return 0

    bridge = TftUartBridge()
    try:
        bridge.set_hud_paused(True)
        time.sleep(0.05)
        if args.info:
            info = query_info(bridge)
            print(f"U24 mode={info['mode']} oled_ver=0x{info['oled_ver']:04X} flash_ver=0x{info['flash_ver']:04X} proto=0x{info['proto']:04X} id={info['id']!r}")
        else:
            flash_tft(bridge, app)
    finally:
        try:
            bridge.set_hud_paused(False)
        except Exception:
            pass
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
