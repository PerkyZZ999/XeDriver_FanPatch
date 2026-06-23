# Testing Results & Findings

## Test Date: June 19, 2026

## System Under Test
- **GPU**: Intel Arc B580 (Battlemage G21), PCI 0000:09:00.0 (8086:e20b)
- **Kernel**: 7.0.12-1-cachyos (CachyOS, clang 22.1.6)
- **Driver**: xe v1.1.0, mei_lb bound
- **Firmware**: fan_control_8086_e20b_8086_1100.bin present in /lib/firmware/xe/

## Test Results Summary

### Step 1: Userspace PCODE Probe (SUCCESS)
- BAR0 mapped successfully at 16MB
- PCODE mailbox responsive, no errors
- Fan count: 1
- Fan RPM: ~1000 RPM at 34°C idle
- Thermal limits: shutdown=125°C, critical=100°C

### Step 2: Kernel Module (SUCCESS)
- Module loaded/unloaded cleanly
- Sysfs attributes readable
- Fan RPM and temperature readings correct
- No kernel panics or driver conflicts

### Step 3: Daemon Dry Run (SUCCESS)
- Auto-detected userspace control method
- Temperature reading from hwmon2 working
- Fan curve interpolation correct

### Step 4: PCODE Mailbox Write Probing (SUCCESS — but no fan control achieved)

## FAN_SPEED_CONTROL (0x7D) Subcommand Map (Confirmed)

| Subcmd | Read | Write | Accepted Values | Fan Changed? | Purpose |
|---|---|---|---|---|---|
| 0x0 | OK (returns 0) | Rejected (Illegal Command) | N/A | No | Status: override active (0=none) |
| 0x1 | Fails (WO) | OK | 0x01-0xFF only (0 and >0xFF rejected) | **No** | Write-only config (no effect without firmware) |
| 0x2 | Rejected | N/A | N/A | No | Does not exist |
| 0x3 | OK (returns 50) | Accepted but no effect | Any | No | Read-only temperature threshold |
| 0x5 | OK (returns 10) | Accepted but no effect | Any | No | Read-only hysteresis |
| 0x6 | Fails (WO) | ALL rejected (Illegal Data) | Unknown format | No | Write-only, requires specific data format |
| 0x7 | OK (returns 0x0a0a) | Accepted, computes response | Packed pair | No | Fan curve calculation (no effect) |
| 0x8 | OK (returns 0) | 0/1 accepted, 2 rejected | 0 or 1 | No | Mode toggle (not persistent) |
| 0x9 | OK (returns 1) | 1 accepted, 0 rejected | 1 only | No | Read-only flag (auto mode) |
| 0xa-0xf | Rejected | N/A | N/A | No | Do not exist |

## Critical Finding: Firmware Not Provisioned

```
V1_FAN_SUPPORTED:      YES
V1_FAN_PROVISIONED:    NO
Fan FW Version:        0.0.0.0
VR FW Version:         0.0.0.0
```

The fan control firmware blob (`fan_control_8086_e20b_8086_1100.bin`) exists on disk and the `mei_lb` driver binds to the xe device, but the firmware is **never actually loaded into the PUnit**. The xe driver's late binding code (`xe_late_bind_fw.c`) runs but provisioning fails silently.

### Why Software Fan Control Doesn't Work

Without the firmware provisioned:
1. PCODE accepts FAN_SPEED_CONTROL commands but **has no firmware backend to execute them**
2. Subcmd 0x1 accepts duty writes (0x01-0xFF) but does nothing — no consumer
3. Subcmd 0x8 mode toggle doesn't persist — no firmware state machine
4. The fan runs on a **hardware default curve** in the PUnit ROM

### Root Cause (Likely)

The CachyOS kernel (7.0.12) is based on a kernel version where the late binding firmware loading infrastructure exists but may not be fully functional for Battlemage. The `xe_late_bind_fw.c` code is present in the module, but the firmware provisioning path may require a newer kernel version or additional patches not yet in CachyOS.

## Conclusion

**Software fan control via PCODE mailbox is not achievable on this system** until the late binding firmware is properly provisioned. This requires an upstream kernel update that fully implements the firmware loading path for Battlemage.

### What to Watch For
- CachyOS kernel updates (check `lb_fan_control_version` after updates — when it shows a non-zero version, the firmware is loaded)
- Upstream xe driver commits to `xe_late_bind_fw.c`
- Intel-xe mailing list for late binding provisioning fixes
- When V1_FAN_PROVISIONED becomes YES, re-run the probe tools in this project

### When Firmware IS Provisioned (Future)
Once `lb_fan_control_version` shows a real version:
1. Re-run `sudo ./src/userspace/xe_pcode_probe --all`
2. Re-test subcmd 0x1 writes — they may now actually control the fan
3. Load the kernel module and test `pwm1` sysfs writes
4. If subcmd 0x1 controls the fan, configure the daemon for automatic control

### Immediate Options
- **Wait for upstream** (recommended — safest, just needs a kernel update)
- **Physical fan redirect** (documented in `src/fallback/HARDWARE_GUIDE.md` — works today but requires hardware modification)

## Safety Note
The testing was safe — no permanent damage occurred. All PCODE state is volatile and resets on reboot. The one display crash was caused by a PCI driver remove/rescan (avoid this on primary display GPUs).
