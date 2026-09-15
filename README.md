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
- current experiment: **V3.5 `DRM_MODE_PAGE_FLIP_ASYNC` A/B test**
- long-term target: **1080p240**

Connector/plane IDs are machine-specific. The defaults above are the proven test platform values.

## Build

On the Orange Pi:

```bash
sudo apt update
sudo apt install -y build-essential pkg-config libdrm-dev python3
make clean
make
```

Optional local checks:

```bash
make check
```

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

The experiment is capability-gated with `DRM_CAP_ASYNC_PAGE_FLIP`. Linux 6.1 rejects async flags on atomic commits, and a newer kernel still needs explicit async support in the DRM driver/plane. Many RK3588 Rockchip/VOP2 kernels are therefore expected to report this test as unsupported. The runner preserves both logs and the normal trace, then exits nonzero so an unsupported async result cannot be mistaken for success.

Results go to `/tmp/hdmirx-v35/`.

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

After extracting this repository and verifying the baseline on the board:

```bash
git init
git add .
git commit -m "Consolidated RK3588 HDMI-RX baseline"
git branch -M main
git tag -a v3.4-known-good -m "Known-good four-buffer explicit-sync baseline"
```

Create each risky experiment on a branch from that tag:

```bash
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
- `docs/results/v3.3-buffer-sweep.md` — buffer-count conclusion
- `docs/results/v3.4-buffer-lifetime.md` — measured phase/ownership result

## License

No license has been selected. Add an explicit license before distributing this as reusable third-party open-source software.
