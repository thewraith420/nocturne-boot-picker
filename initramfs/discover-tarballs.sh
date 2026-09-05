#!/bin/sh
# Finds installable kernel tarballs on the real root, for the picker's
# Install menu.
#
#   discover-tarballs.sh <root-mount>   ->  path\tversion\tsize
#
# Same split as discover-kernels.sh: the shell finds things, picker only
# draws them. Paths are emitted as the initramfs sees them, so
# install-kernel.sh can use them directly.
#
# Deliberately does NOT open the tarballs. The authoritative kernel
# release lives inside as boot/vmlinuz-<release>, but a .tar.gz has to
# be decompressed end-to-end to read its index - seconds each for
# 120MB archives, on every single boot, to populate a menu most boots
# never visit. The filename is good enough to choose from, and
# install-kernel.sh reads the real release from the archive at install
# time, where being exact actually matters.
#
# Uses shell globs rather than find(1) so the initramfs needs no extra
# busybox applet for this.
set -eu

root=${1:?usage: discover-tarballs.sh <root-mount>}
root=${root%/}                       # "/" would give //home/... paths

for f in \
    "$root"/home/*/*installer.tar.gz \
    "$root"/home/*/*/*installer.tar.gz \
    "$root"/home/*/*/*/*installer.tar.gz \
    "$root"/root/*installer.tar.gz \
    "$root"/root/*/*installer.tar.gz
do
    [ -f "$f" ] || continue          # unmatched glob stays literal

    base=${f##*/}
    ver=${base%-installer.tar.gz}
    ver=${ver#BobZKernel-}

    # Bytes -> a human size, without needing du(1).
    size=$(ls -l "$f" 2>/dev/null | awk '{
        b = $5
        if (b >= 1073741824) printf "%.1fG", b/1073741824
        else if (b >= 1048576) printf "%dM", b/1048576
        else printf "%dK", b/1024
    }')

    printf '%s\t%s\t%s\n' "$f" "$ver" "${size:-?}"
done
