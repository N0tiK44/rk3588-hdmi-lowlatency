# RK3588 HDMI-RX → KMS ultra-low-latency passthrough

A consolidated experimental HDMI passthrough for the **RK3588 / Orange Pi 5 Plus**. This repository contains one current C implementation rather than the historical V2/V3 patch chain.

```text
HDMI input
  ↓
Rockchip HDMI-RX / V4L2  (/dev/video0, NV24)
  ↓  DMA-BUF + Rockchip sync-file acquire fence
DRM/KMS atomic plane     (/dev/dri/card0)
  ↓
HDMI display scanout
```

The active path has **no GStreamer, no CPU colour conversion, no framebuffer copy, and no deliberate userspace frame queue**.

## Current known-good configuration

- HDMI-RX: `/dev/video0`
- DRM: `/dev/dri/card0`
- development connector: `217`
- development plane: `114`
- capture/scanout: `NV24`, `1920×1080`
- zero-copy: V4L2 buffer → exported DMA-BUF → DRM PRIME import → KMS framebuffer
- Rockchip HDMI-RX `low_latency`: enabled for a run and restored on exit
- capture acquire fence: read from `v4l2_buffer.timecode.userbits`
- baseline explicit sync: plane `IN_FENCE_FD` + CRTC `OUT_FENCE_PTR`
- stable capture pool: **4 buffers minimum** at the characterized ~60 Hz mode
- V3.5 result: **atomic async rejected as unsupported by the Rockchip 6.1 driver**
- current experiment: **V3.6 non-invasive CRTC/vblank phase profiler**
- long-term target: **1080p240**

Connector/plane IDs are machine-specific. The defaults above are the proven test platform values.

## First checkout and build

On the Orange Pi (Armbian/Ubuntu package names):

```bash
sudo apt update
sudo apt install -y build-essential pkg-config libdrm-dev python3

mkdir -p ~/src
cd ~/src
git clone https://github.com/N0tiK44/rk3588-hdmi-lowlatency.git
cd rk3588-hdmi-lowlatency

make clean
make check
make
```

Do not clone a second copy inside the repository. For every later GitHub update, use the existing checkout:

```bash
cd ~/src/rk3588-hdmi-lowlatency
git switch main
git pull --ff-only
git status --short
make clean
make check
make
```

`git status --short` should print nothing before a routine pull. If it lists files, preserve or commit those local changes before updating instead of overwriting them.

## Baseline run

The baseline keeps the normal nonblocking atomic path and uses the known-good four-buffer pool:

```bash
sudo ./scripts/run-baseline.sh
```

Defaults can be overridden without editing files:

```bash
sudo env DURATION=60 BUFFERS=4 CONNECTOR=217 PLANE=114 ./scripts/run-baseline.sh
```

Results go to `/tmp/hdmirx-baseline/`.

## V3.5 normal-vs-async test

```bash
sudo ./scripts/run-v35-phase.sh
```

This performs two zero-copy runs:

1. normal atomic commits with `IN_FENCE_FD` and `OUT_FENCE_PTR`
2. one normal seed commit, then minimal `FB_ID` + `IN_FENCE_FD` commits with `DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT`

Atomic async flips cannot change `OUT_FENCE_PTR` or repeat unrelated plane state, so the async run uses the page-flip event as its completion/lifetime signal. The CSV labels the seed row and each async row. Tearing is acceptable: V3.5 is a diagnostic test of display-phase bypass, not a presentation-quality mode.

The experiment queries `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP`, which is distinct from the legacy `DRM_CAP_ASYNC_PAGE_FLIP` capability. Linux 6.1 does not expose the atomic capability and rejects async flags on atomic commits. Because this is a diagnostic, the program logs both capabilities and still makes one real async atomic attempt, allowing vendor backports to be detected. A newer kernel also needs explicit async support in the DRM driver/plane. Many RK3588 Rockchip/VOP2 kernels are therefore expected to reject the attempt; the runner preserves both logs and the normal trace, then exits nonzero so an unsupported result cannot be mistaken for success.

