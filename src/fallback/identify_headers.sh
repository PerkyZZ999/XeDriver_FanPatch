#!/bin/bash
# SPDX-License-Identifier: MIT
#
# identify_headers.sh - Scan motherboard PWM/fan headers for GPU fan redirect
#
# Lists every hwmon device, shows PWM channels with current values, enable modes,
# associated fan RPM, and auto-point temperatures. Highlights the it8689 Super I/O
# chip (hwmon3 on this system) and suggests non-CPU headers suitable for a GPU fan.
#
# Usage: sudo ./identify_headers.sh
#

set -euo pipefail

HWMON_BASE="/sys/class/hwmon"

# --- ANSI color helpers ------------------------------------------------------
if [ -t 1 ]; then
    C_RESET="\033[0m";   C_BOLD="\033[1m";   C_RED="\033[31m"
    C_GREEN="\033[32m";  C_YELLOW="\033[33m"; C_BLUE="\033[34m"
    C_MAGENTA="\033[35m";C_CYAN="\033[36m";   C_WHITE="\033[37m"
    C_BG_YELLOW="\033[43m"; C_BG_CYAN="\033[46m"
else
    C_RESET=""; C_BOLD=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""
    C_MAGENTA=""; C_CYAN=""; C_WHITE=""; C_BG_YELLOW=""; C_BG_CYAN=""
fi

header() {
    printf "\n${C_BOLD}${C_CYAN}════════════════════════════════════════════════════════════════\n"
    printf "%s\n" "$1"
    printf "════════════════════════════════════════════════════════════════${C_RESET}\n"
}

subheader() {
    printf "\n${C_BOLD}${C_BLUE}── %s ──${C_RESET}\n" "$1"
}

label()  { printf "  ${C_YELLOW}%-22s${C_RESET}" "$1"; }
val()    { printf "${C_GREEN}%s${C_RESET}" "$1"; }
valr()   { printf "${C_RED}%s${C_RESET}" "$1"; }
warn()   { printf "${C_YELLOW}%s${C_RESET}" "$1"; }
hilite() { printf "${C_BOLD}${C_MAGENTA}%s${C_RESET}" "$1"; }

# it87 pwm_enable meanings (applies to it8689)
enable_mode_str() {
    case "$1" in
        0) echo "0 = full-on (no control)" ;;
        1) echo "1 = manual PWM" ;;
        2) echo "2 = auto (SmartFan)" ;;
        3) echo "3 = closed-loop" ;;
        *) echo "$1 = unknown" ;;
    esac
}

read_file() {
    [ -r "$1" ] && cat "$1" 2>/dev/null || echo "N/A"
}

# --- pre-flight --------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    printf "${C_YELLOW}Note: not running as root — some sysfs files may be unreadable.${C_RESET}\n"
fi

if [ ! -d "$HWMON_BASE" ]; then
    printf "${C_RED}ERROR: %s not found. Is this a Linux system with hwmon support?${C_RESET}\n" "$HWMON_BASE"
    exit 1
fi

# --- 1. Summary of all hwmon devices ----------------------------------------
header "1. HWMON DEVICE SUMMARY"

printf "\n%-10s %-18s %-10s %s\n" "DEVICE" "NAME" "PWM CHs" "FAN CHs"
printf "%-10s %-18s %-10s %s\n" "------" "----" "-------" "-------"

declare -A HWMON_NAMES
HWMON_DIRS=()
for hwmon in "$HWMON_BASE"/hwmon*; do
    [ -d "$hwmon" ] || continue
    name=$(read_file "$hwmon/name")
    dev=$(basename "$hwmon")
    HWMON_NAMES["$dev"]="$name"
    HWMON_DIRS+=("$dev")

    pwm_chs=""
    for f in "$hwmon"/pwm[0-9]; do
        [ -e "$f" ] || continue
        pwm_chs+="$(basename "$f") "
    done
    [ -z "$pwm_chs" ] && pwm_chs="(none)"

    fan_chs=""
    for f in "$hwmon"/fan[0-9]_input; do
        [ -e "$f" ] || continue
        fan_chs+="$(basename "$f" | sed 's/_input//') "
    done
    [ -z "$fan_chs" ] && fan_chs="(none)"

    marker=""
    [ "$name" = "it8689" ] && marker=" ${C_MAGENTA}<-- Super I/O target${C_RESET}"
    [ "$name" = "xe" ]    && marker=" ${C_CYAN}<-- Intel Arc GPU${C_RESET}"

    printf "%-10s %-18s %-10s %s%s\n" "$dev" "$name" "$pwm_chs" "$fan_chs" "$marker"
done

