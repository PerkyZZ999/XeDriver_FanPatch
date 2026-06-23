#!/bin/bash
# SPDX-License-Identifier: MIT
#
# setup_fancontrol.sh - Configure lm-sensors fancontrol for GPU fan via motherboard
#
# Detects the xe hwmon (GPU temperature source) and it8689 hwmon (PWM control),
# generates a /etc/fancontrol configuration that drives a motherboard PWM header
# from GPU temperature, and installs a systemd service override.
#
# Usage:
#   sudo ./setup_fancontrol.sh [pwm_channel]
#
#   pwm_channel  - which it8689 PWM channel to use (1-5). Default: 3
#
# Examples:
#   sudo ./setup_fancontrol.sh        # use pwm3 (SYS_FAN2)
#   sudo ./setup_fancontrol.sh 4      # use pwm4 (SYS_FAN3)
#

set -euo pipefail

HWMON_BASE="/sys/class/hwmon"
FANCONTROL_CONF="/etc/fancontrol"
SYSTEMD_OVERRIDE_DIR="/etc/systemd/system/fancontrol.service.d"
SYSTEMD_OVERRIDE="${SYSTEMD_OVERRIDE_DIR}/override.conf"

PWM_CHANNEL="${1:-3}"

# --- Fan curve (GPU temp → PWM duty) -----------------------------------------
# Mirrors the xe_fanctl daemon curve and CoolerControl profile.
#   30°C →   0% (  0/255)   40°C →  31% ( 80/255)
#   50°C →  47% (120/255)   60°C →  63% (160/255)
#   70°C →  78% (200/255)   80°C → 100% (255/255)
MINTEMP=30
MAXTEMP=80
MINPWM=0
MAXPWM=255
MINSTART=80
MINSTOP=0
INTERVAL=2

err()  { printf "\033[31mERROR: %s\033[0m\n" "$*" >&2; }
ok()   { printf "\033[32m[OK]\033[0m %s\n" "$*"; }
info() { printf "\033[34m[i]\033[0m %s\n" "$*"; }
warn() { printf "\033[33m[!]\033[0m %s\n" "$*"; }

# --- pre-flight checks -------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    err "Must run as root (need to write /etc/fancontrol and systemd unit)."
    exit 1
fi

if ! [[ "$PWM_CHANNEL" =~ ^[1-5]$ ]]; then
    err "PWM channel must be 1-5, got '$PWM_CHANNEL'."
    exit 1
fi

if ! command -v fancontrol >/dev/null 2>&1; then
    err "fancontrol not found. Install lm-sensors: sudo pacman -S lm_sensors  (or apt install lm-sensors)"
    exit 1
fi

# --- detect xe hwmon (temperature source) ------------------------------------
info "Detecting xe hwmon device (GPU temperature source)..."
xe_dev=""
for hwmon in "$HWMON_BASE"/hwmon*; do
    [ -r "$hwmon/name" ] || continue
    if [ "$(cat "$hwmon/name")" = "xe" ]; then
        xe_dev="$(basename "$hwmon")"
        break
    fi
done

if [ -z "$xe_dev" ]; then
    err "xe hwmon not found. Is the xe kernel module loaded? (lsmod | grep xe)"
    exit 1
fi

# find a usable temp input on xe
xe_temp=""
for f in "$HWMON_BASE/$xe_dev"/temp*_input; do
    [ -e "$f" ] || continue
    xe_temp="$(basename "$f")"
    break
done
if [ -z "$xe_temp" ]; then
    err "No temp*_input found under $xe_dev. Cannot use GPU as temperature source."
    exit 1
fi
ok "xe hwmon: $xe_dev, temperature sensor: $xe_temp"

# --- detect it8689 hwmon (PWM control) ---------------------------------------
info "Detecting it8689 hwmon device (PWM control)..."
it_dev=""
for hwmon in "$HWMON_BASE"/hwmon*; do
    [ -r "$hwmon/name" ] || continue
    if [ "$(cat "$hwmon/name")" = "it8689" ]; then
        it_dev="$(basename "$hwmon")"
        break
    fi
done

if [ -z "$it_dev" ]; then
    err "it8689 hwmon not found. Check sensors-detect output and it87 module."
    exit 1
fi
ok "it8689 hwmon: $it_dev"

# --- verify the PWM channel exists and is writable ---------------------------
info "Verifying PWM channel pwm$PWM_CHANNEL on $it_dev..."
pwm_path="$HWMON_BASE/$it_dev/pwm$PWM_CHANNEL"
enable_path="$HWMON_BASE/$it_dev/pwm${PWM_CHANNEL}_enable"
fan_path="$HWMON_BASE/$it_dev/fan${PWM_CHANNEL}_input"

if [ ! -e "$pwm_path" ]; then
    err "PWM control $pwm_path does not exist."
    exit 1
fi
if [ ! -w "$pwm_path" ]; then
    err "PWM control $pwm_path is not writable (run as root, check it87 module)."
    exit 1
fi
ok "pwm$PWM_CHANNEL exists and is writable"

# --- discover DEVPATH for fancontrol (relative device paths) -----------------
info "Resolving DEVPATH entries..."
xe_devpath=$(readlink -f "$HWMON_BASE/$xe_dev/device" 2>/dev/null | sed 's#^/sys/##' || true)
it_devpath=$(readlink -f "$HWMON_BASE/$it_dev/device" 2>/dev/null | sed 's#^/sys/##' || true)

# fancontrol wants paths relative to /sys/devices/...
xe_devpath="${xe_devpath#devices/}"
it_devpath="${it_devpath#devices/}"
if [ -z "$xe_devpath" ]; then xe_devpath="pci0000:00"; fi
if [ -z "$it_devpath" ]; then it_devpath="isa"; fi
ok "DEVPATH: $xe_dev=$xe_devpath  $it_dev=$it_devpath"

