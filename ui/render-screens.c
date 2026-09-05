/* Renders the picker's real screens offscreen, for documentation.
 *
 * This is NOT a mockup and NOT a retouched photo: it includes picker.c
 * and calls the same build_ui() / open_confirm_dialog() / edit_cb() the
 * device runs, through picker's own flush_cb and screenshot() paths.
 * The only substitution is the destination - a malloc'd buffer standing
 * in for the DRM scanout mapping - so what comes out is exactly what
 * the panel would show for the code as it stands right now.
 *
 * That matters because the alternative ways to get a "current" picture
 * are a fresh photo (needs the hardware in hand) or editing an old one
 * (which invents pixels). This needs neither.
 *
 *   make render-screens && ./render-screens <menu.tsv> <outdir>
 */
#define main picker_real_main
#include "picker.c"
#undef main

#define PANEL_W 3000   /* physical panel; logical is the 270-rotated swap */
#define PANEL_H 2000

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: render-screens <menu.tsv> <outdir>\n");
        return 1;
    }
    static struct entry entries[MAX_ENTRIES];
    int n = load_entries(argv[1], entries, MAX_ENTRIES);
    if (n <= 0) { fprintf(stderr, "render-screens: no entries in %s\n", argv[1]); return 1; }
    g_entries = entries;

    /* A drm_dev whose "scanout buffer" is ordinary memory. Everything
     * downstream - flush_cb's rotation, screenshot's un-rotation - runs
     * exactly as it does on the device. */
    struct drm_dev drm = {0};
    drm.width = PANEL_W; drm.height = PANEL_H; drm.stride = PANEL_W * 4;
    drm.map = calloc((size_t)PANEL_W * PANEL_H, 4);
    if (!drm.map) return 1;

    int rot = ROT_270;
    struct picker_ctx ctx = {
        .drm = &drm, .rot = rot,
        .cw = (rot == ROT_90 || rot == ROT_270) ? PANEL_H : PANEL_W,
        .ch = (rot == ROT_90 || rot == ROT_270) ? PANEL_W : PANEL_H,
    };
    g_ctx = &ctx;
    g_shot_dir = argv[2];

    lv_init();
    lv_log_register_print_cb(lvgl_log_to_stderr);
    lv_display_t *disp = lv_display_create(ctx.cw, ctx.ch);
    lv_display_set_dpi(disp, PANEL_DPI);
    lv_display_set_user_data(disp, &ctx);
    lv_display_set_flush_cb(disp, flush_cb);
    size_t buf_size = (size_t)ctx.cw * 64 * 4;
    void *lvgl_buf = malloc(buf_size);
    if (!lvgl_buf) return 1;
    lv_display_set_buffers(disp, lvgl_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_theme_t *theme = lv_theme_default_init(disp, lv_color_hex(0x3d7ee8),
                                              lv_color_hex(0x8ec6ff), true, LV_FONT_DEFAULT);
    lv_display_set_theme(disp, theme);

    /* A couple of fake tarballs so the Install menu has something to
     * draw - discover-tarballs.sh supplies these for real. */
    static struct tarball tb[2] = {
        { "/home/bob/buildstuff/BobZKernel-7.2.3-pixel-slate-installer.tar.gz",
          "7.2.3-pixel-slate", "110M" },
        { "/home/bob/buildstuff/BobZKernel-7.2.2-pixel-slate-installer.tar.gz",
          "7.2.2-pixel-slate", "110M" },
    };
    g_tarballs = tb; g_tarball_n = 2;

    lv_obj_t *countdown_label = NULL;
    build_ui(entries, n, 30, &countdown_label);
    /* The countdown label only gets its text on the first timer tick,
     * so without this the render shows LVGL's placeholder "Text" where
     * the device shows the countdown - an inaccuracy introduced by the
     * renderer itself, which rather defeats the point. */
    if (countdown_label)
        lv_label_set_text_fmt(countdown_label, "Booting default in %ds - tap to choose", 30);
    lv_refr_now(disp);
    screenshot("menu");

    /* Walk the new menu structure using the real screen builders. */
    show_kernel_list();
    lv_refr_now(disp);
    screenshot("kernel-list");

    show_install_list();
    lv_refr_now(disp);
    screenshot("install-list");

    /* The progress screen, with a plausible run of real output. */
    show_install_progress("7.2.3-pixel-slate");
    prog_append("install-kernel: reading /home/bob/buildstuff/BobZKernel-7.2.3-pixel-slate-installer.tar.gz");
    prog_append("install-kernel: kernel release: 7.2.3-BobZKernel-pixel-slate");
    prog_append("install-kernel: remounting /mnt/root read-write");
    prog_append("install-kernel: extracting kernel and modules (this takes a moment)");
    prog_append("install-kernel: preparing chroot");
    prog_append("install-kernel: depmod 7.2.3-BobZKernel-pixel-slate");
    prog_append("install-kernel: update-initramfs -c -k 7.2.3-BobZKernel-pixel-slate (slow - do not power off)");
    lv_refr_now(disp);
    screenshot("install-progress");

    show_kernel_list();
    lv_refr_now(disp);
    /* The real confirm dialog, opened by the real function. */
    open_confirm_dialog(1);
    lv_refr_now(disp);
    g_shot_pending = NULL;          /* it queued its own request; take it here */
    screenshot("confirm-dialog");

    /* Drive Edit through its actual click handler rather than
     * reproducing what it builds - find the footer's first button on
     * the dialog that is now on layer_top. */
    lv_obj_t *top = lv_layer_top();
    lv_obj_t *backdrop = lv_obj_get_child(top, lv_obj_get_child_count(top) - 1);
    lv_obj_t *mbox = lv_obj_get_child(backdrop, 0);
    lv_obj_t *footer = lv_msgbox_get_footer(mbox);
    lv_obj_t *edit_btn = lv_obj_get_child(footer, 0);
    lv_obj_send_event(edit_btn, LV_EVENT_CLICKED, NULL);
    lv_timer_handler();             /* lets the close-async / build settle */
    lv_refr_now(disp);
    g_shot_pending = NULL;
    screenshot("edit-dialog");

    printf("rendered 3 screens into %s\n", argv[2]);
    return 0;
}
