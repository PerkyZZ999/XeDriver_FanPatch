# Hardware Guide: Physical Fan Cable Redirect Fallback

## Overview

Intel Arc B580 (Battlemage) fan control is **firmware-gated**. The `xe` kernel
driver exposes read-only `fan1_input` (RPM) and temperature sensors via hwmon,
but there is **no writable `pwm1`** — the fan curve is encoded inside the
`fan_control_8086_e20b_8086_1100.bin` blob loaded through MEI late binding, and
the PCODE `FAN_SPEED_CONTROL` mailbox subcommands for setting duty are
undocumented and appear locked/unimplemented on Battlemage.

When every software approach (userspace PCODE probe, out-of-tree kernel module,
patched `xe_hwmon.c`) fails to break through the firmware wall, this fallback
redirects the GPU's internal fan to a **motherboard PWM header** that you fully
control from Linux via the ITE IT8689 Super I/O chip. The GPU still reports its
temperature through `hwmon2`; you simply use that temperature to drive a
motherboard fan header instead of the GPU's own.

**Principle:** GPU reports temperature → userspace reads it → userspace writes
PWM duty to a motherboard `SYS_FAN` header → motherboard spins the GPU fan.

---

## Required Materials

| Item | Purpose | Notes |
|------|---------|-------|
| 4-pin PWM fan extension cable (≥30 cm) | Reaches from GPU to motherboard header | Standard 12V PWM, 4-pin female-to-male |
| Mini-4pin (GPU) to standard-4pin adapter | Matches the B580's internal fan connector | ASRock uses a small JST-style 4-pin header on the fan PCB; verify pinout before buying |
| Wire strippers / flush cutters | Preparing cable ends | |
| Small Phillips screwdriver (PH0/PH1) | Disassembling the GPU shroud | |
| Anti-static wrist strap | ESD protection | |
| Heat-shrink tubing + lighter / heat gun | Insulating splices | Optional but recommended |
| Multimeter | Verifying continuity & pinout | |

> **Tip:** Some builders use a "PWM fan splitter" so the motherboard header drives
> the GPU fan **and** a chassis fan simultaneously — both then track GPU
> temperature. Ensure the splitter reports RPM from only one tach wire.

---

## Safety Warnings

> **READ BEFORE STARTING.**

1. **Power off completely.** Shut down the PC, switch off the PSU, and unplug the
   mains cable. Press the power button once to drain capacitors.
2. **Static protection.** Wear an anti-static wrist strap clipped to the chassis
   ground. Work on a hard, non-carpeted surface.
3. **Warranty void.** Opening the ASRock Challenger B580 shroud and modifying the
   fan wiring **voids the GPU warranty**. This is irreversible evidence of
   modification. Proceed only if you accept this risk.
4. **Do not power the GPU fan from a random 12V rail.** The motherboard PWM
   header supplies +12V, GND, tach, and PWM-control pins in a standard layout.
   Do **not** rewire to a Molex/SATA 12V line — you lose PWM control.
5. **Mind the fan connector pinout.** GPU fan connectors are not always standard
   4-pin PWM. The ASRock Challenger B580 uses a mini connector whose pin order
   may differ from a motherboard header. **Verify with a multimeter** before
   splicing. Incorrect wiring can short +12V to the tach/PWM line and damage the
   motherboard Super I/O.
6. **Never block all airflow.** While working, ensure at least one fan remains
   functional before booting. An overheating GPU with no fan will throttle or
   shut down.
