#!/bin/sh
# Installs a BobZKernel-style kernel tarball onto the real system, from
# the picker.
#
#   install-kernel.sh <root-mount> <tarball>
#
# The tarball holds boot/{vmlinuz,System.map,config}-<release> and
# lib/modules/<release>/. Copying those is the easy half. The other half
# - depmod, update-initramfs, update-grub - has to run with the REAL
# system's tools against its own /lib and config, so it runs under
# chroot rather than being reimplemented here. That is what a distro
# installer does, and reimplementing initramfs generation in busybox
# would be both large and subtly wrong.
#
# Why doing this from the picker is worth anything: the root filesystem
# is not in use. No package manager holding locks, no running kernel
# whose modules are being replaced underneath it.
#
# Deliberately additive. Nothing is deleted or overwritten except this
# release's own files, so every currently-bootable kernel stays
# bootable - including the one you would fall back to if this goes
# wrong. update-grub regenerates grub.cfg, which is safe for the picker
# itself: its entry lives in custom.cfg, which 41_custom sources
# separately and grub-mkconfig never rewrites.
set -eu

root=${1:?usage: install-kernel.sh <root-mount> <tarball>}
tarball=${2:?usage: install-kernel.sh <root-mount> <tarball>}

say()  { echo "install-kernel: $*" >&2; }
die()  { echo "install-kernel: ERROR: $*" >&2; exit 1; }

[ -d "$root" ]     || die "root mount $root is not a directory"
[ -f "$tarball" ]  || die "tarball $tarball not found"

# ---------------------------------------------------------------- inspect
# Read the release from the archive rather than the filename: the
# filename is a label, boot/vmlinuz-<release> is the truth, and
# depmod/update-initramfs must be given exactly the right string.
say "reading $tarball"
release=$(tar tzf "$tarball" 2>/dev/null \
          | grep -E '(^|/)boot/vmlinuz-' \
          | head -n1 \
          | awk -F'boot/vmlinuz-' '{print $2}')
release=${release%/}
[ -n "$release" ] || die "no boot/vmlinuz-* inside $tarball - not a kernel tarball?"
say "kernel release: $release"

if [ -e "$root/boot/vmlinuz-$release" ]; then
    say "note: $release is already installed - reinstalling over it"
fi

# ------------------------------------------------------------- make writable
# The picker mounts the real root read-only on purpose. This is the one
# operation that genuinely needs it writable, and it goes back to
# read-only on the way out, including on failure.
say "remounting $root read-write"
mount -o remount,rw "$root" || die "could not remount $root read-write"

cleanup() {
    for d in dev/pts dev proc sys; do
        umount "$root/$d" 2>/dev/null || true
    done
    mount -o remount,ro "$root" 2>/dev/null || \
        say "WARNING: could not remount $root read-only again"
}
trap cleanup EXIT

# ---------------------------------------------------------------- extract
# Straight into the root: the archive's layout (./boot/..., ./lib/...)
# is already the destination layout.
say "extracting kernel and modules (this takes a moment)"
tar xzf "$tarball" -C "$root" ./boot ./lib 2>/dev/null \
    || tar xzf "$tarball" -C "$root" boot lib \
    || die "extract failed"

[ -f "$root/boot/vmlinuz-$release" ] || die "vmlinuz-$release missing after extract"
[ -d "$root/lib/modules/$release" ]  || die "modules for $release missing after extract"
sync

# --------------------------------------------------- finish inside the system
# From here the real system's own tooling does the work.
say "preparing chroot"
mount -t proc  none "$root/proc" 2>/dev/null || die "could not mount /proc in the chroot"
mount -t sysfs none "$root/sys"  2>/dev/null || die "could not mount /sys in the chroot"
mount -o bind /dev "$root/dev"   2>/dev/null || die "could not bind /dev into the chroot"
mount -o bind /dev/pts "$root/dev/pts" 2>/dev/null || true

say "depmod $release"
chroot "$root" /sbin/depmod -a "$release" || die "depmod failed"

# The long one - minutes on eMMC. Without an initramfs the new kernel
# cannot mount root (this hardware's storage/graphics drivers are
# modules in the real kernels), so a failure here means the install must
# not be treated as successful.
say "update-initramfs -c -k $release (slow - do not power off)"
chroot "$root" /usr/sbin/update-initramfs -c -k "$release" \
    || die "update-initramfs failed - $release is NOT bootable; other kernels are untouched"

say "update-grub"
chroot "$root" /usr/sbin/update-grub || die "update-grub failed"

sync
say "installed $release successfully"
say "it will appear in the picker's kernel list on the next boot"
exit 0
