# V3.6 GitHub and Orange Pi runbook

## Publish with the GitHub website

1. Download and extract the V3.6 repository ZIP on the desktop.
2. Open the GitHub repository and select the `main` branch.
3. Choose **Add file → Upload files**.
4. Drag the **contents inside** the extracted `rk3588-hdmi-lowlatency-v3.6` directory onto the page. Do not upload the ZIP itself and do not drag a parent directory that creates another nested `rk3588-hdmi-lowlatency` folder.
5. Confirm that GitHub shows updates under `src/`, `scripts/`, `tools/`, `docs/`, plus the root files.
6. Commit directly to `main` with: `Release V3.6 phase profiler`.

The `.gitignore` is included. A GitHub web upload does not need the Orange Pi's credentials.

## Update the existing Orange Pi checkout

Pulls from a public repository do not require GitHub authentication:

```bash
cd ~/src/rk3588-hdmi-lowlatency
git switch main
git status --short
git pull --ff-only
git log -1 --oneline
```

Stop if `git status --short` prints local changes. Do not delete them blindly. The `git log` line should show the V3.6 upload commit after the pull.

## Build and verify the installed files

```bash
make clean
make check
make

test -f scripts/run-v36-phase-profiler.sh
./hdmirx-kms-lowlat --help 2>&1 | grep -- --phase-profile
```

The last command must print the `--phase-profile` option. If it does not, the old source or old binary is still present.

## Protect the baseline, then run V3.6

First confirm that the normal path still works:

```bash
sudo bash ./scripts/run-baseline.sh
```

Then run the 120-second phase profiler while recording both displays at 240 fps:

```bash
sudo bash ./scripts/run-v36-phase-profiler.sh
```

Successful completion prints a report and creates:

```text
/tmp/hdmirx-v36/phase-b4.csv
/tmp/hdmirx-v36/phase-b4.log
/tmp/hdmirx-v36/report.txt
```

Verify the essentials:

```bash
grep -E 'runtime status|phase profiler samples|missing Rockchip fences' /tmp/hdmirx-v36/phase-b4.log
sed -n '1,160p' /tmp/hdmirx-v36/report.txt
```

Expected: `runtime status: completed`, zero missing fences, no sequence gaps, and thousands of valid phase rows. If the runner exits nonzero, send `phase-b4.log`, `report.txt`, the terminal error, and the camera clip; do not rerun with fewer than four buffers.

The explicit `bash` form is intentional: GitHub's browser uploader may not preserve the executable bit on a newly added script. It runs the same checked-in file without requiring another chmod/commit cycle.

## Mark the hardware-verified state

Only after both runs pass:

```bash
git tag -a v3.6-phase-profiler -m "Hardware-verified V3.6 phase profiler"
git push origin v3.6-phase-profiler
```

Pushing a tag requires the already configured `gh`/Git credential. Pulling and testing do not.
