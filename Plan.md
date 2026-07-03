# Detailed Implementation Plan: Intel Arc B580 Linux Fan Control

## Resolution (July 3, 2026)

**Fan control is working** via Intel patch series **168027** on a custom `linux-cachyos-xefan` kernel.

| Goal | Status |
|------|--------|
| Writable `pwm1` / fan curve sysfs | **Done** — series 168027 |
| CoolerControl integration | **Done** — Fixed % and Graph profiles verified |
| Safe rollback | **Done** — stock `linux-cachyos` coexists, Limine dual-boot |
| Userspace PCODE-only control | **Not viable** — needs full driver path (see June findings below) |
| Physical motherboard redirect | **Not needed** — software path works |

**Guides:** [`docs/CUSTOM_KERNEL.md`](docs/CUSTOM_KERNEL.md) · [`docs/COOLERCONTROL.md`](docs/COOLERCONTROL.md) · [`docs/UPSTREAM_STATUS.md`](docs/UPSTREAM_STATUS.md)

The sections below document the original research plan and June 2026 findings. They remain useful for understanding why raw PCODE probing failed and how the upstream patch differs.

---

## 0. Executive Summary of Findings

### Hardware & System Context
- **GPU**: Intel Arc B580 (Battlemage G21), PCI ID `8086:e20b`, ASRock Challenger
- **Kernel**: 7.0.12-1-cachyos (CachyOS, Arch-based), compiled with clang 22.1.6
- **Driver**: `xe` module loaded, version 1.1.0
- **Late Binding**: `mei_lb` driver bound, `fan_control_8086_e20b_8086_1100.bin` firmware loaded
- **HWMON**: `hwmon2` (xe) exposes read-only `fan1_input` (1072 RPM), NO `pwm1`
- **Motherboard**: GIGABYTE B550M with `it8689` Super I/O (hwmon3) — full PWM control available (pwm1-pwm5)

### Critical Technical Discoveries
1. **Fan control on Battlemage is firmware-driven**, not register-driven. The fan curve is encoded in the `fan_control_*.bin` blob loaded via MEI late binding.
2. **The `FAN_SPEED_CONTROL` PCODE mailbox command (0x7D)** has only one documented subcommand: `FSC_READ_NUM_FANS` (0x4). Subcommands 0x0-0x3 and 0x5+ are undefined but may include fan duty/speed write commands.
3. **i915 does NOT implement writable fan control either** — the Overview.md assumption was incorrect. Both i915 and xe are read-only for fan RPM.
4. **Only 13 xe symbols are exported** (all SRIOV/VFIO) — `xe_pcode_read()`, `xe_pcode_write()`, `xe_mmio_read32()` etc. are NOT exported, so out-of-tree modules must access MMIO directly.
5. **PCI BAR0** is at physical address `0xfb000000` (16MB). All PCODE registers are within this BAR at known offsets.
6. **CONFIG_STRICT_DEVMEM=y** — `/dev/mem` is restricted, but PCI resource files (`/sys/bus/pci/devices/0000:09:00.0/resource0`) can be mmap'd by root.

### Register Map (within BAR0)
| Register | Offset | Description |
|---|---|---|
| PCODE_MAILBOX | 0x138124 | Command/param register (bit 31 = ready, bits 7-0 = cmd, 15-8 = param1, 23-16 = param2) |
| PCODE_DATA0 | 0x138128 | Data in/out (low 32 bits) |
| PCODE_DATA1 | 0x13812C | Data in/out (high 32 bits) |
| BMG_FAN_1_SPEED | 0x138140 | Fan 1 tachometer (read-only pulse accumulator, 2 pulses/rotation) |
| BMG_FAN_2_SPEED | 0x138170 | Fan 2 tachometer |
| BMG_FAN_3_SPEED | 0x1381a0 | Fan 3 tachometer |
| BMG_PACKAGE_TEMPERATURE | 0x138434 | Package temperature (bits 7:0, degrees Celsius) |

### PCODE Mailbox Protocol
```
1. Read PCODE_MAILBOX, verify PCODE_READY (bit 31) is 0
2. Write data to PCODE_DATA0 and PCODE_DATA1 (for writes)
3. Write (PCODE_READY | PCODE_MBOX(cmd, param1, param2)) to PCODE_MAILBOX
4. Poll PCODE_MAILBOX until PCODE_READY (bit 31) becomes 0
5. Read response from PCODE_DATA0 (and PCODE_DATA1 if needed)
6. Check error bits (bits 7:0 of PCODE_MAILBOX): 0=success, 1=illegal cmd, 3=illegal data, 4=illegal subcommand, 6=locked
```

