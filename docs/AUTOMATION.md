# One-command debug workflow

This workflow removes the repetitive update, build, run, result selection and
archive steps. It does not change the zero-copy datapath or relax buffer/fence
ownership.

## Orange Pi

From any directory, run the existing checkout's bootstrap as the normal login
user:

```bash
bash ~/src/rk3588-hdmi-lowlatency/scripts/pi-debug.sh v38
```

Do **not** put `sudo` before this command. The script requests sudo only for the
hardware runner. It then:

1. refuses to continue if the checkout has local changes;
2. switches to `main` and performs `git pull --ff-only origin main`;
3. runs `make clean`, `make check`, and `make`;
4. runs the selected experiment with the proven four-buffer defaults;
5. captures the console, runner output and machine/build metadata;
6. creates a timestamped archive and refreshes
   `~/hdmirx-results/latest.tar.gz`.

The wrapper packages results even when an experimental kernel operation is
unsupported or the runner exits nonzero. A build/check failure prevents the
hardware test but is also recorded and packaged.

Supported selectors are `baseline`, `v35`, `v36`, `v37`, and `v38`. V3.8 is the
default, so this shorter command is equivalent:

```bash
bash ~/src/rk3588-hdmi-lowlatency/scripts/pi-debug.sh
```

Optional overrides remain available without editing files:

```bash
DURATION=300 TARGET_MILLIHZ=59940 \
  bash ~/src/rk3588-hdmi-lowlatency/scripts/pi-debug.sh v38
```

## Windows PC

From the extracted repository root in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\fetch-latest.ps1
```

This downloads the newest result to
`$HOME\Downloads\hdmirx-latest.tar.gz` and prints its size and SHA-256. The
defaults match the current bench (`visionseek@192.168.20.35`). Override them if
the DHCP address changes:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\fetch-latest.ps1 `
  -PiHost 192.168.20.40 -PiUser visionseek
```

The direct fallback, which does not require the helper, is:

```powershell
scp visionseek@192.168.20.35:~/hdmirx-results/latest.tar.gz "$HOME\Downloads\hdmirx-latest.tar.gz"
```

## Recovery

- `local changes` means the wrapper deliberately did not pull. Run
  `git status --short` and preserve or remove those changes consciously.
- A Git authentication prompt is not normally required for a public pull.
- If sudo authentication fails, rerun the same wrapper; completed Git/build
  steps are harmless to repeat.
- Every run has a unique directory and archive under `~/hdmirx-results/`; the
  stable `latest.tar.gz` name always refers to the newest completed package.
