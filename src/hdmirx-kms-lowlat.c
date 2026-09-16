#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#ifndef DRM_MODE_PAGE_FLIP_ASYNC
#define DRM_MODE_PAGE_FLIP_ASYNC 0x02
#endif

/* Added after Linux 6.1; keep the diagnostic build source-compatible with
 * older libdrm header packages while still querying the correct atomic cap. */
#ifndef DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15
#endif

#ifndef DRM_FORMAT_NV24
#define DRM_FORMAT_NV24 fourcc_code('N','V','2','4')
#endif

#define MAX_BUFS 8
#define MAX_SAMPLES 65536

/*
 * CONSOLIDATED V3.7 CADENCE-ALIGNMENT BUILD
 *
 * Working V2 zero-copy path
 * + V3 measurement instrumentation
 * + V3.4 buffer-lifetime instrumentation
 * + V3.5 async page-flip A/B experiment
 * + V3.6 CRTC/vblank phase instrumentation
 * + V3.7 advertised-mode 59.94 Hz alignment experiment
 *
 * Current proven architecture:
 *
 *   RK3588 HDMI-RX
 *        ↓
 *   V4L2 NV24
 *        ↓
 *   DMA-BUF export
 *        ↓
 *   DRM PRIME import
 *        ↓
 *   native NV24 DRM framebuffer
 *        ↓
 *   atomic KMS plane
 *
 * No GStreamer.
 * No CPU colour conversion.
 * No framebuffer copy.
 * No deliberate userspace frame queue.
 */

struct frame_sample {
    uint64_t frame_no;
    uint32_t sequence;
    uint32_t index;
    int fence_present;
    int async_commit;
    int early_submit;
    int overlap_commit;
    uint32_t intentional_drops_before;

    uint64_t v4l2_ts_ns;
    uint64_t dq_ns;
    uint64_t commit_begin_ns;
    uint64_t commit_end_ns;
    uint64_t out_signal_ns;
    uint64_t qbuf_ns;

    uint32_t v4l2_flags;
    int phase_valid;
    uint64_t mode_period_ns;
    uint64_t dq_vblank_sequence;
    uint64_t dq_vblank_ns;
    uint64_t commit_vblank_sequence;
    uint64_t commit_vblank_ns;
    uint64_t out_vblank_sequence;
    uint64_t out_vblank_ns;
};

static struct frame_sample g_samples[MAX_SAMPLES];
static size_t g_sample_count;

static volatile sig_atomic_t g_stop = 0;


/* ------------------------------------------------------------------------- */
/* V3.4 buffer-lifetime instrumentation                                      */
/* ------------------------------------------------------------------------- */

static void v34_mark_qbuf(uint32_t index, uint64_t when_ns)
{
    for (size_t n = g_sample_count; n > 0; n--) {
        struct frame_sample* x = &g_samples[n - 1];

        if (x->index == index && x->qbuf_ns == 0) {
            x->qbuf_ns = when_ns;
            return;
        }
    }
}


/* ------------------------------------------------------------------------- */
/* Timing helpers                                                            */
/* ------------------------------------------------------------------------- */

static uint64_t v3_mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL +
        (uint64_t)ts.tv_nsec;
}


static uint64_t v3_timeval_ns(const struct timeval* tv)
{
    return (uint64_t)tv->tv_sec * 1000000000ULL +
        (uint64_t)tv->tv_usec * 1000ULL;
}


static uint64_t v36_mode_period_ns(const drmModeModeInfo* mode)
{
    if (!mode ||
        !mode->clock ||
        !mode->htotal ||
        !mode->vtotal) {

        return 0;
    }

    uint64_t numerator_hz =
        (uint64_t)mode->clock * 1000ULL;

    uint64_t denominator =
        (uint64_t)mode->htotal *
        (uint64_t)mode->vtotal;

    if (mode->flags & DRM_MODE_FLAG_INTERLACE)
        numerator_hz *= 2ULL;

    if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
        denominator *= 2ULL;

    if (mode->vscan > 1)
        denominator *= mode->vscan;

    return (
        denominator * 1000000000ULL +
        numerator_hz / 2ULL
    ) / numerator_hz;
}


static uint32_t mode_refresh_millihz(const drmModeModeInfo* mode)
{
    uint64_t period_ns = v36_mode_period_ns(mode);

    if (!period_ns)
        return 0;

    return (uint32_t)(
        (1000000000000ULL + period_ns / 2ULL) /
        period_ns
    );
}


static const drmModeModeInfo* pick_refresh_mode(
    const drmModeConnector* conn,
    uint32_t width,
    uint32_t height,
    uint32_t target_millihz
)
{
    const drmModeModeInfo* best = NULL;
    uint32_t best_delta = UINT32_MAX;

    fprintf(
        stderr,
        "Advertised %ux%u modes for V3.7:\n",
        width,
        height
    );

    for (int i = 0; i < conn->count_modes; i++) {
        const drmModeModeInfo* mode = &conn->modes[i];

        if (mode->hdisplay != width || mode->vdisplay != height)
            continue;

        uint32_t refresh = mode_refresh_millihz(mode);

        fprintf(
            stderr,
            "  [%d] %s clock=%u kHz htotal=%u vtotal=%u "
            "refresh=%u.%03u Hz%s\n",
            i,
            mode->name,
            mode->clock,
            mode->htotal,
            mode->vtotal,
            refresh / 1000U,
            refresh % 1000U,
            mode->type & DRM_MODE_TYPE_PREFERRED ? " preferred" : ""
        );

        uint32_t delta =
            refresh > target_millihz
            ? refresh - target_millihz
            : target_millihz - refresh;

        if (delta < best_delta) {
            best = mode;
            best_delta = delta;
        }
    }

    /* Do not silently substitute 60.000 Hz for a requested 59.940 Hz. */
    return best_delta <= 5U ? best : NULL;
}


static bool same_mode_timing(
    const drmModeModeInfo* a,
    const drmModeModeInfo* b
)
{
    return a->clock == b->clock &&
        a->hdisplay == b->hdisplay &&
        a->hsync_start == b->hsync_start &&
        a->hsync_end == b->hsync_end &&
        a->htotal == b->htotal &&
        a->vdisplay == b->vdisplay &&
        a->vsync_start == b->vsync_start &&
        a->vsync_end == b->vsync_end &&
        a->vtotal == b->vtotal &&
        a->vscan == b->vscan &&
        a->flags == b->flags;
}


static int v3_cmp_double(const void* a, const void* b)
{
    double da = *(const double*)a;
    double db = *(const double*)b;

    return (da > db) - (da < db);
}


static void v3_metric(const char* name, const double* src, size_t n)
{
    if (!n) {
        fprintf(stderr, "%-29s : no samples\n", name);
        return;
    }

    double* v = malloc(n * sizeof(*v));

    if (!v) {
        fprintf(stderr, "%-29s : allocation failed\n", name);
        return;
    }

    memcpy(v, src, n * sizeof(*v));
    qsort(v, n, sizeof(*v), v3_cmp_double);

    double sum = 0.0;

    for (size_t i = 0; i < n; i++)
        sum += v[i];

    size_t p50 = (size_t)((n - 1) * 0.50);
    size_t p95 = (size_t)((n - 1) * 0.95);
    size_t p99 = (size_t)((n - 1) * 0.99);

    fprintf(
        stderr,
        "%-29s : avg=%8.3f p50=%8.3f p95=%8.3f "
        "p99=%8.3f min=%8.3f max=%8.3f ms\n",
        name,
        sum / (double)n,
        v[p50],
        v[p95],
        v[p99],
        v[0],
        v[n - 1]
    );

    free(v);
}


static void v3_print_summary(void)
{
    if (!g_sample_count)
        return;

    double* dq_period =
        calloc(g_sample_count, sizeof(double));

    double* v4l2_period =
        calloc(g_sample_count, sizeof(double));

    double* dq_commit =
        calloc(g_sample_count, sizeof(double));

    double* commit_ioctl =
        calloc(g_sample_count, sizeof(double));

    double* commit_out =
        calloc(g_sample_count, sizeof(double));

    double* dq_out =
        calloc(g_sample_count, sizeof(double));

    if (!dq_period ||
        !v4l2_period ||
        !dq_commit ||
        !commit_ioctl ||
        !commit_out ||
        !dq_out) {

        fprintf(stderr, "V3 summary allocation failed\n");

        free(dq_period);
        free(v4l2_period);
        free(dq_commit);
        free(commit_ioctl);
        free(commit_out);
        free(dq_out);

        return;
    }

    size_t dq_n = 0;
    size_t v4l2_n = 0;
    size_t stage_n = 0;

    uint64_t seq_gaps = 0;
    uint64_t intentional_drops = 0;

    for (size_t i = 0; i < g_sample_count; i++) {
        const struct frame_sample* x = &g_samples[i];
        intentional_drops += x->intentional_drops_before;

        if (i) {
            const struct frame_sample* p =
                &g_samples[i - 1];

            if (x->dq_ns >= p->dq_ns) {
                dq_period[dq_n++] =
                    (double)(x->dq_ns - p->dq_ns) / 1e6;
            }

            if (x->v4l2_ts_ns &&
                p->v4l2_ts_ns &&
                x->v4l2_ts_ns >= p->v4l2_ts_ns) {

                v4l2_period[v4l2_n++] =
                    (double)(x->v4l2_ts_ns -
                        p->v4l2_ts_ns) / 1e6;
            }

            if (x->sequence > p->sequence + 1) {
                seq_gaps +=
                    (uint64_t)x->sequence -
                    p->sequence -
                    1;
            }
        }

        if (x->commit_begin_ns >= x->dq_ns &&
            x->commit_end_ns >= x->commit_begin_ns &&
            x->out_signal_ns >= x->commit_end_ns) {

            dq_commit[stage_n] =
                (double)(x->commit_begin_ns -
                    x->dq_ns) / 1e6;

            commit_ioctl[stage_n] =
                (double)(x->commit_end_ns -
                    x->commit_begin_ns) / 1e6;

            commit_out[stage_n] =
                (double)(x->out_signal_ns -
                    x->commit_end_ns) / 1e6;

            dq_out[stage_n] =
                (double)(x->out_signal_ns -
                    x->dq_ns) / 1e6;

            stage_n++;
        }
    }

    fprintf(stderr, "\n=== TIMING SUMMARY ===\n");

    fprintf(
        stderr,
        "measured frames                : %zu\n",
        g_sample_count
    );

    fprintf(
        stderr,
        "V4L2 sequence gaps             : %" PRIu64
        " raw, %" PRIu64 " intentional, %" PRIu64 " unexpected\n",
        seq_gaps,
        intentional_drops,
        seq_gaps > intentional_drops ? seq_gaps - intentional_drops : 0
    );

    fprintf(
        stderr,
        "first / last V4L2 sequence     : %u / %u\n",
        g_samples[0].sequence,
        g_samples[g_sample_count - 1].sequence
    );

    v3_metric(
        "DQBUF -> next DQBUF",
        dq_period,
        dq_n
    );

    v3_metric(
        "V4L2 timestamp cadence",
        v4l2_period,
        v4l2_n
    );

    v3_metric(
        "DQBUF -> commit call",
        dq_commit,
        stage_n
    );

    v3_metric(
        "atomic commit ioctl",
        commit_ioctl,
        stage_n
    );

    v3_metric(
        "commit return -> completion",
        commit_out,
        stage_n
    );

    v3_metric(
        "DQBUF -> completion",
        dq_out,
        stage_n
    );

    free(dq_period);
    free(v4l2_period);
    free(dq_commit);
    free(commit_ioctl);
    free(commit_out);
    free(dq_out);
}