### PCODE Error Codes
| Code | Name | errno | Meaning |
|---|---|---|---|
| 0x0 | PCODE_SUCCESS | 0 | Success |
| 0x1 | PCODE_ILLEGAL_CMD | -ENXIO | Illegal Command |
| 0x2 | PCODE_TIMEOUT | -ETIMEDOUT | Timed out |
| 0x3 | PCODE_ILLEGAL_DATA | -EINVAL | Illegal Data |
| 0x4 | PCODE_ILLEGAL_SUBCOMMAND | -ENXIO | Illegal Subcommand |
| 0x6 | PCODE_LOCKED | -EBUSY | PCODE Locked |
| 0x10 | PCODE_GT_RATIO_OUT_OF_RANGE | -EOVERFLOW | GT ratio out of range |
| 0x11 | PCODE_REJECTED | -EACCES | PCODE Rejected |

### FAN_SPEED_CONTROL (0x7D) Subcommand Space
| Subcommand | Status | Purpose |
|---|---|---|
| 0x0 | UNKNOWN | **Probe target** — may be READ_FAN_DUTY or SET_FAN_DUTY |
| 0x1 | UNKNOWN | **Probe target** |
| 0x2 | UNKNOWN | **Probe target** |
| 0x3 | UNKNOWN | **Probe target** |
| 0x4 | DOCUMENTED | FSC_READ_NUM_FANS — returns fan count |
| 0x5 | UNKNOWN | **Probe target** |
| 0x6-0xF | UNKNOWN | **Probe target** |

### Firmware Blob Analysis
- File: `fan_control_8086_e20b_8086_1100.bin` (4064 bytes, zstd compressed)
- Format: Intel `$FPT` (Flash Partition Tool) container
- Partitions: `LTEP`, `CDMD`, `LTES`, `LTEB`
- Description string: "Intel(R) Battlemage (BMG) Graphics - Late Binding - Fan Configuration"
- Sub-containers: "RootContainer/PunitConfigLateBind", "RootContainer/CDMD", "RootContainer/LateBindingMetadata"
- Authentication string: "FIT AS A FIDDLE"
- The blob is mostly 0xFF padding after the first ~100 bytes of metadata
- This is a firmware container, NOT a simple register map — modifying it requires defeating ME authentication

---

## 1. Implementation Strategy

### Multi-Approach Architecture

Given the technical constraints (no exported xe symbols, strict devmem, firmware-gated fan control), we implement a **layered approach** with increasing levels of hardware access:

```
Layer 1: Userspace PCODE Probe Tool (safest, direct MMIO via PCI resource)
    ↓ if write subcommand found
Layer 2: Out-of-tree Kernel Module (sysfs interface, proper MMIO synchronization)
    ↓ if kernel-level integration needed
Layer 3: Patched xe_hwmon.c (reference patch for upstream contribution)
    ↓ if all software approaches fail
Layer 4: Physical Fan Control Fallback (motherboard PWM header redirect)
```

### Component Overview

| Component | Type | Risk | Purpose |
|---|---|---|---|
| `xe_pcode_probe` | Userspace C tool | Low | Probe FAN_SPEED_CONTROL subcommands via PCI resource0 mmap |
| `xe_fan_probe.ko` | Kernel module | Medium | Sysfs interface for fan control, proper PCODE synchronization |
| `xe_hwmon.patch` | Source patch | High (if applied) | Reference patch for xe_hwmon.c with PWM write support |
| `xe_fanctl` | Python daemon | None | Fan control daemon with configurable curves, hwmon integration |
| `fan_redirect` | Shell scripts | None | Physical fan control fallback via motherboard it8689 |
| `fw_analyzer` | Python tool | None | Analyze fan_control firmware blob structure |

---

## 2. Component Specifications

### 2.1 Component 1: Userspace PCODE Probe Tool (`xe_pcode_probe`)

**Purpose**: Safely probe undocumented PCODE mailbox subcommands from userspace without loading any kernel modules.

**How it works**:
1. Opens `/sys/bus/pci/devices/0000:09:00.0/resource0` (BAR0 MMIO, 16MB)
2. mmaps the file into userspace memory
3. Implements the PCODE mailbox protocol directly:
   - Check PCODE_READY bit
   - Write command to PCODE_MAILBOX
   - Poll for completion
   - Read response from PCODE_DATA0
