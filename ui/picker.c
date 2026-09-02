/*
 * picker: the touch menu itself. Reads a menu.tsv (produced by
 * initramfs/discover-kernels.sh) of "title\tlinux\tinitrd\tcmdline"
 * lines, draws a TWRP-style list on the first connected DRM output
 * using LVGL (main README decision #2, reopened after real-hardware
 * feedback that a plain text list wasn't the goal - see ui/README.md),
 * waits for tap -> confirm-dialog -> confirm on one entry, and writes
 * the selection back out as shell-sourceable
 * SELECTED_LINUX/SELECTED_INITRD/SELECTED_CMDLINE assignments on
 * stdout for initramfs/init to `.` source.
 *
 * LVGL is deliberately kept rotation-agnostic (lv_display_set_rotation
 * is never called): testing showed LVGL's own rotation support hands
 * flush_cb a buffer still laid out in *logical* (unrotated) space, and
 * separately hangs outright when combined with
 * LV_DISPLAY_RENDER_MODE_FULL. Rather than depend on that and a second,
 * possibly differently-conventioned rotation implementation inside
 * LVGL, PICKER_ROTATE is handled by this file's own logical_to_physical
 * / physical_to_logical transform (bijectivity verified with a
 * standalone test harness) at exactly two points: the flush callback
 * (logical LVGL render -> physical framebuffer) and touch input
 * (physical touch digitizer -> logical LVGL coordinates). LVGL's own
 * display is created at the already-swapped *logical* resolution and
 * never told about rotation at all.
 *
 * Safety net: if nothing is tapped within PICKER_TIMEOUT_SECS (default
 * 10, 0 disables it), auto-boots the first entry - GRUB's own menu has
 * exactly this timeout-to-default behavior, and a keyboardless device
 * with no escape hatch otherwise has no recovery path if touch ever
 * fails to register. Also cooperates with VT-switch requests
 * (VT_SETMODE/signalfd, dropping and reacquiring DRM master) so a
 * foreground console switch during development/debugging doesn't wedge
 * the display - found by testing on real hardware: without this,
 * Ctrl+Alt+F1 hung the VT switch hard enough to need a power cycle.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "lvgl.h"

#define MAX_ENTRIES 128
#define DEFAULT_TIMEOUT_SECS 10
/* Pixel Slate panel: 3000x2000 @ 12.3" -> sqrt(3000^2+2000^2)/12.3
 * =~ 293 px/inch. The theme scales its padding/spacing from this, and
 * it's what makes the size constants below mean real-world distances. */
#define PANEL_DPI 293
#define DIALOG_BTN_H 115   /* ~1cm at 293 PPI - comfortable touch target */
#define ROW_H 130          /* kernel list row: ~1.1cm, fits the 36px font */
/* Edit dialog. The keyboard takes the bottom of the screen and the
 * dialog is capped to what remains, so these two are related: raising
 * one shrinks the other. 45% of a 3000px-tall logical display leaves
 * ~337px per key row, comfortably over the ~1cm touch target. */
#define KEYBOARD_PCT_H 45
#define EDIT_TA_H 220      /* ~4 wrapped lines of the 36px font */
/* Confirm dialog's 2x2 button grid. These two are coupled: the row has
 * to fit 2*DIALOG_BTN_W plus one DIALOG_BTN_GAP, so widening the
 * buttons back to 50% makes any nonzero gap overflow and wrap the grid
 * into a 4x1 stack. 47%+47% leaves 6% of the footer for the gap, which
 * at this dialog width is ~60px of room for a 28px gap. Asserted in
 * test-edit-layout.c so the pair cannot drift apart unnoticed. */
#define DIALOG_BTN_W_PCT 47
#define DIALOG_BTN_GAP 28
#define VT_RELEASE_SIG SIGUSR1
#define VT_ACQUIRE_SIG SIGUSR2
#define POLL_PERIOD_MS 30

struct entry {
    char title[256];
    char linux_path[256];
    char initrd_path[256];
    char cmdline[512];
    int is_default;
};

/* ---------------- menu.tsv ---------------- */

static int load_entries(const char *path, struct entry *entries, int max) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "picker: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    char line[1200];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        char *fields[5] = {0};
        char *p = line;
        for (int i = 0; i < 5 && p; i++) {
            fields[i] = p;
            char *tab = strchr(p, '\t');
            if (tab) { *tab = '\0'; p = tab + 1; } else { p = NULL; }
        }
        if (!fields[0] || fields[0][0] == '\0') continue;
        snprintf(entries[n].title, sizeof(entries[n].title), "%s", fields[0] ? fields[0] : "");
        snprintf(entries[n].linux_path, sizeof(entries[n].linux_path), "%s", fields[1] ? fields[1] : "");
        snprintf(entries[n].initrd_path, sizeof(entries[n].initrd_path), "%s", fields[2] ? fields[2] : "");
        snprintf(entries[n].cmdline, sizeof(entries[n].cmdline), "%s", fields[3] ? fields[3] : "");
        entries[n].is_default = (fields[4] && fields[4][0] == '1');
        n++;
    }
    fclose(fp);
    return n;
}

/* ---------------- rotation (see file header - LVGL is not told about this) ---------------- */

enum { ROT_0, ROT_90, ROT_180, ROT_270 };

static int parse_rotation(void) {
    const char *s = getenv("PICKER_ROTATE");
    if (!s) return ROT_0;
    if (!strcmp(s, "90")) return ROT_90;
    if (!strcmp(s, "180")) return ROT_180;
    if (!strcmp(s, "270")) return ROT_270;
    return ROT_0;
}

/* Logical (LVGL's render space, cw x ch) -> physical framebuffer
 * (ch x cw for 90/270, cw x ch for 0/180). Verified bijective with
 * clean round-trips for all four values via a standalone test harness. */
static void logical_to_physical(int rot, int cw, int ch, int lx, int ly, int *px, int *py) {
    switch (rot) {
    case ROT_90:  *px = ch - 1 - ly; *py = lx; break;
    case ROT_180: *px = cw - 1 - lx; *py = ch - 1 - ly; break;
    case ROT_270: *px = ly; *py = cw - 1 - lx; break;
    default:      *px = lx; *py = ly; break;
    }
}