/* ------------------------------------------------------------------------- */
/* CSV output                                                                */
/* ------------------------------------------------------------------------- */

static int v3_write_csv(const char* path)
{
    FILE* f = fopen(path, "w");

    if (!f)
        return -1;

    fprintf(
        f,
        "frame,"
        "sequence,"
        "index,"
        "fence_present,"
        "async_commit,"
        "early_submit,"
        "overlap_commit,"
        "intentional_drops_before,"
        "v4l2_ts_ns,"
        "dq_ns,"
        "commit_begin_ns,"
        "commit_end_ns,"
        "out_signal_ns,"
        "qbuf_ns,"
        "v4l2_flags,"
        "phase_valid,"
        "mode_period_ns,"
        "dq_vblank_sequence,"
        "dq_vblank_ns,"
        "commit_vblank_sequence,"
        "commit_vblank_ns,"
        "out_vblank_sequence,"
        "out_vblank_ns,"
        "dq_to_commit_us,"
        "commit_ioctl_us,"
        "commit_to_out_us,"
        "dq_to_out_us,"
        "out_to_qbuf_us\n"
    );

    for (size_t i = 0; i < g_sample_count; i++) {
        const struct frame_sample* x =
            &g_samples[i];

        uint64_t a =
            x->commit_begin_ns >= x->dq_ns
            ? (x->commit_begin_ns -
                x->dq_ns) / 1000ULL
            : 0;

        uint64_t b =
            x->commit_end_ns >= x->commit_begin_ns
            ? (x->commit_end_ns -
                x->commit_begin_ns) / 1000ULL
            : 0;

        uint64_t c =
            x->out_signal_ns >= x->commit_end_ns
            ? (x->out_signal_ns -
                x->commit_end_ns) / 1000ULL
            : 0;

        uint64_t d =
            x->out_signal_ns >= x->dq_ns
            ? (x->out_signal_ns -
                x->dq_ns) / 1000ULL
            : 0;

        uint64_t e =
            (x->qbuf_ns &&
                x->qbuf_ns >= x->out_signal_ns)
            ? (x->qbuf_ns -
                x->out_signal_ns) / 1000ULL
            : 0;

        fprintf(
            f,
            "%" PRIu64 ","
            "%u,"
            "%u,"
            "%d,"
            "%d,"
            "%d,"
            "%d,"
            "%u,"
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%u,"
            "%d,"
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 "\n",
            x->frame_no,
            x->sequence,
            x->index,
            x->fence_present,
            x->async_commit,
            x->early_submit,
            x->overlap_commit,
            x->intentional_drops_before,
            x->v4l2_ts_ns,
            x->dq_ns,
            x->commit_begin_ns,
            x->commit_end_ns,
            x->out_signal_ns,
            x->qbuf_ns,
            x->v4l2_flags,
            x->phase_valid,
            x->mode_period_ns,
            x->dq_vblank_sequence,
            x->dq_vblank_ns,
            x->commit_vblank_sequence,
            x->commit_vblank_ns,
            x->out_vblank_sequence,
            x->out_vblank_ns,
            a,
            b,
            c,
            d,
            e
        );
    }

    return fclose(f);
}


/* ------------------------------------------------------------------------- */
/* Runtime configuration                                                     */
/* ------------------------------------------------------------------------- */

struct opts {
    const char* video;
    const char* card;

    uint32_t connector_id;
    uint32_t plane_id;

    int seconds;
    int num_buffers;

    bool enable_low_latency;
    bool verbose;
    bool async_flip;
    bool phase_profile;
    bool early_submit;
    uint32_t target_refresh_millihz;

    const char* csv_path;
};


struct cap_buf {
    int dmabuf_fd;

    uint32_t gem_handle;
    uint32_t fb_id;

    bool queued;
};


struct drm_props {
    uint32_t fb_id;
    uint32_t crtc_id;

    uint32_t crtc_x;
    uint32_t crtc_y;
    uint32_t crtc_w;
    uint32_t crtc_h;

    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_w;
    uint32_t src_h;

    uint32_t in_fence_fd;
    uint32_t out_fence_ptr;
};


struct mode_props {
    uint32_t connector_crtc_id;
    uint32_t crtc_mode_id;
    uint32_t crtc_active;
};


/* ------------------------------------------------------------------------- */
/* Generic helpers                                                           */
/* ------------------------------------------------------------------------- */

static void on_sig(int sig)
{
    (void)sig;
    g_stop = 1;
}


static int xioctl(int fd, unsigned long req, void* arg)
{
    int r;

    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);

    return r;
}


static int parse_u32_arg(const char* text, uint32_t* value)
{
    char* end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);

    if (errno || end == text || *end != '\0' || parsed > UINT32_MAX)
        return -1;

    *value = (uint32_t)parsed;
    return 0;
}


static int parse_int_arg(const char* text, int* value)
{
    char* end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);

    if (errno || end == text || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {

        return -1;
    }

    *value = (int)parsed;
    return 0;
}


