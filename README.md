# Intel Arc B580 Linux Fan Control — XeDriver Fan Patch

Research project investigating **fan control for Intel Arc B580 (Battlemage)** GPUs on Linux.

The `xe` kernel driver exposes read-only fan RPM (`fan1_input`) but no writable PWM control. This repository documents the fan control stack, probes the PCODE mailbox, analyzes the late-binding firmware blob, and provides tools for when Intel finishes upstream support.

**Related upstream issue:** [intel/compute-runtime #885 — No Fan control in Linux for Battlemage GPUs](https://github.com/intel/compute-runtime/issues/885)

## Current Status (June 2026)

| Capability | Status |
|------------|--------|
| Read fan RPM / GPU temperature | Works via `xe` hwmon |
| Write fan speed (software) | **Not working** — firmware not provisioned |
| `lb_fan_control_version` sysfs | Not exposed on tested kernel |
| PCODE `FAN_SPEED_CONTROL` writes | Accepted but **no effect** on fan RPM |
| Physical motherboard PWM redirect | Documented fallback |

See [`docs/TESTING_RESULTS.md`](docs/TESTING_RESULTS.md) for full probe results on kernel 7.0.12.

## What This Project Contains

1. **Userspace PCODE probe** (`src/userspace/`) — probes `FAN_SPEED_CONTROL` (0x7D) subcommands via PCI BAR0
2. **Out-of-tree kernel module** (`src/kernel/`) — sysfs interface at `/sys/kernel/xe_fan/`
3. **Reference patch** (`src/patch/`) — example `xe_hwmon.c` PWM write support for upstream
4. **Fan control daemon** (`src/daemon/`) — temperature-curve daemon (ready when control works)
5. **Physical fallback** (`src/fallback/`) — motherboard PWM header redirect guide
6. **Firmware analyzer** (`src/firmware/`) — parses `fan_control_*.bin` late-binding blob
7. **Update monitor** (`src/monitor/`) — watches for kernel/firmware fixes

## Quick Start

### Prerequisites

- Intel Arc B580 (PCI `8086:e20b`) with `xe` driver loaded
- Root access (PCI resource mmap)
- `gcc`, `make`, kernel headers (`linux-headers`)
- Optional: `clang` + `ld.lld` if your kernel was built with clang (CachyOS)

### 1. Build and run the PCODE probe (read-only, safe)

```bash
cd src/userspace
make
sudo ./xe_pcode_probe --all
```

Auto-detects the B580 on PCI. Override with `--pci 0000:BB:DD.F` or `XE_PCI_BDF=0000:09:00.0`.

### 2. Build and load the kernel module

```bash
cd src/kernel
make clean && make
sudo insmod xe_fan_probe.ko
cat /sys/kernel/xe_fan/probe_results
cat /sys/kernel/xe_fan/fan1_input
sudo rmmod xe_fan_probe
```

### 3. Analyze the firmware blob

```bash
python3 src/firmware/fw_analyzer.py src/firmware/fan_control_8086_e20b_8086_1100.bin
```

### 4. Monitor for upstream fixes

```bash
python3 src/monitor/xe_fan_watch.py --source all
```

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/TESTING_RESULTS.md`](docs/TESTING_RESULTS.md) | Probe results and root-cause analysis |
| [`docs/PCODE_PROTOCOL.md`](docs/PCODE_PROTOCOL.md) | PCODE mailbox protocol |
| [`docs/REGISTER_MAP.md`](docs/REGISTER_MAP.md) | MMIO register map |
| [`docs/FIRMWARE_ANALYSIS.md`](docs/FIRMWARE_ANALYSIS.md) | Fan curve tables in firmware blob |
| [`Plan.md`](Plan.md) | Detailed implementation plan |
| [`Overview.md`](Overview.md) | Original project specification |

## Safety

- Read-only probing is safe by default
- Fan duty writes require `--force` on the probe tool
- The kernel module loads/unloads without rebooting
- **Do not** remove/rescan the PCI device while it drives your display
- Keep SSH open when testing kernel modules

## License

- **Kernel module** (`src/kernel/`): [GPL-2.0](LICENSE)
- **All other components**: [MIT](LICENSE-MIT)

## Contributing

This is a research artifact shared to help Intel and the community. Bug reports, probe results from other B580 boards/kernels, and upstream integration are welcome via GitHub issues.

If you post about this project, please link [compute-runtime #885](https://github.com/intel/compute-runtime/issues/885) so Intel can track demand for Battlemage fan control on Linux.

## Research Sources

- [Phoronix: Intel Xe Linux 6.16 Fan Speeds](https://www.phoronix.com/news/Intel-Xe-Linux-6.16-Fan-Speeds)
- [Phoronix: Fan Control BMG Firmware](https://www.phoronix.com/news/Fan-Control-BMG-Firmware)
- [drm/xe GitLab](https://gitlab.freedesktop.org/drm/xe/kernel)
- [intel-xe mailing list](https://lists.freedesktop.org/archives/intel-xe/)
- [compute-runtime #885](https://github.com/intel/compute-runtime/issues/885)
