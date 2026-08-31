/*
 * picker: the touch menu itself. Reads a menu.tsv (produced by
 * initramfs/discover-kernels.sh) of "title\tlinux\tinitrd\tcmdline"
 * lines, draws it on the first connected DRM output using raw
 * KMS + a dumb buffer (no compositor, no libinput - see main
 * README decision #2), waits for tap-then-confirm on one entry
 * (TWRP-style, see ui/README.md), and writes the selection back out
 * as shell-sourceable SELECTED_LINUX/SELECTED_INITRD/SELECTED_CMDLINE
 * assignments on stdout for initramfs/init to `.` source.
 *
 * Text is rendered from a real PSF2 console font (ui/font.psf, from
 * the standard `kbd` package's Lat38-VGA28x16) rather than a hand-
 * authored bitmap table - one less place to introduce silent,
 * hard-to-spot pixel-data mistakes.
 *
 * Safety net: if nothing is tapped within PICKER_TIMEOUT_SECS (default
 * 10, 0 disables it), auto-boots the first entry - this is meant to
 * replace GRUB's own menu, which has exactly this timeout-to-default
 * behavior, and a keyboardless device with no escape hatch otherwise
 * has no recovery path if touch ever fails to register. Also
 * cooperates with VT-switch requests (VT_SETMODE/signalfd, dropping
 * and reacquiring DRM master) so a foreground console switch during
 * development/debugging doesn't wedge the display - found by testing
 * on real hardware: without this, Ctrl+Alt+F1 hung the VT switch hard
 * enough to need a power cycle to recover.
 *
 * PICKER_ROTATE=0|90|180|270 (default 0) rotates rendered content and
 * touch coordinates together to match the panel's physical mounting
 * orientation - needed because there's no kernel-side panel-rotation
 * quirk for this hardware, so it has to be handled here.
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
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_ENTRIES 128
#define FONT_SCALE 3
#define ROW_HEIGHT (28 * FONT_SCALE + 24)
#define COLOR_BG 0x101010u
#define COLOR_TEXT 0xe0e0e0u
#define COLOR_ROW_ALT 0x181818u
#define COLOR_PENDING 0x203a5cu
#define DEFAULT_TIMEOUT_SECS 10
#define VT_RELEASE_SIG SIGUSR1
#define VT_ACQUIRE_SIG SIGUSR2

struct entry {
    char title[256];
    char linux_path[256];
    char initrd_path[256];
    char cmdline[512];
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
        char *fields[4] = {0};
        char *p = line;
        for (int i = 0; i < 4 && p; i++) {
            fields[i] = p;
            char *tab = strchr(p, '\t');
            if (tab) { *tab = '\0'; p = tab + 1; } else { p = NULL; }
        }
        if (!fields[0] || fields[0][0] == '\0') continue;
        snprintf(entries[n].title, sizeof(entries[n].title), "%s", fields[0] ? fields[0] : "");
        snprintf(entries[n].linux_path, sizeof(entries[n].linux_path), "%s", fields[1] ? fields[1] : "");
        snprintf(entries[n].initrd_path, sizeof(entries[n].initrd_path), "%s", fields[2] ? fields[2] : "");
        snprintf(entries[n].cmdline, sizeof(entries[n].cmdline), "%s", fields[3] ? fields[3] : "");
        n++;
    }
    fclose(fp);
    return n;
}

/* ---------------- rotation ---------------- */

enum { ROT_0, ROT_90, ROT_180, ROT_270 };

static int parse_rotation(void) {
    const char *s = getenv("PICKER_ROTATE");
    if (!s) return ROT_0;
    if (!strcmp(s, "90")) return ROT_90;
    if (!strcmp(s, "180")) return ROT_180;
    if (!strcmp(s, "270")) return ROT_270;
    return ROT_0;
}

/* Maps a point in the unrotated "logical" canvas (cw x ch) to the
 * physical framebuffer (ch x cw for 90/270, cw x ch for 0/180). */
static void logical_to_physical(int rot, int cw, int ch, int lx, int ly, int *px, int *py) {
    switch (rot) {
    case ROT_90:  *px = ch - 1 - ly; *py = lx; break;
    case ROT_180: *px = cw - 1 - lx; *py = ch - 1 - ly; break;
    case ROT_270: *px = ly; *py = cw - 1 - lx; break;
    default:      *px = lx; *py = ly; break;
    }
}

