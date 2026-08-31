#!/bin/sh
# Given a real-root mount point and a selected menuentry's paths (as
# they appear in grub.cfg, e.g. /boot/vmlinuz-linux) plus its kernel
# cmdline, kexec straight into it. Called by initramfs/init after the
# picker UI writes a selection.
#
# Usage: kexec-boot.sh <real-root-mount> <linux-path> <initrd-path> <cmdline>

set -eu
root="${1:?usage: kexec-boot.sh <real-root-mount> <linux-path> <initrd-path> <cmdline>}"
linux_path="${2:?}"
initrd_path="${3:-}"
cmdline="${4:-}"

set -- -l "$root$linux_path" --command-line="$cmdline"
if [ -n "$initrd_path" ]; then
    set -- "$@" --initrd="$root$initrd_path"
fi

kexec "$@"
kexec -e
