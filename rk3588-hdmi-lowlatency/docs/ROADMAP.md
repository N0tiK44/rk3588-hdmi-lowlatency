# Roadmap

## V3.5 — current: async page-flip A/B test

Goal: determine whether RK3588 DRM/VOP2 accepts `DRM_MODE_PAGE_FLIP_ASYNC` on this atomic plane path and whether it materially reduces the approximately one-refresh `commit → OUT` delay.

Success criteria:

1. Native NV24 DMA-BUF zero-copy remains intact.
2. Rockchip acquire fences are still consumed through `IN_FENCE_FD` with no FD leak.
3. CRTC `OUT_FENCE_PTR` remains the display-side lifetime guard.
4. Four-buffer cadence remains stable.
5. Async materially reduces `commit return → OUT` versus the normal run.
6. No new full-frame userspace queue is introduced.

If async is rejected by the driver, record that as the V3.5 result and move on; do not hide the failure behind a copy/conversion path.

## V3.6 — phase / refresh control

If async does not bypass the display phase:

- measure exact input vs output phase over long traces,
- capture real CRTC timing and refresh precisely,
- test small output refresh offsets around source cadence,
- determine the phase window needed to land an update before the next latch point,
- distinguish unavoidable scanout time from avoidable one-period waits.

## High-refresh validation

Once the cadence architecture is solved, validate progressively:

- 1080p120
- 1080p144
- 1080p165
- **1080p240**

At 240 Hz, one frame period is about **4.167 ms**, so all stage timings should be reported both in milliseconds and as fractions of the display period.

## Later engineering

- robust startup and signal relock
- HDMI input mode changes
- EDID strategy
- connector/plane discovery instead of fixed development IDs
- watchdog/recovery
- service/daemon packaging
- optical high-speed-camera glass-to-glass validation

## Version-control rule

`main` should always be runnable. Tag every proven baseline with an annotated Git tag. Start one hypothesis per `experiment/*` branch from the latest proven tag. Merge only after the A/B result is captured in `docs/results/`; then tag the merged state. Do not resurrect the historical `apply-vXX.py` patch-chain workflow.
