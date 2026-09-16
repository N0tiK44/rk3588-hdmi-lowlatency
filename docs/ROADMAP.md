# Roadmap

## V3.5 — completed: async page-flip A/B test

Goal: determine whether RK3588 DRM/VOP2 accepts `DRM_MODE_PAGE_FLIP_ASYNC` on this atomic plane path and whether it materially reduces the approximately one-refresh `commit → OUT` delay.

Success criteria:

1. Native NV24 DMA-BUF zero-copy remains intact.
2. Rockchip acquire fences are still consumed through `IN_FENCE_FD` with no FD leak.
3. The normal run retains `OUT_FENCE_PTR`; the async run waits its page-flip event as the lifetime guard required by the async UAPI.
4. Four-buffer cadence remains stable.
5. Async materially reduces `commit return → completion event` versus normal `commit return → OUT`.
6. No new full-frame userspace queue is introduced.

If async is rejected by the driver, record that as the V3.5 result and move on; do not hide the failure behind a copy/conversion path.

Hardware result: Linux 6.1.115-vendor-rk35xx did not advertise atomic async page flips and rejected the diagnostic atomic async commit with `EINVAL`. The normal trace remained healthy. This is an unsupported-feature result, not a regression.

## V3.6 — completed: phase profiler

V3.6 measures before changing timing:

- measure exact input vs output phase over long traces,
- capture real CRTC timing and refresh precisely,
- correlate DQ, commit and OUT with CRTC vblank sequence/timestamps,
- record V4L2 timestamp clock/source flags,
- quantify phase drift over a 120-second run,
- determine the phase window needed to land an update before the next latch point,
- distinguish unavoidable scanout time from avoidable one-period waits.

Refresh-offset experiments belong in V3.7 only after V3.6 establishes the direction and rate of drift.

Hardware result: the 59.94 Hz HDMI-RX stream ran against an exact 60.000 Hz CRTC. Eight frames took two vblank intervals, spaced approximately 980 frames apart, while capture sequence and fence integrity remained perfect.

## V3.7 — completed: advertised 59.94 Hz alignment

Run a controlled A/B comparison between the existing active timing and an EDID-advertised 59.94 Hz output mode. The test must restore the original mode, retain the normal explicit-sync path, and refuse synthetic timings.

Primary success criterion: eliminate or materially reduce the periodic two-vblank events. The normal one-vblank completion delay is expected to remain and will be addressed separately after cadence stability is proven.

## V3.8 — current: controlled early submission

Keep the verified advertised 59.940 Hz output timing and test whether a capture
buffer can be submitted while the preceding atomic update is still pending.
Make exactly one overlapping nonblocking commit after warm-up. Record kernel
acceptance, `EBUSY`, or another rejection separately.

On rejection, safely wait the candidate acquire fence and perform one annotated
phase-prime drop. Compare the remainder of the arm against an aligned reference
to learn whether shifting capture ownership phase can reduce the normal
one-vblank completion wait. No pixel copy, conversion, extra queue, synthetic
mode, or unguarded QBUF is permitted.

V3.8.x is reserved for narrow follow-ups based on the hardware result. V3.9 is
the EDID-forwarding/identity stage after the latency behavior is characterized.

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