Results go to `/tmp/hdmirx-v35/`.

## V3.6 phase profiler

V3.6 keeps the proven normal atomic path. It adds timestamp-only diagnostics around capture dequeue, atomic commit and output-fence completion. No refresh offset, modeset, copy, conversion or extra queue is introduced in this version.

```bash
sudo bash ./scripts/run-v36-phase-profiler.sh
```

The default 120-second trace records the active mode period, V4L2 timestamp flags, and `drmCrtcGetSequence()` sequence/timestamp samples. The analyzer reports capture-to-display phase, predicted time to the next vblank, vblank advances, and phase drift. Results go to `/tmp/hdmirx-v36/`.

Use the high-speed camera during the run. Keep both Windows outputs on the same duplicated image and record which connector/GPU drives each display. The direct MSI-to-Zowie control measured about 16 ms; the path through the Orange Pi measured about 33 ms, so the current working estimate is approximately one additional 16.7 ms display period inside the passthrough path.

## Analyze an existing trace

```bash
python3 tools/analyze.py /tmp/hdmirx-baseline/baseline-b4.csv
```

Compare normal and async traces:

```bash
python3 tools/analyze.py \
  /tmp/hdmirx-v35/sync-b4.csv \
  /tmp/hdmirx-v35/async-b4.csv
```

## Important ownership rule

The input fence FD extracted from the Rockchip V4L2 extension is passed to KMS through `IN_FENCE_FD` and then closed by userspace after the atomic commit ioctl returns. On the normal path, the output fence FD returned through `OUT_FENCE_PTR` is waited and closed before the previously displayed capture buffer is QBUF'd back to HDMI-RX. On the async experiment, the corresponding page-flip event is awaited before that requeue. Do not remove this ownership discipline merely to shorten code.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the exact lifetime.

## Known measured V3.4 result

At 1920×1080 ~60 Hz, the characterized medians were approximately:

| Measurement | Median |
|---|---:|
| previous OUT → DQ | 0.041 ms |
| previous OUT → commit | 0.101 ms |
| DQ → commit return | 0.060 ms |
| commit return → OUT | 16.565 ms |
| own OUT → QBUF | 16.625 ms |

The remaining dominant latency is therefore display/vblank phase and buffer ownership, not the userspace submission path.

## Safe Git workflow for experiments

The GitHub checkout is already a Git repository; do not run `git init` again. Mark a hardware-verified baseline once, then create each risky experiment from that tag:

```bash
git tag -a v3.4-known-good -m "Known-good four-buffer explicit-sync baseline"
git push origin v3.4-known-good
git switch -c experiment/v3.5-async v3.4-known-good
```

Commit small, testable changes. To abandon an experiment safely:

```bash
git switch main
git branch -D experiment/v3.5-async
```

To reproduce the exact known-good baseline at any time:

```bash
git switch --detach v3.4-known-good
```

When an experiment is proven, merge it to `main` and create a new annotated tag (for example `v3.6-phase-control`). Never use patch-application scripts as the versioning system.

## Documentation

- `docs/ARCHITECTURE.md` — datapath, explicit sync, ownership and instrumentation
- `docs/ROADMAP.md` — V3.5 onward and 1080p240 target
- `docs/DEVELOPMENT_HISTORY.md` — consolidated history and historical-package issues
- `docs/VERSIONING.md` — rollback-safe tags and experiment branches
- `docs/V36_RUNBOOK.md` — GitHub upload, Pi update, build and verification commands
- `docs/results/v3.3-buffer-sweep.md` — buffer-count conclusion
- `docs/results/v3.4-buffer-lifetime.md` — measured phase/ownership result
- `docs/results/v3.5-async-result.md` — Linux 6.1 atomic-async rejection
- `docs/results/v3.6-phase-profiler.md` — test procedure and interpretation

## License

No license has been selected. Add an explicit license before distributing this as reusable third-party open-source software.
