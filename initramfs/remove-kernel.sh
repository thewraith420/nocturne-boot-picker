#!/bin/sh
# Removes an installed kernel from the real system, from the picker.
#
#   remove-kernel.sh <root-mount> <kernel-release>
#
# The mirror of install-kernel.sh: delete /boot/{vmlinuz,initrd.img,
# System.map,config}-<release> and /lib/modules/<release>, then run
# update-grub inside the real system so the menu entries go with them.
#
# This is the one operation in this project that can make the machine
# unbootable, so it refuses more than it accepts. The kernel's own
# uninstall.sh checks nothing beyond being root and will cheerfully
# remove your last kernel; from a boot picker that is not acceptable.
#
# Guards, in the order they matter:
#   1. At least one other kernel must remain. Removing the last one
#      leaves nothing for GRUB or the picker to boot.
#   2. Never the running kernel. In the picker that is the picker's own
#      kernel and so never a candidate anyway, but this script is also
#      runnable by hand, where it matters a great deal.
#   3. The kernel must actually be there, so a typo fails loudly rather
#      than "succeeding" having deleted nothing.
#   4. The chroot must be able to resolve the root device BEFORE anything
#      is deleted. Learned the hard way: the first real removal deleted
#      the kernel cleanly and then failed at update-grub, leaving the
#      files gone and grub.cfg still listing them - worse than either
#      finishing or not starting.
set -eu

root=${1:?usage: remove-kernel.sh <root-mount> <kernel-release>}
release=${2:?usage: remove-kernel.sh <root-mount> <kernel-release>}

say() { echo "remove-kernel: $*" >&2; }
die() { echo "remove-kernel: ERROR: $*" >&2; exit 1; }

[ -d "$root" ] || die "root mount $root is not a directory"

case "$release" in
    */*|"") die "implausible kernel release: '$release'" ;;
esac

# ------------------------------------------------------------- guard 3
[ -f "$root/boot/vmlinuz-$release" ] || \
    die "no /boot/vmlinuz-$release on the target - nothing removed"

# ------------------------------------------------------------- guard 2
running=$(uname -r 2>/dev/null || echo "")
if [ -n "$running" ] && [ "$running" = "$release" ]; then
    die "refusing to remove the running kernel ($release)"
fi

# ------------------------------------------------------------- guard 1
# Count what would be left. Globs rather than find(1), so this needs no
# extra applet in the initramfs.
remaining=0
for k in "$root"/boot/vmlinuz-*; do
    [ -f "$k" ] || continue
    [ "$k" = "$root/boot/vmlinuz-$release" ] && continue
    remaining=$((remaining + 1))
done
say "$remaining other kernel(s) would remain"
[ "$remaining" -ge 1 ] || \
    die "refusing to remove the only remaining kernel - the machine would not boot"

# ------------------------------------------------------------ make writable
say "removing $release"
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

# ------------------------------------------------- chroot BEFORE deleting
# Set the chroot up and prove it works while everything is still intact.
# The first real removal deleted the kernel cleanly and then failed at
# update-grub with "grub-probe: failed to get canonical path of
# '/dev/mmcblk0p2'", because /dev was never bind-mounted - install-kernel.sh
# does it, this did not. That left the files gone and grub.cfg still
# listing them, which is a worse state than either finishing or not
# starting.
#
# So: mount everything first, then ASK grub-probe to resolve the root
# device. If it cannot, nothing has been deleted yet and we can refuse.
say "preparing chroot"
mount -t proc  none "$root/proc" 2>/dev/null || die "could not mount /proc in the chroot"
mount -t sysfs none "$root/sys"  2>/dev/null || die "could not mount /sys in the chroot"
# grub-probe resolves the root device through real device nodes; without
# this it cannot canonicalise /dev/mmcblk0p2 and update-grub fails.
mount -o bind /dev "$root/dev" 2>/dev/null || die "could not bind /dev into the chroot"
mount -o bind /dev/pts "$root/dev/pts" 2>/dev/null || true

say "checking the chroot can resolve the disk"
chroot "$root" /usr/sbin/grub-probe --target=device / >/dev/null 2>&1 || \
    die "grub-probe cannot resolve the root device inside the chroot - refusing to delete anything"


# ---------------------------------------------------------------- remove
for f in "vmlinuz-$release" "System.map-$release" "config-$release" \
         "initrd.img-$release" "initramfs-$release.img"; do
    if [ -e "$root/boot/$f" ]; then
        rm -f "$root/boot/$f" && say "removed /boot/$f"
    fi
done
if [ -d "$root/lib/modules/$release" ]; then
    rm -rf "$root/lib/modules/$release" && say "removed /lib/modules/$release"
fi

# The picker's saved default points at a vmlinuz path. Leaving it
# pointing at a kernel that no longer exists is survivable -
# apply-default.sh treats a stale marker as "no marker" - but it would
# silently stop working with no clue why, so clear it here.
marker=$root/boot/picker-default
if [ -f "$marker" ] && grep -qx "/boot/vmlinuz-$release" "$marker" 2>/dev/null; then
    rm -f "$marker" && say "cleared the saved default (it pointed at this kernel)"
fi
sync

# ------------------------------------------------- regenerate the menu
# Without this the entries remain in grub.cfg pointing at files that are
# gone. The chroot was set up and proven above, so reaching here and
# failing means something unusual - say so precisely, because the files
# really are gone by this point.
say "update-grub"
chroot "$root" /usr/sbin/update-grub || \
    die "update-grub failed AFTER the files were removed - grub.cfg may still list $release; run 'sudo update-grub' once booted"

sync
say "removed $release successfully"
exit 0
