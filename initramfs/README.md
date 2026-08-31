# initramfs

The root filesystem the picker kernel boots into: just enough userspace to
bring up the display, read touch input, render the menu, and call `kexec -e`
on selection.

**In place:**
- `init` - PID 1: mounts `/proc` `/sys` `/dev`, mounts the real root
  partition (`/dev/mmcblk0p2`, ext4 - confirmed on hardware, no separate
  `/boot` mount, no LABEL) read-only, runs discovery against
  `/boot/grub/grub.cfg` on it, runs the UI, hands the selection to
  `boot-integration/kexec-boot.sh`.
- `discover-kernels.sh` - parses a GRUB config for `menuentry` stanzas at
  any nesting depth (real kernels turned out to live inside an "Advanced
  options" submenu, not flat - see `docs/nocturne-grub.cfg`) into a
  `title\tlinux\tinitrd\tcmdline` list, filtered to entries whose `linux`
  line points at a `vmlinuz` image (excludes memtest86+ and similar
  non-kernel entries) and excluding the picker's own entry (`--id
  picker`). Verified against the real grub.cfg pulled from the Slate -
  correctly extracts all 25 real kernel entries.

**Not started:**
- `mdev.conf` / static `/dev` table for the touch + DRM devices
- the `picker` UI binary itself (`ui/`, raw DRM+fbdev - see main README
  decision #2)
- the build script that assembles all of this plus `kexec-tools` and
  busybox into a cpio image