7. **Reversibility:** This mod is **reversible** if you keep the original fan
   connector intact and use an extension/adapter rather than cutting the GPU's
   native fan wire (see [Reversibility Notes](#reversibility-notes)).

---

## Step-by-Step Disassembly: ASRock Challenger Arc B580

The Challenger B580 is a dual-fan, dual-slot card with a metal backplate and a
plastic shroud.

### 1. Remove the card from the system
- Power down, unplug, drain caps (above).
- Disconnect the PCIe power cables.
- Unscrew the single PCIe bracket retention screw, release the retention clip,
  and slide the card out.

### 2. Remove the backplate
- Lay the card face-down on an anti-static mat.
- Remove the screws securing the metal backplate (typically 4–6 Phillips screws
  around the perimeter). **Keep screws sorted** — they are different lengths.
- Lift the backplate off gently. Some cards use thermal pads between backplate
  and VRAM; leave them in place.

### 3. Separate the shroud from the heatsink
- With the card face-up, remove the screws that fasten the plastic fan shroud to
  the heatsink assembly (usually 4 screws near the fan hubs and 2 at the rear).
- Do **not** remove the heatsink from the PCB — you only need shroud access.
- Lift the shroud straight up. The fan cables are attached to the shroud and
  plug into headers on the PCB.

### 4. Locate the internal fan connector(s)
- Follow each fan's cable from the fan hub down to the PCB.
- The ASRock Challenger B580 typically uses a **mini 4-pin connector** (JST-PH
  style) per fan, seated in a white/black header on the PCB near the fan exhaust
  edge.
- There are usually **two fans** (fan 1 near the GPU die, fan 2 near the bracket
  edge). For redirect purposes, **fan 1** (the one closest to the GPU die, doing
  most of the thermal work) is the best candidate.
- Note the connector's pin 1 orientation (look for a triangle mark or a missing
  pin key).

### 5. Identify the pinout
Use a multimeter in continuity mode to map the mini connector to a standard
4-pin PWM header:

| Standard PWM pin | Signal | Wire color (typical) |
|------------------|--------|----------------------|
| Pin 1 | GND     | Black |
| Pin 2 | +12V    | Yellow/Red |
| Pin 3 | Tach    | Green/Blue |
| Pin 4 | PWM ctl | Blue/White |

> The mini connector **may omit the PWM-control pin** (3-pin variant) or reorder
> pins. If the GPU fan is 3-pin (no PWM control wire), the motherboard header
> will still spin it via voltage control if you set the header to DC mode
> (`pwmN_mode=0`), but true PWM requires all 4 pins. **Confirm before splicing.**

---

## Routing the Cable to a Motherboard Header

### 6. Install the adapter / extension
- **Preferred (reversible):** Use a mini-4pin → standard-4pin adapter. Unplug
  the GPU fan from the PCB header, plug the adapter's mini end into the fan, and
  route the standard-4pin end out of the GPU bracket.
- **Splice method (not reversible):** Cut the fan cable ~5 cm from the
  connector, splice in a 4-pin PWM extension cable matching the pinout above,
  insulate each splice with heat-shrink tubing.

### 7. Route the cable out of the case
- Feed the extension cable through the PCIe slot bracket opening or an unused
  expansion slot cover. Leave enough slack for the card to seat fully.
- Avoid pinching the cable between the heatsink and PCB on reassembly.

### 8. Reassemble the GPU
- Replace the shroud and backplate with the screws in their original positions.
- Ensure no cable contacts fan blades — spin each fan by hand to confirm
  clearance.

### 9. Connect to the motherboard
- Plug the standard 4-pin end into the chosen `SYS_FAN` header on the GIGABYTE
  B550M (see below).
- Reinstall the GPU, reconnect PCIe power, and close the case.

---

## Which Motherboard Header to Use (GIGABYTE B550M Gaming X WiFi 6)

The B550M uses the **ITE IT8689** Super I/O chip, exposed as `hwmon3` with
`pwm1`–`pwm5`. Run `identify_headers.sh` to confirm the live mapping, but the
typical header→PWM assignment is:

| it8689 PWM | Motherboard header | Use for GPU fan? |
|------------|--------------------|------------------|
| `pwm1` | `CPU_FAN` | **No** — reserved for the CPU cooler |
| `pwm2` | `SYS_FAN1` | OK if free (often front intake) |
| `pwm3` | `SYS_FAN2` | **Recommended** — frequently unused |
| `pwm4` | `SYS_FAN3` | Good alternative |
| `pwm5` | `SYS_FAN4` / `PUMP` | OK; some BIOS run it at 100% by default |

**Selection rules:**
- Pick a header whose `fanN_input` currently reads **0 RPM** (nothing plugged in).
- **Avoid `CPU_FAN` (`pwm1`)** — tying GPU temp to the CPU header risks CPU
  overheating if the daemon misbehaves, and the BIOS may alarm.
- After plugging in, boot and re-run `identify_headers.sh` to verify the GPU fan
  now reports RPM on the chosen header.

---

## Software Configuration After Physical Redirect

Once the GPU fan spins from a `SYS_FAN` header, configure Linux to drive that
header from the **GPU** temperature (`hwmon2/temp2_input`), not the CPU temp.

### Option A: lm-sensors fancontrol

```bash
# 1. Ensure modules are loaded
sudo modprobe it87
sudo modprobe coretemp   # if not built-in

# 2. Run sensors-detect (answer YES to scan it8689)
sudo sensors-detect --auto

# 3. Auto-configure, then run the setup script (pass the PWM channel you chose)
sudo ./setup_fancontrol.sh 3   # pwm3 = SYS_FAN2

# 4. Verify
systemctl status fancontrol
watch -n1 cat /sys/class/hwmon/hwmon3/fan3_input   # GPU fan RPM
```

The generated `/etc/fancontrol` maps `hwmon2/temp2_input` → `hwmon3/pwm3` with
the curve **30°C→0%, 40°C→31%, 50°C→47%, 60°C→63%, 70°C→78%, 80°C→100%**.

### Option B: CoolerControl (GUI)

1. Install CoolerControl: `sudo pacman -S coolercontrol` (or your distro's package).
2. Enable the daemon: `sudo systemctl enable --now coolercontrold`.
3. Open the CoolerControl UI (usually `http://localhost:11987` or the desktop app).
4. Import `coolercontrol-profile.json`:
   - **Settings → Import Configuration**, select the JSON file.
5. The profile "B580 GPU Fan" links:
   - **Sensor:** `hwmon2` (xe) `temp2` — GPU package temperature
   - **Fan:** `hwmon3` (it8689) `pwm3` — the redirected SYS_FAN2 header
6. Apply the profile and enable the fan. Watch the RPM respond to GPU load.

### Option C: xe_fanctl daemon

The project's `xe_fanctl` daemon supports `method = motherboard` in its config:

```ini
[motherboard]
pwm_device = hwmon3
pwm_channel = 3
fan_device = hwmon3
fan_channel = 3
```

This is the most integrated option if you are running the full XeDriver_FanPatch
stack. See the daemon's own documentation for details.

---

## Verification Checklist

- [ ] GPU fan spins at boot (BIOS default PWM) — confirms wiring is correct.
- [ ] `cat /sys/class/hwmon/hwmon3/fanN_input` reports non-zero RPM.
- [ ] `cat /sys/class/hwmon/hwmon2/temp2_input` reports plausible GPU temp.
- [ ] Under GPU load, the chosen PWM duty rises and RPM increases.
- [ ] At idle (~30°C), the fan stops or runs at minimum (curve floor).
- [ ] No BIOS "CPU fan error" (because you used a `SYS_FAN` header, not `CPU_FAN`).
- [ ] System survives a reboot with fancontrol/CoolerControl auto-starting.

---

## Reversibility Notes

- **Fully reversible** if you used an adapter (mini-4pin → standard-4pin) without
  cutting the GPU's native fan wire. To revert: unplug the adapter, replug the
  fan into the GPU PCB header, and remove the software config.
- **Partially reversible** if you spliced the cable: you can re-splice the
  original connector back, but the wire is shortened and the modification is
  visible. The warranty is already void at this point.
- Software configs (`/etc/fancontrol`, CoolerControl profile, `xe_fanctl`) are
  trivially reversible — disable the service or delete the config.
- The `identify_headers.sh` and `setup_fancontrol.sh` scripts make no permanent
  hardware changes; they only write sysfs values and config files. To undo the
  sysfs enable mode, set `pwmN_enable` back to `2` (auto).
