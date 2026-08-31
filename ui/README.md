# ui

The actual touch menu: a list of real installed kernels, rendered on the
Slate's 3000x2000 @ 3:2 panel, with touch targets large enough to hit
comfortably at that DPI without a stylus.

**In place** (main README decision #2 - raw DRM+fbdev, no SDL/LVGL):
`picker.c` - opens the first connected DRM output via legacy KMS (dumb
buffer, `drmModeSetCrtc`, no atomic modesetting - simplest path for a
static list), finds an evdev device advertising `ABS_MT_POSITION_X` (or
plain `ABS_X` as a single-touch fallback), renders `menu.tsv` as a list
using a real PSF2 console font (`font.psf`, bundled from the standard
`kbd` package's `Lat38-VGA28x16` rather than a hand-authored bitmap
table), and implements tap-then-confirm: first tap on a row highlights
it, a second tap on the *same* row selects it, a tap elsewhere re-targets.
`make` builds it (needs `libdrm-dev`/`libdrm-devel` headers).

**First real-hardware test result** (from the Slate): got DRM master and
mode-set the real i915 display successfully - real text rendered on the
real panel. Two problems found and fixed since:

1. **No escape hatch - fixed.** picker.c held DRM master with no
   VT-switch cooperation; a real Ctrl+Alt+F1 during testing hung the VT
   switch hard enough to need a power cycle. Now uses `VT_SETMODE`
   (`VT_PROCESS` + `signalfd` for the release/acquire signals) to drop
   master on a VT-switch request and reacquire + redraw on return.
   Separately, and more importantly for the actual keyboardless
   deployment this is meant for: added a `PICKER_TIMEOUT_SECS` (default
   10, `0` disables) auto-boot-the-first-entry countdown, cancelled by
   the first touch - mirrors GRUB's own timeout-to-default behavior, so
   a wedged or unresponsive touchscreen in the field doesn't strand the
   device with zero recovery path. Not yet tested on hardware.
2. **Rotation + touch both broken, likely one root cause.** Rendered
   content came out sideways on the physical panel, and touch registered
   zero hits in the same test (kernel-level touch stack independently
   confirmed working via `evtest`). There's no kernel-side
   panel-orientation quirk for Nocturne to lean on
   (`drm_panel_orientation_quirks.c` has no matching DMI entry), so this
   has to be handled here. Added `PICKER_ROTATE=0|90|180|270` (default
   `0`) applied to both rendering and touch coordinates together (touch
   coordinates are assumed to share the panel's fixed physical
   orientation, scaled from the touch device's real `ABS_MT_POSITION_X/Y`
   min/max rather than assumed 1:1 with framebuffer pixels - that was a
   latent bug regardless of rotation). Correct rotation value not known
   yet - needs trying each of the four on the real screen.

**Verified without real hardware:** compiles clean with `-Wall -Wextra`;
the DRM ioctl sequence (connector -> encoder -> crtc -> dumb buffer ->
framebuffer -> mmap) runs correctly against a real (non-i915) DRM device
here; font glyph rendering checked by dumping ASCII art and confirming
real letters, not just "didn't crash"; the four rotation transforms
verified bijective with clean round-trips via a standalone harness (every
physical pixel maps to exactly one logical pixel and back, for all of
0/90/180/270); the `signalfd`/`timerfd`/`poll()` mechanics used for VT
cooperation and the auto-boot timeout verified working via a standalone
harness (raised a real `SIGUSR1`, received it through `signalfd`, timer
ticked as expected) - but `VT_SETMODE` itself, and everything touch/i915,
still needs the real hardware this dev environment doesn't have.

**Still needed, on-device**: confirm `PICKER_ROTATE` value that reads
upright (try all four); confirm touch now actually registers hits once
rotation is right; confirm `VT_SETMODE`/`signalfd` actually stops the
Ctrl+Alt+F1 hang; confirm the 10s auto-boot timeout doesn't false-trigger
during normal use; confirm 3x-scaled 16x28 base glyph text reads
comfortably at the panel's DPI without a stylus.

**Open product question, not yet decided**: Bob's feedback after seeing
it on real hardware was that he'd like this to look more like TWRP -
icons, color, real graphics - rather than a plain text list. That's a
legitimate pull back toward the SDL/LVGL side of decision #2's original
tradeoff, so it's flagged back to the user rather than decided here
before more raw-DRM polish (bigger colored rows, simple bitmap icons)
is attempted - see main README.

Input contract with `initramfs/`: reads a `title\tlinux\tinitrd\tcmdline`
list from `initramfs/discover-kernels.sh`'s output (see `initramfs/init`),
writes the selected entry back out as shell-sourceable
`SELECTED_LINUX`/`SELECTED_INITRD`/`SELECTED_CMDLINE` assignments on
stdout.

Interaction model: tap an entry -> confirm (TWRP-style, to avoid a stray touch
kexec-ing into the wrong kernel) -> hand off to the kexec glue in
`boot-integration/`.
