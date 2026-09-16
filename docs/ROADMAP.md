# Roadmap

## 1. Verify and freeze V1.0

- Run `scripts/pi-update-and-diagnose.sh` on the Orange Pi.
- Confirm zero missing fences, zero sequence gaps, stable 59.940 Hz cadence, and normal completion.
- Repeat a visual control with both host outputs driven by the same GPU.
- Tag the exact passing commit `v1.0-hardware-verified`.

## 2. Inspect kernel/driver feasibility

Before another userspace experiment, inspect the exact Rockchip DRM/VOP2 sources used by `6.1.115-vendor-rk35xx`:

- where pending atomic commits are serialized and return `EBUSY`
- whether plane update replacement before latch is structurally possible
- which `atomic_async_check` / `atomic_async_update` hooks are absent
- whether a newer Rockchip or upstream kernel changes these paths

Do this on a separate branch and retain V1.0 as the rollback point.

## 3. EDID transparency

Implement and verify downstream EDID forwarding so the Windows source sees the ZOWIE monitor’s real modes rather than a generic Rockchip identity. EDID forwarding improves source/display negotiation but does not itself bypass the capture/scanout frame boundary.

## 4. 1080p240 target

The ZOWIE EDID advertises approximately 239–240 Hz. At 240 Hz, one refresh is about 4.17 ms, but the entire pipeline must sustain the bandwidth, pixel clock, capture mode, DMA-BUF format, DRM mode, fences, and plane updates. Validate each stage independently before claiming end-to-end 240 Hz.

## Non-goals

- hiding failures with GStreamer or copying
- unsafe early QBUF
- intentional frame dropping as a latency strategy
- accumulating version-specific patch scripts on `main`
