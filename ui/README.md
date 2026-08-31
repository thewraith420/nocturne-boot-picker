# ui

The actual touch menu: a list of real installed kernels, rendered on the
Slate's 3000x2000 @ 3:2 panel, with touch targets large enough to hit
comfortably at that DPI without a stylus.

## Decision #2, reopened

Originally raw DRM+fbdev (minimal dependency footprint, privileged
pre-boot environment). After seeing the raw-DRM version running on real
hardware, Bob asked for a real TWRP-like look (icons, color, theming),
which isn't a good fit for hand-rolled `fill_rect`/bitmap-font code.
Reopened and now uses **LVGL** (v9.2.2, pinned) - bigger dependency and
initramfs, but gets a real themed widget/dialog UI far faster than
hand-rolling one. Fetched at build time via `./fetch-lvgl.sh`, not
vendored into git (same convention as `picker-kernel/linux-*/` -
see `../.gitignore`).

## What's in place

`picker.c`:
- Opens the first connected DRM output via legacy KMS (dumb buffer,
  `drmModeSetCrtc`) exactly as the raw-DRM version did.
- Finds an evdev touch device (`ABS_MT_POSITION_X`, `ABS_X` fallback),
  scaled from the device's real `ABS_MT_POSITION_X/Y` range via
  `EVIOCGABS` - not assumed 1:1 with framebuffer pixels.
- Renders the kernel list as LVGL buttons in a themed dark UI (default
  theme, blue accent); tapping one opens a real confirmation dialog
  (`lv_msgbox`) with a 2x2 button grid - Edit / Set Default on top,
  Boot (accented) / Cancel below, matching a mockup Bob signed off on -
  rather than the old double-tap-same-row scheme. **Edit**: opens an
  `lv_textarea` + `lv_keyboard` pre-filled with the entry's cmdline;
  Save mutates that entry's in-memory copy only (never persisted, same
  one-time-tweak semantics as GRUB's own 'e' edit-before-boot) and
  returns to a fresh confirm dialog showing the edited line. **Set
  Default**: boots this entry now (same as Boot) and also asks
  `initramfs/init` to persist it as the future default via
  `SET_DEFAULT=1` in the picker's stdout output - see
  `initramfs/README.md` for how that's actually written to disk. Both
  found and fixed a real layout bug in testing: the msgbox footer's
  inherited flex gap made two 50%-width buttons individually overflow
  their row, so all four stacked one-per-line instead of forming a 2x2
  grid, until the gap was explicitly zeroed.
- `PICKER_TIMEOUT_SECS` (default 10, `0` disables) auto-boots the first
  entry if nothing's tapped, cancelled by the first touch - mirrors
  GRUB's own timeout-to-default behavior, the actual safety net for a
  keyboardless device with no other escape hatch.
- `VT_SETMODE`/`signalfd` VT-switch cooperation (drops DRM master on a
  release request, reacquires on return) - found necessary by real
  testing: without it, Ctrl+Alt+F1 hung the VT switch hard enough to
  need a power cycle.

**LVGL is deliberately kept rotation-agnostic** - `lv_display_set_rotation`
is never called. Investigating LVGL's rotation support directly (see
"What testing found" below) showed it hands `flush_cb` a buffer still
laid out in *logical* (unrotated) space, and separately hangs outright
when combined with `LV_DISPLAY_RENDER_MODE_FULL`. Rather than depend on
that plus a second, possibly differently-conventioned rotation
implementation living inside LVGL, `PICKER_ROTATE=0|90|180|270` is
handled entirely by this file's own `logical_to_physical`/
`physical_to_logical` transform (carried over from the raw-DRM version,
already verified bijective) at exactly two integration points: the
flush callback (LVGL's logical render -> physical framebuffer) and
touch input (physical digitizer -> LVGL's logical coordinate space).
LVGL's own display is created at the already-rotated *logical*
resolution and never told rotation is happening at all.

## What testing found (this dev environment has no real Slate/i915/touchscreen)

- LVGL v9.2.2 itself: fetched and built clean, 311/311 source files,
  zero errors, against `lv_conf.h` here (color depth bumped to 32 to
  match the DRM dumb buffer's XRGB8888).
- `lv_display_set_rotation(disp, ROTATION_90)` combined with
  `LV_DISPLAY_RENDER_MODE_FULL` hangs outright (confirmed via a
  standalone harness with instrumented tracing - it doesn't even reach
  the first `flush_cb` call). `RENDER_MODE_PARTIAL` doesn't hang, but a
  further check (rendering a known 8x8 square at a known logical
  position and inspecting actual buffer content, not just structural
  flow) showed the pixel data handed to `flush_cb` is still in the
  *unrotated* logical layout regardless of the rotation setting -
  confirming the docs' note that `lv_draw_sw_rotate` is meant to be
  called manually in `flush_cb` for real pixel rotation. This is why
  rotation is handled entirely outside LVGL here instead.
- The actual `flush_cb` pixel-remap loop (LVGL's logical render area ->
  physical DRM buffer via `logical_to_physical`) verified correct via a
  standalone harness reproducing the same loop against fake buffers -
  content lands at the expected physical location for a rotated case.
- The full tap -> confirm-dialog -> Edit -> keyboard/textarea change ->
  Save -> fresh confirm dialog -> Set Default flow verified end-to-end
  via a standalone harness that builds the real UI and drives LVGL's
  indev with synthetic press/release coordinates at the real computed
  positions of each button in turn - confirms the 2x2 grid actually
  renders as two rows of two (not four stacked rows - the bug above),
  that editing the cmdline actually mutates the right entry, and that
  the right kernel index plus the `SET_DEFAULT` flag both come out the
  other end correctly.
- DRM ioctl sequence (connector/encoder/crtc/dumb buffer/framebuffer/
  mmap) still runs correctly against a real (non-i915) DRM device here,
  same as the raw-DRM version - fails only at the final
  `drmModeSetCrtc` due to DRM master contention from another process on
  this dev machine, not expected inside a bare initramfs.
- `signalfd`/`timerfd`/`poll()` mechanics for VT cooperation and the
  auto-boot timeout previously verified working (raw-DRM version); the
  same code carried over unchanged.

## Still needed, on real hardware

Everything touch/i915/VT-switch actually needs the Slate: does DRM
master + i915 modeset still work through LVGL's render path; which
`PICKER_ROTATE` value reads upright; does touch registration work now
that coordinates are properly scaled; does `VT_SETMODE` actually stop
the Ctrl+Alt+F1 hang; does the widget/dialog UI actually look
TWRP-like and legible at the panel's real DPI (font/button sizing was
chosen by eye against LVGL's default theme, not measured against the
real screen); does the 10s auto-boot timeout avoid false-triggering
during normal use.

## Input contract with `initramfs/`

Reads a `title\tlinux\tinitrd\tcmdline` list from
`initramfs/discover-kernels.sh`'s output, by way of `apply-default.sh`
(see `initramfs/init`), writes the selected entry back out as
shell-sourceable `SELECTED_LINUX`/`SELECTED_INITRD`/`SELECTED_CMDLINE`
assignments on stdout, plus `SET_DEFAULT=1` if "Set Default" was used.
