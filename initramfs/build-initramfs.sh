#!/bin/sh
# Assembles the picker initramfs into a cpio.gz the picker kernel can
# boot, then verifies the result can actually work before you trust it.
#
#   ./build-initramfs.sh [output.img]
#
# Run this ON THE TARGET (the Slate), or on a machine with a matching
# userland: the image bundles the local busybox, kexec, and the shared
# libraries ui/picker is linked against, so a mismatched libc here
# means a picker that won't start there.
#
# Everything it needs is checked up front and reported by name - a
# missing piece fails the build rather than producing an image that
# panics at boot with no console to say why.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
out=${1:-$here/picker-initramfs.img}
staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT

# Applets init/discover-kernels.sh/apply-default.sh/kexec-boot.sh use.
# The second group is used only by init's diagnostics (see the boot-log
# block in init): without them a failed boot leaves nothing behind, which
# is what made the first real attempt impossible to debug. They are
# applet symlinks into the one busybox binary, so they cost no space.
APPLETS="sh mount umount mkdir echo printf cut head awk cat ls
         sleep dmesg uname tail sync date wc"

say() { echo "==> $*"; }
die() { echo "build-initramfs: $*" >&2; exit 1; }

# ---------------------------------------------------------------- preflight

missing=
need_cmd() { command -v "$1" >/dev/null 2>&1 || missing="$missing $1"; }
need_cmd busybox
need_cmd cpio
need_cmd gzip
need_cmd fakeroot
[ -n "$missing" ] && die "missing build tools:$missing
  Debian/Ubuntu: sudo apt install busybox-static cpio gzip fakeroot"

kexec_bin=$(command -v kexec || true)
[ -n "$kexec_bin" ] || die "kexec not found - the picker's whole job is to kexec.
  Debian/Ubuntu: sudo apt install kexec-tools"

picker_bin=$repo/ui/picker
[ -x "$picker_bin" ] || die "$picker_bin not built.
  cd $repo/ui && ./fetch-lvgl.sh && make"

for f in "$here/init" "$here/discover-kernels.sh" "$here/apply-default.sh" \
         "$repo/boot-integration/kexec-boot.sh"; do
    [ -r "$f" ] || die "missing source file: $f"
done

for a in $APPLETS; do
    busybox --list | grep -qx "$a" || die "this busybox lacks the '$a' applet"
done

# --------------------------------------------------------------- staging root

say "staging root at $staging"
mkdir -p "$staging"/bin "$staging"/sbin "$staging"/proc "$staging"/sys \
         "$staging"/dev/pts "$staging"/run/picker "$staging"/mnt/root \
         "$staging"/lib "$staging"/lib64 "$staging"/etc

install -m 0755 "$(command -v busybox)" "$staging/bin/busybox"
for a in $APPLETS; do
    [ -e "$staging/bin/$a" ] || ln -s busybox "$staging/bin/$a"
done
# mdev lives in /sbin on most systems; init calls it bare so either works
ln -sf ../bin/busybox "$staging/sbin/mdev"

install -m 0755 "$kexec_bin" "$staging/sbin/kexec"
ln -sf ../sbin/kexec "$staging/bin/kexec"
install -m 0755 "$picker_bin" "$staging/bin/picker"

# init references these by absolute path - keep them in lockstep with
# initramfs/init, which is the source of truth for where they live.
install -m 0755 "$here/init"                        "$staging/init"
install -m 0755 "$here/discover-kernels.sh"         "$staging/bin/discover-kernels.sh"
install -m 0755 "$here/apply-default.sh"            "$staging/bin/apply-default.sh"
install -m 0755 "$repo/boot-integration/kexec-boot.sh" "$staging/sbin/kexec-boot.sh"

# ------------------------------------------------------------ shared libraries

# Copy every .so each binary actually needs, plus the dynamic loader.
# Missing one of these is the classic "init exists but the kernel says
# it can't run it" boot failure, so resolve them from ldd rather than
# guessing a list.
copy_libs_for() {
    bin=$1
    ldd "$bin" 2>/dev/null | while read -r line; do
        case $line in
            *"=>"*) lib=$(echo "$line" | awk '{print $3}') ;;
            /*)     lib=$(echo "$line" | awk '{print $1}') ;;
            *)      continue ;;
        esac
        [ -n "${lib:-}" ] && [ -e "$lib" ] || continue
        dest="$staging$lib"
        [ -e "$dest" ] && continue
        mkdir -p "$(dirname "$dest")"
        cp -L "$lib" "$dest"
        echo "    $lib"
    done
}

say "resolving shared libraries"
for b in "$staging/bin/busybox" "$staging/sbin/kexec" "$staging/bin/picker"; do
    echo "  $(basename "$b"):"
    copy_libs_for "$b"
done

# ---------------------------------------------------------------------- pack

say "packing $out"
( cd "$staging" && find . -print0 |
    fakeroot sh -c '
        # /dev/console must exist before init runs: the kernel opens it
        # for init stdio, and without it any failure message from init
        # (including the rescue-shell path) goes nowhere.
        mknod -m 622 dev/console c 5 1
        mknod -m 666 dev/null    c 1 3
        mknod -m 666 dev/tty     c 5 0
        find . | cpio -o -H newc --quiet
    ' ) | gzip -9 > "$out"

say "built $out ($(du -h "$out" | cut -f1))"
say "verifying"
"$here/verify-initramfs.sh" "$out"
