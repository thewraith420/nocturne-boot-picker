# picker-kernel

The minimal kernel (config + any needed patches) for the touch picker itself.

**Decided**: lives on BobZKernel's `picker-kernel` branch, forked from
`pixel-slate` (currently at `5d8832c`) - not in this repo. That gets the
platform fixes the picker needs for free (i915 backlight quirks,
`hid_google_hammer` crash fix, GOOG0007 button fix) without a from-scratch
config. Not started stripping it down yet.

This kernel only needs to draw a touch menu and kexec - it never needs
storage drivers beyond reading its own initramfs, networking, camera/IPU3,
v4l2loopback, or Waydroid binder support, or anything the real OS kernels
handle. Next step: strip the inherited pixel-slate config down to DRM/KMS +
i915, i2c_hid + hid_multitouch, kexec-tools, and whatever else touch+display
actually needs - nothing else.
