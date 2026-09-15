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

For each dequeued frame:

1. `VIDIOC_DQBUF` returns a capture buffer.
2. The Rockchip acquire-fence FD is extracted from `timecode.userbits`.
3. If the selected plane exposes `IN_FENCE_FD`, that FD is supplied with the atomic plane update. If not, userspace waits it before commit as a fallback.
4. The commit also supplies a userspace pointer through CRTC `OUT_FENCE_PTR`.
5. `drmModeAtomicCommit()` is called with `DRM_MODE_ATOMIC_NONBLOCK`; V3.5 optionally ORs `DRM_MODE_PAGE_FLIP_ASYNC`.
6. After the commit ioctl returns, userspace closes the input fence FD. KMS has consumed its reference as part of the property submission.
7. Userspace waits for the returned KMS output fence, then closes that FD.
8. Only after display completion is observed does userspace QBUF the *previously displayed* capture buffer back to HDMI-RX.

`OUT_FENCE_PTR` is mandatory in this build because capture memory must not be returned to HDMI-RX while the display engine may still scan it.

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

## V3.4 instrumentation retained in V3.5

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

## V3.5 async experiment

`--async-flip` changes only the atomic commit flags:

```text
normal: DRM_MODE_ATOMIC_NONBLOCK
async:  DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_ASYNC
```

The DMA-BUF path, acquire fences, output fence, buffer count, framebuffer layout and ownership behavior remain otherwise identical. That makes the test useful as an A/B measurement of KMS display-phase behavior.
