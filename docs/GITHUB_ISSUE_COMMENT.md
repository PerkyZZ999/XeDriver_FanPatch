# GitHub Issue Comment Template

Use this when posting to [intel/compute-runtime #885](https://github.com/intel/compute-runtime/issues/885).

---

## Short version

> Fan control is working on my Arc B580 (ASRock Challenger) on Linux using Intel's patch series 168027 applied to a custom CachyOS kernel (`7.1.2-1-cachyos-xefan`). CoolerControl can manage the fan via xe hwmon `pwm1` — confirmed manual 50%/100% and graph profiles.
>
> Details, adapted patch, and build guide: https://github.com/PerkyZZ999/XeDriver_FanPatch
>
> Hoping this helps prioritize upstream merge of series 168027 into mainline / distro kernels.

---

## Long version (optional technical addendum)

> ### Hardware
> - GPU: Intel Arc B580, ASRock Challenger (`8086:e20b`)
> - OS: CachyOS, Limine bootloader
>
> ### What works
> After applying Intel patch series **168027** (Karthik Poosa, June 2026) to `linux-cachyos` 7.1.2 as a side-by-side `linux-cachyos-xefan` package:
>
> - `/sys/class/hwmon/.../pwm1`, `pwm1_enable`, `fan1_max`
> - 10-point fan curve (`pwm1_auto_point[1-10]_temp/_pwm`)
> - CoolerControl 4.3.1: fan shows as manageable (not Read-Only)
> - Manual PWM and Fixed % profiles verified — fan RPM tracks setpoint
>
> ### Context from earlier investigation
> On stock kernel 7.0.12, PCODE `FAN_SPEED_CONTROL` subcmd `0x1` accepted duty writes but had no effect on RPM. The missing piece was upstream driver integration (series 168027), not raw mailbox access.
>
> ### Repo
> https://github.com/PerkyZZ999/XeDriver_FanPatch
> - Adapted patch for CachyOS 7.1.2
> - Build/install/rollback guide
> - CoolerControl setup and fail-safe notes
>
> Would appreciate this landing in mainline so stock distro kernels get fan control without a custom build.

---

## Also consider

- Cross-post summary to [drm/xe GitLab issues](https://gitlab.freedesktop.org/drm/xe/kernel/-/issues) — kernel-side merge tracking
- Link the original mbox: `reference/series-168027-fan-control.mbox` in repo
