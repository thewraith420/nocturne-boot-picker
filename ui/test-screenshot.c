/* Verifies the screenshot dump un-rotates correctly, using a synthetic
 * framebuffer - a transposed axis here would only surface as a garbled
 * image in the README. Includes picker.c so it tests the REAL static
 * function rather than a copy of its logic. */
#define main picker_real_main
#include "picker.c"
#undef main

#define PW 3000   /* physical panel */
#define PH 2000
static int fails, passes;
static void ck(int c, const char *m){ printf(c?"  [ok] %s\n":"  [FAIL] %s\n", m); c?passes++:fails++; }

int main(void) {
    struct drm_dev d = {0};
    d.width = PW; d.height = PH; d.stride = PW * 4;
    d.map = calloc((size_t)PW * PH, 4);

    struct picker_ctx c = { .drm = &d, .rot = ROT_270, .cw = PH, .ch = PW };
    g_ctx = &c; g_shot_dir = "/tmp";

    /* ROT_270: logical(0,0) -> physical(px=ly=0, py=cw-1-lx=PH-1).
     * Put a marker there; it must land at the PPM's first pixel. */
    *(uint32_t *)(d.map + (size_t)(PH - 1) * d.stride + 0) = 0x00FF0000; /* red */
    /* logical bottom-right (cw-1, ch-1) -> physical(PW-1, 0) */
    *(uint32_t *)(d.map + (size_t)0 * d.stride + (size_t)(PW - 1) * 4) = 0x000000FF; /* blue */

    screenshot("rot-test");

    FILE *f = fopen("/tmp/01-rot-test.ppm", "rb");
    ck(f != NULL, "screenshot file was written");
    if (!f) return 1;
    int w = 0, h = 0, maxv = 0;
    ck(fscanf(f, "P6 %d %d %d", &w, &h, &maxv) == 3, "PPM header parses");
    fgetc(f);
    ck(w == PH && h == PW, "dumped in LOGICAL orientation (2000x3000, not 3000x2000)");

    unsigned char px[3];
    fread(px, 1, 3, f);
    ck(px[0] == 0xFF && px[1] == 0 && px[2] == 0,
       "logical top-left pixel is the marker placed at its physical position");

    fseek(f, -3, SEEK_END);
    fread(px, 1, 3, f);
    ck(px[0] == 0 && px[1] == 0 && px[2] == 0xFF,
       "logical bottom-right pixel likewise");
    fclose(f);
    remove("/tmp/01-rot-test.ppm");
    printf("\npassed: %d  failed: %d\n", passes, fails);
    return fails ? 1 : 0;
}
