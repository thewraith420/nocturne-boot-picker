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

**Storage/filesystem stripping done** (BobZKernel commit `d4a354e`), unblocked
by nocturne-boot-picker's decision on open question #3 (parse the real
grub.cfg live). Checked the real hardware to answer both the storage and
filesystem questions at once:

- Root + `/boot` are both on `/dev/mmcblk0p2`, ext4 - no separate `/boot`
  mount. `/boot/efi` (`mmcblk0p1`, vfat) only holds GRUB's own EFI binary,
  not `grub.cfg` - the picker never needs to touch it, no vfat support
  needed.
- `mmcblk0` is eMMC via `sdhci_pci`/`cqhci` on PCI - no NVMe, no SATA/AHCI,
  no UFS on this hardware.

Kept: `MMC`/`MMC_BLOCK`/`MMC_SDHCI`/`MMC_SDHCI_PCI`/`MMC_CQHCI` (the real
eMMC path) + `EXT4_FS`. Everything else in the storage/fs tree stripped.

Config-stripping work for this branch is now complete across every planned
category. No build attempted yet - only `olddefconfig` (config resolution).
