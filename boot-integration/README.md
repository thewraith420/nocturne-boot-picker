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
   then `kexec -e`.

Not yet tested against real hardware - both pieces are written against the
GRUB config shape from a synthetic test file, not the Slate's actual
`grub.cfg`.
