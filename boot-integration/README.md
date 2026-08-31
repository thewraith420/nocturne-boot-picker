# boot-integration

Two things live here:

1. **The GRUB entry** (`grub-picker-entry.cfg`) that boots the picker
   kernel+initramfs by default (GRUB itself stays completely unmodified
   otherwise - short/no timeout, no menu changes, no touch support added to
   GRUB). This is the *only* GRUB-facing piece of the whole project. Sets
   `--id picker` so `initramfs/discover-kernels.sh` can exclude it from the
   list it hands to the touch menu. Paths are placeholders until
   `picker-kernel`/`initramfs` build outputs land somewhere real under
   `/boot`.
2. **The kexec glue** (`kexec-boot.sh`): given a selected real kernel's
   linux/initrd paths (as they appear in `grub.cfg`) and cmdline, from
   `initramfs/discover-kernels.sh` via `initramfs/init`, runs `kexec -l`
   then `kexec -e`. Deliberately refuses to run (`set -eu`, `${2:?}`) if
   the linux path is missing/empty - `initramfs/init` is responsible for
   never `exec`ing into this script with an empty selection in the first
   place (it's `exec`'d in place of PID 1, so this script's own clean
   refusal would otherwise surface as a kernel panic instead of anything
   recoverable - see `initramfs/README.md`).

`discover-kernels.sh` (which this depends on for what to offer) has since
been verified against the Slate's actual `grub.cfg`, not just a synthetic
test file - see `initramfs/README.md`. `kexec-boot.sh`/`grub-picker-entry.cfg`
themselves are still untested against real hardware.