4. Probes all FAN_SPEED_CONTROL subcommands (0x0-0xF)
5. Reads fan tachometer and temperature registers
6. Logs all results with error code interpretation
7. Interactive mode: allows sending custom mailbox commands

**Safety measures**:
- Checks PCODE_READY before each command (avoids collision with xe driver)
- Timeout on polling (1ms, matching xe driver's 1ms timeout)
- Read-only by default; write mode requires explicit `--write` flag
- Never writes to fan control subcommands unless `--force` is specified
- All operations logged with timestamps

**Build**: `gcc -O2 -Wall -o xe_pcode_probe xe_pcode_probe.c`

**Usage**:
```
sudo ./xe_pcode_probe --probe          # Probe all FSC subcommands
sudo ./xe_pcode_probe --read-fan       # Read fan RPM from tach registers
sudo ./xe_pcode_probe --read-temp      # Read package temperature
sudo ./xe_pcode_probe --late-bind      # Query late binding fan control status
sudo ./xe_pcode_probe --write-duty 128 # Attempt to set fan duty (0-255) via all subcmds
sudo ./xe_pcode_probe --interactive    # Interactive mailbox command sender
```

### 2.2 Component 2: Out-of-tree Kernel Module (`xe_fan_probe.ko`)

**Purpose**: Provide a proper kernel-level sysfs interface for fan control with correct MMIO synchronization.

**How it works**:
1. On module load: `ioremap`s the GPU's PCI BAR0 physical address (0xfb000000, 16MB)
2. Creates a sysfs class at `/sys/class/xe_fan/`
3. Exposes attributes:
   - `fan1_input` (read): Current fan RPM (from tach register)
   - `fan2_input`, `fan3_input` (read): Additional fan RPMs
   - `temp1_input` (read): Package temperature
   - `pwm1` (read/write): Fan duty cycle 0-255 (if a valid FSC write subcommand is found)
   - `pwm1_enable` (read/write): 0=auto/firmware, 1=manual
   - `probe_results` (read): Results of subcommand probing
   - `subcommand` (write): Manually trigger a specific FSC subcommand
4. Implements PCODE mailbox protocol with spinlock protection
5. On module unload: cleans up sysfs, iounmaps MMIO

**Safety measures**:
- Uses `spinlock` to protect PCODE mailbox access (prevents reentrancy)
- Checks PCODE_READY bit with timeout (prevents deadlock)
- All MMIO writes go through `wmb()` memory barriers
- Module can be loaded/unloaded independently of xe driver
- Does NOT interfere with xe driver's own MMIO access (separate mapping, READY bit synchronization)
- Probing is done once at module load, results cached

**Build**: `make -C /lib/modules/$(uname -r)/build M=$(pwd) modules`

**Usage**:
```
sudo insmod xe_fan_probe.ko           # Load module (auto-probes subcommands)
cat /sys/class/xe_fan/probe_results   # View probe results
cat /sys/class/xe_fan/fan1_input      # Read fan RPM
echo 128 > /sys/class/xe_fan/pwm1     # Set fan duty to 50% (if supported)
echo 1 > /sys/class/xe_fan/pwm1_enable # Enable manual control
sudo rmmod xe_fan_probe               # Unload module
```

### 2.3 Component 3: Patched xe_hwmon.c (`xe_hwmon.patch`)

**Purpose**: Reference patch showing how to add writable PWM support to the upstream xe driver's hwmon interface.

**Changes to xe_hwmon.c**:
1. Add `HWMON_F_INPUT | HWMON_F_TARGET` to fan channel info (or add a separate `pwm` channel)
2. Add `xe_hwmon_fan_is_visible()` case for `hwmon_fan_target` returning `0644`
3. Add `xe_hwmon_fan_write()` function that calls `xe_pcode_write()` with `FAN_SPEED_CONTROL` subcommands
4. Add `FSC_WRITE_FAN_DUTY` macro (placeholder subcommand ID, to be determined by probing)
5. Add bounds checking (0-255 for PWM, 0-100 for percentage)
6. Add `xe_hwmon_fan_target_read()` to read back current fan duty

**This is a reference patch only** — it cannot be applied without the full xe source tree, and the actual subcommand IDs must be determined by probing (Components 1 & 2). Provided as documentation for potential upstream contribution.

### 2.4 Component 4: Userspace Fan Control Daemon (`xe_fanctl`)

**Purpose**: A daemon that monitors GPU temperature and controls fan speed based on configurable curves, working with any of the control methods above.

**How it works**:
1. Reads GPU temperature from `/sys/class/hwmon/hwmon2/temp2_input` (package temp)
2. Applies a configurable fan curve (temperature -> PWM duty mapping)
3. Writes PWM duty via the best available method:
   a. `/sys/class/xe_fan/pwm1` (if kernel module loaded)
   b. Userspace PCODE write tool (if probe found valid subcommand)
   c. Motherboard PWM header (if physical redirect is configured)
4. Runs as a systemd service
5. Configurable via `/etc/xe_fanctl.conf`
6. Logs to syslog/journal

**Configuration format**:
```ini
[general]
interval = 2          # poll interval in seconds
method = auto          # auto, kernel, userspace, motherboard
hysteresis = 3         # degrees Celsius

[curve]
# temp_celsius = pwm_duty (0-255)
30 = 0                # 0% below 30C
40 = 80               # ~31% at 40C
50 = 120              # ~47% at 50C
60 = 160              # ~63% at 60C
70 = 200              # ~78% at 70C
80 = 255              # 100% at 80C+

[motherboard]
# Fallback: which motherboard PWM header controls the GPU fan
pwm_device = hwmon3   # it8689
pwm_channel = 3       # pwm3 (example)
fan_device = hwmon3   # fan input to monitor
fan_channel = 3       # fan3
```

### 2.5 Component 5: Physical Fan Control Fallback (`fan_redirect`)

**Purpose**: Scripts and documentation for redirecting GPU fan control to the motherboard's it8689 Super I/O chip.

**Contents**:
- `identify_headers.sh`: Scans motherboard PWM headers and helps identify which one to connect the GPU fan to
- `setup_fancontrol.sh`: Configures lm-sensors fancontrol for GPU fan via motherboard
- `coolercontrol-profile.json`: Pre-configured CoolerControl profile for B580 fan
- `HARDWARE_GUIDE.md`: Step-by-step guide for the physical fan cable redirect

**Motherboard context**:
- GIGABYTE B550M Gaming X WiFi 6 has multiple 4-pin PWM headers
- The it8689 Super I/O chip (hwmon3) exposes pwm1-pwm5 with full auto/manual control
- The GPU fan's mini-4pin connector can be adapted to a standard 4-pin PWM header

### 2.6 Component 6: Firmware Blob Analyzer (`fw_analyzer`)

**Purpose**: Parse and analyze the `fan_control_8086_e20b_8086_1100.bin` firmware blob to understand its structure.

**How it works**:
1. Parses the `$FPT` (Flash Partition Tool) container format
2. Extracts partition entries (`LTEP`, `CDMD`, `LTES`, `LTEB`)
3. Decodes metadata (version, sizes, offsets)
4. Searches for fan curve data patterns (temperature/duty lookup tables)
5. Compares with known Intel firmware container formats
6. Outputs a human-readable analysis report

---

## 3. Build & Test Plan

### Build Environment
- **Compiler**: gcc 16.1.1 or clang 22.1.6
- **Kernel build tree**: `/lib/modules/7.0.12-1-cachyos/build`
- **Module.symvers**: Available (for out-of-tree module building)
- **All work done in**: project workspace (clone path varies per machine)

### Test Sequence
1. Build userspace probe tool → run read-only probes (safe, no writes)
2. Build kernel module → load, read probe results, unload
3. If a valid FSC write subcommand is found → test PWM write carefully
4. Build fan control daemon → test with auto-detected control method
5. Prepare physical fallback → ready if software control is impossible

### Safety Protocol
- **Never write to undocumented PCODE subcommands without first reading them**
- **Start with read-only probing** (subcommands 0x0-0xF, read mode)
- **If a subcommand returns success on read**, log the data format
- **Only attempt writes** after confirming a subcommand accepts write data
- **Monitor dmesg** for xe driver errors during all testing
- **Have `rmmod` ready** when testing the kernel module
- **Keep SSH session open** when testing (in case of GPU crash)

---

## 4. File Structure

```
XeDriver_FanPatch/
├── Overview.md                    # Original project specification
├── Plan.md                        # This detailed implementation plan
├── README.md                      # Build & usage instructions
├── RESEARCH.md                    # Technical research findings
│
├── src/
│   ├── userspace/
│   │   ├── xe_pcode_probe.c       # PCODE mailbox probe tool
│   │   ├── Makefile
│   │   └── pcode_protocol.h       # Shared PCODE definitions
│   │
│   ├── kernel/
│   │   ├── xe_fan_probe.c         # Out-of-tree kernel module
│   │   ├── Makefile
│   │   └── pcode_regs.h           # Register definitions
│   │
│   ├── daemon/
│   │   ├── xe_fanctl              # Fan control daemon (Python)
│   │   ├── xe_fanctl.conf         # Default configuration
│   │   └── xe_fanctl.service      # Systemd service file
│   │
│   ├── patch/
│   │   ├── xe_hwmon.c.patched                              # Early reference patch
│   │   └── xe-fan-control-168027-cachyos-7.1.2.patch       # Working CachyOS 7.1.2 patch
│   │
│   ├── fallback/
│   │   ├── identify_headers.sh    # Motherboard PWM header scanner
│   │   ├── setup_fancontrol.sh    # fancontrol setup script
│   │   ├── coolercontrol-profile.json
│   │   └── HARDWARE_GUIDE.md      # Physical redirect guide
│   │
│   └── firmware/
│       ├── fw_analyzer.py         # Firmware blob analyzer
│       └── fan_control_8086_e20b_8086_1100.bin  # Copy of firmware blob
│
├── reference/
│   ├── series-168027-fan-control.mbox   # Intel upstream patch series (mbox)
│   ├── xe_hwmon.c                 # Original upstream source (reference)
│   ├── xe_pcode.c                 # Original upstream source (reference)
│   ├── xe_pcode.h                 # Original upstream source (reference)
│   ├── xe_pcode_api.h             # Original upstream source (reference)
│   ├── xe_pcode_regs.h            # Original upstream source (reference)
│   └── xe_hwmon.h                 # Original upstream source (reference)
│
└── docs/
    ├── CUSTOM_KERNEL.md           # Build, install, verify, rollback
    ├── COOLERCONTROL.md           # CoolerControl setup and fail-safe
    ├── UPSTREAM_STATUS.md         # Series 168027 merge status
    ├── GITHUB_ISSUE_COMMENT.md    # Template for compute-runtime #885
    ├── TESTING_RESULTS.md         # June + July 2026 test logs
    ├── REGISTER_MAP.md            # Complete register documentation
    ├── PCODE_PROTOCOL.md          # PCODE mailbox protocol documentation
    └── FIRMWARE_ANALYSIS.md       # Firmware blob analysis report
```

---

## 5. Risk Assessment

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| No FSC write subcommand exists | Medium | Project goal unachievable via software | Physical fan redirect fallback (Component 5) |
| PCODE write crashes GPU/driver | Low | System hang/crash | Read-only probing first, SSH session ready, module unload ready |
| Race condition with xe driver | Low | Corrupted mailbox command | READY bit check, spinlock in kernel module, short timeout |
| Firmware blob is authentication-gated | High (confirmed) | Cannot modify fan curve in firmware | Not attempting firmware modification; focusing on PCODE mailbox |
| Kernel module build failure | Low | Cannot use kernel-level control | Userspace tool as alternative (doesn't need kernel build) |
| Motherboard PWM incompatible | Low | Fallback unavailable | Multiple PWM headers available (pwm1-pwm5) |

---

## 6. Success Criteria

1. **Minimum viable**: Userspace probe tool successfully reads fan RPM, temperature, and probes FSC subcommands — **achieved June 2026**
2. **Partial success**: A valid FSC read subcommand is found that returns fan duty/speed data — **achieved June 2026**
3. **Full success**: Writable fan control via kernel driver — **achieved July 2026** via series 168027 custom kernel + CoolerControl (not userspace PCODE alone)
4. **Fallback success**: Physical fan redirect documented — **available but not required**

## 7. Post-Resolution Artifacts

| Artifact | Location |
|----------|----------|
| Adapted CachyOS 7.1.2 patch | `src/patch/xe-fan-control-168027-cachyos-7.1.2.patch` |
| Original Intel mbox series | `reference/series-168027-fan-control.mbox` |
| Build / install / rollback guide | `docs/CUSTOM_KERNEL.md` |
| CoolerControl + fail-safe | `docs/COOLERCONTROL.md` |
| Upstream merge tracking | `docs/UPSTREAM_STATUS.md` |
| Verified test log | `docs/TESTING_RESULTS.md` (July 2026 section) |
