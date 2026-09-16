# V3.8.1 safe-window profiler

## Run

After the updated repository is on GitHub, perform the one-time Pi update:

```bash
cd ~/src/rk3588-hdmi-lowlatency
git switch main
git pull --ff-only
bash scripts/pi-debug.sh v381
```

Later runs need only:

```bash
bash ~/src/rk3588-hdmi-lowlatency/scripts/pi-debug.sh v381
```

V3.8.1 is one uninterrupted 120-second arm, not two arms. Record both monitors
for the whole run. There is no intentional transition point because the probe
does not modify the stream.

## Required result

```text
missing Rockchip fences: 0
V4L2 sequence gaps: 0 raw, 0 intentional, 0 unexpected
V3.8.1 intentional drops: 0
runtime status: completed
```

Interpret the counters as follows:

- `genuine pre-latch ready`: capture became ready while the CRTC was still in
  the commit's original vblank interval;
- `post-latch fence races`: physical vblank already advanced even though the
  OUT fence had not yet become readable;
- `OUT before/equal ready`: the normal and unambiguous ordering.

Retrieve the archive from any PowerShell directory with:

```powershell
scp visionseek@192.168.20.35:~/hdmirx-results/latest.tar.gz "$HOME\Downloads\hdmirx-latest.tar.gz"
```
