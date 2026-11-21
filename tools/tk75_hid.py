#!/usr/bin/env python3
"""
Cross-platform HID transport helpers for GamaKay TK75 HE V2 tools.

Uses native Linux `/dev/hidraw*` (`sysfs` discovery + `HIDIOCSFEATURE`/`os.write`)
so tools work cleanly with standard udev hidraw permissions without needing root
access to `/dev/bus/usb/*`, while keeping `hidapi` fallback for Windows/macOS.
"""

from __future__ import annotations

import os
import pathlib
import select
import sys
from typing import Protocol

HIDIOCSFEATURE_65 = 0xC0414806
HIDIOCGFEATURE_65 = 0xC0414807

USAGE_PAGE_VENDOR_MIN = 0xFF00
USAGE_PAGE_BOOT = 0xFF01
USAGE_PAGE_VIA = 0xFF60
USAGE_PAGE_STOCK_VENDOR = 0xFFFF


class FeatureTransport(Protocol):
    def send_feature(self, payload64: bytes) -> None: ...
    def recv_feature(self) -> bytes: ...
    def close(self) -> None: ...


class ViaInterruptTransport(Protocol):
    def exchange(self, packet32: bytes, timeout_s: float = 0.5) -> bytes: ...
    def write_only(self, packet32: bytes) -> None: ...
    def close(self) -> None: ...


def parse_hid_usage_page(report_desc: bytes) -> int | None:
    """Extract the first Global Usage Page item from a HID report descriptor."""
    idx = 0
    end = len(report_desc)
    while idx < end:
        prefix = report_desc[idx]
        if prefix == 0xFE:  # Long HID item
            if idx + 1 >= end:
                return None
            idx += 3 + report_desc[idx + 1]
            continue

        size_code = prefix & 0x03
        size = 4 if size_code == 3 else size_code
        tag_type = prefix & 0xFC
        if idx + 1 + size > end:
            return None

        if tag_type == 0x04 and size > 0:
            return int.from_bytes(report_desc[idx + 1 : idx + 1 + size], "little")
        if tag_type == 0xA0:  # Stop at first Collection if no Usage Page preceded it
            return None
        idx += 1 + size
    return None


def parse_hid_id(uevent_text: str) -> tuple[int, int] | None:
    """Parse `(vid, pid)` from Linux `/sys/class/hidraw/hidraw*/device/uevent`."""
    for line in uevent_text.splitlines():
        if not line.startswith("HID_ID="):
            continue
        parts = line.split("=", 1)[1].split(":")
        if len(parts) != 3:
            continue
        try:
            return int(parts[1], 16), int(parts[2], 16)
        except ValueError:
            return None
    return None


def find_hidraw_path(
    vid: int,
    pid: int,
    usage_page: int | None = None,
    prefer_vendor: bool = True,
    sysfs_root: pathlib.Path = pathlib.Path("/sys/class/hidraw"),
) -> str | None:
    """Locate matching `/dev/hidraw*` node using sysfs `uevent` + `report_descriptor`."""
    if not sysfs_root.is_dir():
        return None

    fallback_any: str | None = None
    fallback_vendor: str | None = None
    for node in sorted(sysfs_root.glob("hidraw*")):
        uevent = node / "device" / "uevent"
        if not uevent.is_file():
            continue
        ids = parse_hid_id(uevent.read_text(encoding="utf-8", errors="replace"))
        if ids != (vid, pid):
            continue

        dev_path = f"/dev/{node.name}"
        desc_path = node / "device" / "report_descriptor"
        node_page = parse_hid_usage_page(desc_path.read_bytes()) if desc_path.is_file() else None
        if usage_page is not None:
            if node_page == usage_page:
                return dev_path
            continue

        if fallback_any is None:
            fallback_any = dev_path
        if node_page is not None and node_page >= USAGE_PAGE_VENDOR_MIN and fallback_vendor is None:
            fallback_vendor = dev_path

    if prefer_vendor and fallback_vendor is not None:
        return fallback_vendor
    return fallback_any


def find_hidapi_path(vid: int, pid: int, usage_page: int | None = None) -> bytes | None:
    """Locate HID device path via `hidapi` (Windows/macOS or non-sysfs fallback)."""
    import hid  # type: ignore

    for info in hid.enumerate(vid, pid):
        if usage_page is None or info.get("usage_page") == usage_page:
            return info["path"]
    return None


def find_device_path(vid: int, pid: int, usage_page: int | None = None) -> str | bytes | None:
    """Return `/dev/hidraw*` on Linux or `hidapi` byte path on other platforms."""
    if sys.platform.startswith("linux"):
        raw_path = find_hidraw_path(vid, pid, usage_page=usage_page)
        if raw_path is not None:
            return raw_path
    return find_hidapi_path(vid, pid, usage_page=usage_page)


