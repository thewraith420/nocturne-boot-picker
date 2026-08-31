# boot-integration

Two things live here:

1. **The GRUB entry** that boots the picker kernel+initramfs by default (GRUB
   itself stays completely unmodified otherwise - short/no timeout, no menu
   changes, no touch support added to GRUB). This is the *only* GRUB-facing
   piece of the whole project.
2. **The kexec glue**: given a selected real kernel entry, the actual
   `kexec -l ... && kexec -e` invocation (or equivalent) that jumps to it -
   needs the real kernel's vmlinuz/initrd/cmdline, which ties into open
   question #3 (how the picker discovers what's actually installed).

Not started.
