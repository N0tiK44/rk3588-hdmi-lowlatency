# Consolidated research findings

This document preserves the useful evidence from the historical V2/V3.x work. The old patch chain, version-specific runners, and failed runtime modes are intentionally not part of V1.0.

## Proven baseline

- NV24 1920×1080 V4L2 DMA-BUFs can be imported directly into the Rockchip DRM plane.
- Rockchip HDMI-RX low latency supplies valid acquire fences through V4L2 timecode userbits.
- Explicit `IN_FENCE_FD` and `OUT_FENCE_PTR` work on connector 217 / plane 114.
- Four capture buffers are the minimum stable pool. Three causes cadence starvation.
- Userspace submission is not the dominant latency: previous OUT→DQ and DQ→commit are tens of microseconds.

Representative stable measurements at approximately 59.94 Hz:

| Interval | Median |
|---|---:|
| previous OUT → DQ | 0.041–0.048 ms |
| previous OUT → commit | 0.058–0.101 ms |
| DQ → commit call | 0.004–0.011 ms |
| atomic commit ioctl | 0.055–0.060 ms |
| commit return → OUT | 16.55–16.57 ms |
| own OUT → QBUF | 16.63–16.72 ms |
| same-buffer DQ reuse, four buffers | 66.67–66.73 ms |

## Atomic async experiment

The verified Linux `6.1.115-vendor-rk35xx` stack did not advertise atomic asynchronous page flips. A constrained atomic async attempt was rejected with `EINVAL`. This is a driver/kernel support result, not a baseline failure. The legacy async capability is not equivalent to atomic async support.

V1.0 therefore does not expose an async runtime switch.

## Cadence alignment

Running 59.94 Hz capture against an exact 60.000 Hz output produced periodic cadence slips and two-vblank completions. Selecting the monitor’s EDID-advertised 59.940201 Hz mode removed the practical mismatch while retaining the same zero-copy datapath.

The final 120-second diagnostic measured approximately 59.939758 Hz input against 59.940201 Hz output, with no two-vblank completions.

## Early-submission experiment and correction

An attempted overlapping atomic submission returned `EBUSY`: the Rockchip/Linux 6.1 atomic pipeline serializes the pending update. Intentionally discarding the ready capture buffer was harmful; the next presented frame spanned roughly two vblanks and added visible latency.

The corrected observation-only profiler never dequeues or drops the candidate. In the final run it reported:

- 7,121 post-warmup rows
- zero raw, intentional, or unexpected sequence gaps
- zero missing acquire fences
- 7,061 capture-ready-before-OUT events
- 7,061 genuine pre-latch readiness opportunities
- zero post-latch fence races
- zero intentional drops

This supersedes the earlier interpretation that the readiness event occurred only after latch. A newer capture frame is genuinely ready before the pending display update completes, but userspace cannot submit it while the existing atomic commit is pending on this stack.

## What the evidence means

The remaining opportunity is below the current userspace scheduling path. Likely research directions are:

1. Rockchip/VOP2 support for replacing or queueing a pending atomic plane update.
2. Proper atomic async-flip support in a newer/backported DRM stack.
3. A driver-specific latch mechanism that can accept the newer framebuffer before the next vblank.

Micro-optimizing the current DQ/commit code cannot recover a full refresh interval; it already runs in roughly 0.1 ms or less.

## Glass-to-glass caution

The high-speed videos compared an MSI display driven by the discrete GPU with a ZOWIE display sourced from the host CPU/iGPU and passed through the Orange Pi. Those outputs are not genlocked and the displays have different internal processing. A recurring one-frame difference between them is therefore not a proven fixed host-side offset and must not simply be subtracted from every passthrough measurement.

A valid visual control uses the same GPU and duplicated signal path, preferably with identical displays or a splitter/test pattern, before and after inserting the Orange Pi. The internal timestamp evidence remains useful independently of that external control.