static void physical_to_logical(int rot, int cw, int ch, int px, int py, int *lx, int *ly) {
    switch (rot) {
    case ROT_90:  *lx = py;          *ly = ch - 1 - px; break;
    case ROT_180: *lx = cw - 1 - px; *ly = ch - 1 - py; break;
    case ROT_270: *lx = cw - 1 - py; *ly = px;          break;
    default:      *lx = px;          *ly = py;          break;
    }
}

/* ---------------- DRM/KMS ---------------- */

struct drm_dev {
    int fd;
    uint32_t conn_id, crtc_id, fb_id, handle;
    uint32_t width, height, stride, size;
    uint8_t *map;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc;
};

static int drm_try_open(struct drm_dev *d, const char *path) {
    memset(d, 0, sizeof(*d));
    d->fd = open(path, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        fprintf(stderr, "picker: %s: open failed: %s\n", path, strerror(errno));
        return -1;
    }

    drmModeRes *res = drmModeGetResources(d->fd);
    if (!res) {
        fprintf(stderr, "picker: %s: drmModeGetResources failed: %s\n", path, strerror(errno));
        close(d->fd);
        return -1;
    }

    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(d->fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        drmModeFreeConnector(c);
    }
    if (!conn) {
        fprintf(stderr, "picker: %s: no connected connector with a mode\n", path);
        drmModeFreeResources(res);
        close(d->fd);
        return -1;
    }

    d->mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { d->mode = conn->modes[i]; break; }
    }
    d->conn_id = conn->connector_id;

    uint32_t crtc_id = 0;
    drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(d->fd, conn->encoder_id) : NULL;
    if (enc && enc->crtc_id) {
        crtc_id = enc->crtc_id;
    } else {
        for (int i = 0; i < res->count_encoders && !crtc_id; i++) {
            drmModeEncoder *e = drmModeGetEncoder(d->fd, res->encoders[i]);
            if (!e) continue;
            for (int j = 0; j < res->count_crtcs; j++) {
                if (e->possible_crtcs & (1u << j)) { crtc_id = res->crtcs[j]; break; }
            }
            drmModeFreeEncoder(e);
        }
    }
    if (enc) drmModeFreeEncoder(enc);
    if (!crtc_id) {
        fprintf(stderr, "picker: %s: connector %u has no usable encoder/crtc\n", path, d->conn_id);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(d->fd);
        return -1;
    }
    d->crtc_id = crtc_id;
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    struct drm_mode_create_dumb creq = {0};
    creq.width = d->mode.hdisplay;
    creq.height = d->mode.vdisplay;
    creq.bpp = 32;
    if (drmIoctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        fprintf(stderr, "picker: %s: DRM_IOCTL_MODE_CREATE_DUMB failed: %s\n", path, strerror(errno));
        close(d->fd);
        return -1;
    }
    d->width = creq.width;
    d->height = creq.height;
    d->stride = creq.pitch;
    d->size = creq.size;
    d->handle = creq.handle;

    if (drmModeAddFB(d->fd, d->width, d->height, 24, 32, d->stride, d->handle, &d->fb_id) < 0) {
        fprintf(stderr, "picker: %s: drmModeAddFB failed: %s\n", path, strerror(errno));
        close(d->fd);
        return -1;
    }

    struct drm_mode_map_dumb mreq = {0};
    mreq.handle = d->handle;
    if (drmIoctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        fprintf(stderr, "picker: %s: DRM_IOCTL_MODE_MAP_DUMB failed: %s\n", path, strerror(errno));
        close(d->fd);
        return -1;
    }

    d->map = mmap(0, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)mreq.offset);
    if (d->map == MAP_FAILED) {
        fprintf(stderr, "picker: %s: mmap of dumb buffer failed: %s\n", path, strerror(errno));
        close(d->fd);
        return -1;
    }
    memset(d->map, 0, d->size);

    d->saved_crtc = drmModeGetCrtc(d->fd, d->crtc_id);
    if (drmModeSetCrtc(d->fd, d->crtc_id, d->fb_id, 0, 0, &d->conn_id, 1, &d->mode) < 0) {
        fprintf(stderr, "picker: %s: drmModeSetCrtc failed: %s (need DRM master - "
                        "is another display server running?)\n", path, strerror(errno));
        munmap(d->map, d->size);
        close(d->fd);
        return -1;
    }
    return 0;
}

static int drm_open_first_connected(struct drm_dev *d) {
    for (int i = 0; i < 4; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (access(path, F_OK) != 0) continue;
        if (drm_try_open(d, path) == 0) return 0;
    }
    return -1;
}

static void drm_close(struct drm_dev *d) {
    if (d->saved_crtc) {
        drmModeSetCrtc(d->fd, d->saved_crtc->crtc_id, d->saved_crtc->buffer_id,
                        d->saved_crtc->x, d->saved_crtc->y, &d->conn_id, 1, &d->saved_crtc->mode);
        drmModeFreeCrtc(d->saved_crtc);
    }
    if (d->map) munmap(d->map, d->size);
    if (d->fd >= 0) close(d->fd);
}

/* ---------------- VT switch cooperation ----------------
 *
 * Without this, holding DRM master through a VT switch (Ctrl+Alt+Fn,
 * or anything else that asks the kernel to change the active VT) hung
 * hard on real hardware - confirmed on the Slate, needed a power cycle
 * to recover. VT_PROCESS mode makes the kernel ask us via a signal
 * instead of just switching, so we can drop master first.
 */

static int vt_setup(void) {
    int vt_fd = open("/dev/tty0", O_RDWR);
    if (vt_fd < 0) {
        fprintf(stderr, "picker: cannot open /dev/tty0 for VT switch cooperation: %s "
                        "(continuing without it)\n", strerror(errno));
        return -1;
    }
    struct vt_mode mode = {0};
    mode.mode = VT_PROCESS;
    mode.relsig = VT_RELEASE_SIG;
    mode.acqsig = VT_ACQUIRE_SIG;
    if (ioctl(vt_fd, VT_SETMODE, &mode) < 0) {
        fprintf(stderr, "picker: VT_SETMODE failed: %s (continuing without VT switch cooperation)\n",
                strerror(errno));
        close(vt_fd);
        return -1;
    }
    return vt_fd;
}

static int signalfd_setup(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, VT_RELEASE_SIG);
    sigaddset(&mask, VT_ACQUIRE_SIG);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) return -1;
    return signalfd(-1, &mask, SFD_NONBLOCK);
}

