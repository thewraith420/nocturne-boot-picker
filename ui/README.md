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

**Verified so far** (no real Slate/i915/touchscreen available for this
pass - see below): compiles clean with `-Wall -Wextra`; the DRM ioctl
sequence (connector -> encoder -> crtc -> dumb buffer -> framebuffer ->
mmap) runs correctly end-to-end against a real (non-i915) DRM device
here, failing only at the final `drmModeSetCrtc` because something else
on this dev machine already holds DRM master - expected to succeed on
the Slate since the picker is the only userspace process running in a
bare initramfs; font parsing verified by rendering glyphs to ASCII art
and checking they're the right letters, not just "didn't crash".
Touch-device enumeration compiles and runs correctly against the shape
of `/dev/input/eventN`, but couldn't be exercised against a real
touchscreen here.

**Still needed, on-device (real hardware or the Slate-side session)**:
confirm the picker actually gets DRM master and modesets the real i915
output; confirm the real touch device path/name and that
`ABS_MT_POSITION_X` events come through as expected; confirm 3-line
`FONT_SCALE` (3x on a 16x28 base glyph) reads comfortably at the panel's
DPI without a stylus - may need tuning once seen on the actual screen.

Input contract with `initramfs/`: reads a `title\tlinux\tinitrd\tcmdline`
list from `initramfs/discover-kernels.sh`'s output (see `initramfs/init`),
writes the selected entry back out as shell-sourceable
`SELECTED_LINUX`/`SELECTED_INITRD`/`SELECTED_CMDLINE` assignments on
stdout.

Interaction model: tap an entry -> confirm (TWRP-style, to avoid a stray touch
kexec-ing into the wrong kernel) -> hand off to the kexec glue in
`boot-integration/`.
