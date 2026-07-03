# Testing Results & Findings

## July 3, 2026 — Fan Control Working (Custom Kernel)

### System Under Test
- **GPU**: Intel Arc B580, ASRock Challenger, PCI `0000:09:00.0` (`8086:e20b`)
- **Kernel**: `7.1.2-1-cachyos-xefan` (custom build from CachyOS 7.1.2-3 + series 168027 patch)
- **Stock kernel kept**: `7.1.2-3-cachyos` (rollback via Limine)
- **Bootloader**: Limine 12.3.3
- **Fan UI**: CoolerControl 4.3.1

### Result: SUCCESS

| Test | Result |
|------|--------|
| Boot xefan kernel | Pass |
| `pwm1`, `pwm1_enable`, `fan1_max` in xe hwmon | Present |
| `pwm1_auto_point[1-10]_*` curve attrs | Present |
| Manual PWM sysfs test | Fan RPM responds to `pwm1` writes |
| CoolerControl Fixed % (50%, 100%) | Fan speed matches setpoint |
| CoolerControl device status | **Unmanaged** (not Read-Only) |
| Stock auto curve on boot | `pwm1_enable=2`, ~10% idle floor |

### xe hwmon Path
`/sys/class/hwmon/hwmon2/` (name=`xe`)

### Stock Auto Curve (read from sysfs)

| Point | Temp | PWM (0–255) |
|-------|------|-------------|
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

### Solution Used

Intel patch series **168027** applied to CachyOS `linux-cachyos` PKGBUILD as side-by-side package `linux-cachyos-xefan`. See [`CUSTOM_KERNEL.md`](CUSTOM_KERNEL.md).

### Fail-Safe Notes

- Driver boots in auto mode (stock firmware table)
- `pwm1_enable=0` = full speed, not off
- CoolerControl crash → fan holds last PWM, not 0%
- See [`COOLERCONTROL.md`](COOLERCONTROL.md)

---

## June 19, 2026 — PCODE Probing (Stock Kernel)

### System Under Test
- **GPU**: Intel Arc B580 (Battlemage G21), PCI 0000:09:00.0 (8086:e20b)
- **Kernel**: 7.0.12-1-cachyos (CachyOS, clang 22.1.6)
- **Driver**: xe v1.1.0, mei_lb bound
- **Firmware**: fan_control_8086_e20b_8086_1100.bin present in /lib/firmware/xe/

### Test Results Summary

#### Step 1: Userspace PCODE Probe (SUCCESS)
- BAR0 mapped successfully at 16MB
- PCODE mailbox responsive, no errors
- Fan count: 1
- Fan RPM: ~1000 RPM at 34°C idle
- Thermal limits: shutdown=125°C, critical=100°C

#### Step 2: Kernel Module (SUCCESS)
- Module loaded/unloaded cleanly
- Sysfs attributes readable
- Fan RPM and temperature readings correct
- No kernel panics or driver conflicts

#### Step 3: Daemon Dry Run (SUCCESS)
- Auto-detected userspace control method
- Temperature reading from hwmon2 working
- Fan curve interpolation correct

#### Step 4: PCODE Mailbox Write Probing (SUCCESS — but no fan control achieved)

### FAN_SPEED_CONTROL (0x7D) Subcommand Map (Confirmed)

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

### June Finding: Userspace Writes Insufficient

```
V1_FAN_SUPPORTED:      YES
V1_FAN_PROVISIONED:    NO
Fan FW Version:        0.0.0.0
```

PCODE accepted `FAN_SPEED_CONTROL` writes but had **no driver integration path** on stock 7.0.12. The July 2026 kernel patch (series 168027) provides that path — fan control works without requiring userspace mailbox poking.

### Revised Conclusion

- **Stock kernel**: fan control not available via sysfs or userspace PCODE alone
- **Patched kernel (series 168027)**: full hwmon fan control working
- **Physical redirect**: no longer required if custom kernel is acceptable

### Safety Note
June testing was safe — no permanent damage. The one display crash was caused by PCI driver remove/rescan (avoid on primary display GPUs).
