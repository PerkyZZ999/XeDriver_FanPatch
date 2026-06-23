# Publishing to GitHub

The repository is prepared for a public release. Follow these steps:

## 1. Create the GitHub repository

On GitHub, create a new **public** repository (suggested name: `XeDriver_FanPatch`).

Do **not** initialize with a README if you already have one locally.

## 2. Initialize and push

```bash
cd /path/to/XeDriver_FanPatch

git init
git add .
git status   # verify no build artifacts (*.ko, xe_pcode_probe binary, watch_data.json)
git commit -m "$(cat <<'EOF'
Initial public release: Intel Arc B580 Linux fan control research.

Documents PCODE mailbox probing, late-binding firmware analysis, and
tools for when upstream xe driver fan control is completed.
EOF
)"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/XeDriver_FanPatch.git
git push -u origin main
```

## 3. Update placeholder URLs

After pushing, replace `YOUR_USERNAME` in:

- `src/daemon/xe_fanctl.service` (`Documentation=` line)
- Any issue posts that reference the repo

## 4. Optional: GitHub release

Tag the initial research snapshot:

```bash
git tag -a v0.1.0-research -m "Research snapshot — fan control not yet working upstream"
git push origin v0.1.0-research
```

## What is excluded from git

See [`.gitignore`](.gitignore):

- Compiled binaries (`xe_pcode_probe`, `xe_fan_probe.ko`)
- Kernel module build artifacts
- `src/monitor/watch_data.json` (local monitoring history)

## Posting to Intel

1. Comment on [compute-runtime #885](https://github.com/intel/compute-runtime/issues/885) with your short intro (see issue draft in chat).
2. Follow up with the full technical write-up, linking your repo.
3. Consider cross-posting to [drm/xe GitLab](https://gitlab.freedesktop.org/drm/xe/kernel/-/issues) — late-binding provisioning is kernel-side work.

## Systemd install (optional)

```bash
sudo mkdir -p /opt/xe-fan-patch
sudo cp -a . /opt/xe-fan-patch/
# Edit INSTALL_ROOT in src/daemon/xe_fanctl.service if not using /opt/xe-fan-patch
sudo cp src/daemon/xe_fanctl.service /etc/systemd/system/
sudo systemctl daemon-reload
```

Build the userspace probe before enabling the daemon:

```bash
make -C /opt/xe-fan-patch/src/userspace
```
