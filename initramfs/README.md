# initramfs

The root filesystem the picker kernel boots into: just enough userspace to
bring up the display, read touch input, render the menu, and call `kexec -e`
on selection.

Not started. Needs, at minimum:
- `kexec-tools` (`kexec -l` the selected real kernel + its initrd/cmdline,
  then `kexec -e` to jump to it)
- Whatever the chosen touch-UI approach needs (see main README open
  question #2 - raw DRM+fbdev vs. a small SDL/LVGL setup)
- Enough of `/dev`, `/proc`, `/sys` populated to make DRM/input actually work
  (likely a minimal `mdev`/`udev`-lite setup or a static device table)

Boot entry discovery (main README open question #3) also lives here
conceptually - whatever reads/owns the list of real kernels to offer.
