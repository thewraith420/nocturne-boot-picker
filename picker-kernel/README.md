# picker-kernel

The minimal kernel (config + any needed patches) for the touch picker itself.

Not started. First real decision to make (see main README's open questions):
reuse BobZKernel's existing pixel-slate config as a base, or build a truly
minimal separate config (just DRM/KMS + i915, i2c_hid + hid_multitouch,
kexec-tools, nothing else).

Whichever direction: this kernel only needs to draw a touch menu and kexec -
it never needs storage drivers beyond reading its own initramfs, networking,
or anything the real OS kernels handle. Keep it as small as it can be while
still being real enough to have working touch + display.
