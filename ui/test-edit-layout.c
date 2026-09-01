/* Headless regression test for the Edit dialog's z-order.
 *
 * The bug this exists for: the on-screen keyboard rendered perfectly and
 * was completely dead to touch. lv_msgbox_create(NULL) puts a backdrop
 * on lv_layer_top() at 100%x100% (lv_msgbox.c) with the dialog inside
 * it; a keyboard parented to lv_screen_active() therefore sits on a
 * LOWER layer entirely, so the backdrop both dims it and swallows every
 * tap aimed at its keys.
 *
 * Verified here rather than only on hardware because the failure is
 * purely structural - it is a fact about parents and sibling order,
 * which is exactly the sort of thing that can be checked without a
 * panel. Getting it wrong costs a reboot cycle to find out.
 *
 *   make test-edit-layout && ./test-edit-layout
 */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"

#define W 2000
#define H 3000

static int fails, passes;
static void ok(const char *m)  { printf("  [ok] %s\n", m); passes++; }
static void bad(const char *m) { printf("  [FAIL] %s\n", m); fails++; }
static void check(int cond, const char *m) { cond ? ok(m) : bad(m); }

static void dummy_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    (void)a; (void)px;
    lv_display_flush_ready(d);
}

/* Is `a` drawn above `b`, given they share a parent? Later children
 * render on top in LVGL. */
static int drawn_above(lv_obj_t *a, lv_obj_t *b) {
    return lv_obj_get_parent(a) == lv_obj_get_parent(b) &&
           lv_obj_get_index(a) > lv_obj_get_index(b);
}

static int areas_overlap(lv_obj_t *a, lv_obj_t *b) {
    lv_area_t aa, ba;
    lv_obj_get_coords(a, &aa);
    lv_obj_get_coords(b, &ba);
    return !(aa.x2 < ba.x1 || ba.x2 < aa.x1 || aa.y2 < ba.y1 || ba.y2 < aa.y1);
}

