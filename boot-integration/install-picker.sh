#!/bin/sh
# Installs the picker kernel + initramfs and adds a GRUB menu entry for
# them, without touching anything else about how this machine boots.
#
#   sudo ./install-picker.sh <vmlinuz> <initramfs.img>
#   sudo ./install-picker.sh --uninstall
#
# Deliberately conservative, because the failure mode is "this laptop
# now boots into a stripped-down kernel by default". Three properties,
# each structural rather than a matter of remembering to be careful:
#
#   1. Files go in /boot/picker/, a SUBDIRECTORY. GRUB's 10_linux
#      auto-detection globs /boot/vmlinuz-* and does not recurse, so a
#      kernel here can never be picked up and turned into a menu entry
#      on its own - not now, and not during some future apt upgrade
#      that regenerates grub.cfg at a moment nobody is watching.
#
#   2. The menu entry goes in /boot/grub/custom.cfg, which
#      /etc/grub.d/41_custom sources at boot time. So this NEVER runs
#      update-grub/grub-mkconfig: the existing menu is not regenerated,
#      not reordered, and no other entry changes position. Running the
#      kernel's own install.sh would regenerate the menu, which is what
#      this script exists to avoid.
#
#   3. The entry is appended, so with GRUB_DEFAULT=0 it cannot become
#      the default. The script refuses to run if GRUB_DEFAULT is
#      'saved', where booting the picker once would silently make it
#      the permanent default.
#
# Undo is `--uninstall`, or by hand: delete /boot/picker and the marked
# block in /boot/grub/custom.cfg. Nothing else was modified.

set -eu

BEGIN_MARK="### BEGIN nocturne-boot-picker ###"
END_MARK="### END nocturne-boot-picker ###"
PICKER_DIR=/boot/picker
CUSTOM_CFG=/boot/grub/custom.cfg

die() { echo "install-picker: $*" >&2; exit 1; }
say() { echo "==> $*"; }

[ "$(id -u)" = 0 ] || die "needs root (writes to /boot). Re-run with sudo."

# --------------------------------------------------------------- uninstall

if [ "${1:-}" = "--uninstall" ]; then
    if [ -f "$CUSTOM_CFG" ]; then
        say "removing picker entry from $CUSTOM_CFG"
        sed -i "/$BEGIN_MARK/,/$END_MARK/d" "$CUSTOM_CFG"
        # leave an empty custom.cfg rather than deleting it; something
        # else may be relying on it existing
        [ -s "$CUSTOM_CFG" ] || say "$CUSTOM_CFG is now empty (left in place)"
    fi
    if [ -d "$PICKER_DIR" ]; then
        say "removing $PICKER_DIR"
        rm -rf "$PICKER_DIR"
    fi
    say "done - no grub regeneration was needed, and nothing else was touched"
    exit 0
fi

# ---------------------------------------------------------------- install

kernel=${1:-}
initramfs=${2:-}
[ -n "$kernel" ] && [ -n "$initramfs" ] || \
    die "usage: install-picker.sh <vmlinuz> <initramfs.img>
       install-picker.sh --uninstall"
[ -r "$kernel" ]    || die "cannot read kernel image: $kernel"
[ -r "$initramfs" ] || die "cannot read initramfs: $initramfs"

case $(file -b "$kernel" 2>/dev/null) in
    *"Linux kernel"*|*"bzImage"*) : ;;
    *) die "$kernel does not look like a Linux kernel image" ;;
esac

# The one configuration that could still make this the default boot.
grub_default=$(grep -E '^GRUB_DEFAULT=' /etc/default/grub 2>/dev/null | cut -d= -f2- | tr -d '"' || true)
case $grub_default in
    saved)
        die "GRUB_DEFAULT=saved on this system.
  With 'saved', booting the picker once would make it the permanent
  default - the opposite of the intended rollout. Set GRUB_DEFAULT=0
  first, or add the entry by hand knowing the consequence." ;;
    ''|0) : ;;
    *) echo "install-picker: note: GRUB_DEFAULT=$grub_default (not 0)." >&2
       echo "  The picker entry is appended last, so it should not become" >&2
       echo "  the default, but double-check that's what you expect." >&2 ;;
esac

