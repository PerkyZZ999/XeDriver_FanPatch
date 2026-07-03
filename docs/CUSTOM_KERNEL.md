# Custom Kernel: `linux-cachyos-xefan`

This guide documents how to build and install a **side-by-side** CachyOS kernel with Intel patch series **168027** (xe fan control), verified working on an **ASRock Challenger Arc B580** with kernel `7.1.2-1-cachyos-xefan`.

Stock `linux-cachyos` remains installed for rollback.

## What You Get

On the patched kernel, `/sys/class/hwmon/hwmonX/` (where `name` is `xe`) exposes:

| Attribute | Access | Description |
|-----------|--------|-------------|
| `fan1_input` | read | Fan RPM |
| `fan1_max` | read | Max RPM |
| `pwm1` | read/write | Duty 0–255 (manual mode) |
| `pwm1_enable` | read/write | `0` = full speed, `1` = manual, `2` = auto (stock firmware table) |
| `pwm1_auto_point[1-10]_temp` | read/write | Fan curve temperature points (millidegrees C) |
| `pwm1_auto_point[1-10]_pwm` | read/write | Fan curve PWM points (0–255) |
| `temp2_input` | read | Package (`pkg`) GPU temperature |

Stock kernels only expose read-only `fan1_input`.

## Prerequisites

- CachyOS (or Arch-based) with `linux-cachyos` headers matching target version
- Intel Arc B580 (`8086:e20b`) on `xe` driver
- Limine bootloader (CachyOS default) — mkinitcpio hook adds boot entries automatically
- Build deps: `base-devel`, `git`, `pahole`, `bc`, `bindgen`, `python`, `scdoc`
- ~30–60 min compile time (`-j16`), ~15–25 GB disk for build tree

## Patch Source

| File | Description |
|------|-------------|
| [`reference/series-168027-fan-control.mbox`](../reference/series-168027-fan-control.mbox) | Original Intel mailing-list series (June 2026, Karthik Poosa) |
| [`src/patch/xe-fan-control-168027-cachyos-7.1.2.patch`](../src/patch/xe-fan-control-168027-cachyos-7.1.2.patch) | **Adapted combined patch** for CachyOS 7.1.2 (applies cleanly, no fuzz) |

Series 168027 is **not merged** into mainline as of July 2026. The adapted patch reconciles CachyOS-specific `xe_hwmon.c` differences (PL2 power fields).

## Build Overview

```text
1. Clone CachyOS linux PKGBUILD at matching 7.1.2 commit
2. Copy to linux-cachyos-xefan/
3. Set _pkgsuffix=cachyos-xefan, add patch to source/b2sums
4. makepkg -j16
5. pacman -U the .pkg.tar.zst
6. Reboot → select xefan Limine entry
```

### PKGBUILD Key Changes

```bash
_pkgsuffix=cachyos-xefan    # → pkgbase=linux-cachyos-xefan
pkgrel=1
# Add patch to source=() and b2sums
# conflicts=()              — does NOT replace stock linux-cachyos
# Skip -headers subpackage  — optional, faster build
```

CachyOS PKGBUILD also supports `_localmodcfg:=yes` to compile only modules for your hardware — significantly faster rebuilds.

### Build Command

```bash
export PATH="$HOME/.local/bin:$PATH"   # if bc/bindgen installed locally
cd linux-cachyos-xefan
MAKEFLAGS=-j16 makepkg --noprogressbar --skippgpcheck
```

### Optional: tmpfs Build (faster I/O)

If `/tmp` has enough space (expand tmpfs if needed):

```bash
sudo mount -o remount,size=32G /tmp
cp -a linux-cachyos-xefan /tmp/
cd /tmp/linux-cachyos-xefan && MAKEFLAGS=-j16 makepkg ...
```

## Install

```bash
sudo pacman -U linux-cachyos-xefan-7.1.2-1-x86_64.pkg.tar.zst
```

Limine should show a new entry alongside stock `7.1.2-3-cachyos`. **Keep stock as default** until you verify fan control.

## Verification

```bash
uname -r
# → 7.1.2-1-cachyos-xefan

# Find xe hwmon node
grep -l xe /sys/class/hwmon/hwmon*/name
# e.g. /sys/class/hwmon/hwmon2/name

H=/sys/class/hwmon/hwmon2   # adjust if needed
ls $H | grep -E 'fan|pwm'

cat $H/fan1_input $H/pwm1_enable $H/pwm1
```

### Safe Manual PWM Test

```bash
H=/sys/class/hwmon/hwmon2
echo "Before: $(cat $H/fan1_input) RPM"

sudo bash -c 'echo 1 > /sys/class/hwmon/hwmon2/pwm1_enable'
sudo bash -c 'echo 120 > /sys/class/hwmon/hwmon2/pwm1'
sleep 5
echo "After: $(cat $H/fan1_input) RPM"

# Restore stock auto curve
sudo bash -c 'echo 2 > /sys/class/hwmon/hwmon2/pwm1_enable'
```

### Stock Auto Curve (B580, verified)

Read from sysfs on first boot (millidegrees C → pwm):

| Point | Temp | PWM |
|-------|------|-----|
| 1 | 10°C | 26 |
| 2 | 50°C | 26 |
| 3 | 55°C | 51 |
| 4 | 70°C | 77 |
| 5 | 75°C | 102 |
| 6 | 79°C | 128 |
| 7 | 85°C | 153 |
| 8 | 90°C | 204 |
| 9 | 95°C | 230 |
| 10 | 100°C | 255 |

Driver initializes in **auto mode (`pwm1_enable=2`)** on boot.

## Rollback

| Method | Action |
|--------|--------|
| **Boot stock kernel** | Select `7.1.2-3-cachyos` in Limine |
| **Remove package** | `sudo pacman -R linux-cachyos-xefan` |

No changes to stock `linux-cachyos`. Safe to coexist indefinitely.

## Fail-Safe Behavior

See [`COOLERCONTROL.md`](COOLERCONTROL.md) for full details. Summary:

- `pwm1_enable=0` means **full speed**, not off
- Driver enforces hardware `min_pwm` from GPU firmware (~10% at idle)
- On boot: auto stock table (mode 2)
- If CoolerControl crashes: fan holds **last set speed**, does not drop to 0%
- Set fan to **Unmanaged** in CoolerControl → returns to driver auto (`pwm1_enable=2`)

Optional systemd failsafe on daemon stop:

```ini
# /etc/systemd/system/coolercontrold.service.d/xe-fan-failsafe.conf
[Service]
ExecStopPost=/bin/sh -c 'for h in /sys/class/hwmon/hwmon*/name; do [ "$(cat $h)" = xe ] && echo 2 > "${h%/name}/pwm1_enable"; done'
```

## When Upstream Merges Series 168027

Once Intel's patch lands in `linux-cachyos` mainline:

1. Remove `linux-cachyos-xefan`
2. Boot stock kernel
3. Fan control should work without a custom build

Watch: [intel-xe mailing list](https://lists.freedesktop.org/archives/intel-xe/), [drm/xe GitLab](https://gitlab.freedesktop.org/drm/xe/kernel).

## Verified Hardware

| Component | Value |
|-----------|-------|
| GPU | ASRock Challenger Arc B580 (`8086:e20b`) |
| Kernel built | `7.1.2-1-cachyos-xefan` (from CachyOS 7.1.2-3 base) |
| Bootloader | Limine 12.3.3 |
| Fan control UI | CoolerControl 4.3.1 |
| Date verified | July 3, 2026 |
