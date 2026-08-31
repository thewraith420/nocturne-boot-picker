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
handle.

**Progress**: `configs/config-7.1-picker` in BobZKernel (`picker-kernel`
branch, commit `c9828a8`) has camera/IPU3/v4l2loopback, Waydroid binder,
Bluetooth, audio (SND), IIO sensors, and the full WLAN stack
(WIRELESS/CFG80211/MAC80211/WLAN) stripped, done via `scripts/config
--disable` + `make olddefconfig` (not hand-edited, so Kconfig dependency
resolution stays consistent). Confirmed kept: KEXEC/KEXEC_FILE, DRM/DRM_I915,
i2c_hid + HID_MULTITOUCH.

**Gotcha worth knowing if you touch this config again**: `WLAN`'s Kconfig
unconditionally `select`s `WIRELESS` back on, so disabling `WIRELESS` alone
and running `olddefconfig` silently does nothing - you have to disable the
real driver-tree symbol (`WLAN`/`CFG80211`/`MAC80211`) first, then `WIRELESS`
sticks once nothing is re-selecting it.

**Not yet touched**: storage and filesystem drivers - blocked on open
question #3 below (boot-entry discovery). No build attempted yet, config
resolution (`olddefconfig`) only.