class LinuxHidrawFeatureTransport:
    """Linux `/dev/hidraw*` ioctl Feature Report transport (`HIDIOCSFEATURE_65`/`HIDIOCGFEATURE_65`)."""

    def __init__(self, dev_path: str) -> None:
        import fcntl

        self._fcntl = fcntl
        self._fd = os.open(dev_path, os.O_RDWR)

    def send_feature(self, payload64: bytes) -> None:
        if len(payload64) != 64:
            raise ValueError(f"Expected 64-byte feature payload, got {len(payload64)}")
        self._fcntl.ioctl(self._fd, HIDIOCSFEATURE_65, b"\x00" + payload64)

    def recv_feature(self) -> bytes:
        buf = bytearray(65)
        buf[0] = 0x00
        self._fcntl.ioctl(self._fd, HIDIOCGFEATURE_65, buf)
        return bytes(buf[1:65])

    def close(self) -> None:
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1


class HidapiFeatureTransport:
    """Cross-platform `hidapi` 64-byte Feature Report transport."""

    def __init__(self, path: bytes) -> None:
        import hid  # type: ignore

        self._dev = hid.device()
        self._dev.open_path(path)

    def send_feature(self, payload64: bytes) -> None:
        if len(payload64) != 64:
            raise ValueError(f"Expected 64-byte feature payload, got {len(payload64)}")
        n = self._dev.send_feature_report(b"\x00" + payload64)
        if n < 0:
            raise OSError("HID send_feature_report failed")

    def recv_feature(self) -> bytes:
        raw = bytes(self._dev.get_feature_report(0x00, 65))
        return raw[1:65] if len(raw) == 65 else raw[:64]

    def close(self) -> None:
        self._dev.close()


class LinuxHidrawInterruptTransport:
    """Linux `/dev/hidraw*` interrupt OUT/IN transport for 32-byte VIA packets."""

    def __init__(self, dev_path: str) -> None:
        self._fd = os.open(dev_path, os.O_RDWR)

    def write_only(self, packet32: bytes) -> None:
        if len(packet32) != 32:
            raise ValueError(f"Expected 32-byte VIA packet, got {len(packet32)}")
        # Flush stale unread reports so next read matches our request.
        while select.select([self._fd], [], [], 0.0)[0]:
            os.read(self._fd, 64)
        os.write(self._fd, b"\x00" + packet32)

    def exchange(self, packet32: bytes, timeout_s: float = 0.5) -> bytes:
        self.write_only(packet32)
        ready, _, _ = select.select([self._fd], [], [], timeout_s)
        if not ready:
            raise TimeoutError("Timed out waiting for VIA HID response")
        raw = os.read(self._fd, 64)
        return raw[1:33] if len(raw) == 33 and raw[0] == 0 else raw[:32]

    def close(self) -> None:
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1


class HidapiInterruptTransport:
    """Cross-platform `hidapi` 32-byte VIA packet transport."""

    def __init__(self, path: bytes) -> None:
        import hid  # type: ignore

        self._dev = hid.device()
        self._dev.open_path(path)

    def write_only(self, packet32: bytes) -> None:
        if len(packet32) != 32:
            raise ValueError(f"Expected 32-byte VIA packet, got {len(packet32)}")
        self._dev.write(b"\x00" + packet32)

    def exchange(self, packet32: bytes, timeout_s: float = 0.5) -> bytes:
        self.write_only(packet32)
        raw = bytes(self._dev.read(32, timeout_ms=max(1, int(timeout_s * 1000))))
        if not raw:
            raise TimeoutError("Timed out waiting for VIA HID response")
        return raw[1:33] if len(raw) == 33 and raw[0] == 0 else raw[:32]

    def close(self) -> None:
        self._dev.close()


def open_feature_transport(path: str | bytes) -> FeatureTransport:
    """Open Feature Report transport for `/dev/hidraw*` path or `hidapi` byte path."""
    if isinstance(path, str):
        return LinuxHidrawFeatureTransport(path)
    return HidapiFeatureTransport(path)


def open_via_transport(vid: int, pid: int, usage_page: int = USAGE_PAGE_VIA) -> ViaInterruptTransport:
    """Find and open VIA 0xFF60 interface via hidraw (Linux) or hidapi."""
    path = find_device_path(vid, pid, usage_page=usage_page)
    if path is None:
        raise RuntimeError(f"No VIA device ({vid:04X}:{pid:04X}, page 0x{usage_page:04X}) connected.")
    if isinstance(path, str):
        return LinuxHidrawInterruptTransport(path)
    return HidapiInterruptTransport(path)