/* ---------------- touch input ---------------- */

static int bit_set(const unsigned long *bits, unsigned n) {
    return (bits[n / (sizeof(long) * 8)] >> (n % (sizeof(long) * 8))) & 1;
}

struct touch_dev {
    int fd;
    int code_x, code_y;
    struct input_absinfo abs_x, abs_y;
};

static int device_is_direct(int fd) {
    unsigned long propbits[(INPUT_PROP_MAX / (sizeof(long) * 8)) + 1] = {0};
    if (ioctl(fd, EVIOCGPROP(sizeof(propbits)), propbits) < 0) return 0;
    return bit_set(propbits, INPUT_PROP_DIRECT);
}

/* Some hardware exposes several /dev/input/eventN nodes that all
 * satisfy some capability check - e.g. a combo Wacom pen+touch
 * controller advertising multiple pen/stylus/mouse sub-interfaces
 * alongside the actual finger digitizer. Capability bits alone aren't
 * enough to tell them apart. Two real-hardware rounds were needed to
 * find the right signal:
 *
 * 1. INPUT_PROP_DIRECT ("touchscreen, not pointer device") rules out
 *    plain pointer/mouse-emulation sub-interfaces - but on this
 *    hardware *three* of the five Wacom nodes report DIRECT (the real
 *    finger digitizer, plus two pen/stylus telemetry channels that
 *    also happen to be DIRECT), so DIRECT alone still isn't unique.
 * 2. True multitouch capability (ABS_MT_SLOT/ABS_MT_TRACKING_ID, not
 *    just a plain ABS_X/ABS_Y fallback) is what actually distinguishes
 *    the real finger digitizer from the pen-telemetry DIRECT nodes,
 *    which only ever report ABS_X/ABS_Y/ABS_PRESSURE/ABS_TILT_* -
 *    confirmed by decoding a real /proc/bus/input/devices dump: only
 *    one of the five nodes has ABS_MT_SLOT/ABS_MT_POSITION_X/
 *    ABS_MT_TRACKING_ID at all, and it's not the one either prior fix
 *    picked.
 *
 * So this scores every candidate as direct*2 + is_mt*1 and keeps the
 * highest, which puts a true-MT+DIRECT device above a DIRECT-but-
 * ABS_X-only one, which is in turn above a non-DIRECT match - ties go
 * to whichever is found first. */
static int touch_open(struct touch_dev *t) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;
    struct dirent *de;
    int found_fd = -1, found_direct = 0, found_is_mt = 0, found_score = -1;
    int found_code_x = 0, found_code_y = 0;
    struct input_absinfo found_abs_x = {0}, found_abs_y = {0};
    char found_path[300] = "";
    char found_name[256] = "?";

    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long absbits[(ABS_MAX / (sizeof(long) * 8)) + 1] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0) {
            close(fd);
            continue;
        }
        int code_x, code_y, is_mt;
        if (bit_set(absbits, ABS_MT_POSITION_X)) {
            code_x = ABS_MT_POSITION_X;
            code_y = ABS_MT_POSITION_Y;
            is_mt = 1;
        } else if (bit_set(absbits, ABS_X)) {
            code_x = ABS_X;
            code_y = ABS_Y;
            is_mt = 0;
        } else {
            close(fd);
            continue;
        }
        struct input_absinfo ax, ay;
        if (ioctl(fd, EVIOCGABS(code_x), &ax) < 0 || ioctl(fd, EVIOCGABS(code_y), &ay) < 0) {
            close(fd);
            continue;
        }

        int is_direct = device_is_direct(fd);
        int score = is_direct * 2 + is_mt;
        if (score > found_score) {
            if (found_fd >= 0) close(found_fd);
            found_fd = fd;
            found_code_x = code_x;
            found_code_y = code_y;
            found_abs_x = ax;
            found_abs_y = ay;
            found_direct = is_direct;
            found_is_mt = is_mt;
            found_score = score;
            snprintf(found_path, sizeof(found_path), "%s", path);
            char name[256] = "?";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            snprintf(found_name, sizeof(found_name), "%s", name);
        } else {
            close(fd);
        }
    }
    closedir(dir);
    if (found_fd < 0) return -1;

    fprintf(stderr, "picker: touch device: %s (\"%s\"), INPUT_PROP_DIRECT=%s, multitouch=%s\n",
            found_path, found_name, found_direct ? "yes" : "no", found_is_mt ? "yes" : "no");

    t->fd = found_fd;
    t->code_x = found_code_x;
    t->code_y = found_code_y;
    t->abs_x = found_abs_x;
    t->abs_y = found_abs_y;
    return 0;
}

/* Raw touch device coordinates arrive in the panel's fixed physical
 * orientation (the digitizer is laminated to the panel) - scale into
 * physical framebuffer space first, then into LVGL's logical space via
 * physical_to_logical (see file header on why LVGL itself is never
 * told about rotation). */
static void touch_to_logical(const struct touch_dev *t, int rot, int cw, int ch,
                              uint32_t phys_w, uint32_t phys_h, int raw_x, int raw_y, int *lx, int *ly) {
    int px = (t->abs_x.maximum > t->abs_x.minimum)
        ? (int)((int64_t)(raw_x - t->abs_x.minimum) * (int)phys_w / (t->abs_x.maximum - t->abs_x.minimum))
        : raw_x;
    int py = (t->abs_y.maximum > t->abs_y.minimum)
        ? (int)((int64_t)(raw_y - t->abs_y.minimum) * (int)phys_h / (t->abs_y.maximum - t->abs_y.minimum))
        : raw_y;
    physical_to_logical(rot, cw, ch, px, py, lx, ly);
}

/* ---------------- LVGL display + input glue ---------------- */

