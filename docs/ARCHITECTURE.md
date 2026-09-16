# Architecture

## Datapath

```text
/dev/video0 (NV24 multi-planar capture)
  -> V4L2 MMAP buffers exported as DMA-BUF FDs
  -> DRM PRIME import and native NV24 framebuffer
  -> plane 114 on connector 217
  -> /dev/dri/card0 HDMI-TX
```

The pixel payload remains in DMA-BUF-backed memory. The CPU performs control ioctls and timing measurements, not per-pixel work.

## Synchronization and ownership

With Rockchip HDMI-RX low latency enabled, the capture driver returns an acquire sync-file FD encoded in `v4l2_buffer.timecode.userbits`. The program validates and passes that FD to the KMS plane through `IN_FENCE_FD`. After `drmModeAtomicCommit()` returns, userspace closes its copy of the acquire FD; DRM has consumed or referenced it.

Every normal atomic update requests a display completion sync-file through CRTC `OUT_FENCE_PTR`. Userspace waits and closes that FD before returning the previously displayed capture buffer with `VIDIOC_QBUF`. The newly submitted capture buffer remains owned by scanout until a later update completes.

Four capture buffers produce a stable 66.7 ms per-index reuse cadence at 59.94 Hz. A three-buffer pool starves this ownership pipeline.

## Atomic state

Each update programs the selected plane with native NV24 framebuffer ID, CRTC ID, source/destination rectangles, and `IN_FENCE_FD`. The first update may also apply an EDID-advertised mode through connector `CRTC_ID` and CRTC `MODE_ID`/`ACTIVE`, using `DRM_MODE_ATOMIC_ALLOW_MODESET`. Later updates use `DRM_MODE_ATOMIC_NONBLOCK`.

V1.0 does not request `DRM_MODE_PAGE_FLIP_ASYNC`. That atomic feature is absent/rejected on the verified Rockchip 6.1 kernel.

## Diagnostics

The CSV always records DQ, commit begin/end, OUT-fence signal, and QBUF timestamps. `--phase-profile` adds CRTC sequence/timestamp observations at DQ, commit, and completion. `--window-profile` polls capture readiness against the current OUT fence and classifies the ordering with the CRTC sequence.

The readiness profiler is observation-only: it does not DQ the candidate buffer, submit another atomic request, or discard a frame. It requires `--phase-profile` so classification can be audited.

## Deliberate exclusions

- no GStreamer
- no CPU YUV/RGB conversion
- no framebuffer memcpy
- no hidden fallback queue
- no legacy page-flip path
- no early QBUF based on guessed scanout lifetime
- no intentional capture drops

These are correctness constraints, not merely performance preferences.