static int wait_fd(int fd, int timeout_ms)
{
    struct pollfd p = {
        .fd = fd,
        .events = POLLIN
    };

    int r;

    do {
        r = poll(&p, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);

    if (r == 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    if (r < 0)
        return -1;

    if (p.revents & POLLNVAL) {
        errno = EBADF;
        return -1;
    }

    if (p.revents & (POLLERR | POLLHUP)) {
        errno = EIO;
        return -1;
    }

    if (!(p.revents & POLLIN)) {
        errno = EIO;
        return -1;
    }

    return 0;
}


struct flip_wait {
    bool done;
    uint64_t signal_ns;
};


static void page_flip_handler(
    int fd,
    unsigned int sequence,
    unsigned int tv_sec,
    unsigned int tv_usec,
    void* user_data
)
{
    (void)fd;
    (void)sequence;
    (void)tv_sec;
    (void)tv_usec;

    struct flip_wait* wait = user_data;

    wait->signal_ns = v3_mono_ns();
    wait->done = true;
}


static int wait_flip_event(
    int drmfd,
    struct flip_wait* wait,
    int timeout_ms
)
{
    drmEventContext event = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = page_flip_handler,
    };

    while (!wait->done) {
        struct pollfd p = {
            .fd = drmfd,
            .events = POLLIN,
        };

        int r;

        do {
            r = poll(&p, 1, timeout_ms);
        } while (r < 0 && errno == EINTR && !g_stop);

        if (r == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        if (r < 0)
            return -1;

        if (p.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }

        if (p.revents & (POLLERR | POLLHUP)) {
            errno = EIO;
            return -1;
        }

        if (!(p.revents & POLLIN)) {
            errno = EIO;
            return -1;
        }

        if (drmHandleEvent(drmfd, &event) < 0)
            return -1;
    }

    return 0;
}


static int read_bool_file(
    const char* path,
    char* out
)
{
    int fd =
        open(
            path,
            O_RDONLY | O_CLOEXEC
        );

    if (fd < 0)
        return -1;

    char c = 0;

    ssize_t n =
        read(
            fd,
            &c,
            1
        );

    close(fd);

    if (n != 1)
        return -1;

    *out = c;

    return 0;
}


static int write_bool_file(
    const char* path,
    bool yes
)
{
    int fd =
        open(
            path,
            O_WRONLY | O_CLOEXEC
        );

    if (fd < 0)
        return -1;

    const char c =
        yes ? '1' : '0';

    ssize_t n =
        write(
            fd,
            &c,
            1
        );

    int saved = errno;

    close(fd);

    errno = saved;

    return n == 1 ? 0 : -1;
}


/* ------------------------------------------------------------------------- */
/* DRM discovery                                                             */
/* ------------------------------------------------------------------------- */

static uint32_t prop_id(
    int fd,
    uint32_t obj_id,
    uint32_t obj_type,
    const char* name
)
{
    drmModeObjectProperties* props =
        drmModeObjectGetProperties(
            fd,
            obj_id,
            obj_type
        );

    if (!props)
        return 0;

    uint32_t found = 0;

    for (uint32_t i = 0;
        i < props->count_props;
        i++) {

        drmModePropertyRes* p =
            drmModeGetProperty(
                fd,
                props->props[i]
            );

        if (!p)
            continue;

        if (strcmp(p->name, name) == 0) {
            found = p->prop_id;
            drmModeFreeProperty(p);
            break;
        }

        drmModeFreeProperty(p);
    }

    drmModeFreeObjectProperties(props);

    return found;
}


static bool plane_supports_format(
    drmModePlane* p,
    uint32_t fmt
)
{
    for (uint32_t i = 0;
        i < p->count_formats;
        i++) {

        if (p->formats[i] == fmt)
            return true;
    }

    return false;
}


static void fourcc_to_str(
    uint32_t f,
    char s[5]
)
{
    s[0] = f & 0xff;
    s[1] = (f >> 8) & 0xff;
    s[2] = (f >> 16) & 0xff;
    s[3] = (f >> 24) & 0xff;
    s[4] = 0;
}


static int find_crtc_index(
    drmModeRes* res,
    uint32_t crtc_id
)
{
    for (int i = 0;
        i < res->count_crtcs;
        i++) {

        if (res->crtcs[i] == crtc_id)
            return i;
    }

    return -1;
}


static uint32_t pick_crtc_for_connector(
    int fd,
    drmModeRes* res,
    drmModeConnector* conn
)
{
    if (conn->encoder_id) {
        drmModeEncoder* enc =
            drmModeGetEncoder(
                fd,
                conn->encoder_id
            );

        if (enc) {
            uint32_t id =
                enc->crtc_id;

            drmModeFreeEncoder(enc);

            if (id)
                return id;
        }
    }

    for (int i = 0;
        i < conn->count_encoders;
        i++) {

        drmModeEncoder* enc =
            drmModeGetEncoder(
                fd,
                conn->encoders[i]
            );

        if (!enc)
            continue;

        for (int c = 0;
            c < res->count_crtcs;
            c++) {

            if (enc->possible_crtcs &
                (1u << c)) {

                uint32_t id =
                    res->crtcs[c];

                drmModeFreeEncoder(enc);

                return id;
            }
        }

        drmModeFreeEncoder(enc);
    }

    return 0;
}


static drmModeConnector* pick_connector(
    int fd,
    drmModeRes* res,
    uint32_t requested
)
{
    for (int i = 0;
        i < res->count_connectors;
        i++) {

        drmModeConnector* c =
            drmModeGetConnector(
                fd,
                res->connectors[i]
            );

        if (!c)
            continue;

        if (requested) {
            if (c->connector_id ==
                requested) {

                return c;
            }
        }
        else if (
            c->connection ==
            DRM_MODE_CONNECTED &&
            c->count_modes > 0
            ) {
            return c;
        }

        drmModeFreeConnector(c);
    }

    return NULL;
}


static drmModePlane* pick_plane(
    int fd,
    uint32_t requested,
    int crtc_index
)
{
    drmModePlaneRes* pr =
        drmModeGetPlaneResources(fd);

    if (!pr)
        return NULL;

    drmModePlane* best = NULL;

    fprintf(
        stderr,
        "DRM planes compatible with "
        "CRTC index %d:\n",
        crtc_index
    );

    for (uint32_t i = 0;
        i < pr->count_planes;
        i++) {

        drmModePlane* p =
            drmModeGetPlane(
                fd,
                pr->planes[i]
            );

        if (!p)
            continue;

        if (!(p->possible_crtcs &
            (1u << crtc_index))) {

            drmModeFreePlane(p);
            continue;
        }

        bool nv24 =
            plane_supports_format(
                p,
                DRM_FORMAT_NV24
            );

        fprintf(
            stderr,
            "  plane %u : "
            "NV24=%s formats=",
            p->plane_id,
            nv24 ? "yes" : "no"
        );

        for (uint32_t j = 0;
            j < p->count_formats;
            j++) {

            char s[5];

            fourcc_to_str(
                p->formats[j],
                s
            );

            fprintf(
                stderr,
                "%s%s",
                j ? "," : "",
                s
            );
        }

        fprintf(stderr, "\n");

        if (requested) {
            if (p->plane_id ==
                requested) {

                if (!nv24) {
                    fprintf(
                        stderr,
                        "Requested plane %u "
                        "does not advertise "
                        "DRM_FORMAT_NV24.\n",
                        requested
                    );

                    drmModeFreePlane(p);
                    drmModeFreePlaneResources(pr);

                    return NULL;
                }

                best = p;
                break;
            }
        }
        else if (!best && nv24) {
            best = p;
            continue;
        }

        if (p != best)
            drmModeFreePlane(p);
    }

    drmModeFreePlaneResources(pr);

    return best;
}


/* ------------------------------------------------------------------------- */
/* Rockchip fence extraction                                                 */
/* ------------------------------------------------------------------------- */

static int extract_fence_fd(
    const struct v4l2_buffer* b
)
{
    int fd = -1;

    memcpy(
        &fd,
        b->timecode.userbits,
        sizeof(fd)
    );

    if (fd < 0 ||
        fd == (int)0xffffffff) {

        return -1;
    }

    return fd;
}


/* ------------------------------------------------------------------------- */
/* V4L2 helpers                                                              */
/* ------------------------------------------------------------------------- */

static int qbuf_one(
    int vfd,
    int idx
)
{
    struct v4l2_buffer b;
    struct v4l2_plane p[VIDEO_MAX_PLANES];

    memset(&b, 0, sizeof(b));
    memset(p, 0, sizeof(p));

    b.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    b.memory =
        V4L2_MEMORY_MMAP;

    b.index =
        idx;

    b.length =
        1;

    b.m.planes =
        p;

    /*
     * Rockchip low-latency driver places
     * its sync-file fd here on completion.
     * Reset userbits before requeueing.
     */
    memset(
        b.timecode.userbits,
        0xff,
        sizeof(b.timecode.userbits)
    );

    return xioctl(
        vfd,
        VIDIOC_QBUF,
        &b
    );
}


static int dqbuf_one(
    int vfd,
    struct v4l2_buffer* b,
    struct v4l2_plane* p
)
{
    memset(
        b,
        0,
        sizeof(*b)
    );

    memset(
        p,
        0,
        sizeof(struct v4l2_plane) *
        VIDEO_MAX_PLANES
    );

    b->type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    b->memory =
        V4L2_MEMORY_MMAP;

    b->length =
        1;

    b->m.planes =
        p;

    return xioctl(
        vfd,
        VIDIOC_DQBUF,
        b
    );
}


/* ------------------------------------------------------------------------- */
/* DRM atomic-plane helpers                                                  */
/* ------------------------------------------------------------------------- */

static int add_plane_props(
    int drmfd,
    drmModeAtomicReq* req,
    uint32_t plane_id,
    const struct drm_props* pp,
    uint32_t crtc_id,
    uint32_t fb_id,
    uint32_t w,
    uint32_t h,
    int in_fence_fd
)
{
    (void)drmfd;

#define ADD_PROP(PROP, VAL) do {                         \
    if (!(PROP)) {                                       \
        errno = ENOENT;                                  \
        return -1;                                       \
    }                                                    \
    if (drmModeAtomicAddProperty(                        \
            req,                                         \
            plane_id,                                    \
            (PROP),                                      \
            (VAL)) < 0)                                  \
        return -1;                                       \
} while (0)

    ADD_PROP(
        pp->fb_id,
        fb_id
    );

    ADD_PROP(
        pp->crtc_id,
        crtc_id
    );

    ADD_PROP(
        pp->crtc_x,
        0
    );

    ADD_PROP(
        pp->crtc_y,
        0
    );

    ADD_PROP(
        pp->crtc_w,
        w
    );

    ADD_PROP(
        pp->crtc_h,
        h
    );

    ADD_PROP(
        pp->src_x,
        0
    );

    ADD_PROP(
        pp->src_y,
        0
    );

    ADD_PROP(
        pp->src_w,
        ((uint64_t)w) << 16
    );

    ADD_PROP(
        pp->src_h,
        ((uint64_t)h) << 16
    );

    if (pp->in_fence_fd &&
        in_fence_fd >= 0) {

        if (drmModeAtomicAddProperty(
            req,
            plane_id,
            pp->in_fence_fd,
            (uint64_t)(int64_t)
            in_fence_fd) < 0) {

            return -1;
        }
    }

#undef ADD_PROP

    return 0;
}


/*
 * Atomic async flips are deliberately restricted to a pure framebuffer
 * replacement (plus the acquire fence).  Re-submitting CRTC_ID, geometry or
 * OUT_FENCE_PTR turns the request into a state change that the atomic async
 * UAPI rejects.
 */
static int add_async_plane_props(
    drmModeAtomicReq* req,
    uint32_t plane_id,
    const struct drm_props* pp,
    uint32_t fb_id,
    int in_fence_fd
)
{
    if (!pp->fb_id) {
        errno = ENOENT;
        return -1;
    }

    if (drmModeAtomicAddProperty(
        req,
        plane_id,
        pp->fb_id,
        fb_id) < 0) {

        return -1;
    }

    if (pp->in_fence_fd &&
        in_fence_fd >= 0) {

        if (drmModeAtomicAddProperty(
            req,
            plane_id,
            pp->in_fence_fd,
            (uint64_t)(int64_t)
            in_fence_fd) < 0) {

            return -1;
        }
    }

    return 0;
}


static int add_modeset_props(
    drmModeAtomicReq* req,
    uint32_t connector_id,
    uint32_t crtc_id,
    const struct mode_props* mp,
    uint32_t mode_blob_id
)
{
    if (!mp->connector_crtc_id ||
        !mp->crtc_mode_id ||
        !mp->crtc_active ||
        !mode_blob_id) {

        errno = ENOENT;
        return -1;
    }

    if (drmModeAtomicAddProperty(
            req,
            connector_id,
            mp->connector_crtc_id,
            crtc_id) < 0 ||
        drmModeAtomicAddProperty(
            req,
            crtc_id,
            mp->crtc_mode_id,
            mode_blob_id) < 0 ||
        drmModeAtomicAddProperty(
            req,
            crtc_id,
            mp->crtc_active,
            1) < 0) {

        return -1;
    }

    return 0;
}


/* ------------------------------------------------------------------------- */
/* CLI                                                                       */
/* ------------------------------------------------------------------------- */

static void usage(
    const char* argv0
)
{
    fprintf(
        stderr,

        "Usage: %s [options]\n"
        "  --video /dev/video0      HDMI-RX V4L2 node\n"
        "  --card /dev/dri/card0    DRM card\n"
        "  --connector ID           DRM connector ID (0=auto)\n"
        "  --plane ID               DRM plane ID (0=auto NV24 plane)\n"
        "  --seconds N              auto-stop after N seconds (default 10)\n"
        "  --buffers N              V4L2 buffers 3..8 (default 4)\n"
        "  --no-low-latency         do not toggle rockchip_hdmirx low_latency\n"
        "  --async-flip             request DRM_MODE_PAGE_FLIP_ASYNC (experimental)\n"
        "  --phase-profile          record V3.6 CRTC/vblank phase data\n"
        "  --target-refresh-millihz N  V3.7 advertised-mode test (59940)\n"
        "  --early-submit           V3.8 one-shot overlap/phase-prime experiment\n"
        "  --csv PATH               timing CSV (default /tmp/hdmirx-lowlat.csv)\n"
        "  -v, --verbose            extra per-frame output\n",

        argv0
    );
}


/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(
    int argc,
    char** argv
)
{
    struct opts o = {
        .video =
            "/dev/video0",

        .card =
            "/dev/dri/card0",

        .connector_id =
            0,

        .plane_id =
            0,

        .seconds =
            10,

        .num_buffers =
            4,

        .enable_low_latency =
            true,

        .verbose =
            false,

        .async_flip =
            false,

        .phase_profile =
            false,

        .early_submit =
            false,

        .target_refresh_millihz =
            0,

        .csv_path =
            "/tmp/hdmirx-lowlat.csv",
    };


    static const struct option
        longopts[] = {

            {
                "video",
                required_argument,
                0,
                1
            },

            {
                "card",
                required_argument,
                0,
                2
            },

            {
                "connector",
                required_argument,
                0,
                3
            },

            {
                "plane",
                required_argument,
                0,
                4
            },

            {
                "seconds",
                required_argument,
                0,
                5
            },

            {
                "buffers",
                required_argument,
                0,
                6
            },

            {
                "no-low-latency",
                no_argument,
                0,
                7
            },

            {
                "csv",
                required_argument,
                0,
                8
            },

            {
                "async-flip",
                no_argument,
                0,
                9
            },

            {
                "phase-profile",
                no_argument,
                0,
                10
            },

            {
                "target-refresh-millihz",
                required_argument,
                0,
                11
            },

            {
                "early-submit",
                no_argument,
                0,
                12
            },

            {
                "verbose",
                no_argument,
                0,
                'v'
            },

            {
                "help",
                no_argument,
                0,
                'h'
            },

            {
                0,
                0,
                0,
                0
            }
    };


    int c;

    while (
        (c = getopt_long(
            argc,
            argv,
            "vh",
            longopts,
            NULL
        )) != -1
        ) {

        switch (c) {

        case 1:
            o.video = optarg;
            break;

        case 2:
            o.card = optarg;
            break;

        case 3:
            if (parse_u32_arg(
                optarg,
                &o.connector_id) < 0) {

                fprintf(stderr, "Invalid connector ID: %s\n", optarg);
                return 2;
            }
            break;

        case 4:
            if (parse_u32_arg(
                optarg,
                &o.plane_id) < 0) {

                fprintf(stderr, "Invalid plane ID: %s\n", optarg);
                return 2;
            }
            break;

        case 5:
            if (parse_int_arg(
                optarg,
                &o.seconds) < 0) {

                fprintf(stderr, "Invalid duration: %s\n", optarg);
                return 2;
            }
            break;

        case 6:
            if (parse_int_arg(
                optarg,
                &o.num_buffers) < 0) {

                fprintf(stderr, "Invalid buffer count: %s\n", optarg);
                return 2;
            }
            break;

        case 7:
            o.enable_low_latency =
                false;
            break;

        case 8:
            o.csv_path =
                optarg;
            break;

        case 9:
            o.async_flip =
                true;
            break;

        case 10:
            o.phase_profile =
                true;
            break;

        case 11:
            if (parse_u32_arg(
                optarg,
                &o.target_refresh_millihz) < 0) {

                fprintf(stderr, "Invalid target refresh: %s\n", optarg);
                return 2;
            }
            break;

        case 12:
            o.early_submit = true;
            break;

        case 'v':
            o.verbose =
                true;
            break;

        default:
            usage(argv[0]);

            return
                c == 'h'
                ? 0
                : 2;
        }
    }


    if (optind != argc ||
        o.num_buffers < 3 ||
        o.num_buffers > MAX_BUFS ||
        o.seconds < 1 ||
        (o.async_flip && o.phase_profile) ||
        (o.async_flip && o.early_submit) ||
        (o.early_submit && !o.phase_profile) ||
        (o.async_flip && o.target_refresh_millihz) ||
        (o.target_refresh_millihz &&
            (o.target_refresh_millihz < 1000U ||
             o.target_refresh_millihz > 1000000U))) {

        usage(argv[0]);

        if (o.async_flip && o.phase_profile) {
            fprintf(
                stderr,
                "--phase-profile intentionally measures only the normal "
                "OUT_FENCE_PTR path; do not combine it with --async-flip.\n"
            );
        }

        if (o.async_flip && o.early_submit) {
            fprintf(stderr,
                "--early-submit uses normal atomic OUT fences and cannot be "
                "combined with --async-flip.\n");
        }

        if (o.early_submit && !o.phase_profile) {
            fprintf(stderr,
                "--early-submit requires --phase-profile for auditable data.\n");
        }


        if (o.async_flip && o.target_refresh_millihz) {
            fprintf(
                stderr,
                "V3.7 cadence alignment uses the normal OUT_FENCE_PTR "
                "path and cannot be combined with --async-flip.\n"
            );
        }

        return 2;
    }


    signal(
        SIGINT,
        on_sig
    );

    signal(
        SIGTERM,
        on_sig
    );


    /*
     * Rockchip HDMI-RX low-latency parameter.
     *
     * Historical testing showed this changes
     * delay_line from the normal large value
     * to the driver's low-latency path.
     */
    const char* ll_path =
        "/sys/module/rockchip_hdmirx/"
        "parameters/low_latency";

    char ll_original = '?';

    bool ll_changed = false;


    if (o.enable_low_latency) {

        if (read_bool_file(
            ll_path,
            &ll_original) == 0) {

            fprintf(
                stderr,
                "rockchip_hdmirx "
                "low_latency initially: %c\n",
                ll_original
            );

            if (write_bool_file(
                ll_path,
                true) < 0) {

                perror(
                    "enable low_latency"
                );

                fprintf(
                    stderr,
                    "Run as root (sudo) "
                    "or use "
                    "--no-low-latency.\n"
                );

                return 1;
            }

            ll_changed = true;

            char now = '?';

            if (read_bool_file(
                ll_path,
                &now) == 0) {

                fprintf(
                    stderr,
                    "rockchip_hdmirx "
                    "low_latency now: %c\n",
                    now
                );
            }
        }
        else {
            perror(
                "read low_latency"
            );

            return 1;
        }
    }


    int vfd = -1;
    int drmfd = -1;
    int rc = 1;

    drmModeRes* res = NULL;
    drmModeConnector* conn = NULL;
    drmModeCrtc* crtc = NULL;
    drmModePlane* plane = NULL;
    uint32_t crtc_id = 0;
    uint32_t target_mode_blob = 0;
    uint32_t original_mode_blob = 0;
    bool target_mode_applied = false;
    bool target_mode_verified = false;
    bool mode_changed = false;
    drmModeModeInfo target_mode;
    memset(&target_mode, 0, sizeof(target_mode));
    struct mode_props mp;
    memset(&mp, 0, sizeof(mp));


    struct cap_buf bufs[MAX_BUFS];

    memset(
        bufs,
        0,
        sizeof(bufs)
    );

    for (int i = 0;
        i < MAX_BUFS;
        i++) {

        bufs[i].dmabuf_fd = -1;
    }


    /* --------------------------------------------------------------------- */
    /* Open HDMI-RX                                                          */
    /* --------------------------------------------------------------------- */

    vfd =
        open(
            o.video,
            O_RDWR | O_CLOEXEC
        );

    if (vfd < 0) {
        perror("open video");
        goto out;
    }


    struct v4l2_capability cap;

    memset(
        &cap,
        0,
        sizeof(cap)
    );

    if (xioctl(
        vfd,
        VIDIOC_QUERYCAP,
        &cap) < 0) {

        perror(
            "VIDIOC_QUERYCAP"
        );

        goto out;
    }


    struct v4l2_format fmt;

    memset(
        &fmt,
        0,
        sizeof(fmt)
    );

    fmt.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;


    if (xioctl(
        vfd,
        VIDIOC_G_FMT,
        &fmt) < 0) {

        perror(
            "VIDIOC_G_FMT"
        );

        goto out;
    }


    uint32_t pixfmt =
        fmt.fmt.pix_mp.pixelformat;

    char fourcc[5];

    fourcc_to_str(
        pixfmt,
        fourcc
    );


    uint32_t w =
        fmt.fmt.pix_mp.width;

    uint32_t h =
        fmt.fmt.pix_mp.height;

    uint32_t bpl =
        fmt.fmt.pix_mp
        .plane_fmt[0]
        .bytesperline;

    uint32_t sizeimage =
        fmt.fmt.pix_mp
        .plane_fmt[0]
        .sizeimage;


    fprintf(
        stderr,
        "V4L2: %ux%u "
        "fourcc=%s "
        "planes=%u "
        "bytesperline=%u "
        "sizeimage=%u\n",
        w,
        h,
        fourcc,
        fmt.fmt.pix_mp.num_planes,
        bpl,
        sizeimage
    );


    if (pixfmt != V4L2_PIX_FMT_NV24 ||
        fmt.fmt.pix_mp.num_planes != 1) {

        fprintf(
            stderr,
            "This consolidated build is "
            "intentionally zero-copy and "
            "currently requires the RK3588 "
            "HDMI-RX native single-memory-plane "
            "V4L2 NV24 format.\n"
            "Current format is %s / %u planes.\n",
            fourcc,
            fmt.fmt.pix_mp.num_planes
        );

        goto out;
    }


    uint64_t nv24_required =
        (uint64_t)bpl *
        (uint64_t)h *
        3ULL;


    if (bpl < w ||
        bpl > UINT32_MAX / 2U ||
        nv24_required > UINT32_MAX ||
        sizeimage < nv24_required) {

        fprintf(
            stderr,
            "Unsafe or unsupported NV24 layout: "
            "width=%u height=%u bytesperline=%u "
            "sizeimage=%u required=%" PRIu64 ".\n",
            w,
            h,
            bpl,
            sizeimage,
            nv24_required
        );

        goto out;
    }


    /* --------------------------------------------------------------------- */
    /* Open DRM                                                              */
    /* --------------------------------------------------------------------- */

    drmfd =
        open(
            o.card,
            O_RDWR | O_CLOEXEC
        );

    if (drmfd < 0) {
        perror(
            "open DRM card"
        );

        goto out;
    }


    if (drmSetClientCap(
        drmfd,
        DRM_CLIENT_CAP_UNIVERSAL_PLANES,
        1) < 0) {

        perror(
            "DRM_CLIENT_CAP_UNIVERSAL_PLANES"
        );
    }


    if (drmSetClientCap(
        drmfd,
        DRM_CLIENT_CAP_ATOMIC,
        1) < 0) {

        perror(
            "DRM_CLIENT_CAP_ATOMIC"
        );

        goto out;
    }


    if (o.phase_profile) {
        uint64_t timestamp_monotonic = 0;

        if (drmGetCap(
            drmfd,
            DRM_CAP_TIMESTAMP_MONOTONIC,
            &timestamp_monotonic) < 0) {

            perror("DRM_CAP_TIMESTAMP_MONOTONIC");
            goto out;
        }

        if (!timestamp_monotonic) {
            fprintf(
                stderr,
                "V3.6 requires monotonic DRM vblank timestamps so they "
                "share a clock domain with DQ/commit measurements.\n"
            );

            goto out;
        }

        fprintf(
            stderr,
            "DRM monotonic vblank timestamps: yes\n"
        );
    }


    if (o.async_flip) {
        uint64_t atomic_async_cap = 0;
        uint64_t legacy_async_cap = 0;
        int atomic_cap_result;
        int atomic_cap_errno;

        /*
         * DRM_CAP_ASYNC_PAGE_FLIP describes the legacy page-flip ioctl.
         * Atomic async commits have a separate capability.  Linux 6.1 does
         * not implement that newer capability and rejects the async flag in
         * the atomic ioctl.  Because this program is an explicit diagnostic,
         * continue to one real atomic attempt after a clear warning: this
         * also detects vendor backports that accept the flag without
         * advertising the newer capability.
         */
        atomic_cap_result = drmGetCap(
            drmfd,
            DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP,
            &atomic_async_cap
        );
        atomic_cap_errno = errno;

        if (drmGetCap(
            drmfd,
            DRM_CAP_ASYNC_PAGE_FLIP,
            &legacy_async_cap) < 0) {

            legacy_async_cap = 0;
        }

        if (atomic_cap_result == 0 && atomic_async_cap) {
            fprintf(
                stderr,
                "DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP=yes "
                "(legacy async=%" PRIu64 "); "
                "the first frame will seed the plane synchronously.\n",
                legacy_async_cap
            );
        }
        else {
            fprintf(
                stderr,
                "WARNING: DRM does not advertise atomic async page flips "
                "(query=%s, value=%" PRIu64 ", legacy async=%" PRIu64 ").\n"
                "V3.5 will issue one diagnostic atomic async commit; "
                "EINVAL/unsupported is expected on Linux 6.1 and does not "
                "invalidate the V3.4 baseline.\n",
                atomic_cap_result == 0 ? "ok" : strerror(atomic_cap_errno),
                atomic_async_cap,
                legacy_async_cap
            );
        }
    }


    res =
        drmModeGetResources(
            drmfd
        );

    if (!res) {
        perror(
            "drmModeGetResources"
        );

        goto out;
    }


    conn =
        pick_connector(
            drmfd,
            res,
            o.connector_id
        );

    if (!conn) {
        fprintf(
            stderr,
            "No usable connector found "
            "(requested=%u).\n",
            o.connector_id
        );

        goto out;
    }


    fprintf(
        stderr,
        "Using connector %u, "
        "modes=%d\n",
        conn->connector_id,
        conn->count_modes
    );


    crtc_id =
        pick_crtc_for_connector(
            drmfd,
            res,
            conn
        );

    if (!crtc_id) {
        fprintf(
            stderr,
            "Could not determine a "
            "CRTC for connector %u.\n",
            conn->connector_id
        );

        goto out;
    }


    int crtc_index =
        find_crtc_index(
            res,
            crtc_id
        );

    if (crtc_index < 0) {
        fprintf(
            stderr,
            "CRTC index not found.\n"
        );

        goto out;
    }


    crtc =
        drmModeGetCrtc(
            drmfd,
            crtc_id
        );

    if (!crtc) {
        perror(
            "drmModeGetCrtc"
        );

        goto out;
    }


    if (!crtc->mode_valid ||
        crtc->width == 0 ||
        crtc->height == 0) {

        fprintf(
            stderr,
            "The selected CRTC is not "
            "already active.\n"
            "V3.7 only changes timing from an already-active, "
            "matching-resolution mode.\n"
            "Boot/leave the display at "
            "1920x1080 first, then retry.\n"
        );

        goto out;
    }


    fprintf(
        stderr,
        "CRTC %u active at %ux%u\n",
        crtc_id,
        crtc->width,
        crtc->height
    );


    if (o.target_refresh_millihz) {
        const drmModeModeInfo* selected =
            pick_refresh_mode(
                conn,
                w,
                h,
                o.target_refresh_millihz
            );

        if (!selected) {
            fprintf(
                stderr,
                "No EDID-advertised %ux%u mode within 0.005 Hz of "
                "%u.%03u Hz. V3.7 refuses to synthesize an unadvertised "
                "timing.\n",
                w,
                h,
                o.target_refresh_millihz / 1000U,
                o.target_refresh_millihz % 1000U
            );

            goto out;
        }

        target_mode = *selected;
        mode_changed = !same_mode_timing(&crtc->mode, &target_mode);

        mp.connector_crtc_id = prop_id(
            drmfd,
            conn->connector_id,
            DRM_MODE_OBJECT_CONNECTOR,
            "CRTC_ID"
        );
        mp.crtc_mode_id = prop_id(
            drmfd,
            crtc_id,
            DRM_MODE_OBJECT_CRTC,
            "MODE_ID"
        );
        mp.crtc_active = prop_id(
            drmfd,
            crtc_id,
            DRM_MODE_OBJECT_CRTC,
            "ACTIVE"
        );

        if (!mp.connector_crtc_id || !mp.crtc_mode_id || !mp.crtc_active) {
            fprintf(stderr, "V3.7 modeset properties are unavailable.\n");
            goto out;
        }

        if (drmModeCreatePropertyBlob(
                drmfd,
                &target_mode,
                sizeof(target_mode),
                &target_mode_blob) < 0) {

            perror("create V3.7 target mode blob");
            goto out;
        }

        if (mode_changed &&
            drmModeCreatePropertyBlob(
                drmfd,
                &crtc->mode,
                sizeof(crtc->mode),
                &original_mode_blob) < 0) {

            perror("create original mode blob");
            goto out;
        }

        fprintf(
            stderr,
            "V3.7 target mode: %s clock=%u kHz refresh=%u.%03u Hz "
            "(%s)\n",
            target_mode.name,
            target_mode.clock,
            mode_refresh_millihz(&target_mode) / 1000U,
            mode_refresh_millihz(&target_mode) % 1000U,
            mode_changed ? "temporary modeset" : "already active"
        );
    }


    uint64_t phase_mode_period_ns =
        v36_mode_period_ns(
            o.target_refresh_millihz
            ? &target_mode
            : &crtc->mode
        );

    if (o.phase_profile) {
        if (!phase_mode_period_ns) {
            fprintf(
                stderr,
                "Could not calculate the active CRTC period.\n"
            );

            goto out;
        }

        fprintf(
            stderr,
            "Measurement mode: %s clock=%u kHz htotal=%u "
            "vtotal=%u vscan=%u period=%.6f ms refresh=%.6f Hz\n",
            o.target_refresh_millihz ? target_mode.name : crtc->mode.name,
            o.target_refresh_millihz ? target_mode.clock : crtc->mode.clock,
            o.target_refresh_millihz ? target_mode.htotal : crtc->mode.htotal,
            o.target_refresh_millihz ? target_mode.vtotal : crtc->mode.vtotal,
            o.target_refresh_millihz ? target_mode.vscan : crtc->mode.vscan,
            (double)phase_mode_period_ns / 1e6,
            1e9 / (double)phase_mode_period_ns
        );
    }


    if (crtc->width != w ||
        crtc->height != h) {

        fprintf(
            stderr,
            "Capture is %ux%u but "
            "active output is %ux%u.\n"
            "Refusing scaling in "
            "the latency build.\n",
            w,
            h,
            crtc->width,
            crtc->height
        );

        goto out;
    }


    plane =
        pick_plane(
            drmfd,
            o.plane_id,
            crtc_index
        );


    if (!plane) {
        fprintf(
            stderr,
            "\nNo compatible KMS plane "
            "advertising NV24 was found.\n"
            "Direct zero-copy scanout "
            "cannot be done on this "
            "DRM device/plane without "
            "a hardware format-conversion "
            "stage.\n"
            "Do NOT add CPU videoconvert.\n"
        );

        goto out;
    }


    fprintf(
        stderr,
        "Using plane %u with "
        "native NV24 scanout.\n",
        plane->plane_id
    );


    struct drm_props pp = {

        .fb_id =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "FB_ID"
            ),

        .crtc_id =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "CRTC_ID"
            ),

        .crtc_x =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "CRTC_X"
            ),

        .crtc_y =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "CRTC_Y"
            ),

        .crtc_w =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "CRTC_W"
            ),

        .crtc_h =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "CRTC_H"
            ),

        .src_x =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "SRC_X"
            ),

        .src_y =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "SRC_Y"
            ),

        .src_w =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "SRC_W"
            ),

        .src_h =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "SRC_H"
            ),

        .in_fence_fd =
            prop_id(
                drmfd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "IN_FENCE_FD"
            ),

        .out_fence_ptr =
            prop_id(
                drmfd,
                crtc_id,
                DRM_MODE_OBJECT_CRTC,
                "OUT_FENCE_PTR"
            ),
    };


    if (!pp.fb_id ||
        !pp.crtc_id ||
        !pp.crtc_x ||
        !pp.crtc_y ||
        !pp.crtc_w ||
        !pp.crtc_h ||
        !pp.src_x ||
        !pp.src_y ||
        !pp.src_w ||
        !pp.src_h) {

        fprintf(
            stderr,
            "Selected plane is missing "
            "mandatory atomic properties.\n"
        );

        goto out;
    }


    fprintf(
        stderr,
        "Explicit sync: "
        "plane IN_FENCE_FD=%s, "
        "CRTC OUT_FENCE_PTR=%s\n",
        pp.in_fence_fd
        ? "yes"
        : "no",
        pp.out_fence_ptr
        ? "yes"
        : "no"
    );


    if (o.enable_low_latency &&
        !pp.in_fence_fd) {

        fprintf(
            stderr,
            "IN_FENCE_FD is mandatory while Rockchip low_latency is enabled.\n"
            "Refusing to run the characterized path with implicit or "
            "userspace-only synchronization.\n"
        );

        goto out;
    }


    if (!pp.out_fence_ptr) {
        fprintf(
            stderr,
            "OUT_FENCE_PTR is required "
            "by this build so we never "
            "requeue a capture buffer while "
            "the display engine can still "
            "be scanning it.\n"
            "Aborting rather than adding "
            "a frame queue.\n"
        );

        goto out;
    }


    /* --------------------------------------------------------------------- */
    /* Allocate / export capture buffers                                     */
    /* --------------------------------------------------------------------- */

    struct v4l2_requestbuffers req;

    memset(
        &req,
        0,
        sizeof(req)
    );

    req.count =
        o.num_buffers;

    req.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    req.memory =
        V4L2_MEMORY_MMAP;


    if (xioctl(
        vfd,
        VIDIOC_REQBUFS,
        &req) < 0) {

        perror(
            "VIDIOC_REQBUFS"
        );

        goto out;
    }


    if (req.count <
        (uint32_t)o.num_buffers) {

        fprintf(
            stderr,
            "Driver returned only %u of the requested %d buffers.\n"
            "Aborting instead of silently running below the selected "
            "capture-pool size.\n",
            req.count,
            o.num_buffers
        );

        goto out;
    }


    if (req.count > MAX_BUFS)
        req.count = MAX_BUFS;


    fprintf(
        stderr,
        "Allocated %u "
        "V4L2 buffers.\n",
        req.count
    );


    for (uint32_t i = 0;
        i < req.count;
        i++) {

        struct v4l2_buffer qb;
        struct v4l2_plane
            qp[VIDEO_MAX_PLANES];

        memset(
            &qb,
            0,
            sizeof(qb)
        );

        memset(
            qp,
            0,
            sizeof(qp)
        );


        qb.type =
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

        qb.memory =
            V4L2_MEMORY_MMAP;

        qb.index =
            i;

        qb.length =
            1;

        qb.m.planes =
            qp;


        if (xioctl(
            vfd,
            VIDIOC_QUERYBUF,
            &qb) < 0) {

            perror(
                "VIDIOC_QUERYBUF"
            );

            goto out;
        }


        if ((uint64_t)qp[0].length <
            nv24_required) {

            fprintf(
                stderr,
                "V4L2 buffer %u is too small: "
                "length=%u required=%" PRIu64 ".\n",
                i,
                qp[0].length,
                nv24_required
            );

            goto out;
        }


        struct v4l2_exportbuffer eb;

        memset(
            &eb,
            0,
            sizeof(eb)
        );


        eb.type =
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

        eb.index =
            i;

        eb.plane =
            0;

        eb.flags =
            O_CLOEXEC;


        if (xioctl(
            vfd,
            VIDIOC_EXPBUF,
            &eb) < 0) {

            perror(
                "VIDIOC_EXPBUF"
            );

            goto out;
        }


        bufs[i].dmabuf_fd =
            eb.fd;


        if (drmPrimeFDToHandle(
            drmfd,
            eb.fd,
            &bufs[i].gem_handle) < 0) {

            perror(
                "drmPrimeFDToHandle"
            );

            goto out;
        }


        /*
         * RK3588 HDMI-RX NV24:
         *
         * one exported memory object
         *
         * Y:
         *   offset = 0
         *   pitch  = bpl
         *
         * UV:
         *   offset = bpl * h
         *   pitch  = bpl * 2
         */

        uint32_t handles[4] = {
            bufs[i].gem_handle,
            bufs[i].gem_handle,
            0,
            0
        };

        uint32_t pitches[4] = {
            bpl,
            bpl * 2,
            0,
            0
        };

        uint32_t offsets[4] = {
            0,
            bpl * h,
            0,
            0
        };


        if (drmModeAddFB2(
            drmfd,
            w,
            h,
            DRM_FORMAT_NV24,
            handles,
            pitches,
            offsets,
            &bufs[i].fb_id,
            0) < 0) {

            perror(
                "drmModeAddFB2(NV24)"
            );

            fprintf(
                stderr,
                "The plane may advertise "
                "NV24 but reject this exact "
                "linear layout.\n"
                "If so, inspect modifier/"
                "layout requirements.\n"
            );

            goto out;
        }


        if (qbuf_one(
            vfd,
            i) < 0) {

            perror(
                "VIDIOC_QBUF initial"
            );

            goto out;
        }


        bufs[i].queued =
            true;
    }


    enum v4l2_buf_type type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;


    if (xioctl(
        vfd,
        VIDIOC_STREAMON,
        &type) < 0) {

        perror(
            "VIDIOC_STREAMON"
        );

        goto out;
    }


    fprintf(
        stderr,
        "\n=== LIVE ZERO-COPY TEST ===\n"
        "No CPU colour conversion.\n"
        "No GStreamer queue.\n"
        "No userspace framebuffer copy.\n"
        "V4L2 NV24 DMABUF -> DRM FB "
        "-> atomic KMS plane.\n"
        "Consolidated V3.8 build: "
        "V3.4 lifetime, V3.6 phase, V3.7 cadence, and V3.8 early-submit instrumentation.\n"
        "Atomic commit mode: %s\n"
        "Phase profiler: %s\n"
        "Early-submit probe: %s\n"
        "Target refresh: %s\n"
        "Timing CSV: %s\n"
        "Auto-stop: %d seconds.\n\n",

        o.async_flip
        ? "ASYNC (experimental)"
        : "normal/vblank",

        o.phase_profile
        ? "enabled"
        : "disabled",

        o.early_submit
        ? "enabled (one controlled overlap attempt)"
        : "disabled",

        o.target_refresh_millihz
        ? "advertised-mode experiment"
        : "unchanged active mode",

        o.csv_path,
        o.seconds
    );


    /* --------------------------------------------------------------------- */
    /* Main streaming loop                                                   */
    /* --------------------------------------------------------------------- */

    struct timespec t0;

    clock_gettime(
        CLOCK_MONOTONIC,
        &t0
    );


    int displayed = -1;

    uint64_t frames = 0;
    uint64_t missing_fences = 0;
    uint64_t acquire_waits = 0;
    uint64_t v38_early_dq_before_out = 0;
    uint64_t v38_out_before_dq = 0;
    uint64_t v38_overlap_accepted = 0;
    uint64_t v38_overlap_ebusy = 0;
    uint64_t v38_overlap_other_reject = 0;
    uint64_t v38_intentional_drops = 0;
    uint32_t v38_drops_pending = 0;
    bool v38_probe_done = false;

    /*
     * IMPORTANT:
     *
     * Previous consolidated source returned success whenever
     * frames > 0, even if a fatal DRM/V4L2 error occurred later.
     *
     * V3.5/V3.6 measurements depend on trustworthy exit codes.
     */
    bool run_failed = false;


    while (!g_stop) {

        struct timespec now;

        clock_gettime(
            CLOCK_MONOTONIC,
            &now
        );


        double elapsed =
            (now.tv_sec -
                t0.tv_sec) +

            (now.tv_nsec -
                t0.tv_nsec) / 1e9;


        if (elapsed >= o.seconds)
            break;


        struct pollfd vp = {
            .fd = vfd,
            .events = POLLIN | POLLPRI
        };


        int pr;

        do {
            pr =
                poll(
                    &vp,
                    1,
                    1000
                );
        } while (
            pr < 0 &&
            errno == EINTR
            );


        if (pr == 0) {
            fprintf(
                stderr,
                "V4L2 dequeue timeout.\n"
            );

            continue;
        }


        if (pr < 0) {
            perror(
                "poll video"
            );

            run_failed = true;
            break;
        }


        if (vp.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(
                stderr,
                "V4L2 poll reported device error (revents=0x%x).\n",
                vp.revents
            );

            run_failed = true;
            break;
        }


        if (!(vp.revents & (POLLIN | POLLPRI)))
            continue;


        struct v4l2_buffer b;
        struct v4l2_plane
            p[VIDEO_MAX_PLANES];


        if (dqbuf_one(
            vfd,
            &b,
            p) < 0) {

            if (errno == EAGAIN)
                continue;

            perror(
                "VIDIOC_DQBUF"
            );

            run_failed = true;
            break;
        }


        if (b.index >= req.count) {
            fprintf(
                stderr,
                "Bad buffer index %u\n",
                b.index
            );

            run_failed = true;
            break;
        }


        bufs[b.index].queued =
            false;


        const uint64_t
            v3_dq_ns =
            v3_mono_ns();


        const uint64_t
            v3_v4l2_ts_ns =
            v3_timeval_ns(
                &b.timestamp
            );


        const uint32_t
            v3_sequence =
            b.sequence;


        const uint32_t
            v36_v4l2_flags =
            b.flags;

        const uint32_t v38_drops_before_current =
            v38_drops_pending;

        v38_drops_pending = 0;


        if (o.phase_profile && frames == 0) {
            const uint32_t timestamp_type =
                b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;

            const uint32_t timestamp_source =
                b.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;

            fprintf(
                stderr,
                "V3.6 V4L2 timestamp flags: raw=0x%08x "
                "clock=%s source=%s\n",
                b.flags,
                timestamp_type == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
                ? "MONOTONIC"
                : "UNKNOWN/NON-MONOTONIC",
                timestamp_source == V4L2_BUF_FLAG_TSTAMP_SRC_EOF
                ? "EOF"
                : (
                    timestamp_source == V4L2_BUF_FLAG_TSTAMP_SRC_SOE
                    ? "SOE"
                    : "OTHER"
                )
            );
        }


        uint64_t v36_dq_vblank_sequence = 0;
        uint64_t v36_dq_vblank_ns = 0;

        if (o.phase_profile &&
            drmCrtcGetSequence(
                drmfd,
                crtc_id,
                &v36_dq_vblank_sequence,
                &v36_dq_vblank_ns) < 0) {

            perror("drmCrtcGetSequence at DQ");
            run_failed = true;
            break;
        }


        /* Verify the first modeset after its completed OUT fence. */
        if (target_mode_applied && !target_mode_verified) {
            drmModeCrtc* applied = drmModeGetCrtc(drmfd, crtc_id);

            if (!applied ||
                !applied->mode_valid ||
                !same_mode_timing(&applied->mode, &target_mode)) {

                fprintf(
                    stderr,
                    "V3.7 could not verify that the target CRTC timing "
                    "became active.\n"
                );

                if (applied)
                    drmModeFreeCrtc(applied);

                run_failed = true;
                break;
            }

            fprintf(
                stderr,
                "V3.7 target timing verified active: %u.%03u Hz\n",
                mode_refresh_millihz(&applied->mode) / 1000U,
                mode_refresh_millihz(&applied->mode) % 1000U
            );

            drmModeFreeCrtc(applied);
            target_mode_verified = true;
        }


        /*
         * Rockchip HDMI-RX low-latency fence.
         */
        int in_fence =
            extract_fence_fd(
                &b
            );


        if (in_fence < 0) {
            missing_fences++;

            fprintf(
                stderr,
                "frame %" PRIu64
                ": missing/invalid "
                "Rockchip fence fd\n",
                frames
            );

            if (o.enable_low_latency) {
                fprintf(
                    stderr,
                    "Acquire fences are mandatory in the characterized "
                    "Rockchip low-latency path.\n"
                );

                run_failed = true;
                break;
            }
        }


        /*
         * Preferred path:
         *
         * hand capture fence directly
         * to KMS via IN_FENCE_FD.
         *
         * Fallback only when the selected
         * DRM plane lacks the property.
         */
        if (!pp.in_fence_fd &&
            in_fence >= 0) {

            if (wait_fd(
                in_fence,
                1000) < 0) {

                perror(
                    "wait HDMI-RX fence"
                );

                close(
                    in_fence
                );

                run_failed = true;
                break;
            }

            acquire_waits++;
        }


        const bool async_this_commit =
            o.async_flip &&
            displayed >= 0;

        const bool modeset_this_commit =
            o.target_refresh_millihz &&
            !target_mode_applied;

        int out_fence = -1;

        struct flip_wait flip = {
            .done = false,
            .signal_ns = 0,
        };


        drmModeAtomicReq* ar =
            drmModeAtomicAlloc();


        if (!ar) {
            fprintf(
                stderr,
                "drmModeAtomicAlloc failed\n"
            );

            if (in_fence >= 0)
                close(in_fence);

            run_failed = true;
            break;
        }


        int add_result;

        if (async_this_commit) {
            add_result =
                add_async_plane_props(
                    ar,
                    plane->plane_id,
                    &pp,
                    bufs[b.index].fb_id,
                    in_fence
                );
        }
        else {
            add_result =
                add_plane_props(
                    drmfd,
                    ar,
                    plane->plane_id,
                    &pp,
                    crtc_id,
                    bufs[b.index].fb_id,
                    w,
                    h,
                    in_fence
                );
        }


        if (add_result < 0) {

            perror(
                async_this_commit
                ? "add async plane props"
                : "add plane props"
            );

            drmModeAtomicFree(ar);

            if (in_fence >= 0)
                close(in_fence);

            run_failed = true;
            break;
        }


        if (modeset_this_commit &&
            add_modeset_props(
                ar,
                conn->connector_id,
                crtc_id,
                &mp,
                target_mode_blob) < 0) {

            perror("add V3.7 modeset properties");
            drmModeAtomicFree(ar);

            if (in_fence >= 0)
                close(in_fence);

            run_failed = true;
            break;
        }


        if (!async_this_commit &&
            drmModeAtomicAddProperty(
            ar,
            crtc_id,
            pp.out_fence_ptr,
            (uint64_t)(uintptr_t)
            &out_fence) < 0) {

            perror(
                "add OUT_FENCE_PTR"
            );

            drmModeAtomicFree(ar);

            if (in_fence >= 0)
                close(in_fence);

            run_failed = true;
            break;
        }


        uint64_t v36_commit_vblank_sequence = 0;
        uint64_t v36_commit_vblank_ns = 0;

        if (o.phase_profile &&
            drmCrtcGetSequence(
                drmfd,
                crtc_id,
                &v36_commit_vblank_sequence,
                &v36_commit_vblank_ns) < 0) {

            perror("drmCrtcGetSequence before commit");
            drmModeAtomicFree(ar);

            if (in_fence >= 0)
                close(in_fence);

            run_failed = true;
            break;
        }


        const uint64_t
            v3_commit_begin_ns =
            v3_mono_ns();


        uint32_t commit_flags =
            modeset_this_commit
            ? DRM_MODE_ATOMIC_ALLOW_MODESET
            : DRM_MODE_ATOMIC_NONBLOCK;


        if (async_this_commit) {
            commit_flags |=
                DRM_MODE_PAGE_FLIP_ASYNC |
                DRM_MODE_PAGE_FLIP_EVENT;
        }


        int cr =
            drmModeAtomicCommit(
                drmfd,
                ar,
                commit_flags,
                async_this_commit
                ? &flip
                : NULL
            );


        const uint64_t
            v3_commit_end_ns =
            v3_mono_ns();


        int saved =
            errno;


        drmModeAtomicFree(ar);


        /*
         * Once atomic commit returns,
         * DRM has consumed/reference-counted
         * the IN_FENCE_FD property.
         */
        if (in_fence >= 0)
            close(in_fence);


        errno =
            saved;


        if (cr < 0) {
            perror(
                "drmModeAtomicCommit"
            );

            if (async_this_commit) {
                fprintf(
                    stderr,
                    "The kernel/driver rejected the pure async flip. "
                    "This is a valid V3.5 unsupported result, not a "
                    "V3.4 baseline failure.\n"
                );
            }

            run_failed = true;
            break;
        }


        if (modeset_this_commit) {
            target_mode_applied = true;

            fprintf(
                stderr,
                "V3.7 target timing applied: %u.%03u Hz\n",
                mode_refresh_millihz(&target_mode) / 1000U,
                mode_refresh_millihz(&target_mode) % 1000U
            );
        }


        if (!async_this_commit &&
            out_fence < 0) {

            fprintf(
                stderr,
                "Atomic commit succeeded "
                "but did not return an "
                "out-fence.\n"
            );

            run_failed = true;
            break;
        }


        /*
         * V3.8 controlled overlap probe.
         *
         * The normal Rockchip path keeps one atomic update outstanding until
         * its OUT fence signals.  Once, after warm-up, watch the capture fd
         * and that OUT fence together.  If a new capture arrives first, issue
         * a real second nonblocking atomic commit.  Mainline DRM helpers reject
         * this situation with EBUSY; that is useful evidence, not a failure.
         * The rejected candidate is waited and returned to HDMI-RX, deliberately
         * advancing capture phase by one frame without violating ownership.
         */
        bool v38_overlap_this_iteration = false;
        struct v4l2_buffer v38_b;
        struct v4l2_plane v38_p[VIDEO_MAX_PLANES];
        int v38_out_fence = -1;

        if (o.early_submit &&
            !v38_probe_done &&
            frames >= 120 &&
            displayed >= 0 &&
            !async_this_commit &&
            !modeset_this_commit) {

            struct pollfd pfds[2] = {
                { .fd = out_fence, .events = POLLIN },
                { .fd = vfd, .events = POLLIN | POLLPRI },
            };

            int probe_poll;
            do {
                probe_poll = poll(pfds, 2, 1000);
            } while (probe_poll < 0 && errno == EINTR);

            if (probe_poll < 0) {
                perror("V3.8 overlap poll");
                run_failed = true;
                break;
            }

            if (probe_poll > 0 &&
                (pfds[1].revents & (POLLIN | POLLPRI)) &&
                !(pfds[0].revents & POLLIN)) {

                memset(&v38_b, 0, sizeof(v38_b));
                memset(v38_p, 0, sizeof(v38_p));

                if (dqbuf_one(vfd, &v38_b, v38_p) < 0) {
                    perror("V3.8 early VIDIOC_DQBUF");
                    run_failed = true;
                    break;
                }

                if (v38_b.index >= req.count) {
                    fprintf(stderr, "V3.8 bad early buffer index %u\n", v38_b.index);
                    run_failed = true;
                    break;
                }

                bufs[v38_b.index].queued = false;
                v38_early_dq_before_out++;
                v38_probe_done = true;

                int v38_in_fence = extract_fence_fd(&v38_b);

                if (v38_in_fence < 0) {
                    fprintf(stderr, "V3.8 early buffer had no acquire fence\n");
                    missing_fences++;
                    run_failed = true;
                    break;
                }

                drmModeAtomicReq* v38_ar = drmModeAtomicAlloc();
                if (!v38_ar ||
                    add_plane_props(
                        drmfd,
                        v38_ar,
                        plane->plane_id,
                        &pp,
                        crtc_id,
                        bufs[v38_b.index].fb_id,
                        w,
                        h,
                        v38_in_fence) < 0 ||
                    drmModeAtomicAddProperty(
                        v38_ar,
                        crtc_id,
                        pp.out_fence_ptr,
                        (uint64_t)(uintptr_t)&v38_out_fence) < 0) {

                    perror("V3.8 construct overlapping atomic request");
                    if (v38_ar)
                        drmModeAtomicFree(v38_ar);
                    close(v38_in_fence);
                    run_failed = true;
                    break;
                }

                int v38_cr = drmModeAtomicCommit(
                    drmfd,
                    v38_ar,
                    DRM_MODE_ATOMIC_NONBLOCK,
                    NULL
                );
                int v38_saved = errno;

                drmModeAtomicFree(v38_ar);
                errno = v38_saved;

                if (v38_cr == 0) {
                    close(v38_in_fence);
                    v38_overlap_accepted++;
                    v38_overlap_this_iteration = true;
                    fprintf(stderr,
                        "V3.8: overlapping nonblocking atomic commit accepted.\n");
                }
                else {
                    if (errno == EBUSY) {
                        v38_overlap_ebusy++;
                        fprintf(stderr,
                            "V3.8: overlap rejected with EBUSY; phase-prime drop follows.\n");
                    }
                    else {
                        v38_overlap_other_reject++;
                        fprintf(stderr,
                            "V3.8: overlap rejected with errno=%d (%s); "
                            "phase-prime drop follows.\n",
                            errno,
                            strerror(errno));
                    }

                    if (v38_out_fence >= 0) {
                        close(v38_out_fence);
                        v38_out_fence = -1;
                    }

                    /* The failed atomic commit did not consume the producer. */
                    if (v38_in_fence >= 0) {
                        if (wait_fd(v38_in_fence, 1000) < 0) {
                            perror("V3.8 wait rejected candidate acquire fence");
                            close(v38_in_fence);
                            run_failed = true;
                            break;
                        }
                        close(v38_in_fence);
                    }

                    if (qbuf_one(vfd, v38_b.index) < 0) {
                        perror("V3.8 QBUF rejected candidate");
                        run_failed = true;
                        break;
                    }

                    bufs[v38_b.index].queued = true;
                    v38_intentional_drops++;
                    v38_drops_pending++;
                }
            }
            else if (probe_poll > 0 && (pfds[0].revents & POLLIN)) {
                v38_out_before_dq++;
            }
        }


        uint64_t v3_out_signal_ns;


        if (async_this_commit) {
            if (wait_flip_event(
                drmfd,
                &flip,
                1000) < 0) {

                perror("wait async page-flip event");
                run_failed = true;
                break;
            }

            v3_out_signal_ns =
                flip.signal_ns;
        }
        else {
            /*
             * Normal V3.4 path: OUT_FENCE_PTR remains the display-side
             * lifetime guard.  The async UAPI cannot accept this changing
             * CRTC property, so V3.5 uses the page-flip completion event.
             */
            if (wait_fd(
                out_fence,
                1000) < 0) {

                perror("wait KMS out-fence");
                close(out_fence);
                run_failed = true;
                break;
            }

            v3_out_signal_ns =
                v3_mono_ns();

            close(out_fence);
        }


        uint64_t v36_out_vblank_sequence = 0;
        uint64_t v36_out_vblank_ns = 0;

        if (o.phase_profile &&
            drmCrtcGetSequence(
                drmfd,
                crtc_id,
                &v36_out_vblank_sequence,
                &v36_out_vblank_ns) < 0) {

            perror("drmCrtcGetSequence after completion");
            run_failed = true;
            break;
        }


        /* ------------------------------------------------------------- */
        /* Record measurement sample                                     */
        /* ------------------------------------------------------------- */

        if (g_sample_count <
            MAX_SAMPLES) {

            struct frame_sample* vs =
                &g_samples[
                    g_sample_count++
                ];


            vs->frame_no =
                frames + 1;

            vs->sequence =
                v3_sequence;

            vs->index =
                b.index;

            vs->fence_present =
                in_fence >= 0;

            vs->async_commit =
                async_this_commit;

            vs->early_submit =
                o.early_submit;

            vs->overlap_commit =
                false;

            vs->intentional_drops_before =
                v38_drops_before_current;

            vs->v4l2_ts_ns =
                v3_v4l2_ts_ns;

            vs->dq_ns =
                v3_dq_ns;

            vs->commit_begin_ns =
                v3_commit_begin_ns;

            vs->commit_end_ns =
                v3_commit_end_ns;

            vs->out_signal_ns =
                v3_out_signal_ns;

            vs->qbuf_ns =
                0;

            vs->v4l2_flags =
                v36_v4l2_flags;

            vs->phase_valid =
                o.phase_profile;

            vs->mode_period_ns =
                o.phase_profile
                ? phase_mode_period_ns
                : 0;

            vs->dq_vblank_sequence =
                v36_dq_vblank_sequence;

            vs->dq_vblank_ns =
                v36_dq_vblank_ns;

            vs->commit_vblank_sequence =
                v36_commit_vblank_sequence;

            vs->commit_vblank_ns =
                v36_commit_vblank_ns;

            vs->out_vblank_sequence =
                v36_out_vblank_sequence;

            vs->out_vblank_ns =
                v36_out_vblank_ns;
        }


        /*
         * Return the PREVIOUS displayed
         * capture buffer to HDMI-RX.
         *
         * The newly submitted buffer becomes
         * "displayed" below and remains held
         * until the following completed update.
         */
        if (displayed >= 0 &&
            !bufs[displayed].queued) {

            if (qbuf_one(
                vfd,
                displayed) < 0) {

                perror(
                    "VIDIOC_QBUF recycle"
                );

                run_failed = true;
                break;
            }


            v34_mark_qbuf(
                (uint32_t)displayed,
                v3_mono_ns()
            );


            bufs[displayed].queued =
                true;
        }


        displayed =
            b.index;


        if (v38_overlap_this_iteration) {
            if (v38_out_fence < 0 || wait_fd(v38_out_fence, 1000) < 0) {
                perror("V3.8 wait accepted overlap OUT fence");
                if (v38_out_fence >= 0)
                    close(v38_out_fence);
                run_failed = true;
                break;
            }

            close(v38_out_fence);

            /* The accepted second commit replaced b; it is now safe to reuse. */
            if (!bufs[b.index].queued) {
                if (qbuf_one(vfd, b.index) < 0) {
                    perror("V3.8 QBUF superseded current buffer");
                    run_failed = true;
                    break;
                }
                v34_mark_qbuf(b.index, v3_mono_ns());
                bufs[b.index].queued = true;
            }

            displayed = v38_b.index;
            fprintf(stderr,
                "V3.8: accepted-overlap capability proven; stopping safely.\n");
            g_stop = 1;
        }


        frames++;


        if (o.verbose ||
            frames % 60 == 0) {

            fprintf(
                stderr,
                "frames=%" PRIu64
                " current=%u"
                " missing_fence=%" PRIu64
                " userspace_acquire_waits=%"
                PRIu64 "\n",

                frames,
                b.index,
                missing_fences,
                acquire_waits
            );
        }
    }


    /* --------------------------------------------------------------------- */
    /* Results                                                               */
    /* --------------------------------------------------------------------- */

    v3_print_summary();


    if (v3_write_csv(
        o.csv_path) < 0) {

        perror(
            "write timing CSV"
        );

        /*
         * Measurement run without its trace
         * is not considered successful.
         */
        run_failed = true;
    }
    else {
        fprintf(
            stderr,
            "Timing CSV written to: %s\n",
            o.csv_path
        );
    }


    fprintf(
        stderr,

        "\n=== RESULT ===\n"
        "frames presented:           %" PRIu64 "\n"
        "missing Rockchip fences:     %" PRIu64 "\n"
        "userspace acquire waits:     %" PRIu64 " (%s)\n"
        "phase profiler samples:      %zu (%s)\n"
        "V3.8 early DQ before OUT:     %" PRIu64 "\n"
        "V3.8 OUT before/equal DQ:     %" PRIu64 "\n"
        "V3.8 overlap accepted:        %" PRIu64 "\n"
        "V3.8 overlap EBUSY:           %" PRIu64 "\n"
        "V3.8 other overlap rejects:   %" PRIu64 "\n"
        "V3.8 intentional drops:       %" PRIu64 "\n"
        "last displayed capture buf: %d\n"
        "runtime status:              %s\n",

        frames,
        missing_fences,
        acquire_waits,

        pp.in_fence_fd
        ? "KMS accepted IN_FENCE_FD instead"
        : "plane had no IN_FENCE_FD",

        g_sample_count,
        o.phase_profile
        ? "enabled"
        : "disabled",

        v38_early_dq_before_out,
        v38_out_before_dq,
        v38_overlap_accepted,
        v38_overlap_ebusy,
        v38_overlap_other_reject,
        v38_intentional_drops,

        displayed,

        run_failed
        ? "FAILED"
        : "completed"
    );

    if (o.early_submit) {
        const char* outcome =
            v38_overlap_accepted
            ? "overlap accepted; capability proven"
            : (v38_overlap_ebusy
                ? "overlap rejected with EBUSY; phase-prime completed"
                : (v38_early_dq_before_out
                    ? "overlap rejected with a non-EBUSY error"
                    : "no capture-ready-before-OUT window observed"));

        fprintf(stderr, "V3.8 probe outcome:          %s\n", outcome);
    }


    if (xioctl(
        vfd,
        VIDIOC_STREAMOFF,
        &type) < 0) {

        perror(
            "VIDIOC_STREAMOFF"
        );

        run_failed = true;
    }


    if (run_failed) {
        fprintf(
            stderr,
            "Run terminated with one or more "
            "fatal runtime/measurement errors.\n"
        );
    }


    rc =
        (!run_failed &&
            frames > 0)
        ? 0
        : 1;