# --- safety: set PWM to manual before writing config -------------------------
info "Setting pwm$PWM_CHANNEL to manual mode (enable=1) for safety..."
if [ -e "$enable_path" ] && [ -w "$enable_path" ]; then
    echo 1 > "$enable_path" 2>/dev/null && ok "pwm${PWM_CHANNEL}_enable = 1 (manual)" || warn "Could not set enable mode."
else
    warn "pwm${PWM_CHANNEL}_enable not writable; fancontrol will manage it."
fi

# --- generate fancontrol configuration ---------------------------------------
info "Generating $FANCONTROL_CONF..."

# Back up existing config
if [ -f "$FANCONTROL_CONF" ]; then
    cp "$FANCONTROL_CONF" "${FANCONTROL_CONF}.bak.$(date +%s)"
    warn "Backed up existing config to ${FANCONTROL_CONF}.bak.*"
fi

cat > "$FANCONTROL_CONF" <<EOF
# Generated by setup_fancontrol.sh — GPU fan via motherboard it8689 PWM
# Temperature source: Intel Arc B580 ($xe_dev / $xe_temp)
# PWM control:        it8689 ($it_dev / pwm$PWM_CHANNEL)
# Curve:  30C->0%  40C->31%  50C->47%  60C->63%  70C->78%  80C->100%

INTERVAL=$INTERVAL

DEVPATH=$xe_dev=$xe_devpath $it_dev=$it_devpath
DEVNAME=$xe_dev=xe $it_dev=it8689

FCTEMPS=$it_dev/pwm$PWM_CHANNEL=$xe_dev/$xe_temp
FCFANS=$it_dev/pwm$PWM_CHANNEL=$it_dev/fan${PWM_CHANNEL}_input

MINTEMP=$it_dev/pwm$PWM_CHANNEL=$MINTEMP
MAXTEMP=$it_dev/pwm$PWM_CHANNEL=$MAXTEMP
MINSTART=$it_dev/pwm$PWM_CHANNEL=$MINSTART
MINSTOP=$it_dev/pwm$PWM_CHANNEL=$MINSTOP
MINPWM=$it_dev/pwm$PWM_CHANNEL=$MINPWM
MAXPWM=$it_dev/pwm$PWM_CHANNEL=$MAXPWM
EOF

chmod 644 "$FANCONTROL_CONF"
ok "fancontrol config written"

# --- validate with fancontrol --test ----------------------------------------
info "Validating configuration..."
if fancontrol --test "$FANCONTROL_CONF" >/tmp/fancontrol_test.log 2>&1; then
    ok "fancontrol --test passed"
else
    warn "fancontrol --test reported issues (see /tmp/fancontrol_test.log)."
    warn "This is common when the target fan reads 0 RPM before connecting the GPU fan."
fi

# --- systemd service override ------------------------------------------------
info "Installing systemd override for fancontrol.service..."
mkdir -p "$SYSTEMD_OVERRIDE_DIR"

cat > "$SYSTEMD_OVERRIDE" <<EOF
# Override for fancontrol when driving GPU fan via motherboard PWM.
# Ensures fancontrol starts after the xe and it87 modules are available
# and restarts gracefully if the GPU appears late.

[Unit]
After=lm_sensors.service
Wants=lm_sensors.service

[Service]
# fancontrol config path
Environment="FANCONTROL_CONF=$FANCONTROL_CONF"
# Restart on failure so a transient hwmon rename doesn't kill the service
Restart=on-failure
RestartSec=5
# Hardening
ProtectSystem=full
ReadWritePaths=/etc/fancontrol /sys/class/hwmon
EOF

ok "Systemd override installed: $SYSTEMD_OVERRIDE"

# --- reload and enable -------------------------------------------------------
info "Reloading systemd daemon and enabling fancontrol..."
systemctl daemon-reload
if systemctl enable fancontrol.service 2>/dev/null; then
    ok "fancontrol.service enabled"
else
    warn "Could not enable fancontrol.service (may need: systemctl enable --now fancontrol)"
fi

if systemctl restart fancontrol.service 2>/dev/null; then
    ok "fancontrol.service started"
else
    warn "fancontrol.service failed to start — check: journalctl -u fancontrol -e"
fi

# --- summary -----------------------------------------------------------------
printf "\n\033[1;36m══════════════════════════════════════════════════════\033[0m\n"
printf "\033[1;36m  SETUP COMPLETE\033[0m\n"
printf "\033[1;36m══════════════════════════════════════════════════════\033[0m\n"
printf "  Config:        %s\n" "$FANCONTROL_CONF"
printf "  Temp source:   %s/%s\n" "$xe_dev" "$xe_temp"
printf "  PWM control:   %s/pwm%s\n" "$it_dev" "$PWM_CHANNEL"
printf "  Fan monitor:   %s/fan%s_input\n" "$it_dev" "$PWM_CHANNEL"
printf "  Curve:         %dC→0%% .. %dC→100%%\n" "$MINTEMP" "$MAXTEMP"
printf "\n"
printf "  Verify:  systemctl status fancontrol\n"
printf "           watch -n1 cat %s/fan%s_input   # GPU fan RPM\n" "$HWMON_BASE/$it_dev" "$PWM_CHANNEL"
printf "           cat %s/pwm%s                    # current duty\n" "$HWMON_BASE/$it_dev" "$PWM_CHANNEL"
printf "\n"
printf "  Edit curve:  sudo nano %s\n" "$FANCONTROL_CONF"
printf "  Re-test:     sudo fancontrol --test %s\n" "$FANCONTROL_CONF"
printf "\n"
