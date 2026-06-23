# Firmware Blob Analysis: fan_control_8086_e20b_8086_1100.bin

## Overview

The Intel Arc B580's fan controller firmware is loaded via the MEI Late Binding mechanism during the xe driver's probe sequence. The firmware blob contains the fan curve that the GPU's PUnit/PCODE firmware uses to control fan speed automatically.

- **File**: `fan_control_8086_e20b_8086_1100.bin`
- **Compressed**: `.zst` (4064 bytes uncompressed)
- **Location on system**: `/lib/firmware/xe/fan_control_8086_e20b_8086_1100.bin.zst`
- **Loaded by**: `mei_lb` driver → `xe_late_bind_fw` component
- **Authentication**: Signed/verified by Management Engine (ME)

## Container Format: $FPT (Flash Partition Tool)

The blob uses Intel's `$FPT` container format:

```
$FPT Header (0x00-0x1F):
  Magic:    $FPT
  Version:  3
  Flags:    0x00201021
  Marker:   0x6d674940 ("mgI@")
```

### Partition Entries

| Name | Offset | Size | Description |
|---|---|---|---|
| LTEP | 0x0A00 | 0x120 | Late-binding Temperature/PWM Entry Points (**fan curve data**) |
| CDMD | 0x0B20 | 0x200 | Container Data/Manifest Descriptor (metadata strings) |
| LTES | 0x04C0 | 0x660 | Late-binding Table Entry Set ($CPD sub-container wrapper) |

### Sub-Container Magics

| Offset | Magic | Name |
|---|---|---|
| 0x0000 | $FPT | Flash Partition Tool (root manifest) |
| 0x04C0 | $CPD | Content Partition Data (sub-container) |
| 0x0538 | $MN2 | Manifest v2 (signed manifest header) |

### Metadata Strings

| String | Location | Purpose |
|---|---|---|
| "Intel(R) Battlemage (BMG) Graphics - Late Binding - Fan Configuration" | CDMD | Device description |
| "RootContainer/PunitConfigLateBind" | CDMD | PUnit configuration partition |
| "RootContainer/CDMD" | CDMD | Manifest descriptor partition |
| "RootContainer/LateBindingMetadata" | CDMD | Metadata partition |
| "FIT AS A FIDDLE" | CDMD | Authentication/fitness string |
| "NumberOfImageSubdivisions" | CDMD | Image structure descriptor |
| "ImageSubdivisionsSizes4064i" | CDMD | Total size specification |
| "ImageSubdivisionsSizesUnit" | CDMD | Size unit descriptor |

## Fan Curve Lookup Tables (DISCOVERED)

The firmware analyzer discovered **two fan curve lookup tables** in the LTEP partition:

### Fan Curve Table 1 (Offset 0xA94, 11 entries)

Format: `(duty_value, temperature_celsius)` as u16 pairs

| Temperature (°C) | Duty Value | Estimated PWM % |
|---|---|---|
| 0 | 0 | 0% |
| 30 | 0 | 0% |
| 45 | 0 | 0% |
| 46 | 650 | ~25% |
| 50 | 900 | ~35% |
| 60 | 1300 | ~51% |
| 68 | 1400 | ~55% |
| 72 | 1600 | ~63% |
| 75 | 1800 | ~70% |
| 80 | 2160 | ~85% |
| 85 | 2160 | ~85% (capped) |

**Interpretation**: Fan stays off below 46°C, then ramps up progressively. Maximum duty (2160) is reached at 80°C and capped at 85°C.

### Fan Curve Table 2 (Offset 0xAC0, 10 entries)

| Temperature (°C) | Duty Value | Estimated PWM % |
|---|---|---|
| 0 | 0 | 0% |
| 10 | 638 | ~25% |
| 20 | 1059 | ~42% |
| 30 | 1433 | ~56% |
| 40 | 1776 | ~69% |
| 50 | 2085 | ~81% |
| 60 | 2373 | ~93% |
| 70 | 2629 | ~100%+ |
| 80 | 2869 | ~100%+ |
| 100 | 3394 | ~100%+ |

**Interpretation**: This is a more aggressive curve that starts ramping at 10°C and reaches near-maximum by 60°C. This may be an alternate mode (e.g., performance/overclock mode, or a secondary fan curve).

### Duty Value Scaling

The duty values are NOT standard 0-255 PWM. The maximum observed value is 3394, which is close to 4095 (12-bit max). This suggests the duty values may be in a **12-bit scale (0-4095)** or a custom firmware scale.

If the scale is 0-4095:
- Table 1 max: 2160/4095 ≈ 52.7% (seems low for "max" fan speed)
- Table 2 max: 3394/4095 ≈ 82.9% (more reasonable)

Alternatively, the scale might be 0-2550 (10x PWM):
- Table 1 max: 2160/2550 ≈ 84.7% (reasonable)
- Table 2 max: 3394/2550 ≈ 133% (exceeds 100%, unlikely)

Or a custom firmware internal scale where the PUnit interprets these values according to its own calibration.

## Authentication and Modification

The firmware blob contains a `$MN2` (Manifest v2) signed header at offset 0x538. This is Intel's signature manifest that the Management Engine (ME) verifies before allowing the firmware to be loaded by the PUnit.

**Modifying the fan curve in the firmware blob is NOT feasible** because:
1. The `$MN2` manifest contains a cryptographic signature
2. The ME rejects unsigned or improperly signed firmware
3. Intel's signing keys are not publicly available
4. Defeating the authentication would require breaking Intel's security chain

## Implications for the Project

The fan curve data discovery has several implications:

1. **The fan curve is firmware-defined**: The PUnit follows the curve in the LTEP partition. There is no register-based "set fan to X%" mechanism visible in the firmware.

2. **PCODE FAN_SPEED_CONTROL write subcommands (0x7D) may still exist**: Even though the firmware defines the default curve, PCODE may support runtime override commands that bypass or supplement the firmware curve. These are the undocumented subcommands (0x0-0x3, 0x5-0xF) that our probe tools target.

3. **The firmware curve is conservative**: The default curve keeps the fan off below 46°C and only reaches ~85% at 80°C. A custom curve could be more aggressive for better cooling, or quieter for less noise.

4. **Physical fan redirect remains the most reliable fallback**: Since firmware modification is blocked by authentication, redirecting the fan to a motherboard PWM header gives full user-space control over the fan curve.

## Tool

Run the firmware analyzer:
```bash
python3 src/firmware/fw_analyzer.py src/firmware/fan_control_8086_e20b_8086_1100.bin
```
