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
Touch on this hardware took three real-hardware rounds to actually fix
- each one genuinely progressed the diagnosis, none was a wasted guess,
but none alone was sufficient either:

1. **Device selection, round 1**: `touch_open()` originally picked the
   first `/dev/input/eventN` matching `ABS_MT_POSITION_X`/`ABS_X`
   capability bits. The Slate exposes five Wacom (`WCOM50C1`)
   sub-interfaces sharing one physical pen+touch controller, and
   capability bits alone don't distinguish the real finger digitizer
   from pen/stylus telemetry channels. Fixed to prefer
   `INPUT_PROP_DIRECT` ("touchscreen, not pointer") over plain
   capability matching - verified against real kernel-level `/dev/uinput`
   devices reproducing the bug shape (two pointer-prop devices, one
   with `ABS_MT_POSITION_X`, enumerated before a direct one; correctly
   picked the direct one regardless of order). Real-hardware retest:
   correctly stopped selecting a pointer device, but still landed on
   the wrong one (`event5`) - `INPUT_PROP_DIRECT` alone isn't unique
   either, since three of the five Wacom nodes report it.
2. **Down/up signal**: with `event5` selected, touch still didn't
   register at all. Reasoned that `event5` might be `hid_multitouch`
   (Type B protocol, matching this project's confirmed hardware facts),
   which signals finger down/up via `ABS_MT_TRACKING_ID` rather than
   `BTN_TOUCH` - fixed to also watch tracking-ID transitions, verified
   with real `struct input_event` records through a real pipe into the
   actual event loop. Real-hardware retest: no change at all - this
   diagnosis was itself wrong, because `event5` was never the real
   digitizer to begin with (see below), so no fix to *its* down/up
   signal could have helped.
3. **Device selection, round 2 - the actual fix**: pulled a full
   `/proc/bus/input/devices` dump over SSH and decoded every Wacom
   node's real `ABS` bitmask by hand rather than guessing again.
   Only one of the five (`event2`, unsuffixed base interface) has
   `ABS_MT_SLOT`/`ABS_MT_TRACKING_ID`/true `ABS_MT_POSITION_X` at all -
   `event4` ("Stylus") and `event5` ("UNKNOWN", the one twice
   mis-selected) both report `INPUT_PROP_DIRECT` but only ever expose
   plain `ABS_X`/`ABS_Y` plus pressure/tilt, i.e. pen telemetry, not
   touch. Fixed by scoring every candidate as `is_direct*2 + is_mt*1`
   and keeping the highest - a true-multitouch `DIRECT` device now
   outranks a `DIRECT`-but-`ABS_X`-only one unconditionally. Confirmed
   against the real decoded capability data for all five actual Slate
   devices (not synthetic ones this time): `event2` scores strictly
   higher (3) than every other node (max 2), so the fix isn't even
   order-dependent for this specific hardware. Also broadened the
   stderr diagnostic to print whether `multitouch=yes/no` was used, not
   just `INPUT_PROP_DIRECT`, since that distinction is what round 1's
   diagnostic was missing.

## The 64kB allocator, and why everything was tiny (or frozen)

With touch working, the next problem was that the whole UI rendered
absurdly small: measured on real hardware, the confirm dialog was
260px wide with **13px (1.1mm) tall buttons** and **16px (1.4mm)
text** on a 293 PPI panel. Three separate causes, all now fixed:

1. **`lv_msgbox`'s width default is a hardcoded `LV_DPI_DEF * 2`
   (260px)** - a compile-time constant with no relationship to the
   real display size. Both dialogs now set their own width explicitly
   (`lv_pct(55)` / `lv_pct(70)`).
2. **`LV_FONT_DEFAULT` was Montserrat 14**, a fixed pixel size that
   doesn't scale with DPI at all, and every larger font was disabled
   in `lv_conf.h`. Now Montserrat 28/36/48 are enabled with 36 as the
   default (40px line height = 3.5mm), and the dialog buttons and list
   rows get explicit heights (`DIALOG_BTN_H` / `ROW_H`, ~1cm each at
   this panel's DPI) rather than being sized by the font alone.
3. **`lv_display_set_dpi()` was never called**, so the theme scaled all
   its padding/spacing for a 130 DPI screen. Now set to the panel's
   real 293 PPI, before `lv_theme_default_init()` (which samples it
   once at call time).

**The trap that made this take so long:** the obvious fix (just make
the dialog wider) made the picker *freeze solid* - and, in some
variants, segfault instead. It looked exactly like an LVGL layout bug,
and a long round of bisecting individual style calls produced
maddeningly inconsistent results: near-identical test variants would
hang, crash, or pass. That inconsistency was the real clue. The actual
cause was `LV_USE_STDLIB_MALLOC = LV_STDLIB_BUILTIN` with
`LV_MEM_SIZE = 64kB` - LVGL's fixed pool for bare-metal MCUs, in a
Linux userspace program that has a perfectly good libc. Rendering a
translucent dialog requires a compositing **layer buffer sized to the
dialog**; a 1100px-wide one asks for ~44kB in one allocation, and the
25-row kernel list already holds ~27kB of that 64kB pool. The
allocation fails, and LVGL's recovery path for a failed layer buffer
is to log `Allocating layer buffer failed. Try later` and retry -
forever. Hence: small dialog fine, big dialog frozen; and "inconsistent"
results were just different variants landing either side of the
64kB line. Switching to `LV_STDLIB_CLIB` fixed all of it at once,
including the "high DPI is pathologically slow" symptom, which was the
same exhaustion (bigger padding -> bigger layer buffers).

Enabling `LV_USE_LOG` at WARN turned that multi-hour mystery into a
two-line diagnosis, so it stays on - routed to **stderr** via
`lv_log_register_print_cb()`, never stdout, which carries the
`SELECTED_*` contract that `initramfs/init` sources.

Measured after the fix, at the real 2000x3000 logical resolution with
the full 25-entry menu: dialog 1100x298 (55% of width), footer buttons
531x115 (10.0mm - a real touch target), font line height 40px (3.5mm),
2x2 grid intact, first render 4-8ms, every subsequent render pass
0.0-0.3ms, and zero LVGL warnings. The edit dialog plus on-screen
keyboard - by far the heaviest allocation path, and the one that would
have hung worst - was verified the same way, including a simulated key
tap.

## Real-hardware results so far

Confirmed on the Slate: DRM master + i915 modeset work through LVGL's
render path; `PICKER_ROTATE=270` is the correct upright orientation;
the `VT_SETMODE` fix works (no repeat of the Ctrl+Alt+F1 hang); and
**touch works** - tapping a row opens the confirm dialog naming that
same entry (verified by Bob on-device after the `event2`/multitouch
selection fix).

## Still needed, on real hardware

Confirm the resized/re-fonted UI actually reads well on the panel
(sizes above are computed from the real DPI and verified in a harness,
but nobody has looked at the new build on the actual screen yet);
confirm the on-screen keyboard is usable by finger at this size;
confirm the 10s auto-boot timeout doesn't false-trigger during normal
use.

## Input contract with `initramfs/`

Reads a `title\tlinux\tinitrd\tcmdline` list from
`initramfs/discover-kernels.sh`'s output, by way of `apply-default.sh`
(see `initramfs/init`), writes the selected entry back out as
shell-sourceable `SELECTED_LINUX`/`SELECTED_INITRD`/`SELECTED_CMDLINE`
assignments on stdout, plus `SET_DEFAULT=1` if "Set Default" was used.
