# Intel Arc B580 Register Map

> **Note (July 2026):** Writable fan control is available through the patched kernel's hwmon interface (`pwm1`), not direct BAR0 userspace access. Register map below documents June 2026 research.

## PCI Configuration

| Field | Value |
|---|---|
| PCI Address | 0000:09:00.0 |
| Vendor ID | 8086 (Intel) |
| Device ID | e20b (Battlemage G21 / Arc B580) |
| Subsystem | ASRock 6021 |
| Driver | xe (v1.1.0) |
| Stepping | B0 |
| BAR0 | 0xfb000000 (16MB, non-prefetchable) |
| BAR2 | 0x7800000000 (16GB, prefetchable, VRAM) |
| BAR6 | 0xfc000000 (2MB, Expansion ROM) |

## MMIO Register Map (within BAR0)

### PCODE Mailbox Registers

| Register | Offset | Type | Description |
|---|---|---|---|
| PCODE_MAILBOX | 0x138124 | R/W | Command/status register |
| PCODE_DATA0 | 0x138128 | R/W | Data in/out (low 32 bits) |
| PCODE_DATA1 | 0x13812C | R/W | Data in/out (high 32 bits) |

### PCODE_MAILBOX Bit Fields

```
31    | 30-24  | 23-16     | 15-8      | 7-0
READY | rsvd   | PARAM2    | PARAM1    | COMMAND/ERROR
```

| Bit(s) | Name | Description |
|---|---|---|
| 31 | PCODE_READY | Set by software to trigger command, cleared by firmware when done |
| 23:16 | PARAM2 | Second parameter (e.g., fan channel) |
| 15:8 | PARAM1 | First parameter (e.g., subcommand) |
| 7:0 | COMMAND/ERROR | Written as command ID; returns error code on completion |

### Fan Tachometer Registers (Read-Only)

| Register | Offset | Description |
|---|---|---|
| BMG_FAN_1_SPEED | 0x138140 | Fan 1 pulse accumulator (2 pulses/rotation) |
| BMG_FAN_2_SPEED | 0x138170 | Fan 2 pulse accumulator |
| BMG_FAN_3_SPEED | 0x1381a0 | Fan 3 pulse accumulator |

Tachometer formula: `RPM = (delta_count / 2) * 60000 / elapsed_ms`

### Temperature Registers

| Register | Offset | Format |
|---|---|---|
| BMG_PACKAGE_TEMPERATURE | 0x138434 | bits 7:0 = degrees Celsius (unsigned) |
| BMG_VRAM_TEMPERATURE | 0x1382c0 | bits 30:8 = signed integer, bits 7:0 = fraction |
| BMG_VRAM_TEMPERATURE_N(n) | 0x138260 + n*4 | Per-channel VRAM temp (same format) |

### PCODE Scratch Registers

| Register | Offset | Notable Fields |
|---|---|---|
| PCODE_SCRATCH0 | 0x138320 | BREADCRUMB_VERSION[31:29], FDO_MODE[4], BOOT_STATUS[3:1] |
| PCODE_SCRATCH(n) | 0x138320 + n*4 | General purpose scratch registers |

### PCIe Capability Register

| Register | Offset | Description |
|---|---|---|
| BMG_PCIE_CAP | 0x138340 | LINK_DOWNGRADE[1:0], DOWNGRADE_CAPABLE |

### Energy/Power Registers (Platform-Specific)

| Register | Offset | Platforms |
|---|---|---|
| CRI_PACKAGE_ENERGY_STATUS | 0x138120 | Crescent Island |
| CRI_PLATFORM_ENERGY_STATUS | 0x138458 | Crescent Island |
| PVC_GT0_PACKAGE_ENERGY_STATUS | 0x281004 | PVC |
| PVC_GT0_PACKAGE_RAPL_LIMIT | 0x281008 | PVC |
| PVC_GT0_PACKAGE_POWER_SKU | 0x281080 | PVC |

## PCODE Mailbox Command Reference

### Command IDs

| Command | ID | Subcommands |
|---|---|---|
| PCODE_WRITE_MIN_FREQ_TABLE | 0x8 | - |
| PCODE_READ_MIN_FREQ_TABLE | 0x9 | - |
| PCODE_THERMAL_INFO | 0x25 | READ_THERMAL_LIMITS(0x0), READ_THERMAL_CONFIG(0x1), READ_THERMAL_DATA(0x2) |
| PCODE_FREQUENCY_CONFIG | 0x6E | READ_FUSED_P0(0x0), READ_FUSED_PN(0x1) |
| PCODE_LATE_BINDING | 0x5C | GET_CAPABILITY_STATUS(0x0), GET_VERSION_LOW(0x1), GET_VERSION_HIGH(0x2) |
| PCODE_POWER_SETUP | 0x7C | READ_I1(0x4), WRITE_I1(0x5), READ/WRITE_POWER_LIMIT(0x6-0x9) |
| FAN_SPEED_CONTROL | 0x7D | READ_NUM_FANS(0x4), 0x0-0x3/0x5-0xF = **UNKNOWN** |
| DGFX_PCODE_STATUS | 0x7E | GET_INIT_STATUS(0x0) |

### Error Codes

| Code | Name | errno | Description |
|---|---|---|---|
| 0x0 | SUCCESS | 0 | Operation completed successfully |
| 0x1 | ILLEGAL_CMD | -ENXIO | Command ID not recognized |
| 0x2 | TIMEOUT | -ETIMEDOUT | Firmware timed out |
| 0x3 | ILLEGAL_DATA | -EINVAL | Data value rejected |
| 0x4 | ILLEGAL_SUBCOMMAND | -ENXIO | Subcommand (param1) not recognized |
| 0x6 | LOCKED | -EBUSY | PCODE locked by another operation |
| 0x10 | GT_RATIO_OUT_OF_RANGE | -EOVERFLOW | Frequency request out of range |
| 0x11 | REJECTED | -EACCES | Command rejected by firmware |

## Mailbox Composition

```c
#define PCODE_MBOX(cmd, param1, param2) \
    ((cmd & 0xFF) | ((param1 & 0xFF) << 8) | ((param2 & 0xFF) << 16))
```

## Protocol Flow

```
1. Read PCODE_MAILBOX, verify READY (bit 31) == 0
2. Write data to PCODE_DATA0 and PCODE_DATA1 (for writes)
3. Write (READY | PCODE_MBOX(cmd, p1, p2)) to PCODE_MAILBOX
4. Poll PCODE_MAILBOX until READY (bit 31) == 0 (timeout: 1ms)
5. Read response from PCODE_DATA0 (and PCODE_DATA1)
6. Check error code in PCODE_MAILBOX bits 7:0
```

## Sysfs HWMON Attributes (Current - Kernel 7.0.12)

| Attribute | Permissions | Description |
|---|---|---|
| fan1_input | 0444 (RO) | Fan 1 RPM |
| temp2_input | 0444 (RO) | Package temperature (millidegrees) |
| temp2_crit | 0444 (RO) | Package critical temperature |
| temp2_emergency | 0444 (RO) | Package shutdown temperature |
| temp2_max | 0444 (RO) | Package max temperature |
| power1_cap | 0664 (RW) | Power limit (PL1) |
| power1_cap_interval | 0664 (RW) | PL1 time window |
| power1_crit | 0644 (RW) | Power critical limit (I1) |
| power1_label | 0444 (RO) | "card" |
| energy1_input | 0444 (RO) | Card energy counter |
| energy2_input | 0444 (RO) | Package energy counter |

**Missing**: `pwm1`, `pwm1_enable`, `fan1_target` — no writable fan control exists upstream.
