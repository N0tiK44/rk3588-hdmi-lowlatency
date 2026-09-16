# V3.7 GitHub and Orange Pi runbook

## Publish

Extract the V3.7 ZIP, upload the contents inside its top-level directory to the GitHub `main` branch, and commit as `Release V3.7 cadence alignment`. Update `.gitignore` through GitHub's pencil editor if the browser hides dotfiles. Do not upload the ZIP itself or create a nested repository directory.

## Update and build

```bash
cd ~/src/rk3588-hdmi-lowlatency
git switch main
git status --short
git pull --ff-only
git log -1 --oneline

make clean
make check
make

./hdmirx-kms-lowlat --help 2>&1 | grep -- --target-refresh-millihz
```

Stop if `git status --short` prints local changes. The final command must display the V3.7 option.

## Protect the proven path

```bash
sudo bash ./scripts/run-baseline.sh
```

Do not start the modeset experiment unless the baseline completes normally.

## Run V3.7

```bash
sudo bash ./scripts/run-v37-cadence-alignment.sh
```

Expect two 120-second arms and a brief resynchronization/black flash when the target timing is applied and restored. Do not interrupt power during the mode transition. `Ctrl+C` is supported and cleanup still attempts restoration.

Results are written beneath `/tmp/hdmirx-v37/`:

```text
reference-b4.csv
reference-b4.log
aligned-59940mhz-b4.csv
aligned-59940mhz-b4.log
report.txt
```

Verify restoration and runtime status:

```bash
grep -E 'target timing applied|Restored original DRM mode|runtime status' /tmp/hdmirx-v37/*.log
sed -n '1,220p' /tmp/hdmirx-v37/report.txt
```

If the aligned arm is unsupported, the report and aligned log explain whether the EDID lacked 59.94 Hz or the driver rejected the atomic modeset. The runner does not force an unadvertised timing.

## Emergency display recovery

The program restores the startup mode on normal and error exits. If the terminal nevertheless remains unsynchronized, switch to another virtual terminal or reboot normally from SSH:

```bash
sudo reboot
```

Do not power-cycle while filesystem writes are active.

## Package results

```bash
tar -czf "$HOME/hdmirx-v37-results.tar.gz" -C /tmp/hdmirx-v37 \
  reference-b4.csv reference-b4.log \
  aligned-59940mhz-b4.csv aligned-59940mhz-b4.log report.txt
```

Retrieve it from Windows PowerShell:

```powershell
scp visionseek@192.168.20.35:~/hdmirx-v37-results.tar.gz "$HOME\Downloads\hdmirx-v37-results.tar.gz"
```