out:

    /* --------------------------------------------------------------------- */
    /* Cleanup                                                               */
    /* --------------------------------------------------------------------- */

    if (drmfd >= 0 &&
        mode_changed &&
        target_mode_applied &&
        original_mode_blob &&
        plane) {

        drmModeAtomicReq* restore = drmModeAtomicAlloc();
        bool restore_ok = restore != NULL;

        if (restore_ok &&
            (drmModeAtomicAddProperty(
                restore,
                plane->plane_id,
                pp.fb_id,
                0) < 0 ||
             drmModeAtomicAddProperty(
                restore,
                plane->plane_id,
                pp.crtc_id,
                0) < 0 ||
             add_modeset_props(
                restore,
                conn->connector_id,
                crtc_id,
                &mp,
                original_mode_blob) < 0)) {

            restore_ok = false;
        }

        if (restore_ok &&
            drmModeAtomicCommit(
                drmfd,
                restore,
                DRM_MODE_ATOMIC_ALLOW_MODESET,
                NULL) < 0) {

            restore_ok = false;
        }

        if (restore)
            drmModeAtomicFree(restore);

        if (restore_ok) {
            fprintf(stderr, "Restored original DRM mode after V3.7 run.\n");
        }
        else {
            perror("restore original DRM mode");
            rc = 1;
        }
    }


    if (drmfd >= 0 && target_mode_blob)
        drmModeDestroyPropertyBlob(drmfd, target_mode_blob);

    if (drmfd >= 0 && original_mode_blob)
        drmModeDestroyPropertyBlob(drmfd, original_mode_blob);


    if (drmfd >= 0) {

        for (int i = 0;
            i < MAX_BUFS;
            i++) {

            if (bufs[i].fb_id) {
                drmModeRmFB(
                    drmfd,
                    bufs[i].fb_id
                );
            }
        }
    }


    for (int i = 0;
        i < MAX_BUFS;
        i++) {

        if (bufs[i].dmabuf_fd >= 0)
            close(
                bufs[i].dmabuf_fd
            );
    }


    if (plane)
        drmModeFreePlane(
            plane
        );


    if (crtc)
        drmModeFreeCrtc(
            crtc
        );


    if (conn)
        drmModeFreeConnector(
            conn
        );


    if (res)
        drmModeFreeResources(
            res
        );


    if (drmfd >= 0)
        close(
            drmfd
        );


    if (vfd >= 0)
        close(
            vfd
        );


    /*
     * Restore original HDMI-RX low_latency
     * state instead of blindly disabling it.
     */
    if (ll_changed) {

        bool restore_yes =
            (
                ll_original == 'Y' ||
                ll_original == 'y' ||
                ll_original == '1'
                );


        if (write_bool_file(
            ll_path,
            restore_yes) < 0) {

            perror(
                "restore low_latency"
            );
        }
        else {
            fprintf(
                stderr,
                "Restored rockchip_hdmirx "
                "low_latency to %c\n",
                restore_yes
                ? 'Y'
                : 'N'
            );
        }
    }


    return rc;
}