struct picker_ctx {
    struct drm_dev *drm;
    int rot;
    int cw, ch; /* logical dims, as passed to lv_display_create */
    lv_indev_t *indev;
    int touch_x, touch_y, touch_down;
};

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    struct picker_ctx *ctx = lv_display_get_user_data(disp);
    struct drm_dev *d = ctx->drm;
    const int cw = ctx->cw, ch = ctx->ch;
    const int aw = area->x2 - area->x1 + 1;   /* source row stride */

    /* Clamp once rather than bounds-testing every pixel. Given the
     * cw/ch invariant (they are the drm dimensions, swapped for 90/270)
     * a logical point inside the display always maps to a physical one
     * inside the framebuffer, so this is the only check needed. */
    const int x1 = area->x1 < 0 ? 0 : area->x1;
    const int y1 = area->y1 < 0 ? 0 : area->y1;
    const int x2 = area->x2 >= cw ? cw - 1 : area->x2;
    const int y2 = area->y2 >= ch ? ch - 1 : area->y2;
    if (x1 > x2 || y1 > y2) { lv_display_flush_ready(disp); return; }

    /* The fast paths below drop the old per-pixel bounds test, which
     * is only safe while cw/ch really are the framebuffer dimensions
     * (swapped for 90/270). That holds wherever picker sets them up,
     * but "holds today" is not a memory-safety argument: if it were
     * ever violated the specialised loops would write past the
     * framebuffer, trading a visible glitch for silent corruption. So
     * check it once per flush - two comparisons - and skip the frame
     * rather than scribble. A dropped frame in an impossible
     * configuration is a cheap price for that guarantee. */
    const int exp_cw = (ctx->rot == ROT_90 || ctx->rot == ROT_270) ? (int)d->height : (int)d->width;
    const int exp_ch = (ctx->rot == ROT_90 || ctx->rot == ROT_270) ? (int)d->width  : (int)d->height;
    if (cw != exp_cw || ch != exp_ch) {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "picker: display %dx%d does not match framebuffer %ux%u "
                            "for rotation - skipping flush\n", cw, ch, d->width, d->height);
        }
        lv_display_flush_ready(disp);
        return;
    }

    const int w = x2 - x1 + 1;
    uint8_t *const map = d->map;
    const size_t stride = d->stride;

    /* Specialised per rotation, with the case hoisted out of the inner
     * loop. Previously this called logical_to_physical() per pixel - a
     * switch and two multiplies six million times for a full-screen
     * refresh. Rotation is fixed for the life of the process, so the
     * work is loop-invariant; for ROT_0 the row is contiguous and
     * becomes a memcpy, and 90/270 walk a physical column by adding or
     * subtracting the stride. Proven pixel-identical to the old
     * per-pixel version for all four rotations in test-flush.c. */
    for (int ly = y1; ly <= y2; ly++) {
        const uint32_t *srow = (const uint32_t *)px_map
                             + (size_t)(ly - area->y1) * aw + (x1 - area->x1);
        switch (ctx->rot) {
        case ROT_90: {
            /* px = ch-1-ly (constant down the row), py = lx */
            uint8_t *p = map + (size_t)x1 * stride + (size_t)(ch - 1 - ly) * 4;
            for (int i = 0; i < w; i++, p += stride) *(uint32_t *)p = srow[i];
            break;
        }
        case ROT_180: {
            uint32_t *drow = (uint32_t *)(map + (size_t)(ch - 1 - ly) * stride)
                           + (cw - 1 - x1);
            for (int i = 0; i < w; i++) drow[-i] = srow[i];
            break;
        }
        case ROT_270: {
            /* px = ly (constant), py = cw-1-lx */
            uint8_t *p = map + (size_t)(cw - 1 - x1) * stride + (size_t)ly * 4;
            for (int i = 0; i < w; i++, p -= stride) *(uint32_t *)p = srow[i];
            break;
        }
        default:
            memcpy((uint32_t *)(map + (size_t)ly * stride) + x1, srow, (size_t)w * 4);
            break;
        }
    }
    lv_display_flush_ready(disp);
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    struct picker_ctx *ctx = lv_indev_get_user_data(indev);
    data->point.x = ctx->touch_x;
    data->point.y = ctx->touch_y;
    data->state = ctx->touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---------------- UI ---------------- */

/* ---------------- screenshots ----------------
 *
 * picker already mmaps the scanout buffer for its flush path, so a
 * screenshot is just a copy of memory it is already holding - no second
 * DRM readback path, no extra ioctls.
 *
 * Dumped in LOGICAL orientation (un-rotated through the same transform
 * flush_cb uses), so the file comes out the way the tablet is held
 * rather than the way the panel scans. A sideways screenshot needs
 * hand-rotating before it is any use in a README.
 *
 * Triggered by PICKER_SCREENSHOT_DIR rather than a signal: in the real
 * boot there is no shell in the initramfs to send one from, and
 * event-triggered dumps are repeatable across test rounds in a way that
 * "press the thing at the right moment" is not.
 *
 * PPM because it is ~15 lines of code and needs no zlib in the
 * initramfs; ui/ppm-to-png.sh converts afterwards on a normal machine. */
static struct picker_ctx *g_ctx;
static const char *g_shot_dir;
static const char *g_shot_pending;
static int g_shot_n;

static void screenshot(const char *tag) {
    if (!g_shot_dir || !g_ctx || !g_ctx->drm->map) return;
    struct drm_dev *d = g_ctx->drm;
    int cw = g_ctx->cw, ch = g_ctx->ch;

    char path[512];
    snprintf(path, sizeof(path), "%s/%02d-%s.ppm", g_shot_dir, ++g_shot_n, tag);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "picker: screenshot %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", cw, ch);

    unsigned char *row = malloc((size_t)cw * 3);
    if (!row) { fclose(f); return; }
    for (int ly = 0; ly < ch; ly++) {
        for (int lx = 0; lx < cw; lx++) {
            int px, py;
            logical_to_physical(g_ctx->rot, cw, ch, lx, ly, &px, &py);
            uint32_t c = 0;
            if (px >= 0 && py >= 0 && (uint32_t)px < d->width && (uint32_t)py < d->height)
                c = *(uint32_t *)(d->map + (size_t)py * d->stride + (size_t)px * 4);
            row[lx * 3 + 0] = (c >> 16) & 0xff;  /* XRGB8888 */
            row[lx * 3 + 1] = (c >> 8) & 0xff;
            row[lx * 3 + 2] = c & 0xff;
        }
        fwrite(row, 1, (size_t)cw * 3, f);
    }
    free(row);
    fclose(f);
    fprintf(stderr, "picker: screenshot -> %s (%dx%d)\n", path, cw, ch);
}

/* Requested from inside an event callback, taken by the main loop after
 * the next lv_timer_handler(): the dialog that triggered it has not
 * been drawn yet at callback time, and forcing a redraw from inside
 * LVGL's own event dispatch is asking for re-entrancy trouble. */
