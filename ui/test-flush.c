/* Proves the optimised flush_cb is pixel-identical to the per-pixel
 * version it replaced, for every rotation.
 *
 * The reference below IS the old implementation, kept deliberately as
 * an oracle: it is obviously correct (it calls logical_to_physical for
 * each pixel, the same transform the header documents as verified
 * bijective) and slow, which is exactly what you want to check a fast
 * version against. Rotation bugs are expensive here - they cost a
 * reboot cycle to see - so the fast path should not ship on reasoning
 * alone. */
#define main picker_real_main
#include "picker.c"
#undef main

static void flush_reference(struct picker_ctx *ctx, const lv_area_t *area, uint8_t *px_map) {
    int w = area->x2 - area->x1 + 1;
    uint32_t *src = (uint32_t *)px_map;
    for (int ly = area->y1; ly <= area->y2; ly++) {
        for (int lx = area->x1; lx <= area->x2; lx++) {
            int px, py;
            logical_to_physical(ctx->rot, ctx->cw, ctx->ch, lx, ly, &px, &py);
            if (px < 0 || py < 0 || (uint32_t)px >= ctx->drm->width || (uint32_t)py >= ctx->drm->height) continue;
            uint32_t color = src[(ly - area->y1) * w + (lx - area->x1)];
            uint32_t *dst_row = (uint32_t *)(ctx->drm->map + (size_t)py * ctx->drm->stride);
            dst_row[px] = color;
        }
    }
}

#define PW 800
#define PH 600
static int fails, passes;

int main(void) {
    lv_init();
    size_t fbsz = (size_t)PW * PH * 4;
    struct drm_dev da = {0}, db = {0};
    da.width = db.width = PW; da.height = db.height = PH;
    da.stride = db.stride = PW * 4;
    da.map = malloc(fbsz); db.map = malloc(fbsz);

    struct picker_ctx ca = { .drm = &da }, cb = { .drm = &db };
    lv_display_t *disp = lv_display_create(PW, PH);   /* only for flush_ready */
    lv_display_set_user_data(disp, &cb);

    srand(12345);
    const int rots[4] = { ROT_0, ROT_90, ROT_180, ROT_270 };
    const char *names[4] = { "0", "90", "180", "270" };

    for (int r = 0; r < 4; r++) {
        int rot = rots[r];
        int cw = (rot == ROT_90 || rot == ROT_270) ? PH : PW;
        int ch = (rot == ROT_90 || rot == ROT_270) ? PW : PH;
        ca.rot = cb.rot = rot; ca.cw = cb.cw = cw; ca.ch = cb.ch = ch;

        int mismatches = 0;
        for (int trial = 0; trial < 60; trial++) {
            memset(da.map, 0xAA, fbsz);
            memset(db.map, 0xAA, fbsz);
            lv_area_t a;
            /* full-screen, single rows, single pixels, and random
             * sub-rectangles - partial refreshes produce all of these */
            if (trial == 0)      { a.x1 = 0; a.y1 = 0; a.x2 = cw - 1; a.y2 = ch - 1; }
            else if (trial == 1) { a.x1 = 0; a.y1 = ch / 2; a.x2 = cw - 1; a.y2 = ch / 2; }
            else if (trial == 2) { a.x1 = cw - 1; a.y1 = ch - 1; a.x2 = cw - 1; a.y2 = ch - 1; }
            else {
                a.x1 = rand() % cw; a.y1 = rand() % ch;
                a.x2 = a.x1 + rand() % (cw - a.x1);
                a.y2 = a.y1 + rand() % (ch - a.y1);
            }
            int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
            uint32_t *src = malloc((size_t)w * h * 4);
            for (int i = 0; i < w * h; i++) src[i] = (uint32_t)rand();

            flush_reference(&ca, &a, (uint8_t *)src);
            flush_cb(disp, &a, (uint8_t *)src);
            if (memcmp(da.map, db.map, fbsz) != 0) mismatches++;
            free(src);
        }
        if (mismatches == 0) { printf("  [ok] rot %-4s pixel-identical to the reference over 60 areas\n", names[r]); passes++; }
        else { printf("  [FAIL] rot %-4s differs from reference in %d/60 areas\n", names[r], mismatches); fails++; }
    }
    printf("\npassed: %d  failed: %d\n", passes, fails);
    return fails ? 1 : 0;
}