/* Inverse of logical_to_physical, for turning touch coordinates
 * (which arrive in physical panel space) back into logical space for
 * hit-testing against the (logical-space) row layout. */
static void physical_to_logical(int rot, int cw, int ch, int px, int py, int *lx, int *ly) {
    switch (rot) {
    case ROT_90:  *lx = py;          *ly = ch - 1 - px; break;
    case ROT_180: *lx = cw - 1 - px; *ly = ch - 1 - py; break;
    case ROT_270: *lx = cw - 1 - py; *ly = px;          break;
    default:      *lx = px;          *ly = py;          break;
    }
}

/* ---------------- PSF font ---------------- */

struct font {
    unsigned char *glyphs;
    unsigned width, height, bytes_per_glyph, num_glyphs;
};

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int font_load(struct font *f, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "picker: cannot open font %s: %s\n", path, strerror(errno));
        return -1;
    }
    unsigned char hdr[32];
    if (fread(hdr, 1, 2, fp) != 2) { fclose(fp); return -1; }

    if (hdr[0] == 0x36 && hdr[1] == 0x04) {
        unsigned char rest[2];
        if (fread(rest, 1, 2, fp) != 2) { fclose(fp); return -1; }
        f->width = 8;
        f->height = rest[1];
        f->bytes_per_glyph = rest[1];
        f->num_glyphs = (rest[0] & 1) ? 512 : 256;
    } else {
        if (fread(hdr + 2, 1, 30, fp) != 30) { fclose(fp); return -1; }
        if (rd32(hdr) != 0x864ab572u) {
            fprintf(stderr, "picker: %s is not a PSF1/PSF2 font\n", path);
            fclose(fp);
            return -1;
        }
        uint32_t headersize = rd32(hdr + 8);
        f->num_glyphs = rd32(hdr + 16);
        f->bytes_per_glyph = rd32(hdr + 20);
        f->height = rd32(hdr + 24);
        f->width = rd32(hdr + 28);
        if (fseek(fp, headersize, SEEK_SET) != 0) { fclose(fp); return -1; }
    }

    size_t total = (size_t)f->bytes_per_glyph * f->num_glyphs;
    f->glyphs = malloc(total);
    if (!f->glyphs || fread(f->glyphs, 1, total, fp) != total) {
        free(f->glyphs);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* ---------------- drawing (logical-space, rotation-aware) ---------------- */

struct canvas {
    uint8_t *fb;
    uint32_t stride;
    uint32_t phys_w, phys_h;
    int rot;
    int w, h; /* logical dimensions */
};

static void putpixel(struct canvas *cv, int lx, int ly, uint32_t color) {
    if (lx < 0 || ly < 0 || lx >= cv->w || ly >= cv->h) return;
    int px, py;
    logical_to_physical(cv->rot, cv->w, cv->h, lx, ly, &px, &py);
    if (px < 0 || py < 0 || (uint32_t)px >= cv->phys_w || (uint32_t)py >= cv->phys_h) return;
    uint32_t *rowptr = (uint32_t *)(cv->fb + (size_t)py * cv->stride);
    rowptr[px] = color;
}

static void fill_rect(struct canvas *cv, int x, int y, int w, int h, uint32_t color) {
    for (int ly = y; ly < y + h; ly++)
        for (int lx = x; lx < x + w; lx++)
            putpixel(cv, lx, ly, color);
}

static void draw_glyph(struct canvas *cv, const struct font *f, unsigned char c, int x, int y, uint32_t color) {
    if (c >= f->num_glyphs) return;
    unsigned bytes_per_row = f->bytes_per_glyph / f->height;
    const unsigned char *g = f->glyphs + (size_t)c * f->bytes_per_glyph;
    for (unsigned row = 0; row < f->height; row++) {
        for (unsigned col = 0; col < f->width; col++) {
            unsigned char byte = g[row * bytes_per_row + col / 8];
            if (!(byte & (0x80 >> (col % 8)))) continue;
            for (int sy = 0; sy < FONT_SCALE; sy++)
                for (int sx = 0; sx < FONT_SCALE; sx++)
                    putpixel(cv, x + (int)col * FONT_SCALE + sx, y + (int)row * FONT_SCALE + sy, color);
        }
    }
}

static void draw_text(struct canvas *cv, const struct font *f, const char *s, int x, int y, uint32_t color) {
    int cx = x;
    for (; *s; s++) {
        draw_glyph(cv, f, (unsigned char)*s, cx, y, color);
        cx += ((int)f->width + 2) * FONT_SCALE;
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

static int touch_open(struct touch_dev *t) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;
    struct dirent *de;
    int found = -1;
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
        int code_x, code_y;
        if (bit_set(absbits, ABS_MT_POSITION_X)) {
            code_x = ABS_MT_POSITION_X;
            code_y = ABS_MT_POSITION_Y;
        } else if (bit_set(absbits, ABS_X)) {
            code_x = ABS_X;
            code_y = ABS_Y;
        } else {
            close(fd);
            continue;
        }
        if (ioctl(fd, EVIOCGABS(code_x), &t->abs_x) < 0 || ioctl(fd, EVIOCGABS(code_y), &t->abs_y) < 0) {
            close(fd);
            continue;
        }
        t->fd = fd;
        t->code_x = code_x;
        t->code_y = code_y;
        found = 0;
        break;
    }
    closedir(dir);
    return found;
}

/* Raw touch device coordinates arrive in the panel's fixed physical
 * orientation (the digitizer is laminated to the panel), independent
 * of how we choose to rotate rendered content - so this scales into
 * physical framebuffer space first, then applies the same rotation
 * used for drawing (inverted) to land in logical space for hit_test. */
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

/* ---------------- layout + rendering ---------------- */

struct layout {
    int x, y, w, h;
};

static void compute_layout(struct layout *rows, int n, int cw, int ch) {
    (void)ch;
    int margin = cw / 40;
    int y = ch / 10;
    for (int i = 0; i < n; i++) {
        rows[i].x = margin;
        rows[i].y = y;
        rows[i].w = cw - 2 * margin;
        rows[i].h = ROW_HEIGHT - 8;
        y += ROW_HEIGHT;
    }
}

static void render(struct canvas *cv, const struct font *font, struct entry *entries, int n,
                    struct layout *rows, int pending, int seconds_left) {
    fill_rect(cv, 0, 0, cv->w, cv->h, COLOR_BG);
    draw_text(cv, font, "Tap a kernel to select, tap again to boot it",
              rows[0].x, rows[0].y - ROW_HEIGHT + 12, COLOR_TEXT);
    if (seconds_left > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Booting default in %d...", seconds_left);
        draw_text(cv, font, msg, rows[0].x, rows[0].y - ROW_HEIGHT + 12 + (int)font->height * FONT_SCALE + 8,
                  COLOR_TEXT);
    }
    for (int i = 0; i < n; i++) {
        uint32_t bg = (i == pending) ? COLOR_PENDING : (i % 2 ? COLOR_ROW_ALT : COLOR_BG);
        fill_rect(cv, rows[i].x, rows[i].y, rows[i].w, rows[i].h, bg);
        draw_text(cv, font, entries[i].title, rows[i].x + 16,
                  rows[i].y + (rows[i].h - (int)font->height * FONT_SCALE) / 2, COLOR_TEXT);
    }
}

static int hit_test(struct layout *rows, int n, int x, int y) {
    for (int i = 0; i < n; i++) {
        if (x >= rows[i].x && x < rows[i].x + rows[i].w && y >= rows[i].y && y < rows[i].y + rows[i].h)
            return i;
    }
    return -1;
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <menu.tsv> [font.psf]\n", argv[0]);
        return 2;
    }
    const char *font_path = argc > 2 ? argv[2] : "/etc/picker-font.psf";

    int timeout_secs = DEFAULT_TIMEOUT_SECS;
    const char *timeout_env = getenv("PICKER_TIMEOUT_SECS");
    if (timeout_env) timeout_secs = atoi(timeout_env);

    struct entry entries[MAX_ENTRIES];
    int n = load_entries(argv[1], entries, MAX_ENTRIES);
    if (n <= 0) {
        fprintf(stderr, "picker: no kernel entries found in %s\n", argv[1]);
        return 1;
    }

    struct font font;
    if (font_load(&font, font_path) != 0) return 1;

    struct drm_dev drm;
    if (drm_open_first_connected(&drm) != 0) {
        fprintf(stderr, "picker: no connected DRM output found\n");
        return 1;
    }

    int rot = parse_rotation();
    struct canvas cv = {
        .fb = drm.map,
        .stride = drm.stride,
        .phys_w = drm.width,
        .phys_h = drm.height,
        .rot = rot,
        .w = (rot == ROT_90 || rot == ROT_270) ? (int)drm.height : (int)drm.width,
        .h = (rot == ROT_90 || rot == ROT_270) ? (int)drm.width : (int)drm.height,
    };

    struct layout rows[MAX_ENTRIES];
    compute_layout(rows, n, cv.w, cv.h);

    struct touch_dev touch;
    if (touch_open(&touch) != 0) {
        fprintf(stderr, "picker: no touch input device found\n");
        drm_close(&drm);
        return 1;
    }

    int vt_fd = vt_setup();
    int sig_fd = signalfd_setup();
    int timer_fd = -1;
    int seconds_left = timeout_secs;
    if (timeout_secs > 0) {
        timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        struct itimerspec its = {.it_value = {.tv_sec = 1}, .it_interval = {.tv_sec = 1}};
        if (timer_fd >= 0) timerfd_settime(timer_fd, 0, &its, NULL);
    }

    int pending = -1;
    int selected = -1;
    int cur_x = -1, cur_y = -1, down = 0, was_down = 0;
    int have_master = 1;

    render(&cv, &font, entries, n, rows, pending, seconds_left);

    struct pollfd fds[3];
    while (selected < 0) {
        int nfds = 0;
        int touch_idx = -1, sig_idx = -1, timer_idx = -1;
        fds[nfds].fd = touch.fd; fds[nfds].events = POLLIN; touch_idx = nfds++;
        if (sig_fd >= 0) { fds[nfds].fd = sig_fd; fds[nfds].events = POLLIN; sig_idx = nfds++; }
        if (timer_fd >= 0) { fds[nfds].fd = timer_fd; fds[nfds].events = POLLIN; timer_idx = nfds++; }

        int pr = poll(fds, nfds, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
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
                        render(&cv, &font, entries, n, rows, pending, seconds_left);
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
                    selected = 0;
                } else if (have_master) {
                    render(&cv, &font, entries, n, rows, pending, seconds_left);
                }
            }
        }

        if (selected >= 0) break;

        if (fds[touch_idx].revents & POLLIN) {
            struct input_event ev;
            while (read(touch.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_ABS) {
                    if (ev.code == touch.code_x) cur_x = ev.value;
                    else if (ev.code == touch.code_y) cur_y = ev.value;
                } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    down = ev.value;
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    if (have_master && down && timer_fd >= 0 && seconds_left > 0) {
                        /* first interaction: cancel the auto-boot timeout */
                        struct itimerspec off = {0};
                        timerfd_settime(timer_fd, 0, &off, NULL);
                        seconds_left = 0;
                        if (have_master) render(&cv, &font, entries, n, rows, pending, 0);
                    }
                    /* Ignore taps while the VT (and so the display) isn't
                     * ours - a selection made on a screen the user can't
                     * see would be surprising at best. */
                    if (have_master && was_down && !down && cur_x >= 0 && cur_y >= 0) {
                        int lx, ly;
                        touch_to_logical(&touch, rot, cv.w, cv.h, drm.width, drm.height, cur_x, cur_y, &lx, &ly);
                        int tapped = hit_test(rows, n, lx, ly);
                        if (tapped >= 0) {
                            if (tapped == pending) {
                                selected = tapped;
                            } else {
                                pending = tapped;
                                if (have_master) render(&cv, &font, entries, n, rows, pending, seconds_left);
                            }
                        }
                    }
                    was_down = down;
                }
            }
        }
    }

    close(touch.fd);
    if (sig_fd >= 0) close(sig_fd);
    if (timer_fd >= 0) close(timer_fd);
    if (vt_fd >= 0) close(vt_fd);
    drm_close(&drm);

    if (selected < 0) {
        fprintf(stderr, "picker: touch input ended with no selection\n");
        return 1;
    }

    shell_quote(stdout, "SELECTED_LINUX", entries[selected].linux_path);
    shell_quote(stdout, "SELECTED_INITRD", entries[selected].initrd_path);
    shell_quote(stdout, "SELECTED_CMDLINE", entries[selected].cmdline);
    return 0;
}