# --- 2. xe GPU hwmon (temperature source) -----------------------------------
header "2. INTEL ARC GPU (xe) HWMON — TEMPERATURE SOURCE"

xe_dev=""
for dev in "${HWMON_DIRS[@]}"; do
    if [ "${HWMON_NAMES[$dev]}" = "xe" ]; then
        xe_dev="$dev"
        break
    fi
done

if [ -n "$xe_dev" ]; then
    hilite "Found xe hwmon: $xe_dev\n\n"
    for f in "$HWMON_BASE/$xe_dev"/temp[0-9]_input; do
        [ -e "$f" ] || continue
        t=$(cat "$f" 2>/dev/null || echo "N/A")
        if [ "$t" != "N/A" ]; then
            tc=$((t / 1000))
            label "$(basename "$f")"; val "${tc}°C (raw ${t})"; printf "\n"
        fi
    done
    for f in "$HWMON_BASE/$xe_dev"/fan[0-9]_input; do
        [ -e "$f" ] || continue
        rpm=$(cat "$f" 2>/dev/null || echo "N/A")
        label "$(basename "$f")"; val "${rpm} RPM"; printf "\n"
    done
    pwm_check=""
    for f in "$HWMON_BASE/$xe_dev"/pwm[0-9]; do
        [ -e "$f" ] && pwm_check="writable pwm found: $(basename "$f")"
    done
    if [ -z "$pwm_check" ]; then
        printf "  ${C_RED}No writable PWM control exposed by xe driver (firmware-gated).${C_RESET}\n"
        printf "  %s\n" "$(warn '→ This is why the physical motherboard redirect fallback exists.')"
    fi
else
    printf "  ${C_RED}xe hwmon not found. Is the xe module loaded?${C_RESET}\n"
fi

# --- 3. it8689 detailed PWM breakdown ----------------------------------------
header "3. IT8689 SUPER I/O — PWM CONTROL DETAIL"

it_dev=""
for dev in "${HWMON_DIRS[@]}"; do
    if [ "${HWMON_NAMES[$dev]}" = "it8689" ]; then
        it_dev="$dev"
        break
    fi
done

if [ -z "$it_dev" ]; then
    printf "  ${C_RED}it8689 hwmon not found on this system.${C_RESET}\n"
    printf "  ${C_YELLOW}Showing PWM detail for all hwmon devices with pwm controls instead:${C_RESET}\n"
    target_devs=("${HWMON_DIRS[@]}")
else
    hilite "Found it8689 hwmon: $it_dev (pwm1-pwm5 available)\n"
    target_devs=("$it_dev")
fi

