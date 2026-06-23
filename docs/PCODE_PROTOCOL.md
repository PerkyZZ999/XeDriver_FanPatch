# PCODE Mailbox Protocol Documentation

## Overview

The PCODE (Platform Code) firmware is a low-level microcontroller embedded in Intel GPUs that handles power management, thermal control, and fan regulation. The xe kernel driver communicates with PCODE via a **mailbox mechanism** — a set of MMIO registers that implement a request/response protocol.

## Mailbox Registers

| Register | Offset | Purpose |
|---|---|---|
| PCODE_MAILBOX | 0x138124 | Command/status (write command, read status) |
| PCODE_DATA0 | 0x138128 | Data word 0 (input/output) |
| PCODE_DATA1 | 0x13812C | Data word 1 (input/output) |

## Register Format

### PCODE_MAILBOX (0x138124)

```
Bit 31:     READY flag
              Write 1 to trigger command
              Hardware clears to 0 when done
Bits 23-16: PARAM2 (third parameter byte)
Bits 15-8:  PARAM1 (subcommand/second parameter byte)
Bits 7-0:   COMMAND (command ID on write) / ERROR (status on read)
```

### Mailbox Composition

```c
#define PCODE_MBOX(cmd, param1, param2) \
    ((cmd & 0xFF) | ((param1 & 0xFF) << 8) | ((param2 & 0xFF) << 16))
```

Example: `PCODE_MBOX(FAN_SPEED_CONTROL, FSC_READ_NUM_FANS, 0)` = `0x00047D`

## Protocol Flow

### Read Operation

```
1. Read PCODE_MAILBOX
   - If bit 31 (READY) is set, wait for it to clear (timeout: 1ms)

2. Write 0 to PCODE_DATA0 and PCODE_DATA1 (clear previous data)

3. Write (0x80000000 | PCODE_MBOX(cmd, p1, p2)) to PCODE_MAILBOX
   - Bit 31 (READY) must be set to trigger the command

4. Poll PCODE_MAILBOX until bit 31 (READY) clears
   - Timeout: 1ms (matching xe driver behavior)

5. Read PCODE_DATA0 (response data)
   - Optionally read PCODE_DATA1 for 64-bit responses

6. Read PCODE_MAILBOX bits 7:0 for error code
   - 0x00 = success
   - 0x04 = illegal subcommand
   - 0x06 = PCODE locked
```

### Write Operation

```
1. Read PCODE_MAILBOX
   - If bit 31 (READY) is set, wait

2. Write data value to PCODE_DATA0
   - Optionally write to PCODE_DATA1 for 64-bit data

3. Write (0x80000000 | PCODE_MBOX(cmd, p1, p2)) to PCODE_MAILBOX

4. Poll PCODE_MAILBOX until bit 31 (READY) clears

5. Read PCODE_MAILBOX bits 7:0 for error code
```

## Concurrency Safety

The xe driver protects mailbox access with a per-tile mutex (`tile->pcode.lock`). Out-of-tree modules and userspace tools must implement their own synchronization:

- **Kernel module**: Use a spinlock (`spin_lock_irqsave`) to prevent reentrancy
- **Userspace**: Check the READY bit before each command; if set, wait for it to clear
- **Race window**: Very small (microseconds) — PCODE commands execute quickly
- **Mitigation**: The READY bit provides basic mutual exclusion at the hardware level

## Error Handling

| Code | Name | Meaning | Action |
|---|---|---|---|
| 0x0 | SUCCESS | Command completed | Read data registers |
| 0x1 | ILLEGAL_CMD | Unknown command ID | Check command encoding |
| 0x2 | TIMEOUT | Firmware timeout | Retry or abort |
| 0x3 | ILLEGAL_DATA | Invalid data value | Check bounds/format |
| 0x4 | ILLEGAL_SUBCOMMAND | Unknown subcommand | Subcommand not supported |
| 0x6 | LOCKED | PCODE busy/locked | Retry after delay |
| 0x10 | GT_RATIO_OUT_OF_RANGE | Frequency out of range | Check frequency limits |
| 0x11 | REJECTED | Command refused | Firmware policy denial |

## FAN_SPEED_CONTROL (0x7D) Subcommand Space

This is the critical command for fan control. Only one subcommand is documented:

| Subcommand | ID | Status | Purpose |
|---|---|---|---|
| FSC_READ_NUM_FANS | 0x4 | **Documented** | Returns number of fans (1-3) |
| 0x0 | UNKNOWN | **Probe target** | May be READ_FAN_DUTY or SET_FAN_DUTY |
| 0x1 | UNKNOWN | **Probe target** | May be READ_FAN_CURVE or SET_FAN_CURVE |
| 0x2 | UNKNOWN | **Probe target** | ? |
| 0x3 | UNKNOWN | **Probe target** | ? |
| 0x5 | UNKNOWN | **Probe target** | ? |
| 0x6-0xF | UNKNOWN | **Probe target** | ? |

### Probing Strategy

1. **Read probe**: Send `PCODE_MBOX(0x7D, subcmd, 0)` for each subcmd 0x0-0xF
   - Success (error=0): Subcommand exists, examine returned data
   - ILLEGAL_SUBCOMMAND (0x4): Subcommand not supported, skip
   - Other errors: Log and investigate

2. **Channel probe**: For successful subcommands, try param2=0,1,2 (fan channels)
   - May return per-fan data or fail for invalid channels

3. **Write probe** (DANGEROUS): For successful read subcommands, try writing
   - Send `PCODE_MBOX(0x7D, subcmd, 0)` with data in PCODE_DATA0
   - Monitor fan RPM before/after to detect actual fan speed changes
   - Use small duty values first (e.g., 64 = 25%) to avoid sudden fan changes

## Late Binding Architecture

The Battlemage fan controller uses a **late binding firmware** mechanism:

```
User/Kernel
    |
    v
MEI Interface (mei_lb driver)
    |
    v
Management Engine (ME) — authenticates firmware
    |
    v
PUnit/PCODE — receives authenticated firmware blob
    |
    v
Fan Controller — executes fan curve from firmware
```

### Key Components

1. **`fan_control_8086_e20b_8086_1100.bin`**: Firmware blob containing fan curve
2. **`mei_lb`**: MEI Late Binding driver (loads firmware via ME)
3. **`xe_late_bind_fw`**: Xe driver component that coordinates loading
4. **PCODE LATE_BINDING (0x5C)**: Mailbox command to query firmware status

### Late Binding Query Commands

```
PCODE_MBOX(0x5C, 0x0, 0)  → Capability Status
  Bit 0:   V1_FAN_SUPPORTED
  Bit 3:   VR_PARAMS_SUPPORTED
  Bit 16:  V1_FAN_PROVISIONED (firmware loaded)
  Bit 19:  VR_PARAMS_PROVISIONED

PCODE_MBOX(0x5C, 0x1, 1)  → Fan FW Version Low (major.minor)
PCODE_MBOX(0x5C, 0x2, 1)  → Fan FW Version High (hotfix.build)
```

### System State (Confirmed)

On the target system:
- `mei_lb` driver bound: YES
- `fan_control_*.bin` firmware loaded: YES
- V1_FAN_PROVISIONED: YES (confirmed via kernel log)
- PCODE mailbox power limits working: YES