static void screenshot_soon(const char *tag) {
    if (g_shot_dir) g_shot_pending = tag;
}

static volatile int g_selected = -1;
static volatile int g_set_default = 0;
static struct entry *g_entries;

static void open_confirm_dialog(int idx);

/* Nothing else to do here: the main loop exits as soon as g_selected
 * is set (checked right after this fires, since click handling happens
 * inside lv_timer_handler()), so there's no need to close the msgbox -
 * the whole display is torn down immediately after anyway. */
static void confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_user_data(e);
    g_selected = (int)(intptr_t)lv_obj_get_user_data(mbox);
}

/* Set Default also boots into this entry now, same as Boot - the user
 * is already looking at the confirm dialog for this specific entry, so
 * "remember this AND go" is the natural reading, not "remember this
 * but stay on the menu". initramfs/init persists the preference (see
 * initramfs/README.md) before kexec once it sees SET_DEFAULT=1. */
static void set_default_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_user_data(e);
    g_set_default = 1;
    g_selected = (int)(intptr_t)lv_obj_get_user_data(mbox);
}

static void cancel_cb(lv_event_t *e) {
    lv_msgbox_close_async(lv_event_get_user_data(e));
}

/* Edit: GRUB-style one-time cmdline tweak, never persisted - just
 * mutates this entry's in-memory copy for the rest of this run, same
 * as GRUB's own 'e' edit-before-boot. */
struct edit_ctx {
    int idx;
    lv_obj_t *mbox;
    lv_obj_t *kb;
    lv_obj_t *ta;
};

static void edit_close(struct edit_ctx *ctx) {
    lv_obj_delete_async(ctx->kb);
    lv_msgbox_close_async(ctx->mbox);
    free(ctx);
}

static void edit_save_cb(lv_event_t *e) {
    struct edit_ctx *ctx = lv_event_get_user_data(e);
    snprintf(g_entries[ctx->idx].cmdline, sizeof(g_entries[ctx->idx].cmdline), "%s", lv_textarea_get_text(ctx->ta));
    int idx = ctx->idx;
    edit_close(ctx);
    open_confirm_dialog(idx);
}

static void edit_cancel_cb(lv_event_t *e) {
    struct edit_ctx *ctx = lv_event_get_user_data(e);
    int idx = ctx->idx;
    edit_close(ctx);
    open_confirm_dialog(idx);
}

