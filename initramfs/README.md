# initramfs

The root filesystem the picker kernel boots into: just enough userspace to
bring up the display, read touch input, render the menu, and call `kexec -e`
on selection.

**In place:**
- `init` - PID 1: mounts `/proc` `/sys` `/dev`, mounts the real root
  partition (`/dev/mmcblk0p2`, ext4 - confirmed on hardware, no separate
  `/boot` mount, no LABEL) read-only, runs discovery against
  `/boot/grub/grub.cfg` on it, runs the UI, hands the selection to
  `boot-integration/kexec-boot.sh`. Has a real fallback chain, not just
  the happy path: if the root mount or `discover-kernels.sh` fails (or
  finds zero entries), drops to a rescue shell (`exec /bin/sh`) instead
  of continuing with nothing to boot; if `picker` itself fails or
  produces no usable selection (no touch device, no DRM output, a
  crash), falls back to the first discovered kernel instead - same
  "first entry is the default" convention `PICKER_TIMEOUT_SECS` uses in
  `ui/picker.c`. This matters because `kexec-boot.sh` is `exec`'d in
  place of PID 1: its own `set -eu`/`${2:?}` correctly refuses to run
  with a missing kernel path, but that clean refusal becomes "Attempted
  to kill init!" instead of anything recoverable if nothing upstream
  caught the failure first. Found by code review (not hardware-
  dependent - pure shell control flow) and verified against the real
  file with mocked dependencies covering both the happy path and every
  failure branch.
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
- confirming `/bin/sh` (the rescue-shell fallback) and a keyboard input
  path actually exist/work in the assembled initramfs - the rescue
  shell is only a real safety net if something can actually be typed
  into it
- the build script that assembles all of this plus `ui/picker` (see
  `ui/README.md`), `kexec-tools`, and busybox into a cpio image