for dev in "${target_devs[@]}"; do
    name="${HWMON_NAMES[$dev]}"
    [ "$name" = "it8689" ] || subheader "$dev ($name) — PWM channels"

    for f in "$HWMON_BASE/$dev"/pwm[0-9]; do
        [ -e "$f" ] || continue
        ch=$(basename "$f")
        n=${ch#pwm}

        printf "\n  ${C_BOLD}${C_CYAN}┌─ %s/%s ─────────────────────────${C_RESET}\n" "$dev" "$ch"

        pwm_val=$(read_file "$f")
        enable=$(read_file "$HWMON_BASE/$dev/${ch}_enable")
        mode=$(read_file "$HWMON_BASE/$dev/${ch}_mode" 2>/dev/null || echo "N/A")

        label "pwm value";      [ "$pwm_val" != "N/A" ] && val "$pwm_val/255 ($((pwm_val * 100 / 255))%)" || valr "N/A"; printf "\n"
        label "enable mode";    val "$(enable_mode_str "$enable")"; printf "\n"
        [ "$mode" != "N/A" ] && { label "mode"; val "$mode"; printf "\n"; }

        # Associated fan RPM
        fan_file="$HWMON_BASE/$dev/fan${n}_input"
        if [ -e "$fan_file" ]; then
            rpm=$(cat "$fan_file" 2>/dev/null || echo "N/A")
            label "fan${n}_input"
            if [ "$rpm" = "0" ] || [ "$rpm" = "N/A" ]; then
                valr "$rpm RPM ${C_YELLOW}(header unused — good GPU candidate)${C_RESET}"
            else
                val "$rpm RPM"
            fi
            printf "\n"
        else
            label "fan${n}_input"; valr "missing"; printf "\n"
        fi

        # Auto-point temperatures and PWM (SmartFan curve)
        ap_found=0
        for ap in "$HWMON_BASE/$dev"/${ch}_auto_point_temp_*; do
            [ -e "$ap" ] && ap_found=1 && break
        done

        if [ "$ap_found" -eq 1 ]; then
            printf "  ${C_BLUE}│  Auto-point curve (SmartFan):${C_RESET}\n"
            printf "  ${C_BLUE}│  %-6s %-12s %-12s${C_RESET}\n" "pt" "temp(°C)" "pwm(%)"
            idx=1
            while true; do
                tf="$HWMON_BASE/$dev/${ch}_auto_point_temp_${idx}"
                pf="$HWMON_BASE/$dev/${ch}_auto_point_pwm_${idx}"
                [ -e "$tf" ] || [ -e "$pf" ] || break
                tv=$(read_file "$tf")
                pv=$(read_file "$pf")
                if [ "$tv" != "N/A" ] && [ "$pv" != "N/A" ]; then
                    printf "  ${C_BLUE}│  %-6s %-12s %-12s${C_RESET}\n" "$idx" "$tv" "$((pv * 100 / 255))%"
                fi
                idx=$((idx + 1))
            done
        fi

        printf "  ${C_BOLD}${C_CYAN}└──────────────────────────────────────────${C_RESET}\n"
    done
done

# --- 4. Suggestions ----------------------------------------------------------
header "4. GPU FAN HEADER SUGGESTIONS"

if [ -n "$it_dev" ]; then
    printf "\n%s\n" "$(warn 'Goal: route the GPU fan to a motherboard header that is NOT the CPU fan.')"
    printf "%s\n\n" "$(warn 'Best candidates = headers whose fan input reads 0 RPM (currently unused).')"

    printf "  %-10s %-10s %-12s %-20s %s\n" "DEVICE" "CHANNEL" "FAN RPM" "ENABLE" "RECOMMENDATION"
    printf "  %-10s %-10s %-12s %-20s %s\n" "------" "-------" "-------" "------" "--------------"

    for f in "$HWMON_BASE/$it_dev"/pwm[0-9]; do
        [ -e "$f" ] || continue
        ch=$(basename "$f")
        n=${ch#pwm}
        pwm_val=$(read_file "$f")
        enable=$(read_file "$HWMON_BASE/$it_dev/${ch}_enable")

        fan_file="$HWMON_BASE/$it_dev/fan${n}_input"
        rpm="N/A"
        [ -e "$fan_file" ] && rpm=$(cat "$fan_file" 2>/dev/null || echo "N/A")

        rec=""
        if [ "$n" = "1" ]; then
            rec="${C_RED}AVOID — CPU_FAN header${C_RESET}"
        elif [ "$rpm" = "0" ] || [ "$rpm" = "N/A" ]; then
            rec="${C_GREEN}GOOD — unused header (GIGABYTE SYS_FAN$n)${C_RESET}"
        else
            rec="${C_YELLOW}OK — in use, check if chassis fan${C_RESET}"
        fi

        printf "  %-10s %-10s %-12s %-20s %b\n" "$it_dev" "$ch" "${rpm} RPM" "$(enable_mode_str "$enable")" "$rec"
    done

    printf "\n${C_BOLD}${C_GREEN}Recommendation:${C_RESET}\n"
    printf "  Use a SYS_FAN header (pwm2-pwm5) that currently shows 0 RPM.\n"
    printf "  On the GIGABYTE B550M Gaming X WiFi 6, typical mapping:\n"
    printf "    pwm1 → CPU_FAN     (AVOID)\n"
    printf "    pwm2 → SYS_FAN1    (front intake)\n"
    printf "    pwm3 → SYS_FAN2    (often unused — ${C_GREEN}recommended${C_RESET})\n"
    printf "    pwm4 → SYS_FAN3\n"
    printf "    pwm5 → SYS_FAN4 / PUMP\n"
    printf "\n  Run identify_headers.sh again AFTER plugging the GPU fan into a\n"
    printf "  candidate header to confirm fan${C_BOLD}N${C_RESET}_input reads non-zero RPM.\n"
else
    printf "  ${C_RED}No it8689 device detected. Identify your Super I/O PWM headers manually.${C_RESET}\n"
fi

# --- 5. Next steps -----------------------------------------------------------
header "5. NEXT STEPS"
printf "  1. Power down, connect GPU fan to chosen SYS_FAN header via adapter.\n"
printf "  2. Boot, re-run this script to confirm RPM appears on the new header.\n"
printf "  3. Run ${C_BOLD}setup_fancontrol.sh${C_RESET} to configure fancontrol.\n"
printf "  4. Or import ${C_BOLD}coolercontrol-profile.json${C_RESET} into CoolerControl.\n"
printf "  5. See ${C_BOLD}HARDWARE_GUIDE.md${C_RESET} for full physical redirect instructions.\n"

printf "\n${C_BOLD}Done.${C_RESET}\n"
