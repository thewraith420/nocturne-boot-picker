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
  failure branch. Also sets `PICKER_ROTATE=270` (overridable), since
  `ui/picker.c` defaults to `0` and 270 is upright on the Slate's panel
  - without it the menu renders sideways. That stayed hidden for a long
  time because rotation drives the display *and* touch transforms
  together, so an unset value is merely sideways rather than
  unresponsive, and because every hardware test so far ran `./picker` by
  hand from a VT with `PICKER_ROTATE` already exported in the shell -
  never through `init`, the one path that didn't set it.
- `test-init.sh` - runs the **real** `init` (absolute paths rewritten
  into a sandbox, so it can't drift from a copy) against mocked
  dependencies, covering the happy path, picker crashing, picker
  selecting nothing, a failed root mount and failed discovery - 20
  assertions over what gets kexec'd, what reaches the screen, and what
  lands in the boot log. It has already earned its keep twice: it caught
  a redirection-order bug (`2>/dev/null >&2` aims stdout at the
  /dev/null fd2 just became, silently discarding the on-screen error
  replay) and a duplicated outcome string from pairing `${v:+...}` with
  `${v:-...}`.
- `discover-tarballs.sh` - finds installable kernel tarballs
  (`*-installer.tar.gz`) under `/home` and `/root` on the real system, for
  the Install menu, emitting `path\tversion\tsize`. Deliberately does not
  open them: the authoritative kernel release is inside as
  `boot/vmlinuz-<release>`, but a `.tar.gz` must be decompressed
  end-to-end to read its index - seconds each for 120MB archives, on
  every boot, to populate a menu most boots never open. The filename is
  enough to choose from, and `install-kernel.sh` reads the real release
  from the archive when it matters. Uses shell globs rather than
  `find(1)` so the image needs no extra applet.
- `install-kernel.sh` - installs one of those tarballs. Unpacking
  `boot/` and `lib/modules/` is the easy half; `depmod`,
  `update-initramfs` and `update-grub` run **inside the real system via
  chroot**, using its own tools against its own configuration, because
  reimplementing initramfs generation in busybox would be both large and
  subtly wrong. Doing this from the picker is worth something precisely
  because the root filesystem is not in use - no package manager locks,
  no running kernel having its modules swapped underneath it. Additive
  by design: nothing is deleted, so every currently-bootable kernel
  (including whatever you would fall back to) stays bootable, and
  `update-grub` regenerating `grub.cfg` is safe for the picker because
  its own entry lives in `custom.cfg`, which `grub-mkconfig` never
  rewrites. The root goes back to read-only on the way out, including on
  failure.
- `remove-kernel.sh` - the mirror of `install-kernel.sh`: deletes
  `/boot/{vmlinuz,initrd.img,System.map,config}-<release>` and
  `/lib/modules/<release>`, then runs `update-grub` inside the real
  system so the menu entries go with the files. **This is the one script
  here that can make the machine unbootable, so it refuses more than it
  accepts.** It will not remove the last remaining kernel (the guard
  that actually matters - the kernel's own `uninstall.sh` checks nothing
  beyond being root and will happily leave you with none), nor the
  running kernel, nor a release whose `vmlinuz` isn't there, nor
  anything path-shaped. It also clears `/boot/picker-default` when that
  pointed at the kernel just removed - `apply-default.sh` tolerates a
  stale marker, but it would silently stop working with no clue why.
  `test-remove-kernel.sh` covers all of that against a fake root, and
  most of its 18 assertions are cases where the right answer is "refuse
  and change nothing".
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

- `build-initramfs.sh` - assembles everything above plus busybox,
  `kexec`, `ui/picker` and the libraries they're linked against into a
  gzipped cpio image (~2MB compressed, ~4MB unpacked). **Run it on the
  Slate**, or somewhere with a matching userland: it bundles the local
  libc, so a mismatch means a picker that won't start on the target.
  Everything it needs is checked up front and named individually -
  a missing `kexec` or unbuilt `ui/picker` fails the build with an
  apt/make line to fix it, rather than producing an image that panics
  at boot with no console to explain why. Device nodes are created
  under `fakeroot`, so no root is needed to build.
- `verify-initramfs.sh` - unpacks a built image and checks the things
  that would otherwise only surface as a bare kernel panic: `/init`
  exists, is executable, and its shebang points at a shell that's
  actually *in the image*; every absolute path `init` references is
  present (that list is read out of `init` itself, so it can't drift
  out of sync); every ELF binary's `DT_NEEDED` libraries **and its
  dynamic loader** are present *inside the image* rather than merely on
  the build host - the classic cause of "kernel can't run init" with no
  further explanation; no dangling busybox applet symlinks; and
  `/dev/console` and `/dev/null` exist as real character devices, since
  without them `init`'s own error messages and the rescue shell have
  nowhere to go. Runs automatically at the end of a build, and can be
  pointed at any image by hand. Confirmed to actually catch breakage,
  not just pass: deliberately removing `libdrm`, removing a script
  `init` calls, and clearing `init`'s executable bit each produce a
  specific failure.

## init races the drivers; a hand-run picker never does

Every hardware test so far ran `ui/picker` by hand on a fully booted
system, where the eMMC, i915 and the I2C-HID touch controller had
finished probing minutes earlier. As PID 1 the situation is inverted:
devtmpfs creates each node when its driver binds, and `init` reaches
`picker` within milliseconds of the kernel handing over. `picker` does
not degrade if hardware is missing - it returns non-zero when `/dev/dri`
has no card (`ui/picker.c:748`) or no touch device is found
(`ui/picker.c:754`) - and the fallback then boots the default silently.

**A device that appears 200 ms late is indistinguishable from one that
is missing entirely**, and both produce exactly the "nothing happened,
straight to the desktop" that the first real boot attempt produced.

The second attempt confirmed it, and the boot log made it a five-minute
diagnosis instead of a guess: `picker: no touch input device found`,
exit 1, `/dev/dri` populated, `/dev/input` holding only `event0-2` plus
`mice`, and a captured `dmesg` whose **last** line is at 0.851s. init
reaches picker within about 850 ms of the kernel starting; the I2C-HID
controller simply had not enumerated yet.

**Where the waiting lives matters, and the first attempt at it got this
wrong.** init originally waited for `/dev/input/event*` - which the
unrelated `event0-2` satisfy instantly, so it waited zero seconds and
picker still failed. A proxy for a check is a *second definition* of the
thing being checked, and it disagreed with the real one. `touch_open()`
scores candidates on `INPUT_PROP_DIRECT` + multitouch (after five Wacom
sub-interfaces once made a naive capability-bit match pick the wrong
node); no shell approximation of that stays correct.

So the split is by who owns the criteria:

- **`picker` waits for DRM and touch** (`PICKER_WAIT_SECS`, default 20,
  0 disables), by retrying the real `drm_open_first_connected()` and
  `touch_open()` every 100 ms. The retry re-runs the actual open, so
  "usable device" keeps exactly one definition however the criteria
  evolve. It reports `touch input device appeared after 1.400s` or
  `gave up waiting ... after 20s` - different facts needing different
  responses.
- **`init` waits only for the root device** (`PICKER_WAIT_ROOT`,
  default 15s), the one device whose criteria it owns: can I mount it.
  Never fatal - a timeout is logged and boot continues.

Note also that i915's DMC firmware lives on the **root** filesystem
(`/lib/firmware/i915/`, 130 files on the Slate) and is *not* in the
initramfs. Missing DMC normally degrades power management rather than
killing the display, so this is a suspect rather than a diagnosis - but
if the boot log ever shows `/dev/dri` populated and picker still failing
to drive the panel, look here next.

## The boot log: why a silent fallback is worse than it sounds

`init`'s fallbacks exist so a picker failure never strands anyone - it
kexecs the first discovered kernel and you land in your normal OS. The
first real boot attempt did exactly that, and the result was a machine
that booted straight to the desktop with no menu, no message and no
trace. From the outside that is *identical* to "GRUB ignored the
selection", and there was no way to tell which had happened: no journal
in that window, no scrollback, and the console output scrolled past
behind the kexec.

So a failure now leaves evidence in two places:

- **On screen**, on the fallback path only: a banner naming the failure,
  the last 15 lines of picker's own stderr, and a
  `PICKER_FALLBACK_PAUSE` (default 8s) hold so it can be read or
  photographed before the kexec wipes the display. This is the only
  channel that works on a machine with no network yet.
- **`/boot/picker-last-boot.log`** on the real root, written on every
  boot: outcome, stage trail, picker's exit code and full stderr,
  `ls` of `/dev/dri` and `/dev/input` (if picker never drew anything,
  a missing DRM node explains it instantly and is unguessable from a
  blank screen), the menu it was working from, and the last 150 lines
  of the picker kernel's `dmesg`. Written via the same brief
  `remount,rw` dance `apply-default.sh` uses, `sync`'d before the kexec
  because the incoming kernel does not replay this page cache, and
  best-effort throughout - diagnostics must never be why a machine
  fails to come up.

`dmesg`, `sleep`, `uname`, `tail`, `sync`, `date` and `wc` are in
`build-initramfs.sh`'s applet list *for this* - they're symlinks into
the one busybox binary, so they cost nothing.

## This init loads no kernel modules - drivers must be built in

`init` mounts `/proc`, `/sys`, `/dev`, runs `mdev -s`, and goes
straight to work. There is no `modprobe`, no `/lib/modules`, and no
module dependency resolution in the image. Everything the picker
touches therefore has to be **built into the picker kernel** (`=y`),
not a module (`=m`):

| Needed for | Config |
| --- | --- |
| finding the root partition at all | `MMC_SDHCI_PCI`, `MMC_BLOCK`, `EXT4_FS` |
| drawing anything | `DRM`, `DRM_I915` |
| touch | `I2C_HID`, `I2C_HID_ACPI`, `HID_MULTITOUCH`, `INPUT_EVDEV` |
| the entire point | `KEXEC` |

Verified against BobZKernel's `configs/config-7.1-picker`
(`picker-kernel` branch): all of the above are `=y` there, so the two
halves fit. **Don't let these drift to `=m`** - the failure is
particularly unhelpful, because without the eMMC driver the root mount
fails before anything can be displayed, so it lands in the rescue
shell with no picker and no obvious reason why.

This is also why the image can't just be booted with the *installed*
BobZKernel to skip waiting on a picker-kernel build: that kernel has
`DRM_I915`, `HID_MULTITOUCH`, `I2C_HID_ACPI` and `MMC_SDHCI_PCI` all
as modules (checked on the Slate), so it would drop to the rescue
shell immediately.

**Not started:**
- `mdev.conf` / static `/dev` table for the touch + DRM devices
- confirming `/bin/sh` (the rescue-shell fallback) and a keyboard input
  path actually exist/work in the assembled initramfs - the rescue
  shell is only a real safety net if something can actually be typed
  into it
- **actually booting it.** Nothing has yet run this image for real:
  every test so far has run `ui/picker` by hand from a VT on the
  already-running OS, so `init` has never been PID 1 and
  `kexec-boot.sh` has never actually kexec'd anything. The image builds
  and verifies clean, which is a different claim from "it boots."