static void edit_cb(lv_event_t *e) {
    lv_obj_t *confirm_mbox = lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(confirm_mbox);
    lv_msgbox_close_async(confirm_mbox);

    struct edit_ctx *ctx = malloc(sizeof(*ctx));
    ctx->idx = idx;

    ctx->mbox = lv_msgbox_create(NULL);
    /* lv_msgbox's class default width is a hardcoded LV_DPI_DEF*2
     * (260px), unrelated to the real display size - unreadably small
     * on this panel, so both dialogs size themselves explicitly. */
    /* Wider than the confirm dialog: this one has to show a 250+
     * character command line, not a kernel title. */
    lv_obj_set_width(ctx->mbox, lv_pct(92));
    lv_msgbox_add_title(ctx->mbox, "Edit boot command line");
    lv_msgbox_add_text(ctx->mbox, "One-time change - not saved for next boot.");

    ctx->ta = lv_textarea_create(lv_msgbox_get_content(ctx->mbox));
    /* NOT one_line: a real cmdline here is 250+ characters, and in
     * one-line mode the textarea shows a narrow horizontally-scrolled
     * slice of it - you cannot see what you are editing. Wrapped over a
     * few lines shows the whole thing. */
    lv_textarea_set_one_line(ctx->ta, false);
    /* Size BEFORE text: the text wraps against whatever width the
     * textarea has at set_text time, so setting it first wraps against
     * the default width and re-wraps later. Harmless here (measured -
     * the layout comes out identical either way) but there is no reason
     * to depend on the re-wrap. */
    lv_obj_set_width(ctx->ta, lv_pct(100));
    lv_obj_set_height(ctx->ta, EDIT_TA_H);
    lv_textarea_set_text(ctx->ta, g_entries[idx].cmdline);
    /* set_text leaves the cursor at the end; show the START of the
     * command line, which is the part worth reading first. */
    lv_textarea_set_cursor_pos(ctx->ta, 0);
    lv_obj_scroll_to_y(ctx->ta, 0, LV_ANIM_OFF);

    /* Parented to lv_layer_top(), NOT lv_screen_active().
     *
     * lv_msgbox_create(NULL) puts a backdrop object on lv_layer_top()
     * sized 100%x100% (lv_msgbox.c) and the dialog inside it. A
     * keyboard on the screen layer therefore sits UNDERNEATH that
     * backdrop entirely: it renders greyed-out behind the dim and the
     * backdrop swallows every tap meant for its keys. Found on real
     * hardware - the keyboard drew perfectly and was completely dead.
     *
     * Created after the msgbox, so as a later sibling of the backdrop
     * it draws above it and receives touches. Deliberately a SIBLING
     * rather than a child of the backdrop: edit_close() deletes the
     * keyboard explicitly, and as a child it would be deleted a second
     * time when the msgbox tears its backdrop down. */
    ctx->kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(ctx->kb, lv_pct(100), lv_pct(KEYBOARD_PCT_H));
    lv_obj_align(ctx->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(ctx->kb, ctx->ta);

    /* The keyboard owns the bottom half of the screen, so a centred
     * dialog fights it for space. Pin the dialog to the top and cap its
     * height at what is left. */
    lv_obj_set_style_max_height(ctx->mbox, lv_pct(100 - KEYBOARD_PCT_H - 4), 0);
    lv_obj_align(ctx->mbox, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *save_btn = lv_msgbox_add_footer_button(ctx->mbox, "Save");
    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(ctx->mbox, "Cancel");
    /* Same 43px hardcoded footer/header height as the confirm dialog. */
    lv_obj_set_height(lv_msgbox_get_footer(ctx->mbox), LV_SIZE_CONTENT);
    lv_obj_set_height(lv_msgbox_get_header(ctx->mbox), LV_SIZE_CONTENT);
    lv_obj_set_height(save_btn, DIALOG_BTN_H);
    lv_obj_set_height(cancel_btn, DIALOG_BTN_H);
    lv_obj_add_event_cb(save_btn, edit_save_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(cancel_btn, edit_cancel_cb, LV_EVENT_CLICKED, ctx);

    /* On hardware the textarea came up blank until the first keystroke,
     * even though the text was set and - measured in the headless
     * harness - laid out correctly and inside the visible content area.
     * So it is not layout or scrolling: the content simply was not
     * painted until some later event forced a redraw. Force it here,
     * the same remedy the VT-reacquire path above needs, and for the
     * same underlying reason: this display is driven manually, so
     * nothing else will decide a repaint is due.
     *
     * NOT verified headlessly - a dummy flush callback cannot reproduce
     * a real partial-render pass, which is exactly why this one had to
     * be found on a panel. */
    lv_obj_update_layout(ctx->mbox);
    lv_obj_invalidate(lv_layer_top());
    screenshot_soon("edit-dialog");
}

/* 2x2 footer grid (Edit/Set Default on top, Boot/Cancel below),
 * matching the approved mockup - lv_msgbox's footer is a plain flex
 * row by default, so it's wrapped into two rows of two by giving each
 * button 50% width. */
static void open_confirm_dialog(int idx) {
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    /* Same hardcoded-260px default as the edit dialog. */
    lv_obj_set_width(mbox, lv_pct(55));
    lv_obj_set_user_data(mbox, (void *)(intptr_t)idx);
    lv_msgbox_add_title(mbox, "Confirm boot");
    char body[300];
    snprintf(body, sizeof(body), "Boot into:\n\n%s", g_entries[idx].title);
    lv_msgbox_add_text(mbox, body);

    lv_obj_t *edit_btn = lv_msgbox_add_footer_button(mbox, "Edit");
    lv_obj_t *default_btn = lv_msgbox_add_footer_button(mbox, "Set Default");
    lv_obj_t *confirm_btn = lv_msgbox_add_footer_button(mbox, "Boot");
    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(mbox, "Cancel");

    lv_obj_t *footer = lv_msgbox_get_footer(mbox);
    /* The footer and header classes default to a hardcoded
     * LV_DPI_DEF/3 (43px) height - same "constant unrelated to the
     * real display" problem as lv_msgbox's width. Two rows of
     * DIALOG_BTN_H buttons need ~230px, so without this they render
     * as a squashed unreadable strip (seen on hardware). Let both
     * size to their content instead. */
    lv_obj_set_height(footer, LV_SIZE_CONTENT);
    lv_obj_set_height(lv_msgbox_get_header(mbox), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW_WRAP);
    /* The gap and the button width are a matched pair. Two 50%-width
     * buttons plus ANY nonzero gap exceed the row, so the grid wraps
     * into a 4x1 stack - found by testing, and the reason this was
     * pinned to zero gap and 50% width for a while. Narrowing the
     * buttons to DIALOG_BTN_W_PCT buys the room for a real gap, so the
     * buttons are no longer edge-to-edge. Change one, check the other. */
    lv_obj_set_style_pad_column(footer, DIALOG_BTN_GAP, 0);
    lv_obj_set_style_pad_row(footer, DIALOG_BTN_GAP, 0);
    lv_obj_t *footer_btns[4] = {edit_btn, default_btn, confirm_btn, cancel_btn};
    for (int i = 0; i < 4; i++) {
        lv_obj_set_width(footer_btns[i], lv_pct(DIALOG_BTN_W_PCT));
        /* Explicit touch target: the theme sizes these from the font
         * alone, which left them ~13px (about 1mm) tall on this panel -
         * measured, not guessed. DIALOG_BTN_H is ~1cm at the panel's
         * real DPI, which is a comfortable finger target. */
        lv_obj_set_height(footer_btns[i], DIALOG_BTN_H);
    }

    lv_obj_set_style_text_color(edit_btn, lv_color_hex(0xc9d3db), 0);
    lv_obj_set_style_text_color(default_btn, lv_color_hex(0xc9d3db), 0);
    lv_obj_set_style_text_color(cancel_btn, lv_color_hex(0x93a0aa), 0);
    lv_obj_set_style_text_color(confirm_btn, lv_color_hex(0xdce9fb), 0);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(0x3d7ee8), 0);
    lv_obj_set_style_bg_opa(confirm_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_side(confirm_btn, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_side(cancel_btn, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(confirm_btn, 1, 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_border_color(confirm_btn, lv_color_hex(0x232c35), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0x232c35), 0);

    lv_obj_add_event_cb(edit_btn, edit_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(default_btn, set_default_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(confirm_btn, confirm_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, mbox);
    screenshot_soon("confirm-dialog");
}

static void row_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    open_confirm_dialog(idx);
}

static void build_ui(struct entry *entries, int n, int timeout_secs, lv_obj_t **countdown_label_out) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_set_style_pad_row(scr, 10, 0);

    lv_obj_t *header = lv_label_create(scr);
    lv_label_set_text(header, LV_SYMBOL_POWER "  Select a kernel to boot");
    lv_obj_set_style_text_color(header, lv_color_hex(0x8ec6ff), 0);

    if (timeout_secs > 0) {
        lv_obj_t *cd = lv_label_create(scr);
        lv_obj_set_style_text_color(cd, lv_color_hex(0x808a94), 0);
        *countdown_label_out = cd;
    } else {
        *countdown_label_out = NULL;
    }

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    for (int i = 0; i < n; i++) {
        lv_obj_t *btn = lv_button_create(list);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, ROW_H);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1c2530), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a3a4d), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 10, 0);

        /* The default-entry checkmark sits at a fixed spot on the
         * right so it's never pushed out of view by a long title -
         * the title label gets ellipsis-truncated and a reserved
         * right margin instead of being centered over the whole row. */
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text_fmt(label, LV_SYMBOL_USB "  %s", entries[i].title);
        lv_obj_set_style_text_color(label, lv_color_hex(0xe8eef4), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, lv_pct(entries[i].is_default ? 82 : 100));
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);

        if (entries[i].is_default) {
            lv_obj_t *mark = lv_label_create(btn);
            lv_label_set_text(mark, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(mark, lv_color_hex(0x8ec6ff), 0);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -16, 0);
        }

        lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void lvgl_log_to_stderr(lv_log_level_t level, const char *buf) {
    (void)level;
    fprintf(stderr, "picker: lvgl: %s", buf);
}

static void shell_quote(FILE *out, const char *name, const char *value) {
    fprintf(out, "%s='", name);
    for (const char *p = value; *p; p++) {
        if (*p == '\'')
            fputs("'\\''", out);
        else
            fputc(*p, out);
    }
    fputs("'\n", out);
}

/* Retries a device open until it succeeds or PICKER_WAIT_SECS (default
 * 20, 0 disables) elapses. Exactly one of drm/touch is non-NULL - the
 * point is that the retry re-runs the REAL open, so "usable device"
 * keeps exactly one definition no matter how the criteria evolve.
 *
 * Reports how long it waited, because "touch appeared after 1.4s" and
 * "gave up after 20s" call for completely different next steps, and the
 * boot log is often the only account of a failure anyone gets. */
#define WAIT_POLL_MS 100
static int wait_for_device(const char *what, struct drm_dev *drm, struct touch_dev *touch) {
    const char *s = getenv("PICKER_WAIT_SECS");
    int limit_ms = (s ? atoi(s) : 20) * 1000;
    if (limit_ms < 0) limit_ms = 0;

    int waited = 0;
    for (;;) {
        if ((drm ? drm_open_first_connected(drm) : touch_open(touch)) == 0) {
            if (waited)
                fprintf(stderr, "picker: %s appeared after %d.%03ds\n",
                        what, waited / 1000, waited % 1000);
            return 0;
        }
        if (waited >= limit_ms) {
            if (limit_ms)
                fprintf(stderr, "picker: gave up waiting for %s after %ds\n",
                        what, limit_ms / 1000);
            return -1;
        }
        usleep(WAIT_POLL_MS * 1000);
        waited += WAIT_POLL_MS;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <menu.tsv>\n", argv[0]);
        return 2;
    }

    int timeout_secs = DEFAULT_TIMEOUT_SECS;
    const char *timeout_env = getenv("PICKER_TIMEOUT_SECS");
    if (timeout_env) timeout_secs = atoi(timeout_env);

    struct entry entries[MAX_ENTRIES];
    int n = load_entries(argv[1], entries, MAX_ENTRIES);
    if (n <= 0) {
        fprintf(stderr, "picker: no kernel entries found in %s\n", argv[1]);
        return 1;
    }
    g_entries = entries;

    /* Booted from the initramfs we are in a footrace with driver probe:
     * init reaches this point within ~0.85s of the kernel starting
     * (measured from a real boot log), while i915 and the I2C-HID touch
     * controller are still enumerating. Run by hand on a booted system
     * everything settled minutes ago, which is why this never showed up
     * in testing - the first two real boot attempts both died here with
     * "no touch input device found" while the panel was merely late.
     *
     * The retry lives HERE, not in init, on purpose. What counts as a
     * usable touch device is decided by touch_open()'s scoring
     * (INPUT_PROP_DIRECT + multitouch, after five Wacom sub-interfaces
     * once made a capability-bit match pick the wrong node). init can
     * only approximate that from shell, and its approximation - "does
     * any /dev/input/event* exist" - was satisfied instantly by the
     * unrelated event0-2 that are present from the start, so it waited
     * for nothing and picker still found no touch. A proxy for a check
     * is a second definition that can disagree with the real one; the
     * component that owns the criteria should own the waiting. */
    struct drm_dev drm;
    if (wait_for_device("connected DRM output", &drm, NULL) != 0) {
        fprintf(stderr, "picker: no connected DRM output found\n");
        return 1;
    }

    struct touch_dev touch;
    if (wait_for_device("touch input device", NULL, &touch) != 0) {
        fprintf(stderr, "picker: no touch input device found\n");
        drm_close(&drm);
        return 1;
    }

    int rot = parse_rotation();
    struct picker_ctx ctx = {
        .drm = &drm,
        .rot = rot,
        .cw = (rot == ROT_90 || rot == ROT_270) ? (int)drm.height : (int)drm.width,
        .ch = (rot == ROT_90 || rot == ROT_270) ? (int)drm.width : (int)drm.height,
    };

    g_ctx = &ctx;
    /* Directory must already exist - picker does not create it, so a
     * typo'd path fails loudly at the first dump rather than silently
     * scattering files somewhere unexpected. */
    g_shot_dir = getenv("PICKER_SCREENSHOT_DIR");

    lv_init();
    /* Route LVGL's own warnings to stderr - never stdout, which
     * carries the SELECTED_* contract that initramfs/init sources.
     * Worth having on: an exhausted allocator inside LVGL surfaces
     * here as a plain "No memory" line instead of an unexplained
     * freeze (see the LV_STDLIB_CLIB note in lv_conf.h). */
    lv_log_register_print_cb(lvgl_log_to_stderr);
    lv_display_t *disp = lv_display_create(ctx.cw, ctx.ch);
    /* Must be set before lv_theme_default_init(), which samples it once. */
    lv_display_set_dpi(disp, PANEL_DPI);
    lv_display_set_user_data(disp, &ctx);
    lv_display_set_flush_cb(disp, flush_cb);
    size_t buf_size = (size_t)ctx.cw * 64 * 4; /* partial buffer, 64 logical rows */
    void *lvgl_buf = malloc(buf_size);
    if (!lvgl_buf) {
        fprintf(stderr, "picker: out of memory allocating LVGL draw buffer\n");
        drm_close(&drm);
        return 1;
    }
    lv_display_set_buffers(disp, lvgl_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_theme_t *theme = lv_theme_default_init(disp, lv_color_hex(0x3d7ee8), lv_color_hex(0x8ec6ff), true, LV_FONT_DEFAULT);
    lv_display_set_theme(disp, theme);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    lv_indev_set_user_data(indev, &ctx);
    ctx.indev = indev;

    lv_obj_t *countdown_label = NULL;
    build_ui(entries, n, timeout_secs, &countdown_label);

    int vt_fd = vt_setup();
    int sig_fd = signalfd_setup();
    int timer_fd = -1;
    int seconds_left = timeout_secs;
    int countdown_cancelled = 0;
    /* Distinguishes the timeout's auto-boot from a real tap. Without
     * this both look identical downstream, and a boot where the panel
     * stayed dark and the timeout fired reported itself as "booted user
     * selection" - which reads as though someone chose it. */
    int g_selected_by_timeout = 0;
    if (timeout_secs > 0) {
        timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        struct itimerspec its = {.it_value = {.tv_sec = 1}, .it_interval = {.tv_sec = 1}};
        if (timer_fd >= 0) timerfd_settime(timer_fd, 0, &its, NULL);
        if (countdown_label) {
            lv_label_set_text_fmt(countdown_label, "Booting default in %ds - tap to choose", seconds_left);
        }
    }

    int have_master = 1;
    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    lv_timer_handler();

    struct pollfd fds[3];
    while (g_selected < 0) {
        int nfds = 0;
        int touch_idx = -1, sig_idx = -1, timer_idx = -1;
        fds[nfds].fd = touch.fd; fds[nfds].events = POLLIN; touch_idx = nfds++;
        if (sig_fd >= 0) { fds[nfds].fd = sig_fd; fds[nfds].events = POLLIN; sig_idx = nfds++; }
        if (timer_fd >= 0) { fds[nfds].fd = timer_fd; fds[nfds].events = POLLIN; timer_idx = nfds++; }

        int pr = poll(fds, nfds, POLL_PERIOD_MS);
        if (pr < 0 && errno != EINTR) break;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t elapsed_ms = (uint32_t)((now.tv_sec - last_tick.tv_sec) * 1000 +
                                          (now.tv_nsec - last_tick.tv_nsec) / 1000000);
        if (elapsed_ms > 0) {
            lv_tick_inc(elapsed_ms);
            last_tick = now;
        }

        if (sig_idx >= 0 && (fds[sig_idx].revents & POLLIN)) {
            struct signalfd_siginfo si;
            if (read(sig_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                if (si.ssi_signo == VT_RELEASE_SIG) {
                    drmDropMaster(drm.fd);
                    have_master = 0;
                    if (vt_fd >= 0) ioctl(vt_fd, VT_RELDISP, 1);
                } else if (si.ssi_signo == VT_ACQUIRE_SIG) {
                    if (drmSetMaster(drm.fd) == 0) {
                        have_master = 1;
                        drmModeSetCrtc(drm.fd, drm.crtc_id, drm.fb_id, 0, 0, &drm.conn_id, 1, &drm.mode);
                        /* Both layers: dialogs and the on-screen
                         * keyboard live on lv_layer_top() (msgbox puts
                         * its backdrop there), so invalidating only the
                         * active screen would leave an open dialog
                         * unpainted after a VT switch back. */
                        lv_obj_invalidate(lv_screen_active());
                        lv_obj_invalidate(lv_layer_top());
                    }
                    if (vt_fd >= 0) ioctl(vt_fd, VT_RELDISP, VT_ACKACQ);
                }
            }
        }

        if (timer_idx >= 0 && (fds[timer_idx].revents & POLLIN)) {
            uint64_t ticks;
            if (read(timer_fd, &ticks, sizeof(ticks)) == (ssize_t)sizeof(ticks)) {
                seconds_left -= (int)ticks;
                if (seconds_left <= 0) {
                    g_selected = 0;
                    g_selected_by_timeout = 1;
                } else if (countdown_label) {
                    lv_label_set_text_fmt(countdown_label, "Booting default in %ds - tap to choose", seconds_left);
                }
            }
        }
        if (g_selected >= 0) break;

        if (fds[touch_idx].revents & POLLIN) {
            struct input_event ev;
            static int last_raw_x = -1, last_raw_y = -1;
            while (read(touch.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                int new_down = -1; /* -1 = this event doesn't carry a down/up state */
                if (ev.type == EV_ABS) {
                    if (ev.code == touch.code_x) {
                        last_raw_x = ev.value;
                    } else if (ev.code == touch.code_y) {
                        last_raw_y = ev.value;
                    } else if (ev.code == ABS_MT_TRACKING_ID) {
                        /* Type B multitouch (hid_multitouch, which is
                         * what this hardware's touch stack actually
                         * uses) signals finger down/up via tracking ID
                         * rather than BTN_TOUCH - confirmed on real
                         * hardware that this device sends no BTN_TOUCH
                         * at all, which is why touch still didn't
                         * register even after selecting the right
                         * INPUT_PROP_DIRECT device. -1 means lifted. */
                        new_down = (ev.value != -1);
                    }
                    if (last_raw_x >= 0 && last_raw_y >= 0) {
                        int lx, ly;
                        touch_to_logical(&touch, rot, ctx.cw, ctx.ch, drm.width, drm.height,
                                          last_raw_x, last_raw_y, &lx, &ly);
                        ctx.touch_x = lx;
                        ctx.touch_y = ly;
                    }
                } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    new_down = ev.value;
                }

                if (new_down >= 0 && new_down != ctx.touch_down) {
                    ctx.touch_down = new_down;
                    if (new_down && timer_fd >= 0 && !countdown_cancelled) {
                        /* first touch: cancel the auto-boot countdown */
                        countdown_cancelled = 1;
                        struct itimerspec off = {0};
                        timerfd_settime(timer_fd, 0, &off, NULL);
                        if (countdown_label) lv_label_set_text(countdown_label, "");
                    }
                }
            }
        }

        if (have_master) lv_timer_handler();

        /* After the render, so the dialog that asked for this is
         * actually on screen. The first one is the menu itself. */
        if (g_shot_dir && have_master) {
            if (g_shot_pending) {
                screenshot(g_shot_pending);
                g_shot_pending = NULL;
            } else if (g_shot_n == 0) {
                screenshot("menu");
            }
        }
    }

    close(touch.fd);
    if (sig_fd >= 0) close(sig_fd);
    if (timer_fd >= 0) close(timer_fd);
    if (vt_fd >= 0) close(vt_fd);
    drm_close(&drm);
    free(lvgl_buf);

    if (g_selected < 0) {
        fprintf(stderr, "picker: touch input ended with no selection\n");
        return 1;
    }

    shell_quote(stdout, "SELECTED_LINUX", entries[g_selected].linux_path);
    shell_quote(stdout, "SELECTED_INITRD", entries[g_selected].initrd_path);
    shell_quote(stdout, "SELECTED_CMDLINE", entries[g_selected].cmdline);
    if (g_set_default) shell_quote(stdout, "SET_DEFAULT", "1");
    shell_quote(stdout, "SELECTED_BY", g_selected_by_timeout ? "timeout" : "user");
    fprintf(stderr, "picker: selection made by %s\n",
            g_selected_by_timeout ? "TIMEOUT (nothing was tapped)" : "user tap");
    return 0;
}
