# CoolerControl Setup — Arc B580 (xe hwmon)

Verified July 3, 2026 on `linux-cachyos-xefan` with CoolerControl **4.3.1**.

## Prerequisites

- Patched kernel installed (see [`CUSTOM_KERNEL.md`](CUSTOM_KERNEL.md))
- Booted into `*-cachyos-xefan` kernel
- CoolerControl installed (`coolercontrol` + `coolercontrold`)

## Device Detection

After reboot into the xefan kernel:

1. Open CoolerControl
2. Find the **xe** device (Intel GPU hwmon)
3. Fan should show as **Unmanaged** (not Read-Only)

On stock kernels without the patch, the same fan appears **Read-Only**.

## Profile Types

| Profile | Use case |
|---------|----------|
| **Fixed %** | Constant duty — good for quick tests |
| **Graph** | Temperature-based curve — recommended for daily use |
| **Mix / Overlay** | Combine multiple sensors |

### Recommended Graph Profile

- **Temperature source:** `pkg` (package GPU temp on xe hwmon)
- **Minimum curve point:** ~15–20% duty — never set 0% at low temps
- Match or slightly raise the stock curve if you want quieter idle

Stock firmware floor is ~10% PWM (pwm 26) at idle. Setting 0% in a curve may be clamped by the driver but is not recommended.

## pwm1_enable Modes (Driver)

| Value | Meaning |
|-------|---------|
| `0` | Full speed (100%) — **not** off |
| `1` | Manual / user curve (CoolerControl active control) |
| `2` | Automatic — GPU stock firmware fan table |

CoolerControl uses mode `1` when managing the fan. Setting the fan to **Unmanaged** in the UI typically writes mode `2` back to the driver.

## Fail-Safe: What Happens If Something Breaks?

| Scenario | Fan behavior |
|----------|--------------|
| CoolerControl daemon crash | Stays at **last PWM** CoolerControl set |
| Clean daemon stop | Improved in CC 4.x; may not always restore auto |
| Set fan to **Unmanaged** | Returns to stock auto curve (`pwm1_enable=2`) |
| Reboot | Driver starts in auto mode — stock table |
| Boot stock kernel (no patch) | GPU firmware handles fan internally (pre-patch behavior) |

**Fan will not drop to 0%** from a curve or patch failure:

1. `pwm1_enable=0` means full speed, not zero
2. Driver clamps to hardware `min_pwm` from GPU firmware
3. Stock auto table has ~10% minimum at idle

### Optional: Systemd Failsafe

Force auto mode when `coolercontrold` stops:

```bash
sudo mkdir -p /etc/systemd/system/coolercontrold.service.d
sudo tee /etc/systemd/system/coolercontrold.service.d/xe-fan-failsafe.conf <<'EOF'
[Service]
ExecStopPost=/bin/sh -c 'for h in /sys/class/hwmon/hwmon*/name; do [ "$(cat $h)" = xe ] && echo 2 > "${h%/name}/pwm1_enable"; done'
EOF
sudo systemctl daemon-reload
```

Adjust if your xe hwmon index changes — the script finds it by name.

## Manual sysfs Control (without CoolerControl)

```bash
# Find xe hwmon
H=$(dirname "$(grep -l xe /sys/class/hwmon/hwmon*/name)")

# Manual 50%
echo 1 | sudo tee $H/pwm1_enable
echo 128 | sudo tee $H/pwm1

# Back to stock auto
echo 2 | sudo tee $H/pwm1_enable
```

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Fan shows Read-Only | Boot xefan kernel; verify `pwm1` exists in sysfs |
| PWM write permission denied | Run CoolerControl daemon as root (default) |
| Fan stuck after CC crash | `echo 2 \| sudo tee .../pwm1_enable` or reboot |
| hwmon index changed | Use `grep -l xe /sys/class/hwmon/hwmon*/name` after reboot |

## References

- [CoolerControl FAQ — Unmanaged](https://docs.coolercontrol.org/wiki/faq.html)
- [CoolerControl Hardware Support](https://docs.coolercontrol.org/hardware-support.html)
- Intel patch series: [`reference/series-168027-fan-control.mbox`](../reference/series-168027-fan-control.mbox)
