# Architecture

## Datapath

The low-latency path is intentionally direct:

```text
RK3588 HDMI receiver
    │
    │ V4L2 multi-planar capture, native NV24
    ▼
/dev/video0
    │
    │ VIDIOC_EXPBUF
    ▼
DMA-BUF fd
    │
    │ drmPrimeFDToHandle + drmModeAddFB2(DRM_FORMAT_NV24)
    ▼
DRM framebuffer
    │
    │ atomic plane update
    ▼
VOP2 / KMS plane → HDMI TX / display
```

There is no GStreamer pipeline in the current implementation, no `videoconvert`, no CPU copy into a second framebuffer, and no application-level frame queue.

## Capture format and layout

The program requires the HDMI-RX node to report V4L2 `NV24` as one memory plane. The same exported DMA-BUF is imported into DRM and described as two NV24 image planes using the source buffer's linear layout:

- Y offset: `0`, pitch: `bytesperline`
- UV offset: `bytesperline * height`, pitch: `bytesperline * 2`

The program refuses scaling and requires the active CRTC mode to match the capture dimensions.

## Explicit synchronization

Rockchip's HDMI-RX driver exposes a sync-file FD in `v4l2_buffer.timecode.userbits`. The C path copies those bytes into a signed `int` and treats negative/`0xffffffff` as absent.

For each dequeued frame on the known-good normal path:

1. `VIDIOC_DQBUF` returns a capture buffer.
2. The Rockchip acquire-fence FD is extracted from `timecode.userbits`.
3. The selected plane's `IN_FENCE_FD` property receives that FD. This property and a valid acquire fence are mandatory when Rockchip `low_latency` is enabled.
4. The commit also supplies a userspace pointer through CRTC `OUT_FENCE_PTR`.
5. `drmModeAtomicCommit()` is called with `DRM_MODE_ATOMIC_NONBLOCK`.
6. After the commit ioctl returns, userspace closes the input fence FD. KMS has consumed its reference as part of the property submission.
7. Userspace waits for the returned KMS output fence, then closes that FD.
8. Only after display completion is observed does userspace QBUF the *previously displayed* capture buffer back to HDMI-RX.

`OUT_FENCE_PTR` is mandatory in this build because the baseline must not return capture memory to HDMI-RX while the display engine may still scan it.

## Buffer ownership

Four capture buffers are the known stable minimum on the characterized ~59.94/60 Hz configuration. Three buffers caused cadence starvation. The default is therefore four; the CLI still permits 3–8 for controlled investigation.

The lifecycle is:

```text
HDMI-RX owns/queues buffer
        ↓
DQBUF: userspace/KMS path owns buffer
        ↓
atomic commit + OUT fence wait
        ↓
new frame becomes displayed
        ↓
previous displayed buffer QBUF
        ↓
HDMI-RX may capture into it again
```

This deliberately avoids requeueing a buffer that KMS may still be reading.

## V3.4 instrumentation retained in V3.6

Each sample records:

- V4L2 sequence and V4L2 timestamp
- buffer index
- whether the Rockchip acquire fence was present
- `dq_ns`
- `commit_begin_ns`
- `commit_end_ns`
- `out_signal_ns`
- `qbuf_ns` for the frame when its capture buffer is returned

The CSV additionally includes precomputed microsecond deltas. `tools/analyze.py` derives inter-frame cadence, previous-OUT phase, own-OUT-to-QBUF, and same-buffer reuse.

Early V3 measurement packages could emit literal `\\n` sequences into CSV output. The consolidated C writer emits normal physical newlines; the analyzer still repairs old traces for compatibility.

## V3.6 phase instrumentation

With `--phase-profile`, the normal atomic path also samples `drmCrtcGetSequence()` immediately after DQ, immediately before commit, and after output-fence completion. The raw CRTC sequence/timestamp values, active mode period, and V4L2 buffer timestamp flags are written to the CSV. DRM monotonic vblank timestamps are required so all events share the `CLOCK_MONOTONIC` domain.

These extra ioctls are diagnostic overhead and V3.6 does not claim they reduce latency. The analyzer uses them to estimate each event's phase after the most recent vblank, time until the predicted next vblank, vblank sequence advances, timestamp sawtooth wraps and multi-vblank completions.

## V3.7 temporary advertised-mode change

V3.7 optionally selects a 1920×1080 connector mode within 0.005 Hz of the requested millihertz value. The candidate must come directly from the connector's EDID mode list. The program creates atomic mode blobs, includes connector `CRTC_ID` plus CRTC `MODE_ID`/`ACTIVE` in the first normal NV24 commit, and uses `DRM_MODE_ATOMIC_ALLOW_MODESET`.

## V3.8 negative result and V3.8.1 correction

V3.8 showed that sync-file readability is not the physical latch boundary. A
capture-ready event arrived after the CRTC sequence advanced but before the OUT
fence became readable. Treating that as a pre-latch opportunity led to an
`EBUSY`, an intentional dropped capture, and a two-vblank next completion.

V3.8.1 retains the two-fd poll only as an observation. On capture-ready-first,
it calls `drmCrtcGetSequence()` and compares the result with the sequence taken
immediately before the current commit. It does not call `VIDIOC_DQBUF` inside
the probe. The buffer remains queued until the current OUT fence completes and
the ordinary next loop iteration consumes it. Consequently the profiler cannot
change capture order, KMS ownership, buffer cadence, or displayed latency.

The capture format, plane, DMA-BUF objects, acquire fences, output fences and four-buffer ownership rule remain unchanged. When the selected timing differs from the startup timing, cleanup disables the experiment plane and atomically restores the original CRTC mode before framebuffer removal. Failure to find, apply or restore the advertised mode is a failed experiment; there is no synthetic-timing fallback.

## V3.5 async experiment

Atomic async flips are more restricted than normal atomic commits. In particular, the request must be a pure framebuffer flip; changing `OUT_FENCE_PTR`, `CRTC_ID`, or geometry makes it an unsupported state change. The consolidated test therefore:

1. queries `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP` (and logs the distinct legacy `DRM_CAP_ASYNC_PAGE_FLIP` value),
2. performs one normal full-state seed commit with `OUT_FENCE_PTR`,
3. submits subsequent frames with only `FB_ID` and `IN_FENCE_FD`,
4. uses `DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT`, and
5. waits for the DRM page-flip event before returning the previous capture buffer.

The DMA-BUF layout, acquire-fence handoff, four-buffer pool, zero-copy behavior and ownership boundary remain comparable. Only the completion primitive differs: normal rows measure output-fence readiness; async rows measure page-flip event delivery. The CSV's `async_commit` column distinguishes the normal seed from true async rows, and the analyzer excludes the seed from async statistics.

Kernel/driver support is not assumed. Linux 6.1 neither exposes the atomic-async capability nor accepts the async flag for atomic commits. The V3.5 diagnostic deliberately makes one attempt after warning when the capability is absent, so a vendor backport can still be discovered and the exact rejection can be captured. Newer DRM core code also requires the driver to advertise and implement async flips; upstream Rockchip VOP2 commonly does not. A clear nonzero “unsupported” result is therefore expected on many RK3588 images and does not invalidate V3.4.