int main(void) {
    lv_init();
    lv_display_t *disp = lv_display_create(W, H);
    static uint8_t buf[W * 10 * 4];
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, dummy_flush);

    printf("edit dialog z-order (%dx%d logical)\n", W, H);

    /* Mirrors picker.c's edit_cb construction order. */
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_obj_set_width(mbox, lv_pct(92));
    lv_msgbox_add_title(mbox, "Edit boot command line");
    lv_obj_t *ta = lv_textarea_create(lv_msgbox_get_content(mbox));
    lv_textarea_set_one_line(ta, false);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_height(ta, 220);
    lv_textarea_set_text(ta,
        "root=UUID=076aa633-aff9-4f0f-98a7-f939eb74e7ff ro quiet splash "
        "module_blacklist=hid_google_hammer,cros_usbpd_notify "
        "i915.enable_dpcd_backlight=2 i915.enable_psr=0 "
        "crashkernel=2G-4G:320M,4G-32G:512M,32G-64G:1024M");
    lv_textarea_set_cursor_pos(ta, 0);
    lv_obj_scroll_to_y(ta, 0, LV_ANIM_OFF);

    lv_obj_t *kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(kb, lv_pct(100), lv_pct(45));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);

    lv_obj_set_style_max_height(mbox, lv_pct(100 - 45 - 4), 0);
    lv_obj_align(mbox, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_update_layout(lv_screen_active());
    lv_obj_update_layout(lv_layer_top());
    lv_refr_now(disp);

    lv_obj_t *backdrop = lv_obj_get_parent(mbox);

    /* --- the actual bug --- */
    check(lv_obj_get_parent(kb) == lv_layer_top(),
          "keyboard is on lv_layer_top(), not the screen layer");
    check(backdrop == NULL || lv_obj_get_parent(backdrop) == lv_layer_top(),
          "msgbox backdrop is on lv_layer_top() (the reason the old code failed)");
    check(drawn_above(kb, backdrop),
          "keyboard is a LATER sibling of the backdrop, so it draws above the dim");
    check(areas_overlap(kb, backdrop),
          "they do overlap - so sibling order genuinely decides who gets the tap");

    /* --- lifetime: sibling, not child, or edit_close's explicit
     *     delete would double-free when the msgbox tears the backdrop
     *     down --- */
    check(lv_obj_get_parent(kb) != backdrop,
          "keyboard is NOT a child of the backdrop (avoids double-delete on close)");

    /* --- layout: dialog and keyboard must not fight for the screen --- */
    lv_area_t ka, ma;
    lv_obj_get_coords(kb, &ka);
    lv_obj_get_coords(mbox, &ma);
    check(ma.y2 < ka.y1, "dialog sits entirely above the keyboard (no overlap)");
    check(ka.y2 <= H - 1 && ka.x1 <= 0 + 1 && ka.x2 >= W - 2,
          "keyboard spans the full width, pinned to the bottom");

    /* --- the textarea must show the cmdline, not a 1-line slice --- */
    check(lv_obj_get_height(ta) > 150,
          "cmdline textarea is multi-line (a 250+ char cmdline is unreadable on one)");
    check(lv_obj_get_width(ta) > W / 2,
          "cmdline textarea is wide enough to be worth reading");

    /* The blank-textarea bug was a repaint problem, not layout - but
     * assert the layout half here so a future change cannot quietly
     * push the text out of the visible content area instead. */
    lv_obj_t *content = lv_msgbox_get_content(mbox);
    lv_obj_t *lab = lv_textarea_get_label(ta);
    lv_area_t ca, la2;
    lv_obj_get_coords(content, &ca);
    lv_obj_get_coords(lab, &la2);
    check(la2.y2 >= ca.y1 && la2.y1 <= ca.y2,
          "cmdline text is inside the dialog's visible content area");
    check(lv_obj_get_scroll_y(ta) == 0,
          "textarea is scrolled to the top, showing the start of the cmdline");

    /* --- confirm dialog: the 2x2 grid must stay 2x2 ---
     * Button width and the flex gap are a matched pair: two 50%-wide
     * buttons plus any nonzero gap overflow the row and the grid wraps
     * into a 4x1 stack. That is a silent, purely visual failure, so
     * assert the shape rather than trusting the arithmetic. */
    lv_obj_t *cmbox = lv_msgbox_create(NULL);
    lv_obj_set_width(cmbox, lv_pct(55));
    lv_msgbox_add_title(cmbox, "Confirm boot");
    lv_msgbox_add_text(cmbox, "Boot into:\n\nUbuntu, with Linux 7.1.12-BobZKernel-pixel-slate");
    lv_obj_t *b[4];
    const char *names[4] = {"Edit", "Set Default", "Boot", "Cancel"};
    for (int i = 0; i < 4; i++) b[i] = lv_msgbox_add_footer_button(cmbox, names[i]);
    lv_obj_t *footer = lv_msgbox_get_footer(cmbox);
    lv_obj_set_height(footer, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(footer, 28, 0);
    lv_obj_set_style_pad_row(footer, 28, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_width(b[i], lv_pct(47));
        lv_obj_set_height(b[i], 115);
    }
    lv_obj_update_layout(lv_layer_top());

    lv_area_t r[4];
    for (int i = 0; i < 4; i++) lv_obj_get_coords(b[i], &r[i]);
    check(r[0].y1 == r[1].y1 && r[2].y1 == r[3].y1 && r[2].y1 > r[0].y1,
          "confirm footer is 2x2 (two rows of two), not a 4x1 stack");
    check(r[1].x1 > r[0].x2 && r[3].x1 > r[2].x2,
          "the two buttons in each row are side by side");
    check(r[1].x1 - r[0].x2 >= 20 && r[3].x1 - r[2].x2 >= 20,
          "there is a real horizontal gap between the buttons");
    check(r[2].y1 - r[0].y2 >= 20,
          "there is a real vertical gap between the rows");

    printf("\npassed: %d  failed: %d\n", passes, fails);
    return fails ? 1 : 0;
}
