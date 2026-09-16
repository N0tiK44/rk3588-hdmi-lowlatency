# Operations and repository workflow

## First clone

```bash
mkdir -p ~/src
cd ~/src
git clone https://github.com/N0tiK44/rk3588-hdmi-lowlatency.git
cd rk3588-hdmi-lowlatency
make clean && make check && make
```

Do not clone the repository inside another copy of itself. The correct path is `~/src/rk3588-hdmi-lowlatency`, with `README.md` and `Makefile` directly inside it.

## Routine board command

```bash
cd ~/src/rk3588-hdmi-lowlatency && bash scripts/pi-update-and-diagnose.sh
```

The script stops rather than destroying local edits. Resolve or commit intentional work before retrying. It uses `git pull --ff-only`, so an unexpected divergent branch cannot be silently merged.

## Fetch results to Windows

The final dot in `scp ... .` means “the current PowerShell directory.” To make the destination unambiguous, use:

```powershell
scp visionseek@192.168.20.35:~/hdmirx-latest.tar.gz "$HOME\Downloads\hdmirx-latest.tar.gz"
```

Then verify it:

```powershell
Get-Item "$HOME\Downloads\hdmirx-latest.tar.gz"
```

## Stable release and experiments

Tag a hardware-confirmed baseline once:

```bash
git switch main
git pull --ff-only
git tag -a v1.0-hardware-verified -m "Hardware-verified RK3588 passthrough V1.0"
git push origin v1.0-hardware-verified
```

Start future work without destabilizing `main`:

```bash
git switch -c experiment/pending-atomic-replacement v1.0-hardware-verified
```

If it fails, retain the result in `docs/RESEARCH_FINDINGS.md`, then delete the disposable branch:

```bash
git switch main
git branch -D experiment/pending-atomic-replacement
```

Never tag code as hardware verified before the board run passes.
