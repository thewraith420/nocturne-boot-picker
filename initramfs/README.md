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
- `apply-default.sh` - reorders `discover-kernels.sh`'s output so a
  previously "Set Default"-marked entry (see `ui/README.md`) is moved
  to the front, if it's still present; a missing or stale marker just
  passes the list through unchanged. This is the only place that needs
  to know about a persisted preference - `ui/picker.c`'s auto-boot
  timeout and `init`'s picker-failure fallback both already just take
  "the first entry". Also appends a 5th `is_default` field ("1" or
  empty) to every line, so `ui/picker.c` can show a checkmark on the
  current default directly in the menu, not just act on it silently at
  boot time. Verified with a synthetic menu.tsv covering all four
  cases: no marker, marker matches one entry, marker matches multiple
  entries sharing a linux path (the "Ubuntu"/"Ubuntu, with Linux X"
  pairing `discover-kernels.sh` already produces), marker names a
  kernel that's since been removed - checking the 5th field lands
  correctly in every case, not just the reordering.

  Persisting the marker itself needs a brief `mount -o remount,rw` of
  the real root (deliberately mounted read-only otherwise, to keep the
  picker's normal blast radius small), done in `init` right before the final
  `kexec-boot.sh` call, then `remount,ro` again immediately after.
  Best-effort throughout: a failure at any step (remount fails, write
  fails, remount-back-to-ro fails) just logs a warning and boots
  anyway - saving a preference should never be able to block getting
  to a working OS. The remount/write/remount sequence itself was
  rehearsed against a real (throwaway, unprivileged-namespace) mount,
  not just reasoned about - confirmed writes are genuinely blocked
  while read-only, genuinely succeed after the read-write remount, and
  are genuinely blocked again after remounting read-only a second
  time. The full pipeline (first launch sets a default via the picker
  UI, second simulated launch picks it up automatically with no user
  interaction) was also verified end-to-end with mocked
  discover-kernels.sh/picker/kexec-boot.sh.

**Not started:**
- `mdev.conf` / static `/dev` table for the touch + DRM devices
- confirming `/bin/sh` (the rescue-shell fallback) and a keyboard input
  path actually exist/work in the assembled initramfs - the rescue
  shell is only a real safety net if something can actually be typed
  into it
- the build script that assembles all of this plus `ui/picker` (see
  `ui/README.md`), `kexec-tools`, and busybox into a cpio image
