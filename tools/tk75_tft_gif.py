#!/usr/bin/env python3
"""
GamaKay TK75 HE V2 Custom Animated GIF Screensaver CLI (U9 SPI NOR @ 0x6C0000..0x7BFFFF).

Converts any animated GIF / WebP / PNG into up to 31 native 128x128 RGB565 frames and
flashes them safely into the dedicated 1.0 MB U9 GIF partition via U24 patch v1.3+
(cmd=0x28 flasher, cmd=0x29 player) over the AT32F405 VIA raw UART7 bridge.

Also supports switching between all 7 idle screensaver modes via VIA ch=7 val_id=9:
  off | clock | eyes | cat | matrix | dvd | gif
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import time
import zlib
from typing import Sequence

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import tk75_hid  # noqa: E402
from tk75_tft_flash import TftUartBridge  # noqa: E402

U9_GIF_MIN_ADDR = 0x006C0000
U9_GIF_MAX_ADDR = 0x007C0000
U9_GIF_HDR_ADDR = 0x006C0000
U9_GIF_DATA_ADDR = 0x006C8000
U9_GIF_FRAME_BYTES = 0x8000  # 128 * 128 * 2
U9_GIF_MAX_FRAMES = 31
U9_GIF_HDR_MAGIC = b"TKGF"
U9_GIF_HDR_VERSION = 2
U9_GIF_HDR_V1_SIZE = 32
U9_GIF_HDR_SIZE = 32 + U9_GIF_MAX_FRAMES * 2  # 94 bytes (base 32B + 31 x uint16 per-frame delays)
U9_PROG_CHUNK = 64  # Evenly divides 256 B flash page and fits 3 VIA packets

CMD_VERSION = 0x25
CMD_U9_FLASH = 0x28
CMD_GIF_PLAY = 0x29

SAVER_MODES: dict[str, int] = {
    "off": 0,
    "clock": 1,
    "eyes": 2,
    "cat": 3,
    "matrix": 4,
    "dvd": 5,
    "gif": 6,
}


def rgb_image_to_col_major_be565(rgb_img) -> bytes:
    """Convert 128x128 RGB Pillow image into ST7735S column-major big-endian RGB565 bytes."""
    w, h = rgb_img.size
    if (w, h) != (128, 128):
        raise ValueError(f"Expected 128x128 image, got {w}x{h}")
    raw = rgb_img.tobytes()
    out = bytearray(w * h * 2)
    idx = 0
    for x in range(w):
        for y in range(h):
            off = (y * w + x) * 3
            r = raw[off]
            g = raw[off + 1]
            b = raw[off + 2]
            val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[idx] = (val >> 8) & 0xFF
            out[idx + 1] = val & 0xFF
            idx += 2
    return bytes(out)


def load_gif_frames(
    path: pathlib.Path,
    max_frames: int = U9_GIF_MAX_FRAMES,
    fps_override: float | None = None,
    stretch: bool = False,
) -> tuple[list[bytes], list[int]]:
    """Decode GIF/WebP/PNG into <=31 128x128 column-major BE RGB565 frames + per-frame delays (ms)."""
    from PIL import Image, ImageOps, ImageSequence

    if not (1 <= max_frames <= U9_GIF_MAX_FRAMES):
        raise ValueError(f"max_frames must be 1..{U9_GIF_MAX_FRAMES}")

    raw_rgba_frames: list[Image.Image] = []
    durations_ms: list[int] = []

    with Image.open(path) as im:
        for frame in ImageSequence.Iterator(im):
            dur = int(frame.info.get("duration", im.info.get("duration", 100)) or 100)
            durations_ms.append(max(33, min(10000, dur)))
            raw_rgba_frames.append(frame.convert("RGBA"))

    if not raw_rgba_frames:
        raise ValueError(f"No frames decoded from {path}")

    # Evenly subsample when source GIF has more frames than U9 slot limit (31)
    total = len(raw_rgba_frames)
    if total > max_frames:
        indices = [int(round(i * (total - 1) / (max_frames - 1))) for i in range(max_frames)]
        selected = [raw_rgba_frames[i] for i in indices]
        scale = total / float(max_frames)
        selected_durs = [max(33, min(10000, int(round(durations_ms[i] * scale)))) for i in indices]
    else:
        selected = raw_rgba_frames
        selected_durs = durations_ms

    if fps_override is not None:
        if fps_override <= 0:
            raise ValueError("--fps must be > 0")
        fixed = max(33, min(10000, int(round(1000.0 / fps_override))))
        selected_durs = [fixed] * len(selected)

    encoded: list[bytes] = []
    for rgba in selected:
        canvas = Image.new("RGB", (128, 128), (0, 0, 0))
        if stretch:
            resized = rgba.resize((128, 128), Image.Resampling.LANCZOS)
            canvas.paste(resized, (0, 0), resized)
        else:
            fitted = ImageOps.contain(rgba, (128, 128), method=Image.Resampling.LANCZOS)
            ox = (128 - fitted.width) // 2
            oy = (128 - fitted.height) // 2
            canvas.paste(fitted, (ox, oy), fitted)
        encoded.append(rgb_image_to_col_major_be565(canvas))

    return encoded, selected_durs


def build_gif_header(
    name: str,
    frame_count: int,
    delay_ms: int | Sequence[int],
    data_crc32: int,
) -> bytes:
    """Pack 94-byte U9 'TKGF' v2 header (32B base + 31 x uint16 per-frame delays) @ 0x006C0000."""
    if not (1 <= frame_count <= U9_GIF_MAX_FRAMES):
        raise ValueError(f"frame_count must be 1..{U9_GIF_MAX_FRAMES}")
    if isinstance(delay_ms, int):
        delays = [max(33, min(10000, delay_ms))] * frame_count
    else:
        delays = [max(33, min(10000, int(d))) for d in delay_ms][:frame_count]
        while len(delays) < frame_count:
            delays.append(delays[-1] if delays else 100)
    default_delay = delays[0]
    padded_delays = delays + [default_delay] * (U9_GIF_MAX_FRAMES - len(delays))
    name_bytes = name.encode("ascii", errors="ignore")[:15]
    name_padded = name_bytes.ljust(16, b"\x00")
    base = struct.pack(
        "<4sBBHII16s",
        U9_GIF_HDR_MAGIC,
        U9_GIF_HDR_VERSION,
        frame_count,
        default_delay,
        data_crc32 & 0xFFFFFFFF,
        0,
        name_padded,
    )
    return base + struct.pack(f"<{U9_GIF_MAX_FRAMES}H", *padded_delays)


def parse_gif_header(raw: bytes) -> dict[str, object] | None:
    """Decode U9 'TKGF' v1 (32B) or v2 (94B) header; return None if magic/version/count invalid."""
    if len(raw) < U9_GIF_HDR_V1_SIZE:
        return None
    magic, ver, count, delay_ms, crc32, _res, raw_name = struct.unpack_from("<4sBBHII16s", raw, 0)
    if magic != U9_GIF_HDR_MAGIC or ver not in (1, 2) or not (1 <= count <= U9_GIF_MAX_FRAMES):
        return None
    name = raw_name.split(b"\x00", 1)[0].decode("ascii", errors="replace")
    if ver == 2 and len(raw) >= U9_GIF_HDR_SIZE:
        all_d = struct.unpack_from(f"<{U9_GIF_MAX_FRAMES}H", raw, U9_GIF_HDR_V1_SIZE)
        frame_delays = list(all_d[:count])
    else:
        frame_delays = [delay_ms] * count
    return {
        "version": ver,
        "frame_count": count,
        "delay_ms": delay_ms,
        "frame_delays_ms": frame_delays,
        "crc32": crc32,
        "name": name,
    }


def _pack_addr24(addr: int) -> bytes:
    return bytes([addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF])


def _u9_erase_32k(bridge: TftUartBridge, addr: int) -> None:
    if not (U9_GIF_MIN_ADDR <= addr < U9_GIF_MAX_ADDR) or (addr & 0x7FFF) != 0:
        raise ValueError(f"Refusing out-of-bounds U9 erase @ 0x{addr:06X}")
    rx_cmd, pay = bridge.exchange(CMD_U9_FLASH, b"\x00" + _pack_addr24(addr), timeout_s=2.0)
    if rx_cmd != 0xA8 or not pay or pay[0] != 0x00:
        raise RuntimeError(f"U9 erase 0x{addr:06X} failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")


def _u9_prog_range(bridge: TftUartBridge, base_addr: int, data: bytes, progress_cb=None) -> None:
    if base_addr < U9_GIF_MIN_ADDR or (base_addr + len(data)) > U9_GIF_MAX_ADDR:
        raise ValueError(f"Refusing out-of-bounds U9 write @ 0x{base_addr:06X}+{len(data)}")
    off = 0
    total = len(data)
    while off < total:
        addr = base_addr + off
        page_rem = 256 - (addr & 0xFF)
        n = min(U9_PROG_CHUNK, page_rem, total - off)
        chunk = data[off : off + n]
        rx_cmd, pay = bridge.exchange(CMD_U9_FLASH, b"\x01" + _pack_addr24(addr) + chunk, timeout_s=1.0)
        if rx_cmd != 0xA8 or not pay or pay[0] != 0x00:
            raise RuntimeError(f"U9 write 0x{addr:06X} failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")
        off += n
        if progress_cb is not None:
            progress_cb(off, total)


def _u9_read_bytes(bridge: TftUartBridge, base_addr: int, length: int) -> bytes:
    out = bytearray()
    off = 0
    while off < length:
        n = min(16, length - off)
        addr = base_addr + off
        rx_cmd, pay = bridge.exchange(CMD_U9_FLASH, b"\x02" + _pack_addr24(addr) + bytes([n]), timeout_s=1.0)
        if rx_cmd != 0xA8 or len(pay) < (1 + n) or pay[0] != 0x00:
            raise RuntimeError(f"U9 read 0x{addr:06X} failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")
        out.extend(pay[1 : 1 + n])
        off += n
    return bytes(out)


def _u9_crc32(bridge: TftUartBridge, base_addr: int, length: int) -> int:
    payload = b"\x03" + _pack_addr24(base_addr) + struct.pack("<I", length)
    rx_cmd, pay = bridge.exchange(CMD_U9_FLASH, payload, timeout_s=10.0)
    if rx_cmd != 0xA8 or len(pay) < 5 or pay[0] != 0x00:
        raise RuntimeError(f"U9 CRC32 failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")
    return struct.unpack_from("<I", pay, 1)[0]


def _check_u24_v13(bridge: TftUartBridge) -> tuple[int, int]:
    rx_cmd, pay = bridge.exchange(CMD_VERSION, b"", timeout_s=1.0)
    if rx_cmd != 0xA5 or len(pay) < 3 or pay[0] != 0x00:
        raise RuntimeError(f"Unexpected U24 version reply: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")
    maj, minor = pay[1], pay[2]
    if (maj, minor) < (1, 3):
        raise RuntimeError(
            f"U24 patch v{maj}.{minor} detected; v1.3+ required. "
            "Flash backups/patched_u24_lcd_v102_custom.bin with tools/tk75_tft_flash.py first."
        )
    return maj, minor


def set_via_screensaver_mode(mode_id: int, save_eeprom: bool = True) -> None:
    """Set VIA ch=7 val_id=9 (tft_screensaver) and optionally commit EEPROM."""
    via = tk75_hid.open_via_transport(0x3151, 0x502B, tk75_hid.USAGE_PAGE_VIA)
    try:
        pkt = bytearray(32)
        pkt[0] = 0x07  # ID_CUSTOM_SET_VALUE
        pkt[1] = 7     # VIA_CH_TK75_TFT_ENC
        pkt[2] = 9     # tft_screensaver
        pkt[3] = mode_id & 0xFF
        via.exchange(bytes(pkt), timeout_s=0.5)
        if save_eeprom:
            save_pkt = bytearray(32)
            save_pkt[0] = 0x09  # ID_CUSTOM_SAVE
            via.exchange(bytes(save_pkt), timeout_s=0.5)
    finally:
        via.close()


def _play_preview(bridge: TftUartBridge, frame_delays_ms: Sequence[int], seconds: float) -> None:
    frame_count = len(frame_delays_ms)
    if frame_count <= 0 or seconds <= 0:
        return
    print(f"Previewing {frame_count} frames ({min(frame_delays_ms)}..{max(frame_delays_ms)} ms/frame) for {seconds:.1f}s...")
    deadline = time.monotonic() + seconds
    idx = 0
    while time.monotonic() < deadline:
        t0 = time.monotonic()
        rx_cmd, pay = bridge.exchange(CMD_GIF_PLAY, bytes([1, idx]), timeout_s=1.0)
        if rx_cmd != 0xA9 or not pay or pay[0] != 0x00:
            raise RuntimeError(f"Preview frame {idx} failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")
        step_s = max(0.033, frame_delays_ms[idx] / 1000.0)
        idx = (idx + 1) % frame_count
        rem = min(step_s - (time.monotonic() - t0), max(0.0, deadline - time.monotonic()))
        if rem > 0:
            time.sleep(rem)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="GamaKay TK75 HE V2 Animated GIF Screensaver CLI")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_up = sub.add_parser("upload", help="Convert & flash animated GIF/WebP/PNG to U9 GIF slot")
    p_up.add_argument("gif", type=pathlib.Path)
    p_up.add_argument("--name", type=str, default=None, help="Short label stored in header (<=15 ASCII)")
    p_up.add_argument("--max-frames", type=int, default=U9_GIF_MAX_FRAMES)
    p_up.add_argument("--fps", type=float, default=None, help="Override playback FPS")
    p_up.add_argument("--stretch", action="store_true", help="Stretch instead of aspect-preserving fit")
    p_up.add_argument("--no-activate", action="store_true", help="Do not switch SAVR mode to GIF after upload")
    p_up.add_argument("--preview", type=float, default=0.0, help="Live-preview uploaded GIF for N seconds")
    p_up.add_argument("--dry-run", action="store_true", help="Convert & validate without touching hardware")

    sub.add_parser("info", help="Read currently stored custom GIF header from U9")

    p_prev = sub.add_parser("preview", help="Live-preview stored U9 GIF on keyboard TFT now")
    p_prev.add_argument("--seconds", type=float, default=6.0)

    sub.add_parser("clear", help="Erase custom GIF header in U9 (0x6C0000)")

    p_mode = sub.add_parser("set-mode", help="Switch idle screensaver mode via VIA (ch=7 val=9)")
    p_mode.add_argument("mode", choices=list(SAVER_MODES.keys()))
    p_mode.add_argument("--dry-run", action="store_true")

    args = parser.parse_args(argv)

    if args.cmd == "set-mode":
        mid = SAVER_MODES[args.mode]
        if args.dry_run:
            print(f"Dry-run set-mode: {args.mode} -> {mid}")
            return 0
        set_via_screensaver_mode(mid)
        print(f"Set idle screensaver mode to '{args.mode}' ({mid}) and saved EEPROM")
        return 0

    if args.cmd == "upload":
        frames, frame_delays = load_gif_frames(
            args.gif,
            max_frames=args.max_frames,
            fps_override=args.fps,
            stretch=args.stretch,
        )
        blob = b"".join(frames)
        crc = zlib.crc32(blob) & 0xFFFFFFFF
        label = args.name if args.name else args.gif.stem
        hdr = build_gif_header(label, len(frames), frame_delays, crc)
        if args.dry_run:
            print(
                f"Dry-run OK: {args.gif.name} -> {len(frames)} frames, "
                f"{min(frame_delays)}..{max(frame_delays)} ms (loop {sum(frame_delays) / 1000.0:.2f}s), "
                f"{len(blob)} B, crc32=0x{crc:08X}"
            )
            return 0

        bridge = TftUartBridge()
        try:
            bridge.set_hud_paused(True)
            time.sleep(0.05)
            maj, minor = _check_u24_v13(bridge)
            print(f"U24 patch v{maj}.{minor} ready. Uploading '{label}' ({len(frames)} frames, {len(blob) // 1024} KB)...")

            # 1. Erase header block + required frame blocks (atomic: header written last)
            _u9_erase_32k(bridge, U9_GIF_HDR_ADDR)
            for i in range(len(frames)):
                _u9_erase_32k(bridge, U9_GIF_DATA_ADDR + i * U9_GIF_FRAME_BYTES)

            # 2. Program all frames
            t0 = time.monotonic()
            total_b = len(blob)
            written_prev = 0
            for i, fbytes in enumerate(frames):
                faddr = U9_GIF_DATA_ADDR + i * U9_GIF_FRAME_BYTES

                def _cb(done_in_frame: int, _ftot: int, base: int = written_prev) -> None:
                    done = base + done_in_frame
                    if done == total_b or (done % 4096) == 0:
                        pct = (done * 100) // total_b
                        dt = max(0.001, time.monotonic() - t0)
                        kbps = (done / 1024.0) / dt
                        print(f"\rFlashing U9 GIF: {done // 1024}/{total_b // 1024} KB ({pct}%) [{kbps:.1f} KB/s]", end="", flush=True)

                _u9_prog_range(bridge, faddr, fbytes, progress_cb=_cb)
                written_prev += len(fbytes)
            print()

            # 3. Hardware CRC32 verification before committing header
            dev_crc = _u9_crc32(bridge, U9_GIF_DATA_ADDR, total_b)
            if dev_crc != crc:
                raise RuntimeError(f"U9 CRC32 mismatch: device=0x{dev_crc:08X} expected=0x{crc:08X}")

            # 4. Commit 94-byte TKGF v2 header & verify via cmd=0x29 op=0
            _u9_prog_range(bridge, U9_GIF_HDR_ADDR, hdr)
            rx_cmd, pay = bridge.exchange(CMD_GIF_PLAY, b"\x00", timeout_s=1.0)
            if rx_cmd != 0xA9 or len(pay) < 5 or pay[0] != 0x00 or pay[1] != 1:
                raise RuntimeError(f"Header commit verify failed: cmd=0x{rx_cmd:02X} pay={pay.hex(' ')}")
            print(
                f"Verified U9 GIF OK! frames={pay[2]} loop={sum(frame_delays) / 1000.0:.2f}s "
                f"({min(frame_delays)}..{max(frame_delays)}ms) crc32=0x{crc:08X}"
            )

            if args.preview > 0:
                _play_preview(bridge, frame_delays, args.preview)
        finally:
            try:
                bridge.set_hud_paused(False)
            except Exception:
                pass
            bridge.close()

        if not args.no_activate:
            set_via_screensaver_mode(SAVER_MODES["gif"])
            print("Activated idle screensaver mode 'gif' (6) and saved EEPROM")
        return 0

    bridge = TftUartBridge()
    try:
        bridge.set_hud_paused(True)
        time.sleep(0.05)
        maj, minor = _check_u24_v13(bridge)

        if args.cmd == "clear":
            _u9_erase_32k(bridge, U9_GIF_HDR_ADDR)
            print(f"Cleared U9 GIF header @ 0x{U9_GIF_HDR_ADDR:06X} (U24 v{maj}.{minor})")
            return 0

        raw_hdr = _u9_read_bytes(bridge, U9_GIF_HDR_ADDR, U9_GIF_HDR_SIZE)
        info = parse_gif_header(raw_hdr)
        if info is None:
            print(f"No valid custom GIF stored in U9 (U24 v{maj}.{minor})")
            return 0
        f_durs: list[int] = list(info["frame_delays_ms"])  # type: ignore[arg-type]
        print(
            f"U9 GIF (v{info['version']}): name={info['name']!r} frames={info['frame_count']} "
            f"delays={min(f_durs)}..{max(f_durs)}ms (loop {sum(f_durs) / 1000.0:.2f}s) "
            f"crc32=0x{int(info['crc32']):08X}"
        )
        if args.cmd == "preview":
            _play_preview(bridge, f_durs, args.seconds)
    finally:
        try:
            bridge.set_hud_paused(False)
        except Exception:
            pass
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
