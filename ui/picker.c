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
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

static void draw_glyph(uint8_t *fb, uint32_t stride, uint32_t fb_w, uint32_t fb_h,
                        const struct font *f, unsigned char c, int x, int y, uint32_t color) {
    if (c >= f->num_glyphs) return;
    unsigned bytes_per_row = f->bytes_per_glyph / f->height;
    const unsigned char *g = f->glyphs + (size_t)c * f->bytes_per_glyph;
    for (unsigned row = 0; row < f->height; row++) {
        for (unsigned col = 0; col < f->width; col++) {
            unsigned char byte = g[row * bytes_per_row + col / 8];
            if (!(byte & (0x80 >> (col % 8)))) continue;
            for (int sy = 0; sy < FONT_SCALE; sy++) {
                int py = y + (int)row * FONT_SCALE + sy;
                if (py < 0 || py >= (int)fb_h) continue;
                uint32_t *rowptr = (uint32_t *)(fb + (size_t)py * stride);
                for (int sx = 0; sx < FONT_SCALE; sx++) {
                    int px = x + (int)col * FONT_SCALE + sx;
                    if (px < 0 || px >= (int)fb_w) continue;
                    rowptr[px] = color;
                }
            }
        }
    }
}

static void draw_text(uint8_t *fb, uint32_t stride, uint32_t fb_w, uint32_t fb_h,
                       const struct font *f, const char *s, int x, int y, uint32_t color) {
    int cx = x;
    for (; *s; s++) {
        draw_glyph(fb, stride, fb_w, fb_h, f, (unsigned char)*s, cx, y, color);
        cx += ((int)f->width + 2) * FONT_SCALE;
    }
}

static void fill_rect(uint8_t *fb, uint32_t stride, uint32_t fb_w, uint32_t fb_h,
                       int x, int y, int w, int h, uint32_t color) {
    for (int py = y; py < y + h && py < (int)fb_h; py++) {
        if (py < 0) continue;
        uint32_t *rowptr = (uint32_t *)(fb + (size_t)py * stride);
        for (int px = x; px < x + w && px < (int)fb_w; px++) {
            if (px < 0) continue;
            rowptr[px] = color;
        }
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

/* ---------------- touch input ---------------- */

static int bit_set(const unsigned long *bits, unsigned n) {
    return (bits[n / (sizeof(long) * 8)] >> (n % (sizeof(long) * 8))) & 1;
}

static int find_touch_device(char *path_out, size_t len) {
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
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0 &&
            (bit_set(absbits, ABS_MT_POSITION_X) || bit_set(absbits, ABS_X))) {
            snprintf(path_out, len, "%s", path);
            found = 0;
            close(fd);
            break;
        }
        close(fd);
    }
    closedir(dir);
    return found;
}

/* ---------------- layout + main loop ---------------- */

struct layout {
    int x, y, w, h;
};

static void compute_layout(struct layout *rows, int n, uint32_t fb_w, uint32_t fb_h) {
    int margin = fb_w / 40;
    int y = fb_h / 10;
    for (int i = 0; i < n; i++) {
        rows[i].x = margin;
        rows[i].y = y;
        rows[i].w = (int)fb_w - 2 * margin;
        rows[i].h = ROW_HEIGHT - 8;
        y += ROW_HEIGHT;
    }
}

static void render(struct drm_dev *d, const struct font *font, struct entry *entries, int n,
                    struct layout *rows, int pending) {
    fill_rect(d->map, d->stride, d->width, d->height, 0, 0, (int)d->width, (int)d->height, COLOR_BG);
    draw_text(d->map, d->stride, d->width, d->height, font,
              "Tap a kernel to select, tap again to boot it",
              rows[0].x, rows[0].y - ROW_HEIGHT + 12, COLOR_TEXT);
    for (int i = 0; i < n; i++) {
        uint32_t bg = (i == pending) ? COLOR_PENDING : (i % 2 ? COLOR_ROW_ALT : COLOR_BG);
        fill_rect(d->map, d->stride, d->width, d->height, rows[i].x, rows[i].y, rows[i].w, rows[i].h, bg);
        draw_text(d->map, d->stride, d->width, d->height, font, entries[i].title,
                  rows[i].x + 16, rows[i].y + (rows[i].h - (int)font->height * FONT_SCALE) / 2, COLOR_TEXT);
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

    struct layout rows[MAX_ENTRIES];
    compute_layout(rows, n, drm.width, drm.height);

    char touch_path[300];
    if (find_touch_device(touch_path, sizeof(touch_path)) != 0) {
        fprintf(stderr, "picker: no touch input device found\n");
        drm_close(&drm);
        return 1;
    }
    int touch_fd = open(touch_path, O_RDONLY);
    if (touch_fd < 0) {
        fprintf(stderr, "picker: cannot open %s: %s\n", touch_path, strerror(errno));
        drm_close(&drm);
        return 1;
    }

    int pending = -1;
    int selected = -1;
    int cur_x = -1, cur_y = -1, down = 0;

    render(&drm, &font, entries, n, rows, pending);

    struct input_event ev;
    while (selected < 0 && read(touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) cur_x = ev.value;
            else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) cur_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            down = ev.value;
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            static int was_down = 0;
            if (was_down && !down && cur_x >= 0 && cur_y >= 0) {
                int tapped = hit_test(rows, n, cur_x, cur_y);
                if (tapped >= 0) {
                    if (tapped == pending) {
                        selected = tapped;
                    } else {
                        pending = tapped;
                        render(&drm, &font, entries, n, rows, pending);
                    }
                }
            }
            was_down = down;
        }
    }

    close(touch_fd);
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
