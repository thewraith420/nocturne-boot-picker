# boot-integration

Two things live here:

1. **The GRUB entry** (`grub-picker-entry.cfg`) - the only GRUB-facing
   piece of the whole project, no other changes to GRUB itself. Not made
   the default/only boot path until the picker has proven itself (Bob's
   call - see the file's own comments for the rollout plan): lands first
   as a plain selectable entry alongside GRUB's normal menu/timeout,
   only later flipped to `GRUB_DEFAULT=picker`/`GRUB_TIMEOUT=0` once
   reliable. Sets `--id picker` so `initramfs/discover-kernels.sh` can
   exclude it from the list it hands to the touch menu. Paths are
   placeholders until `picker-kernel`/`initramfs` build outputs land
   somewhere real under `/boot`.
2. **The kexec glue** (`kexec-boot.sh`): given a selected real kernel's
   linux/initrd paths (as they appear in `grub.cfg`) and cmdline, from
   `initramfs/discover-kernels.sh` via `initramfs/init`, runs `kexec -l`
   then `kexec -e`. Deliberately refuses to run (`set -eu`, `${2:?}`) if
   the linux path is missing/empty - `initramfs/init` is responsible for
   never `exec`ing into this script with an empty selection in the first
   place (it's `exec`'d in place of PID 1, so this script's own clean
   refusal would otherwise surface as a kernel panic instead of anything
   recoverable - see `initramfs/README.md`).

3. **The installer** (`install-picker.sh`): puts the picker kernel and
   initramfs in place and adds the menu entry, without changing how
   anything else boots. Three properties, each structural rather than a
   matter of being careful:
   - Files go in **`/boot/picker/`**, a subdirectory. GRUB's `10_linux`
     globs `/boot/vmlinuz-*` and doesn't recurse, so the picker kernel
     can never be auto-detected into a menu entry on its own - not now,
     and not during some future `apt upgrade` that regenerates
     `grub.cfg` when nobody's watching.
   - The entry goes in **`/boot/grub/custom.cfg`**, which
     `/etc/grub.d/41_custom` sources at boot. So it **never runs
     `update-grub`**: the existing menu isn't regenerated, isn't
     reordered, and no other entry moves. (The kernel's own
     `install.sh` *does* regenerate the menu - that's the thing this
     avoids.)
   - It's appended, so with `GRUB_DEFAULT=0` it can't become the
     default; and it refuses to run at all under `GRUB_DEFAULT=saved`,
     where booting the picker once would silently make it permanent.

   `--uninstall` reverses it completely (two files and one marked
   block). Verified against the Slate: `GRUB_DEFAULT=0`,
   `41_custom` sources `custom.cfg`, `/boot` is on the root filesystem
   (so entry paths keep the leading `/boot`), and the detected UUID
   matches the one `initramfs/init` mounts.

`discover-kernels.sh` (which this depends on for what to offer) has since
been verified against the Slate's actual `grub.cfg`, not just a synthetic
test file - see `initramfs/README.md`. `kexec-boot.sh`/`grub-picker-entry.cfg`
themselves are still untested against real hardware.
