# New-chat handoff prompt

Copy the text below into the new GPT-6 Astra discussion and attach the V1.0 repository ZIP plus the latest `hdmirx-latest.tar.gz` result archive.

---

I am continuing an RK3588 / Orange Pi 5 Plus ultra-low-latency HDMI passthrough project. Treat the attached V1.0 repository as the only current codebase. Read every file carefully before proposing changes. Do not reconstruct old V2/V3 patch chains or reintroduce retired experimental modes.

Hardware-verified architecture:

- Orange Pi 5 Plus / RK3588, Armbian kernel `6.1.115-vendor-rk35xx`
- HDMI-RX `/dev/video0`
- DRM `/dev/dri/card0`, connector `217`, plane `114`
- capture NV24 1920×1080
- direct V4L2 DMA-BUF → DRM/KMS native NV24 framebuffer
- Rockchip HDMI-RX `low_latency=1`
- acquire sync-file FD returned in V4L2 `timecode.userbits`
- plane `IN_FENCE_FD`, CRTC `OUT_FENCE_PTR`
- userspace closes acquire/output fence FDs correctly
- four capture buffers minimum; three starves
- EDID-advertised target mode 59.940201 Hz
- no GStreamer, CPU colour conversion, framebuffer copy, or deliberate extra queue

The ownership rule is critical: after an atomic update completes, QBUF only the previously displayed capture buffer. The newly submitted buffer remains held for scanout. Do not shorten or alter that rule without proving the DRM lifetime boundary.

Important measurements:

- previous OUT→DQ about 0.04–0.05 ms
- previous OUT→commit about 0.06–0.10 ms
- DQ→commit about 0.004–0.011 ms
- atomic ioctl about 0.055–0.060 ms
- commit return→OUT about 16.55–16.57 ms
- four-buffer reuse about 66.67 ms

Completed findings:

1. Atomic `DRM_MODE_PAGE_FLIP_ASYNC` is unsupported/rejected with `EINVAL` on this Rockchip 6.1 stack.
2. Exact 60.000 Hz output against 59.94 Hz input caused periodic phase slips. Selecting the monitor’s EDID-advertised 59.940201 Hz mode removed the practical cadence mismatch.
3. An overlapping atomic commit returned `EBUSY`. Intentionally dropping the ready capture frame caused a two-vblank completion and added latency; that approach is retired.
4. The corrected observation-only readiness profiler produced 7,121 post-warmup rows, zero sequence gaps, zero missing fences, and 7,061 genuine pre-latch readiness opportunities with zero post-latch races or drops.
5. Therefore a newer capture frame is usually available before the pending display update completes, but the current driver serializes the atomic commit. The likely next breakthrough requires kernel/driver work: pending-update replacement/queueing, Rockchip/VOP2 latch support, or real atomic async support—not another unsafe userspace drop.

Glass-to-glass caveat: prior slow-motion comparisons used an MSI monitor on the discrete GPU and a ZOWIE path sourced from the CPU/iGPU through the Orange Pi. These outputs are not genlocked. Do not assume their one-frame difference is a fixed baseline to subtract. The next visual control should drive both paths from the same GPU.

Working preferences:

- keep `main` stable and hardware-verifiable
- branch experiments from the `v1.0-hardware-verified` tag
- consolidate successful changes; do not create a patch-chain repository
- preserve the one-command Orange Pi workflow and automatic result archive
- ask for actual CSV/log archives before drawing timing conclusions
- clearly distinguish measured facts, inferences, and untested hypotheses

First task: audit the attached V1.0 source and latest result archive, confirm the baseline invariants, then propose a narrowly scoped next step for inspecting or modifying the exact Rockchip DRM/VOP2 kernel path that returns `EBUSY`. Do not change the stable userspace datapath until the evidence justifies it.

Repository URL: https://github.com/N0tiK44/rk3588-hdmi-lowlatency

---
