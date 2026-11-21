from __future__ import annotations

import datetime
import json
import pathlib
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import tk75_dfu
import tk75_hid
import tk75_tft_cli


def test_c_firmware_unit_suite(tmp_path: pathlib.Path) -> None:
    """Compile and run host C11 unit tests under strict warnings-as-errors."""
    exe = tmp_path / "c_unit_tests"
    c_files = [
        ROOT / "tests/c_unit_tests.c",
        ROOT / "src/hall/hall_engine.c",
        ROOT / "src/hall/hall_cal.c",
        ROOT / "src/hall/mode_switches.c",
        ROOT / "src/hall/encoder.c",
        ROOT / "src/storage/factory_cal.c",
        ROOT / "src/storage/flash_eeprom.c",
        ROOT / "src/keymap/keymap.c",
        ROOT / "src/keymap/macro_player.c",
        ROOT / "src/keymap/macro_recorder.c",
        ROOT / "src/via/via_protocol.c",
        ROOT / "src/rgb/rgb_effects.c",
        ROOT / "src/tft/uart7_tft.c",
        ROOT / "src/tft/hud_engine.c",
        ROOT / "src/tft/pixel_hud.c",
        ROOT / "src/rf/spi3_rf.c",
    ]
    inc_dirs = [
        ROOT / "src/hall",
        ROOT / "src/storage",
        ROOT / "src/keymap",
        ROOT / "src/via",
        ROOT / "src/rgb",
        ROOT / "src/tft",
        ROOT / "src/rf",
    ]
    cmd = [
        "gcc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O2",
        *(f"-I{d}" for d in inc_dirs),
        *(str(f) for f in c_files),
        "-o",
        str(exe),
    ]
    compile_res = subprocess.run(cmd, capture_output=True, text=True, check=False)
    assert compile_res.returncode == 0, compile_res.stderr

    run_res = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
    assert run_res.returncode == 0, run_res.stderr
    assert "ALL_C_TESTS_PASSED" in run_res.stdout


