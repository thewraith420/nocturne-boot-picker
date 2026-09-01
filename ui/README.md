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
  `EVIOCGABS` - not assumed 1:1 with framebuffer pixels. Real-hardware
  testing found capability bits alone aren't enough to pick the right
  device: the Slate exposes five separate Wacom pen/touch sub-interface
  nodes, and one of the non-touch ones happened to advertise
  `ABS_MT_POSITION_X` too, so `touch_open()` locked onto it and touch
  was completely dead - not misaligned, zero response. Fixed by scanning
  every `/dev/input/eventN` candidate (not stopping at the first
  capability match) and preferring one that reports `INPUT_PROP_DIRECT`
  via `EVIOCGPROP` - the kernel's actual "this is a touchscreen, not a
  pointer device" signal, which correctly disqualifies the Wacom
  sub-interfaces even though some of them expose matching ABS axes.
  Also prints the selected device's path and name to stderr, so a
  future hardware round shows exactly what it locked onto rather than
  inferring it from behavior.
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
  `initramfs/README.md` for how that's actually written to disk.
  Confirmed with Bob: "Set Default" boots immediately, not just
  saves-and-returns-to-the-menu - matches the implementation as-is.
- Whichever entry is currently the persisted default shows a
  `LV_SYMBOL_OK` checkmark, fixed to the row's right edge (not part of
  the scrolling title text, so a long title can never push it out of
  view) - `apply-default.sh` marks it via a 5th `is_default` TSV field
  it appends to every line, and `load_entries()`/`build_ui()` read and
  render it. Long titles get `LV_LABEL_LONG_DOT` ellipsis-truncated
  instead of overlapping the checkmark - verified directly (not just
  visually reasoned about) via a standalone harness checking the
  title label's and checkmark's actual computed pixel bounds don't
  intersect, using the real "Ubuntu, with Linux ... (recovery mode)"
  style long title from the real grub.cfg.

The confirm dialog's 2x2 grid also found and fixed a real layout bug
in testing: the msgbox footer's inherited flex gap made two 50%-width
buttons individually overflow their row, so all four stacked
one-per-line instead of forming a 2x2 grid, until the gap was
explicitly zeroed.
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
- `touch_open()`'s device-selection fix (preferring `INPUT_PROP_DIRECT`
  over capability-bits-only matching, after real hardware showed touch
  completely dead - selecting one of the Slate's non-touch Wacom
  sub-interfaces instead of the real digitizer): verified against real
  kernel-level virtual input devices, not mocked. Created three actual
  `/dev/input` nodes via `/dev/uinput` reproducing the exact bug shape -
  two "pointer"-prop devices (one with plain `ABS_X`, one with
  `ABS_MT_POSITION_X` - the actual trap that fooled the old logic) both
  enumerated *before* a third `INPUT_PROP_DIRECT` device with matching
  MT axes - and ran the real extracted `touch_open()` against them,
  confirming it correctly skips both pointer-prop devices despite the
  ordering and picks the direct one. Actually opening the resulting
  `/dev/input/eventN` node hit a real permissions wall in this sandbox
  (not in `input` group, no passwordless sudo) - not expected to apply
  on the Slate, where the picker runs as root - so the device-open step
  itself still needs on-device confirmation, but the selection logic
  that was actually wrong has real-device-backed verification now.

- **Touch down/up detection**: after the `INPUT_PROP_DIRECT` fix above
  landed, real-hardware retest showed `touch_open()` now correctly
  selects `/dev/input/event5` ("WCOM50C1:00 2D1F:486C UNKNOWN",
  `INPUT_PROP_DIRECT=yes`) - but touch still didn't register at all.
  Root cause: this device is driven by `hid_multitouch` (Type B
  multitouch protocol, matching this project's own confirmed hardware
  facts - Wacom often supplies the silicon, but the generic
  `hid_multitouch` kernel driver binds it), which signals finger
  down/up via `ABS_MT_TRACKING_ID` (a real ID means down, `-1` means
  lifted) - it sends no `BTN_TOUCH` at all, which was the only signal
  the touch loop was watching. Fixed by also treating
  `ABS_MT_TRACKING_ID` transitions as a down/up source, alongside
  `BTN_TOUCH` for devices that do send it. Verified with real
  `struct input_event` records pushed through a real pipe (not mocked
  function calls) into the actual event-handling loop copied
  verbatim: a Type-B-style sequence (tracking ID assigned/positions/
  `-1` to lift, no `BTN_TOUCH`) correctly drives `touch_down` through
  1 then back to 0, and a plain `BTN_TOUCH`-only sequence still works
  identically to before (no regression for devices that do send it).

## Real-hardware results so far (`f866572` + touch fixes)

Confirmed on the Slate: DRM master + i915 modeset work through LVGL's
render path; `PICKER_ROTATE=270` is the correct upright orientation;
the `VT_SETMODE` fix works (no repeat of the Ctrl+Alt+F1 hang). Touch
was found completely dead, root-caused in two steps (wrong device
selected, then wrong down/up signal watched even once the right
device was selected) - see the two `touch_open()`/event-loop fixes
above, not yet retested on hardware.

## Still needed, on real hardware

Retest touch now that `touch_open()` prefers `INPUT_PROP_DIRECT` -
tapping the first row should open a confirm dialog naming that same
first entry, not a different one (would indicate a touch-axis-swap bug
independent of device selection); does the widget/dialog UI actually look
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
