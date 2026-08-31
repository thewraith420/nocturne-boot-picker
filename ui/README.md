# ui

The actual touch menu: a list of real installed kernels, rendered on the
Slate's 3000x2000 @ 3:2 panel, with touch targets large enough to hit
comfortably at that DPI without a stylus.

Not started - blocked on open question #2 in the main README (raw DRM+fbdev
drawing vs. a small SDL/LVGL setup). Whichever is chosen, keep the dependency
footprint minimal - this runs in a privileged pre-boot environment with no
real OS protections around it.

Interaction model: tap an entry -> confirm (TWRP-style, to avoid a stray touch
kexec-ing into the wrong kernel) -> hand off to the kexec glue in
`boot-integration/`.
