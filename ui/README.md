# ui

The actual touch menu: a list of real installed kernels, rendered on the
Slate's 3000x2000 @ 3:2 panel, with touch targets large enough to hit
comfortably at that DPI without a stylus.

**Decided** (main README decision #2): raw DRM+fbdev, no SDL/LVGL - keep the
dependency footprint minimal, this runs in a privileged pre-boot environment
with no real OS protections around it. Not started writing code yet - this
is the piece most coupled to real hardware behavior (framebuffer setup,
evdev device paths), so it's being tackled as its own dedicated pass rather
than alongside the initramfs/boot-integration plumbing.

Input contract with `initramfs/`: reads a `title\tlinux\tinitrd\tcmdline`
list from `initramfs/discover-kernels.sh`'s output (see `initramfs/init`),
writes the selected entry back out as shell-sourceable
`SELECTED_LINUX`/`SELECTED_INITRD`/`SELECTED_CMDLINE` assignments.

Interaction model: tap an entry -> confirm (TWRP-style, to avoid a stray touch
kexec-ing into the wrong kernel) -> hand off to the kexec glue in
`boot-integration/`.
