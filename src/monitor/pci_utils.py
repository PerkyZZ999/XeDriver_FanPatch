"""Shared PCI / hwmon discovery helpers for Intel Arc B580 (8086:e20b)."""

from __future__ import annotations

from pathlib import Path

B580_VENDOR = "0x8086"
B580_DEVICE = "0xe20b"


def find_b580_pci_bdf() -> str | None:
    """Return PCI BDF (e.g. 0000:09:00.0) for the first Battlemage GPU."""
    pci_root = Path("/sys/bus/pci/devices")
    if not pci_root.is_dir():
        return None

    for dev in sorted(pci_root.iterdir()):
        device_path = dev / "device"
        vendor_path = dev / "vendor"
        if not device_path.is_file():
            continue
        try:
            device = device_path.read_text().strip().lower()
            vendor = vendor_path.read_text().strip().lower() if vendor_path.is_file() else ""
        except OSError:
            continue
        if device == B580_DEVICE and (not vendor or vendor == B580_VENDOR):
            return dev.name
    return None


def lb_fan_version_glob(pci_bdf: str | None = None) -> str:
    bdf = pci_bdf or find_b580_pci_bdf() or "*"
    return f"/sys/devices/pci*/{bdf}/lb_fan_control_version"


def find_xe_hwmon_dir() -> Path | None:
    """Return hwmon sysfs path for the xe GPU driver, if present."""
    hwmon_root = Path("/sys/class/hwmon")
    if not hwmon_root.is_dir():
        return None

    for entry in sorted(hwmon_root.glob("hwmon*")):
        name_path = entry / "name"
        if not name_path.is_file():
            continue
        try:
            name = name_path.read_text().strip().lower()
        except OSError:
            continue
        if name == "xe":
            return entry
    return None


def xe_hwmon_path(attr: str) -> str | None:
    hwmon = find_xe_hwmon_dir()
    if hwmon is None:
        return None
    path = hwmon / attr
    return str(path) if path.is_file() else str(path)
