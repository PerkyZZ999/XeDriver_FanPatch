# Publishing to GitHub

The repository is prepared for public release. Follow these steps:

## 1. Create the GitHub repository

On GitHub, create a new **public** repository (suggested name: `XeDriver_FanPatch`).

Do **not** initialize with a README if you already have one locally.

## 2. Initialize and push

```bash
cd /path/to/XeDriver_FanPatch

git add .
git status   # verify no build artifacts (*.ko, xe_pcode_probe binary, watch_data.json)
git commit -m "$(cat <<'EOF'
Document working B580 fan control via series 168027 custom kernel.

Adds CachyOS-adapted patch, build/rollback guides, CoolerControl setup,
fail-safe notes, and July 2026 verification results.
EOF
)"
git branch -M main
git remote add origin https://github.com/PerkyZZ999/XeDriver_FanPatch.git
git push -u origin main
```

## 3. GitHub release tag

Tag the working-fan-control milestone:

```bash
git tag -a v0.2.0-xefan-working -m "Fan control verified — series 168027 on linux-cachyos-xefan"
git push origin v0.2.0-xefan-working
```

Previous research-only tag (if published): `v0.1.0-research`

## What is excluded from git

See [`.gitignore`](.gitignore):

- Compiled binaries (`xe_pcode_probe`, `xe_fan_probe.ko`)
- Kernel module build artifacts
- `src/monitor/watch_data.json` (local monitoring history)
- Local kernel build trees (`/mnt/data-z/kernel-build/` — build on your machine)

## Posting to Intel / community

1. Comment on [compute-runtime #885](https://github.com/intel/compute-runtime/issues/885) — template in [`docs/GITHUB_ISSUE_COMMENT.md`](docs/GITHUB_ISSUE_COMMENT.md)
2. Link repo: https://github.com/PerkyZZ999/XeDriver_FanPatch
3. Optional: cross-post to [drm/xe GitLab issues](https://gitlab.freedesktop.org/drm/xe/kernel/-/issues)

## Key docs for new visitors

| Doc | Audience |
|-----|----------|
| [`README.md`](README.md) | Overview and quick links |
| [`docs/CUSTOM_KERNEL.md`](docs/CUSTOM_KERNEL.md) | Build and install patched kernel |
| [`docs/COOLERCONTROL.md`](docs/COOLERCONTROL.md) | Daily fan management |
| [`docs/TESTING_RESULTS.md`](docs/TESTING_RESULTS.md) | Proof it works |

## Systemd install (optional — legacy daemon)

The `xe_fanctl` daemon is **not required** when using CoolerControl + patched kernel. If you still want it:

```bash
sudo mkdir -p /opt/xe-fan-patch
sudo cp -a . /opt/xe-fan-patch/
sudo cp src/daemon/xe_fanctl.service /etc/systemd/system/
sudo systemctl daemon-reload
make -C /opt/xe-fan-patch/src/userspace
```

CoolerControl is the recommended control path on the xefan kernel.
