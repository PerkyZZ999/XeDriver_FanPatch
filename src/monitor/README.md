# xe_fan_watch - Intel Arc B580 Fan Control Monitor

Monitors 6 sources for fan control updates and notifies you when they're available.

## Quick Start

### One-time check (CLI)
```bash
python3 xe_fan_watch.py --source all --notify
```

### Web Dashboard
```bash
python3 xe_fan_dashboard.py --check
```
Then open http://localhost:8585 in your browser.

### Continuous monitoring (daemon)
```bash
python3 xe_fan_watch.py --daemon --notify --interval 21600  # Every 6 hours
```

### Systemd timer (runs every 6 hours automatically)

```bash
# Set INSTALL_ROOT to your clone path, then install unit files
export INSTALL_ROOT=/opt/xe-fan-patch
sed "s|INSTALL_ROOT=/opt/xe-fan-patch|INSTALL_ROOT=$INSTALL_ROOT|g" \
  xe_fan_watch.service > /tmp/xe_fan_watch.service
sed "s|INSTALL_ROOT=/opt/xe-fan-patch|INSTALL_ROOT=$INSTALL_ROOT|g" \
  xe_fan_dashboard.service > /tmp/xe_fan_dashboard.service
sudo cp /tmp/xe_fan_watch.service /tmp/xe_fan_watch.timer \
  /tmp/xe_fan_dashboard.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now xe_fan_watch.timer
sudo systemctl enable --now xe_fan_dashboard.service
```

## Monitored Sources

| Source | What it checks | URL |
|---|---|---|
| **Local Sysfs** | Firmware version, fan RPM, GPU temp | Auto-detected B580 PCI BDF + xe hwmon |
| **CachyOS Packages** | Kernel package updates | `pacman -Qu linux-cachyos` |
| **drm-xe GitLab** | Fan/pwm/hwmon/late_bind MRs | `gitlab.freedesktop.org/drm/xe/kernel` |
| **intel-xe Mailing List** | Fan control patches | `lists.freedesktop.org/archives/intel-xe/` |
| **Phoronix** | Intel xe fan control articles | `phoronix.com/rss.php` |
| **GitHub compute-runtime** | Issue #885 fan control status | `github.com/intel/compute-runtime/issues/885` |

## Notifications

With `--notify`, sends desktop notifications via `notify-send` when:
- Firmware gets provisioned (critical urgency)
- New GitLab MRs found
- Kernel update available
- GitHub issue #885 gets closed
- New relevant Phoronix articles

## Dashboard Features

- Dark-themed web UI (no external dependencies, pure Python stdlib)
- Real-time local GPU status (firmware, fan RPM, temperature)
- Status cards for each monitored source
- Direct links to relevant MRs, issues, and articles
- Auto-refresh every 60 seconds
- JSON API at `/api/json`
- Manual check trigger at `/api/check`
- Background checking with `--background-check` flag

## Files

| File | Purpose |
|---|---|
| `xe_fan_watch.py` | Monitor script (CLI + daemon mode) |
| `xe_fan_dashboard.py` | Web dashboard |
| `xe_fan_watch.service` | Systemd service (one-shot check) |
| `xe_fan_watch.timer` | Systemd timer (every 6 hours) |
| `xe_fan_dashboard.service` | Systemd service (web dashboard) |
| `watch_data.json` | Stored check results (auto-generated) |

## No Dependencies Required

Both scripts use only Python standard library + `requests` (already installed on your system). No pip install needed.

## What to Do When Firmware IS Provisioned

When the dashboard shows "PROVISIONED" or `lb_fan_control_version` shows a real version:

1. Run the probe tool:
   ```bash
   sudo ./src/userspace/xe_pcode_probe --all
   ```

2. Test fan control:
   ```bash
   sudo ./src/userspace/xe_pcode_probe --interactive
   # Try: W 7d 1 0 80  (set fan to ~50%)
   ```

3. If it works, load the kernel module and use the daemon:
   ```bash
   sudo insmod src/kernel/xe_fan_probe.ko
   echo 0x1 | sudo tee /sys/kernel/xe_fan/write_subcmd
   sudo src/daemon/xe_fanctl
   ```
