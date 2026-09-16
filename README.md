# RK3588 HDMI ultra-low-latency passthrough

Version 1.0 is a clean release candidate consolidated from the hardware-verified Orange Pi 5 Plus datapath. It captures the RK3588 HDMI receiver as NV24 and scans the same DMA-BUFs out through DRM/KMS. There is no GStreamer pipeline, CPU colour conversion, framebuffer copy, or deliberate userspace frame queue. Tag it as hardware verified only after this exact V1.0 tree passes on the board.

## Verified test configuration

- Orange Pi 5 Plus, Armbian kernel `6.1.115-vendor-rk35xx`
- HDMI-RX: `/dev/video0`
- DRM: `/dev/dri/card0`
- connector `217`, plane `114`
- NV24, 1920×1080
- four V4L2 capture buffers
- EDID-advertised 59.940 Hz output mode
- Rockchip HDMI-RX `low_latency=1`
- Rockchip acquire sync-file from V4L2 `timecode.userbits`
- KMS plane `IN_FENCE_FD`; CRTC `OUT_FENCE_PTR`

DRM object IDs are installation-specific. The supplied scripts use the verified IDs above, while the binary also supports automatic selection with ID `0`.

## Install and build on the Orange Pi

```bash
sudo apt update
sudo apt install -y build-essential pkg-config libdrm-dev python3 git

cd ~/src/rk3588-hdmi-lowlatency
make clean
make check
make
```

`make check` performs C syntax/warning checks, Python compilation, and shell syntax checks. Hardware execution must happen on the RK3588 board.

## Run

Continuous passthrough until `Ctrl+C`:

```bash
cd ~/src/rk3588-hdmi-lowlatency
sudo bash scripts/run.sh
```

One safe 120-second diagnostic arm:

```bash
sudo bash scripts/diagnose.sh
```

The diagnostic retains the identical zero-copy/fence/ownership path. Its phase and readiness-window observations do not dequeue an extra capture buffer, overlap atomic commits, or drop a frame.

## One-command update, test, and package

Run as the normal user, not through `sudo`:

```bash
cd ~/src/rk3588-hdmi-lowlatency && bash scripts/pi-update-and-diagnose.sh
```

It refuses a dirty worktree, fast-forwards `main`, checks and builds the project, runs one diagnostic arm with hardware privileges, and creates `~/hdmirx-latest.tar.gz`.

Fetch that archive from Windows PowerShell with a command that works from any directory:

```powershell
scp visionseek@192.168.20.35:~/hdmirx-latest.tar.gz "$HOME\Downloads\hdmirx-latest.tar.gz"
```

The optional helper is usable only after cloning this repository on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\fetch-results.ps1
```

## Safety rules

- Four buffers are the stable minimum. Three causes cadence starvation.
- Never requeue the currently displayed capture buffer. V1.0 requeues the previous buffer only after the next KMS OUT fence completes.
- Userspace closes the V4L2 acquire-fence FD after the atomic commit ioctl returns and closes every KMS OUT-fence FD after waiting it.
- Do not add GStreamer, CPU conversion, framebuffer copies, or an extra frame queue to this path.
- Do not reintroduce the retired async/drop experiments. The Rockchip 6.1 driver rejected atomic async, and intentionally dropping a capture frame added latency.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Orange Pi and Git workflow](docs/OPERATIONS.md)
- [Consolidated research findings](docs/RESEARCH_FINDINGS.md)
- [Next engineering work](docs/ROADMAP.md)
- [Prompt for a new ChatGPT/Codex discussion](docs/HANDOFF_PROMPT.md)

## Versioning

After this exact tree passes on hardware, tag it:

```bash
git tag -a v1.0-hardware-verified -m "Hardware-verified RK3588 passthrough V1.0"
git push origin v1.0-hardware-verified
```

Future experiments should start on `experiment/<name>` branches from that tag. A failed experiment is documented and discarded; it is not left as another runtime mode on `main`.