def test_built_firmware_binary_passes_verify_image() -> None:
    bin_path = ROOT / "build" / "tk75_he_v2_via.bin"
    assert bin_path.exists(), "Firmware binary must be built before running tool tests"
    res = subprocess.run(
        [sys.executable, str(ROOT / "tools/verify_image.py"), str(bin_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert res.returncode == 0, res.stderr
    assert "OK:" in res.stdout


def test_via_json_v3_definition_and_matrix_coverage() -> None:
    via_path = ROOT / "via" / "tk75hev2_optionb_via.json"
    data = json.loads(via_path.read_text(encoding="utf-8"))

    assert data["vendorId"] == "0x3151"
    assert data["productId"] == "0x502B"
    assert data["matrix"] == {"rows": 6, "cols": 16}

    iso_coords: list[tuple[int, int]] = []
    encoders: list[str] = []
    for row in data["layouts"]["keymap"]:
        for cell in row:
            if not isinstance(cell, str):
                continue
            lines = cell.split("\n")
            if any(line.startswith("e") for line in lines):
                encoders.append(cell)
                continue
            opt = lines[3] if len(lines) > 3 else ""
            if opt == "0,0":  # ANSI-only variant key (e.g. 2,13)
                continue
            parts = lines[0].split(",")
            if len(parts) == 2 and all(p.isdigit() for p in parts):
                rc = (int(parts[0]), int(parts[1]))
                assert 0 <= rc[0] < 6 and 0 <= rc[1] < 16
                iso_coords.append(rc)

    assert len(iso_coords) == 82, f"Expected 82 physical ISO switches, found {len(iso_coords)}"
    assert len(set(iso_coords)) == 82, "Duplicate row,col found in VIA KLE layout"
    assert len(encoders) == 1, "Expected 1 rotary encoder widget e0"
    assert (5, 4) not in iso_coords, "Slot (5,4) is VBAT ADC and must not appear as a switch"

    menu_labels = [m["label"] for m in data["menus"]]
    assert menu_labels == ["Lighting", "Hall Effect", "Sidelight", "TFT & Encoder", "Wireless & Power"]


def test_dfu_padding_checksum_and_mock_flash_roundtrip(tmp_path: pathlib.Path) -> None:
    # Non-multiple of 64 to verify 0xFF tail padding matches bootloader C logic
    raw = bytearray(b"AT32F405 8KMKB  " + (b"\xAA" * (0x200 - 16)))
    raw += b"\x00\x98\x01\x20" + b"\xb1\x5a\x00\x08"  # SP + odd Thumb Reset_Handler
    raw += b"\x11\x22\x33" * 9  # 27 trailing bytes -> 547 B total (non-multiple of 64)

    ok, _ = tk75_dfu.validate_firmware_blob(bytes(raw))
    assert ok

    padded = tk75_dfu.pad_firmware_64(bytes(raw))
    assert len(padded) % 64 == 0
    assert padded.endswith(b"\xFF" * (len(padded) - len(raw)))
    csum24 = tk75_dfu.calculate_bootloader_checksum24(padded)

    start = tk75_dfu.build_dfu_start_packet(len(padded))
    blocks = len(padded) // 64
    assert start[:4] == bytes([0xBA, 0xC0, blocks & 0xFF, (blocks >> 8) & 0xFF])

    complete = tk75_dfu.build_dfu_complete_packet(len(padded), csum24)
    assert complete[:2] == bytes([0xBA, 0xC2])
    assert (complete[4] | (complete[5] << 8) | (complete[6] << 16)) == csum24

    class MockBoot:
        def __init__(self, force_nack: bool = False) -> None:
            self.sent: list[bytes] = []
            self.blocks = 0
            self.force_nack = force_nack

        def send_feature(self, payload64: bytes) -> None:
            self.sent.append(payload64)

        def recv_feature(self) -> bytes:
            last = self.sent[-1]
            rep = bytearray(64)
            if last[0] == 0xBA and last[1] == 0xC0:
                rep[0:4] = bytes([0xAB, 0xC0, last[2], last[3]])
            elif last[0] == 0xBA and last[1] == 0xC2:
                rep[0:4] = bytes([0xAB, 0xC2, last[2], last[3]])
                rep[4] = 0xAA if self.force_nack else 0x55
            return bytes(rep)

        def close(self) -> None:
            pass

    ok_dev = MockBoot()
    got_sum = tk75_dfu.flash_via_transport(ok_dev, bytes(raw))
    assert got_sum == csum24
    assert len(ok_dev.sent) == 1 + blocks + 1

    bad_dev = MockBoot(force_nack=True)
    with pytest.raises(RuntimeError, match="NACK"):
        tk75_dfu.flash_via_transport(bad_dev, bytes(raw))

    fake_bin = tmp_path / "fw.bin"
    fake_bin.write_bytes(bytes(raw))
    assert tk75_dfu.main(["--dry-run", "--flash", str(fake_bin)]) == 0


def test_tft_cli_clock_and_image_converter(tmp_path: pathlib.Path) -> None:
    from PIL import Image

    dt = datetime.datetime(2026, 11, 15, 13, 45, 59)
    pkt = tk75_tft_cli.build_rtc_packet(dt)
    assert len(pkt) == 32
    assert list(pkt[:10]) == [0x07, 8, 2, 0x07, 0xEA, 11, 15, 13, 45, 59]

    img_path = tmp_path / "badge.png"
    img = Image.new("RGB", (2, 2), (0, 0, 0))
    img.putpixel((0, 0), (255, 0, 0))  # red 0xF800
    img.putpixel((0, 1), (0, 0, 255))  # blue 0x001F (col 0, row 1 -> 2nd in column-major!)
    img.putpixel((1, 0), (0, 255, 0))  # green 0x07E0 (col 1, row 0 -> 3rd in column-major!)
    img.save(img_path)
    rgb565 = tk75_tft_cli.image_to_rgb565_bytes(img_path, width=2, height=2)
    assert len(rgb565) == 2 * 2 * 2
    assert rgb565[:2] == bytes([0xF8, 0x00])  # (0,0) red big-endian
    assert rgb565[2:4] == bytes([0x00, 0x1F])  # (0,1) blue big-endian (column-major)
    assert rgb565[4:6] == bytes([0x07, 0xE0])  # (1,0) green big-endian

    big_blob = bytes(range(128))
    chunks = tk75_tft_cli.chunk_media_packets(big_blob, slot=2, width=8, height=8)
    assert len(chunks) == 1 + 6  # PREP + (5*24 + 8)
    assert chunks[0][3] == 0x21 and chunks[0][4:6] == bytes([0xFF, 0xFF])  # PREP seq=0xFFFF
    assert chunks[1][3] == 0x20  # slot 2 data start (PREP already sent bit0)
    assert chunks[-1][3] == 0x22  # slot 2 end bit
    assert chunks[-1][6] == 8

    erase_pkt = tk75_tft_cli.build_media_erase_packet()
    assert list(erase_pkt[:4]) == [0x07, 8, 3, 0x04]
    assert tk75_tft_cli.main(["erase-screen", "--dry-run"]) == 0


def test_hid_sysfs_discovery_and_stock_checksum(tmp_path: pathlib.Path) -> None:
    assert tk75_hid.parse_hid_usage_page(bytes.fromhex("05010906a101")) == 0x0001
    assert tk75_hid.parse_hid_usage_page(bytes.fromhex("06ffff0902a101")) == 0xFFFF
    assert tk75_hid.parse_hid_usage_page(bytes.fromhex("0660ff0901a101")) == 0xFF60

    h0 = tmp_path / "hidraw0" / "device"
    h2 = tmp_path / "hidraw2" / "device"
    h0.mkdir(parents=True)
    h2.mkdir(parents=True)
    (h0 / "uevent").write_text("HID_ID=0003:00003151:00005030\n", encoding="utf-8")
    (h0 / "report_descriptor").write_bytes(bytes.fromhex("05010906a101"))
    (h2 / "uevent").write_text("HID_ID=0003:00003151:00005030\n", encoding="utf-8")
    (h2 / "report_descriptor").write_bytes(bytes.fromhex("06ffff0902a101"))

    assert tk75_hid.find_hidraw_path(0x3151, 0x5030, 0xFFFF, sysfs_root=tmp_path) == "/dev/hidraw2"
    assert tk75_hid.find_hidraw_path(0x3151, 0x5030, None, sysfs_root=tmp_path) == "/dev/hidraw2"
    assert tk75_hid.find_hidraw_path(0x3151, 0x502B, 0xFF60, sysfs_root=tmp_path) is None

    prep = tk75_dfu.build_stock_feature_packet(bytes([0xC5, 0x3A]))
    boot = tk75_dfu.build_stock_feature_packet(bytes([0x7F, 0x55, 0xAA, 0x55, 0xAA]))
    assert len(prep) == 64 and len(boot) == 64
    assert (sum(prep[:8]) & 0xFF) == 0xFF and prep[7] == 0x00
    assert (sum(boot[:8]) & 0xFF) == 0xFF and boot[7] == 0x82


def test_open_bootloader_retries_transient_udev_errors(monkeypatch: pytest.MonkeyPatch) -> None:
    attempts: list[str | bytes] = []
    sleeps: list[float] = []
    fake_transport = object()

    def _open(path: str | bytes):
        attempts.append(path)
        if len(attempts) == 1:
            raise PermissionError(13, "Permission denied", path)
        if len(attempts) == 2:
            raise FileNotFoundError(2, "No such file", path)
        return fake_transport  # type: ignore[return-value]

    monkeypatch.setattr(tk75_dfu.time, "sleep", sleeps.append)
    transport = tk75_dfu.open_bootloader_with_retry(
        wait_fn=lambda: "/dev/hidraw9",
        open_fn=_open,
        timeout_s=1.0,
    )
    assert transport is fake_transport
    assert attempts == ["/dev/hidraw9", "/dev/hidraw9", "/dev/hidraw9"]
    assert len(sleeps) == 2


def test_open_bootloader_raises_after_udev_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    now = [0.0]

    def _monotonic() -> float:
        return now[0]

    def _sleep(sec: float) -> None:
        now[0] += sec

    monkeypatch.setattr(tk75_dfu.time, "monotonic", _monotonic)
    monkeypatch.setattr(tk75_dfu.time, "sleep", _sleep)

    with pytest.raises(PermissionError):
        tk75_dfu.open_bootloader_with_retry(
            wait_fn=lambda: "/dev/hidraw9",
            open_fn=lambda _p: (_ for _ in ()).throw(PermissionError(13, "Permission denied")),
            timeout_s=0.1,
        )


def test_encoder_pins_match_tk75_hardware_polarity() -> None:
    pins_h = (ROOT / "src/bsp/tk75_pins.h").read_text(encoding="utf-8")
    assert "#define TK75_ENC_A_PIN              GPIO_PINS_11" in pins_h
    assert "#define TK75_ENC_B_PIN              GPIO_PINS_10" in pins_h
    assert "#define TK75_EEPROM_BASE_ADDR       0x0803C000UL" in pins_h
    assert "#define TK75_EEPROM_SLOT_BYTES      4096UL" in pins_h


def test_via_backup_and_restore_roundtrip(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> None:
    class FakeVia:
        def __init__(self) -> None:
            self.layout_options = 0x1234
            self.keymap = bytearray(i & 0xFF for i in range(tk75_dfu.VIA_KEYMAP_BYTES))
            self.macros = bytearray((i * 3) & 0xFF for i in range(tk75_dfu.VIA_MACRO_BYTES))
            self.encoders: dict[tuple[int, int], int] = {
                (layer, cw): (0x00E0 + layer * 2 + cw)
                for layer in range(tk75_dfu.VIA_LAYERS)
                for cw in (0, 1)
            }
            self.custom: dict[tuple[int, int], list[int]] = {
                (ch, val_id): ([10 + val_id] if blen == 1 else [100, 200])
                for ch, val_id, blen, _name in tk75_dfu.VIA_CUSTOM_FIELDS
            }
            self.per_key: dict[tuple[int, int], tuple[int, int]] = {(1, 2): (15, 0x83)}
            self.saved = False
            self.closed = False

        def exchange(self, packet32: bytes, timeout_s: float = 0.5) -> bytes:
            rep = bytearray(packet32)
            cmd = packet32[0]
            if cmd == tk75_dfu.VIA_ID_GET_KEYBOARD_VALUE and packet32[1] == tk75_dfu.VIA_KBD_LAYOUT_OPTIONS:
                rep[2:6] = self.layout_options.to_bytes(4, "big")
            elif cmd == tk75_dfu.VIA_ID_SET_KEYBOARD_VALUE and packet32[1] == tk75_dfu.VIA_KBD_LAYOUT_OPTIONS:
                self.layout_options = int.from_bytes(packet32[2:6], "big")
            elif cmd == tk75_dfu.VIA_ID_KEYMAP_GET_BUF:
                off = (packet32[1] << 8) | packet32[2]
                sz = packet32[3]
                rep[4 : 4 + sz] = self.keymap[off : off + sz]
            elif cmd == tk75_dfu.VIA_ID_KEYMAP_SET_BUF:
                off = (packet32[1] << 8) | packet32[2]
                sz = packet32[3]
                self.keymap[off : off + sz] = packet32[4 : 4 + sz]
            elif cmd == tk75_dfu.VIA_ID_MACRO_GET_SIZE:
                sz = len(self.macros)
                rep[1] = (sz >> 8) & 0xFF
                rep[2] = sz & 0xFF
            elif cmd == tk75_dfu.VIA_ID_MACRO_GET_BUF:
                off = (packet32[1] << 8) | packet32[2]
                sz = packet32[3]
                rep[4 : 4 + sz] = self.macros[off : off + sz]
            elif cmd == tk75_dfu.VIA_ID_MACRO_SET_BUF:
                off = (packet32[1] << 8) | packet32[2]
                sz = packet32[3]
                self.macros[off : off + sz] = packet32[4 : 4 + sz]
            elif cmd == tk75_dfu.VIA_ID_GET_ENCODER:
                val = self.encoders[(packet32[1], packet32[3])]
                rep[4] = (val >> 8) & 0xFF
                rep[5] = val & 0xFF
            elif cmd == tk75_dfu.VIA_ID_SET_ENCODER:
                self.encoders[(packet32[1], packet32[3])] = (packet32[4] << 8) | packet32[5]
            elif cmd == tk75_dfu.VIA_ID_CUSTOM_GET_VALUE:
                ch, val_id = packet32[1], packet32[2]
                if (ch, val_id) == (8, 4):
                    act_s, rt_b = self.per_key.get((packet32[3], packet32[4]), (0, 0))
                    rep[5] = act_s
                    rep[6] = rt_b
                else:
                    vals = self.custom[(ch, val_id)]
                    rep[3 : 3 + len(vals)] = bytes(vals)
            elif cmd == tk75_dfu.VIA_ID_CUSTOM_SET_VALUE:
                ch, val_id = packet32[1], packet32[2]
                if (ch, val_id) == (8, 4):
                    r, c, act_s, rt_b = packet32[3], packet32[4], packet32[5], packet32[6]
                    if act_s == 0 and rt_b == 0:
                        self.per_key.pop((r, c), None)
                    else:
                        self.per_key[(r, c)] = (act_s, rt_b)
                else:
                    old = self.custom[(ch, val_id)]
                    self.custom[(ch, val_id)] = list(packet32[3 : 3 + len(old)])
            elif cmd == tk75_dfu.VIA_ID_CUSTOM_SAVE:
                self.saved = True
            return bytes(rep)

        def write_only(self, packet32: bytes) -> None:
            pass

        def close(self) -> None:
            self.closed = True

    src_via = FakeVia()
    dst_via = FakeVia()
    dst_via.layout_options = 0
    dst_via.keymap = bytearray(tk75_dfu.VIA_KEYMAP_BYTES)
    dst_via.macros = bytearray(tk75_dfu.VIA_MACRO_BYTES)
    dst_via.encoders = {(l, cw): 0 for l in range(tk75_dfu.VIA_LAYERS) for cw in (0, 1)}
    dst_via.custom = {k: [0] * len(v) for k, v in src_via.custom.items()}
    dst_via.per_key = {(0, 0): (9, 1)}

    backup_file = tmp_path / "cfg.json"
    monkeypatch.setattr(tk75_dfu.tk75_hid, "open_via_transport", lambda *_a, **_kw: src_via)
    assert tk75_dfu.main(["--backup-config", str(backup_file)]) == 0
    assert backup_file.is_file()

    monkeypatch.setattr(tk75_dfu.tk75_hid, "open_via_transport", lambda *_a, **_kw: dst_via)
    assert tk75_dfu.main(["--restore-config", str(backup_file)]) == 0
    assert dst_via.saved is True
    assert dst_via.layout_options == src_via.layout_options
    assert dst_via.keymap == src_via.keymap
    assert dst_via.macros == src_via.macros
    assert dst_via.encoders == src_via.encoders
    assert dst_via.custom == src_via.custom
    assert dst_via.per_key == src_via.per_key

    # Legacy 384 B macro backup upgrades cleanly onto 2048 B firmware (zero-padded)
    legacy_via = FakeVia()
    legacy_via.macros = bytearray((i * 5) & 0xFF for i in range(384))
    legacy_file = tmp_path / "legacy_384.json"
    monkeypatch.setattr(tk75_dfu.tk75_hid, "open_via_transport", lambda *_a, **_kw: legacy_via)
    assert tk75_dfu.main(["--backup-config", str(legacy_file)]) == 0
    dst_via.macros = bytearray(tk75_dfu.VIA_MACRO_BYTES)
    monkeypatch.setattr(tk75_dfu.tk75_hid, "open_via_transport", lambda *_a, **_kw: dst_via)
    assert tk75_dfu.main(["--restore-config", str(legacy_file)]) == 0
    assert dst_via.macros[:384] == legacy_via.macros
    assert dst_via.macros[384:] == bytes(tk75_dfu.VIA_MACRO_BYTES - 384)