say "installing into $PICKER_DIR (invisible to GRUB auto-detection)"
mkdir -p "$PICKER_DIR"
cp "$kernel"    "$PICKER_DIR/vmlinuz"
cp "$initramfs" "$PICKER_DIR/initramfs.img"
chmod 0644 "$PICKER_DIR/vmlinuz" "$PICKER_DIR/initramfs.img"

# Resolve the GRUB device spec for whatever /boot lives on, so the
# entry works whether or not /boot is its own partition.
boot_uuid=$(findmnt -no UUID --target /boot 2>/dev/null || true)
[ -n "$boot_uuid" ] || die "could not determine the UUID of the filesystem holding /boot"

# Paths inside the entry must be relative to that filesystem's root:
# with a separate /boot partition the leading /boot is not part of the
# path GRUB sees.
if findmnt -no TARGET --target /boot | grep -qx /boot; then
    kpath=/picker/vmlinuz
    ipath=/picker/initramfs.img
else
    kpath=/boot/picker/vmlinuz
    ipath=/boot/picker/initramfs.img
fi

# The picker kernel needs the same panel quirks every other entry on
# this machine already carries. Without i915.enable_dpcd_backlight=2 and
# i915.enable_psr=0 the Slate's panel produces NO VISIBLE OUTPUT - and
# it fails silently in the worst way: drmModeSetCrtc returns success, so
# picker's own error handling has nothing to catch. Modeset works, the
# backlight simply never lights. That cost a boot cycle where the entire
# chain (root mount, discovery, DRM, touch, render, timeout, kexec) ran
# perfectly against a black screen.
#
# Rather than hardcode the quirks, take them from /proc/cmdline: the
# running system is BY DEFINITION a working display configuration on
# this hardware, so whatever makes the panel light up now gets carried
# to the picker. Only i915.* is copied - root=, quiet, splash and
# crashkernel belong to the real OS, not to an initramfs that mounts its
# own root and wants its diagnostics visible.
if [ -n "${PICKER_CMDLINE+x}" ]; then
    picker_cmdline=$PICKER_CMDLINE
    say "using PICKER_CMDLINE from the environment"
else
    picker_cmdline=$(tr ' ' '\n' < /proc/cmdline | grep '^i915\.' | tr '\n' ' ' | sed 's/ *$//')
fi

if [ -n "$picker_cmdline" ]; then
    say "picker kernel cmdline: $picker_cmdline"
else
    echo "install-picker: note: no i915.* options found in /proc/cmdline, so the" >&2
    echo "  picker entry gets a bare kernel line. If the picker boots to a black" >&2
    echo "  screen but the boot log shows it ran fine, this is the first suspect:" >&2
    echo "  set PICKER_CMDLINE='...' and re-run." >&2
fi

say "adding menu entry to $CUSTOM_CFG (no grub regeneration)"
touch "$CUSTOM_CFG"
# Replace any previous block so re-running doesn't stack duplicates.
sed -i "/$BEGIN_MARK/,/$END_MARK/d" "$CUSTOM_CFG"
cat >> "$CUSTOM_CFG" <<EOF
$BEGIN_MARK
# Added by nocturne-boot-picker/boot-integration/install-picker.sh
# Remove this block (or run install-picker.sh --uninstall) to undo.
# Deliberately NOT the default entry: select it from the GRUB menu.
menuentry 'Boot Picker (touch)' --id picker {
        insmod gzio
        insmod part_gpt
        insmod ext2
        search --no-floppy --fs-uuid --set=root $boot_uuid
        linux   $kpath $picker_cmdline
        initrd  $ipath
}
$END_MARK
EOF

say "installed:"
echo "    $PICKER_DIR/vmlinuz        ($(du -h "$PICKER_DIR/vmlinuz" | cut -f1))"
echo "    $PICKER_DIR/initramfs.img  ($(du -h "$PICKER_DIR/initramfs.img" | cut -f1))"
echo "    menu entry 'Boot Picker (touch)' appended to $CUSTOM_CFG"
echo
echo "  grub.cfg was NOT regenerated and no existing entry moved."
echo "  Reboot and choose 'Boot Picker (touch)' from the menu; every"
echo "  normal entry still boots exactly as before. Undo at any time:"
echo "    sudo $0 --uninstall"
