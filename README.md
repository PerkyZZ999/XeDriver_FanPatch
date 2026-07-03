# Intel Arc B580 Linux Fan Control — XeDriver Fan Patch

Research and working solution for **fan control on Intel Arc B580 (Battlemage)** GPUs on Linux.

The stock `xe` kernel driver exposes read-only fan RPM (`fan1_input`) but no writable PWM. Intel's patch series **168027** (June 2026) adds full hwmon fan control. This repository documents the investigation, provides probe tools, and includes an **adapted kernel patch** verified working on CachyOS with CoolerControl.

**Related upstream issue:** [intel/compute-runtime #885 — No Fan control in Linux for Battlemage GPUs](https://github.com/intel/compute-runtime/issues/885)

## Current Status (July 2026)

| Capability | Stock kernel | `linux-cachyos-xefan` |
|------------|--------------|------------------------|
| Read fan RPM / GPU temperature | Yes | Yes |
| Write fan speed (`pwm1`) | **No** | **Yes** |
| Fan curve (`pwm1_auto_point*`) | **No** | **Yes** |
| CoolerControl management | Read-Only | **Unmanaged / Manual / Graph** |
| PCODE userspace writes (no patch) | Accepted, no effect | N/A — use driver path |

**Verified working** July 3, 2026 — ASRock Challenger B580, kernel `7.1.2-1-cachyos-xefan`, CoolerControl 4.3.1.

### Quick Path to Working Fan Control

1. Build and install custom kernel → [`docs/CUSTOM_KERNEL.md`](docs/CUSTOM_KERNEL.md)
2. Reboot into `*-cachyos-xefan` Limine entry
3. Configure CoolerControl → [`docs/COOLERCONTROL.md`](docs/COOLERCONTROL.md)
4. Rollback anytime → boot stock kernel or `pacman -R linux-cachyos-xefan`

## What This Project Contains

1. **Adapted kernel patch** (`src/patch/`) — series 168027 for CachyOS 7.1.2, applies cleanly
2. **Userspace PCODE probe** (`src/userspace/`) — probes `FAN_SPEED_CONTROL` via PCI BAR0 (research)
3. **Out-of-tree kernel module** (`src/kernel/`) — sysfs at `/sys/kernel/xe_fan/` (research)
4. **Fan control daemon** (`src/daemon/`) — temperature-curve daemon (superseded by CoolerControl + patched kernel)
5. **Physical fallback** (`src/fallback/`) — motherboard PWM redirect (not needed if patch works)
6. **Firmware analyzer** (`src/firmware/`) — parses `fan_control_*.bin` late-binding blob
7. **Update monitor** (`src/monitor/`) — watches for kernel/firmware fixes
8. **Reference** (`reference/`) — upstream mbox series, xe source snapshots

## Quick Start (Research Tools)

### Prerequisites

- Intel Arc B580 (PCI `8086:e20b`) with `xe` driver loaded
- Root access for probes; patched kernel for actual fan control

### PCODE probe (read-only, stock kernel)

```bash
cd src/userspace
make
sudo ./xe_pcode_probe --all
```

### Fan control (patched kernel)

See [`docs/CUSTOM_KERNEL.md`](docs/CUSTOM_KERNEL.md) and [`docs/COOLERCONTROL.md`](docs/COOLERCONTROL.md).

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/CUSTOM_KERNEL.md`](docs/CUSTOM_KERNEL.md) | **Build, install, verify, rollback** — start here |
| [`docs/COOLERCONTROL.md`](docs/COOLERCONTROL.md) | CoolerControl profiles and fail-safe behavior |
| [`docs/UPSTREAM_STATUS.md`](docs/UPSTREAM_STATUS.md) | Intel series 168027 and merge timeline |
| [`docs/TESTING_RESULTS.md`](docs/TESTING_RESULTS.md) | June PCODE probes + July 2026 success |
| [`docs/PCODE_PROTOCOL.md`](docs/PCODE_PROTOCOL.md) | PCODE mailbox protocol |
| [`docs/REGISTER_MAP.md`](docs/REGISTER_MAP.md) | MMIO register map |
| [`docs/FIRMWARE_ANALYSIS.md`](docs/FIRMWARE_ANALYSIS.md) | Fan curve tables in firmware blob |
| [`docs/GITHUB_ISSUE_COMMENT.md`](docs/GITHUB_ISSUE_COMMENT.md) | Template for compute-runtime #885 |
| [`Plan.md`](Plan.md) | Detailed implementation plan |
| [`Overview.md`](Overview.md) | Original project specification |

## Safety

- Custom kernel installs **side-by-side** — stock `linux-cachyos` untouched
- Boot stock Limine entry for instant rollback
- Driver defaults to **auto stock fan table** on boot (`pwm1_enable=2`)
- Fan will not drop to 0% on CoolerControl crash — see [`docs/COOLERCONTROL.md`](docs/COOLERCONTROL.md)
- Read-only PCODE probing is safe; avoid PCI remove/rescan on display GPU

## License

- **Kernel patch / module** (`src/patch/`, `src/kernel/`): [GPL-2.0](LICENSE)
- **All other components**: [MIT](LICENSE-MIT)

## Contributing

Probe results from other B580 boards/kernels and upstream integration updates are welcome via GitHub issues.

If you post about this project, link [compute-runtime #885](https://github.com/intel/compute-runtime/issues/885).

## Research Sources

- Intel patch series 168027 — [`reference/series-168027-fan-control.mbox`](reference/series-168027-fan-control.mbox)
- [Phoronix: Intel Xe Linux 6.16 Fan Speeds](https://www.phoronix.com/news/Intel-Xe-Linux-6.16-Fan-Speeds)
- [drm/xe GitLab](https://gitlab.freedesktop.org/drm/xe/kernel)
- [intel-xe mailing list](https://lists.freedesktop.org/archives/intel-xe/)
- [compute-runtime #885](https://github.com/intel/compute-runtime/issues/885)
