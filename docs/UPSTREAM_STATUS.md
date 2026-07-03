# Upstream Fan Control Status

## Timeline

| Date | Event |
|------|-------|
| June 2026 | Intel posts patch series **168027** — full xe hwmon fan control |
| June 19, 2026 | This project: PCODE probing on stock 7.0.12 — writes accepted, no effect (firmware path incomplete) |
| July 3, 2026 | **Working fan control** via custom `linux-cachyos-xefan` kernel + series 168027 patch |

## Intel Patch Series 168027

**Author:** Karthik Poosa  
**List:** intel-xe@lists.freedesktop.org  
**Archive:** [`reference/series-168027-fan-control.mbox`](../reference/series-168027-fan-control.mbox)

### What the Series Adds

- Writable `pwm1`, `pwm1_enable`, `fan1_max`
- 10-point fan curve (`pwm1_auto_point[1-10]_temp/_pwm`)
- PCODE `FAN_SPEED_CONTROL` integration via proper driver path (not raw userspace mailbox)
- Stock fan table readback and user table overlay
- Three `pwm1_enable` modes: full speed (0), manual user table (1), auto stock table (2)

### Merge Status (July 2026)

**Not merged** into Linux mainline or CachyOS stock kernel. Requires custom kernel build until distro picks it up.

### CachyOS Adaptation

Raw Intel patches need fuzz on CachyOS 7.1.2 due to PL2 power field additions in `xe_hwmon.c`. The adapted patch in [`src/patch/xe-fan-control-168027-cachyos-7.1.2.patch`](../src/patch/xe-fan-control-168027-cachyos-7.1.2.patch) applies with `patch -p1` without fuzz.

## Why June PCODE Probing Failed But Kernel Patch Works

June 2026 testing showed:

- `FAN_SPEED_CONTROL` subcmd `0x1` accepted duty writes from userspace
- Fan RPM did not change — no driver integration path
- `V1_FAN_PROVISIONED: NO` on 7.0.12

Series 168027 implements the **full driver stack**: stock table readback, user table activation, PCODE writes through `xe_pcode_write()` with proper locking, and hwmon sysfs. The firmware backend was always capable; the missing piece was upstream driver code, not just a subcommand ID.

## What to Watch

- [compute-runtime #885](https://github.com/intel/compute-runtime/issues/885) — user demand tracker
- [drm/xe GitLab](https://gitlab.freedesktop.org/drm/xe/kernel) — merge requests for fan control
- [intel-xe mailing list](https://lists.freedesktop.org/archives/intel-xe/) — series 168027 follow-ups
- CachyOS `linux-cachyos` updates — when `pwm1` appears on stock kernel, custom build can be removed

## Reporting Success

If fan control works on your B580 with the patched kernel, comment on [compute-runtime #885](https://github.com/intel/compute-runtime/issues/885) with:

- Board model (e.g. ASRock Challenger)
- Kernel version (`uname -r`)
- Confirmation that `pwm1` / CoolerControl works
- Link to this repo

This helps Intel prioritize upstream merge.
